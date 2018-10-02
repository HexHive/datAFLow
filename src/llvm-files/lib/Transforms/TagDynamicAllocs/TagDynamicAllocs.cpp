//===-- TagDynamicAllocs.cpp - Tag dynamic memory allocs with unique ID ---===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
///
/// \file
/// This pass tags calls to dynamic memory allocation functions (e.g.,
/// \p malloc, \p calloc, etc.) with a randomly-generated identifier that is
/// understood by the fuzzalloc memory allocation library. The original
/// function calls are redirected to their corresponding fuzzalloc version.
///
//===----------------------------------------------------------------------===//

#include <map>
#include <set>

#include <stdint.h>
#include <stdlib.h>

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Analysis/IndirectCallVisitor.h"
#include "llvm/Analysis/MemoryBuiltins.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/Pass.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Transforms/IPO/PassManagerBuilder.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/Cloning.h"

#include "Utils/FuzzallocUtils.h"
#include "debug.h"     // from afl
#include "fuzzalloc.h" // from fuzzalloc

using namespace llvm;

#define DEBUG_TYPE "fuzzalloc-tag-dyn-allocs"

// Adapted from http://c-faq.com/lib/randrange.html
#define RAND(x, y) ((tag_t)((x) + random() / (RAND_MAX / ((y) - (x) + 1) + 1)))

static cl::opt<std::string>
    ClLogPath("fuzzalloc-tag-log",
              cl::desc("Path to log file containing values to tag"));

static cl::opt<bool> ClEnableIndirectCallTag(
    "enable-indirect-call-tagging",
    cl::desc("Enable static tagging of indirect call sites when possible"),
    cl::init(true), cl::Hidden);

STATISTIC(NumOfTaggedDirectCalls, "Number of tagged direct function calls.");
STATISTIC(NumOfTaggedIndirectCalls,
          "Number of tagged indirect function calls.");
STATISTIC(NumOfTaggedFunctions, "Number of tagged functions.");
STATISTIC(NumOfTaggedGlobalVariables, "Number of tagged global variables.");
STATISTIC(NumOfTaggedGlobalAliases, "Number of tagged global aliases.");
STATISTIC(NumOfTrampolines, "Number of trampoline functions inserted.");

namespace {

/// TagDynamicAllocs: Tag dynamic memory allocation function calls (\p malloc,
/// \p calloc and \p realloc) with a randomly-generated identifier (to identify
/// their call site) and call the fuzzalloc function instead
class TagDynamicAllocs : public ModulePass {
private:
  using FuncTypeString = std::pair<std::string, std::string>;

  Module *Mod;

  Function *ReturnAddrF;
  Function *AbortF;
  Function *FuzzallocMallocF;
  Function *FuzzallocCallocF;
  Function *FuzzallocReallocF;

  IntegerType *TagTy;
  IntegerType *SizeTTy;

  SmallPtrSet<Function *, 8> FunctionsToTag;
  SmallPtrSet<Function *, 8> TaggedFunctions;
  SmallPtrSet<GlobalVariable *, 8> GlobalVariablesToTag;
  SmallPtrSet<GlobalAlias *, 8> GlobalAliasesToTag;
  std::map<StructOffset, FuncTypeString> StructOffsetsToTag;
  SmallPtrSet<Argument *, 8> FunctionArgsToTag;

  Constant *castAbort(Type *) const;
  Function *createTrampoline(Function *);

  ConstantInt *generateTag() const;
  void getTagSites();

  bool isTaggableFunction(const Function *) const;
  bool isCustomAllocationFunction(const Function *) const;

  FunctionType *translateTaggedFunctionType(const FunctionType *) const;
  Function *translateTaggedFunction(const Function *) const;
  GlobalVariable *translateTaggedGlobalVariable(GlobalVariable *) const;

  void tagUser(User *, Function *, const TargetLibraryInfo *);
  Instruction *tagCall(CallBase *, Value *) const;
  Instruction *tagPossibleIndirectCall(CallBase *);
  Function *tagFunction(Function *);
  GlobalVariable *tagGlobalVariable(GlobalVariable *);
  GlobalAlias *tagGlobalAlias(GlobalAlias *);

public:
  static char ID;
  TagDynamicAllocs() : ModulePass(ID) {}

  void getAnalysisUsage(AnalysisUsage &) const override;
  bool doInitialization(Module &) override;
  bool runOnModule(Module &) override;
};

} // anonymous namespace

static const char *const AbortFuncName = "abort";
static const char *const FuzzallocMallocFuncName = "__tagged_malloc";
static const char *const FuzzallocCallocFuncName = "__tagged_calloc";
static const char *const FuzzallocReallocFuncName = "__tagged_realloc";

char TagDynamicAllocs::ID = 0;

// Adapted from llvm::checkSanitizerInterfaceFunction
static Function *checkFuzzallocFunc(FunctionCallee FuncOrBitcast) {
  assert(FuncOrBitcast && "Invalid function callee");
  if (isa<Function>(FuncOrBitcast.getCallee()->stripPointerCasts())) {
    return cast<Function>(FuncOrBitcast.getCallee()->stripPointerCasts());
  }

  FuncOrBitcast.getCallee()->print(errs());
  errs() << '\n';
  std::string Err;
  raw_string_ostream OS(Err);
  OS << "fuzzalloc function redefined: " << *FuncOrBitcast.getCallee();
  OS.flush();
  report_fatal_error(Err);
}

Constant *TagDynamicAllocs::castAbort(Type *Ty) const {
  return ConstantExpr::getBitCast(this->AbortF, Ty);
}

Function *TagDynamicAllocs::createTrampoline(Function *OrigF) {
  const auto TrampolineFName =
      "fuzzalloc.__trampoline_" + OrigF->getName().str();
  Function *TrampolineF = this->Mod->getFunction(TrampolineFName);
  if (TrampolineF) {
    return TrampolineF;
  }

  // Create the trampoline function
  LLVMContext &C = this->Mod->getContext();
  TrampolineF =
      Function::Create(OrigF->getFunctionType(), Function::WeakAnyLinkage,
                       TrampolineFName, this->Mod);
  BasicBlock *TrampolineBB = BasicBlock::Create(C, "", TrampolineF);

  IRBuilder<> IRB(TrampolineBB);

  // Use the trampoline's return address (modulo the max tag) as the allocation
  // site tag
  auto *RetAddr = IRB.CreateCall(this->ReturnAddrF, IRB.getInt32(0));

  // XXX I have no idea why I need to cast the return address pointer to a large
  // integer, then mask off the MSBs before casting to the correct type (tag_t).
  // However, if I don't do this, the LLVM backend generates the wrong mov
  // instruction and everything breaks :(
  Value *Tag = IRB.CreatePtrToInt(RetAddr, this->SizeTTy);
  Value *CastTag =
      IRB.CreateIntCast(IRB.CreateAnd(Tag, FUZZALLOC_TAG_MASK), this->TagTy,
                        /* isSigned */ false);
  Value *ModTag =
      IRB.CreateURem(CastTag, ConstantInt::get(this->TagTy, ClDefSiteTagMax));

  // Call a tagged version of the dynamic memory allocation function and return
  // its result
  Function *TaggedF = translateTaggedFunction(OrigF);
  SmallVector<Value *, 4> TaggedCallArgs = {ModTag};
  for (auto &Arg : TrampolineF->args()) {
    TaggedCallArgs.push_back(&Arg);
  }
  IRB.CreateRet(IRB.CreateCall(TaggedF, TaggedCallArgs));

  NumOfTrampolines++;

  return TrampolineF;
}

/// Generate a random tag
ConstantInt *TagDynamicAllocs::generateTag() const {
  return ConstantInt::get(this->TagTy, RAND(ClDefSiteTagMin, ClDefSiteTagMax));
}

void TagDynamicAllocs::getTagSites() {
  // Ensure that we always tag malloc/calloc/realloc
  auto *MallocF = this->Mod->getFunction("malloc");
  if (MallocF) {
    this->FunctionsToTag.insert(MallocF);
  }
  auto *CallocF = this->Mod->getFunction("calloc");
  if (CallocF) {
    this->FunctionsToTag.insert(CallocF);
  }
  auto *ReallocF = this->Mod->getFunction("realloc");
  if (ReallocF) {
    this->FunctionsToTag.insert(ReallocF);
  }

  if (ClLogPath.empty()) {
    return;
  }

  LLVMContext &C = this->Mod->getContext();

  auto InputOrErr = MemoryBuffer::getFile(ClLogPath);
  std::error_code EC = InputOrErr.getError();
  if (EC) {
    std::string Err;
    raw_string_ostream OS(Err);
    OS << "Unable to read fuzzalloc tag log at " << ClLogPath << ": "
       << EC.message();
    OS.flush();
    report_fatal_error(Err);
  }

  SmallVector<StringRef, 16> Lines;
  StringRef InputData = InputOrErr.get()->getBuffer();
  InputData.split(Lines, '\n', /* MaxSplit */ -1, /* KeepEmpty */ false);

  for (auto Line : Lines) {
    if (Line.startswith(FunctionLogPrefix + LogSeparator)) {
      // Parse function
      SmallVector<StringRef, 3> FString;
      Line.split(FString, LogSeparator);

      auto *F = this->Mod->getFunction(FString[1]);
      if (!F) {
        continue;
      }

      // XXX ignore the type (for now)

      this->FunctionsToTag.insert(F);
    } else if (Line.startswith(GlobalVariableLogPrefix + LogSeparator)) {
      // Parse global variable
      SmallVector<StringRef, 2> GVString;
      Line.split(GVString, LogSeparator);

      auto *GV = this->Mod->getGlobalVariable(GVString[1]);
      if (!GV) {
        continue;
      }

      this->GlobalVariablesToTag.insert(GV);
    } else if (Line.startswith(GlobalAliasLogPrefix + LogSeparator)) {
      // Parse global alias
      SmallVector<StringRef, 2> GAString;
      Line.split(GAString, LogSeparator);

      auto *GA = this->Mod->getNamedAlias(GAString[1]);
      if (!GA) {
        continue;
      }

      this->GlobalAliasesToTag.insert(GA);
    } else if (Line.startswith(StructOffsetLogPrefix + LogSeparator)) {
      // Parse struct offset
      SmallVector<StringRef, 6> SOString;
      Line.split(SOString, LogSeparator);

      auto *StructTy = StructType::getTypeByName(C, SOString[1]);
      if (!StructTy) {
        continue;
      }

      unsigned Offset;
      if (SOString[2].getAsInteger(10, Offset)) {
        continue;
      }

      // Record the struct function (and type) as a string so that we can later
      // use getOrInsertFunction when we encounter an indirect call
      this->StructOffsetsToTag.emplace(
          std::make_pair(StructTy, Offset),
          std::make_pair(/* Function name */ SOString[3],
                         /* Function type */ SOString[4]));
    } else if (Line.startswith(FunctionArgLogPrefix + LogSeparator)) {
      // Parse function argument
      SmallVector<StringRef, 3> FAString;
      Line.split(FAString, LogSeparator);

      auto *F = this->Mod->getFunction(FAString[1]);
      if (!F) {
        continue;
      }

      unsigned ArgIdx;
      if (FAString[2].getAsInteger(10, ArgIdx)) {
        continue;
      }

      this->FunctionArgsToTag.insert(F->arg_begin() + ArgIdx);
    }
  }
}

bool TagDynamicAllocs::isTaggableFunction(const Function *F) const {
  StringRef Name = F->getName();

  return Name == "malloc" || Name == "calloc" || Name == "realloc" ||
         this->FunctionsToTag.count(F) > 0;
}

bool TagDynamicAllocs::isCustomAllocationFunction(const Function *F) const {
  StringRef Name = F->getName();

  return Name != "malloc" && Name != "calloc" && Name != "realloc" &&
         this->FunctionsToTag.count(F) > 0;
}

/// Translates a function type to its tagged version.
///
/// This inserts a tag (i.e., the call site identifier) as the first argument
/// to the given function type.
FunctionType *TagDynamicAllocs::translateTaggedFunctionType(
    const FunctionType *OrigFTy) const {
  SmallVector<Type *, 4> TaggedFParams = {this->TagTy};
  TaggedFParams.append(OrigFTy->param_begin(), OrigFTy->param_end());

  return FunctionType::get(OrigFTy->getReturnType(), TaggedFParams,
                           OrigFTy->isVarArg());
}

/// Translates a function to its tagged version.
///
/// This inserts a tag (i.e., the call site identifier) as the first argument
/// and prepends the function name with "__tagged_".
Function *
TagDynamicAllocs::translateTaggedFunction(const Function *OrigF) const {
  const auto NewFName = "__tagged_" + OrigF->getName().str();

  FunctionType *NewFTy = translateTaggedFunctionType(OrigF->getFunctionType());
  FunctionCallee NewC = this->Mod->getOrInsertFunction(NewFName, NewFTy);

  assert(NewC && "Translated tagged function not a function");
  auto *NewF = cast<Function>(NewC.getCallee()->stripPointerCasts());

  NewF->getArg(0)->setName("tag");
  for (unsigned I = 0; I < OrigF->arg_size(); ++I) {
    NewF->getArg(I + 1)->setName(OrigF->getArg(I)->getName());
  }

  return NewF;
}

/// Translate a dynamic allocation function stored in a global variable to its
/// tagged version.
///
/// This inserts a tag (i.e., the call site identifier) as the first argument
/// of the function and prepends the global variable name with "__tagged_".
GlobalVariable *
TagDynamicAllocs::translateTaggedGlobalVariable(GlobalVariable *OrigGV) const {
  const auto NewGVName = "__tagged_" + OrigGV->getName().str();

  FunctionType *NewGVTy = translateTaggedFunctionType(
      cast<FunctionType>(OrigGV->getValueType()->getPointerElementType()));
  auto *NewConst =
      this->Mod->getOrInsertGlobal(NewGVName, NewGVTy->getPointerTo());
  assert(isa<GlobalVariable>(NewConst) &&
         "Translated tagged global variable not a global variable");

  auto *NewGV = cast<GlobalVariable>(NewConst);
  NewGV->copyAttributesFrom(OrigGV);

  return NewGV;
}

/// Translate users of a dynamic memory allocation function so that they use the
/// tagged version instead
void TagDynamicAllocs::tagUser(User *U, Function *F,
                               const TargetLibraryInfo *TLI) {
  LLVM_DEBUG(dbgs() << "replacing user " << *U << " of tagged function "
                    << F->getName() << '\n');

  if (auto *CB = dyn_cast<CallBase>(U)) {
    // The result of a dynamic memory allocation function call is typically
    // cast. Strip this cast to determine the actual function being called
    auto *CalledValue = CB->getCalledOperand()->stripPointerCasts();

    // Work out which tagged function we need to replace the existing
    // function with
    Function *NewF = nullptr;

    if (isMallocLikeFn(U, TLI, /*LookThroughBitCast=*/true)) {
      NewF = this->FuzzallocMallocF;
    } else if (isCallocLikeFn(U, TLI, /*LookThroughBitCast=*/true)) {
      NewF = this->FuzzallocCallocF;
    } else if (isReallocLikeFn(U, TLI, /*LookThroughBitCast=*/true)) {
      NewF = this->FuzzallocReallocF;
    } else if (auto *CalledFunc = dyn_cast<Function>(CalledValue)) {
      if (this->FunctionsToTag.count(CalledFunc) > 0) {
        // The user is the called function itself. Tag the function call
        NewF = translateTaggedFunction(CalledFunc);
      } else {
        // The user of a dynamic allocation function must be an argument to the
        // function call
        //
        // There isn't much we can do in this case (because we do not perform an
        // interprocedural analysis) except to replace the function pointer with
        // a pointer to a trampoline function. The trampoline will calculate a
        // tag dynamically (based on the runtime return address) and pass this
        // tag to a tagged version of the dynamic allocation function
        U->replaceUsesOfWith(F, createTrampoline(F));
      }
    }

    // Replace the original dynamic memory allocation function call
    if (NewF) {
      tagCall(CB, NewF);
    }
  } else if (auto *Store = dyn_cast<StoreInst>(U)) {
    if (auto *GV = dyn_cast<GlobalVariable>(Store->getPointerOperand())) {
      // Save global variables to tag later
      this->GlobalVariablesToTag.insert(GV);
    } else {
      // Replace the stored function with a trampoline, because we have no idea
      // how it will be used. The trampoline will calculate a tag dynamically
      // (based on the runtime return address) and pass this tag to a tagged
      // version of the dynamic allocation function
      Store->replaceUsesOfWith(F, createTrampoline(F));
    }
  } else if (auto *GV = dyn_cast<GlobalVariable>(U)) {
    // Save global variables to tag later
    this->GlobalVariablesToTag.insert(GV);
  } else if (auto *GA = dyn_cast<GlobalAlias>(U)) {
    // Save global aliases to tag later
    this->GlobalAliasesToTag.insert(GA);
  } else if (auto *Const = dyn_cast<Constant>(U)) {
    // Replace unsupported constant with a trampoline.
    //
    // This is treated separately because we cannot call replaceUsesOfWith on a
    // constant
    Const->handleOperandChange(F, createTrampoline(F));
  } else {
    // Replace unsupported user with a trampoline
    U->replaceUsesOfWith(F, createTrampoline(F));
  }
}

/// Replace a function call site (`CS`) with a call to `NewCallee` that is
/// tagged with an allocation site identifier.
///
/// The caller must update the users of the original function call site to use
/// the tagged version.
Instruction *TagDynamicAllocs::tagCall(CallBase *CB, Value *NewCallee) const {
  LLVM_DEBUG(dbgs() << "tagging call site " << *CB << " (in function "
                    << CB->getFunction()->getName() << ") with call to "
                    << NewCallee->getName() << '\n');

  assert(NewCallee->getType()->isPointerTy() &&
         NewCallee->getType()->getPointerElementType()->isFunctionTy() &&
         "Must be a function pointer");

  // The tag value depends where the function call is occuring. If the tagged
  // function is being called from within another tagged function, just pass
  // the first argument (which is guaranteed to be the tag) straight through.
  // Otherwise, generate a new tag. This is determined by reading the metadata
  // of the function
  auto *ParentF = CB->getFunction();
  Value *Tag = this->TaggedFunctions.count(ParentF) > 0
                   ? ParentF->arg_begin()
                   : static_cast<Value *>(generateTag());

  // Copy the original allocation function call's arguments so that the tag is
  // the first argument passed to the tagged function
  SmallVector<Value *, 4> TaggedCallArgs = {Tag};
  TaggedCallArgs.append(CB->arg_begin(), CB->arg_end());

  Value *CastNewCallee;
  IRBuilder<> IRB(CB);

  if (auto *BitCast = dyn_cast<BitCastOperator>(CB->getCalledOperand())) {
    // Sometimes the result of the original dynamic memory allocation function
    // call is cast to some other pointer type. Because this is a function
    // call, the underlying type should still be a function type
    Type *OrigBitCastTy = BitCast->getDestTy()->getPointerElementType();
    assert(isa<FunctionType>(OrigBitCastTy) &&
           "Must be a function call bitcast");

    // Add the tag (i.e., the call site identifier) as the first argument to
    // the cast function type
    FunctionType *NewBitCastTy =
        translateTaggedFunctionType(cast<FunctionType>(OrigBitCastTy));

    // The callee is a cast version of the tagged function
    CastNewCallee = IRB.CreateBitCast(NewCallee, NewBitCastTy->getPointerTo());
  } else {
    // The function call result was not cast, so there is no need to do
    // anything to the callee
    CastNewCallee = NewCallee;
  }

  // Create the call/invoke to the callee/invokee
  Instruction *TaggedCall = nullptr;
  FunctionType *TaggedCallTy =
      cast<FunctionType>(CastNewCallee->getType()->getPointerElementType());
  if (isa<CallInst>(CB)) {
    TaggedCall = IRB.CreateCall(TaggedCallTy, CastNewCallee, TaggedCallArgs);
  } else if (auto *Invoke = dyn_cast<InvokeInst>(CB)) {
    TaggedCall =
        IRB.CreateInvoke(TaggedCallTy, CastNewCallee, Invoke->getNormalDest(),
                         Invoke->getUnwindDest(), TaggedCallArgs);
  }
  TaggedCall->setMetadata(this->Mod->getMDKindID("fuzzalloc.tagged_alloc"),
                          MDNode::get(IRB.getContext(), None));

  if (CB->isIndirectCall()) {
    NumOfTaggedIndirectCalls++;
  } else {
    NumOfTaggedDirectCalls++;
  }

  // Replace the users of the original call site
  CB->replaceAllUsesWith(TaggedCall);
  CB->eraseFromParent();

  return TaggedCall;
}

/// Possibly replace an indirect function call site (`CS`) with a call to a
/// tagged version of the function.
///
/// The function call will only be replaced if the function being called is
/// stored within a recorded struct. That is, a struct where a whitelisted
/// allocation function was stored into.
///
/// If the call is not replaced, the original function call is returned.
Instruction *TagDynamicAllocs::tagPossibleIndirectCall(CallBase *CB) {
  LLVM_DEBUG(dbgs() << "(possibly) tagging indirect function call " << *CB
                    << " (in function " << CB->getFunction()->getName()
                    << ")\n");

  const DataLayout &DL = this->Mod->getDataLayout();
  auto *CalledValue = CB->getCalledOperand();
  auto *CalledValueTy = CB->getFunctionType();

  // Get the source of the indirect call. If the called value didn't come from a
  // load, we can't do anything about it
  Value *Obj = getUnderlyingObject(CalledValue);
  if (!isa<LoadInst>(Obj)) {
    return CB;
  }
  auto *ObjLoad = cast<LoadInst>(Obj);

  int64_t ByteOffset = 0;
  auto *ObjBase =
      GetPointerBaseWithConstantOffset(ObjLoad->getOperand(0), ByteOffset, DL);
  Type *ObjBaseElemTy = ObjBase->getType()->getPointerElementType();

  // Check that the load is actually from a struct
  if (!isa<StructType>(ObjBaseElemTy)) {
    return CB;
  }

  // If the called value did originate from a struct, check if the struct
  // offset is one we previously recorded (in the collect tags pass)
  auto StructOffset =
      getStructOffset(cast<StructType>(ObjBaseElemTy), ByteOffset, DL);
  if (!StructOffset.hasValue()) {
    return CB;
  }

  auto StructOffsetIt = this->StructOffsetsToTag.find(*StructOffset);
  if (StructOffsetIt == this->StructOffsetsToTag.end()) {
    return CB;
  }

  // The struct type was recorded. Retrieve the function that was assigned to
  // this struct element and tag it
  StringRef OrigFStr = StructOffsetIt->second.first;

  // Sanity check the function type
  //
  // XXX comparing strings seems hella dirty...
  std::string OrigCallTyStr;
  raw_string_ostream OS(OrigCallTyStr);
  OS << *CalledValueTy;
  OS.flush();
  assert(OrigCallTyStr == StructOffsetIt->second.second);

  // get-or-insert the function, rather than just getting it. Since the original
  // funtion is being called indirectly (via a struct), it is highly-likely that
  // the original function is not actually defined in this module (otherwise
  // we'd just call it directly)
  //
  // Save the function so that we can delete it later
  Function *OrigF = checkFuzzallocFunc(
      this->Mod->getOrInsertFunction(OrigFStr, CalledValueTy));
  this->FunctionsToTag.insert(OrigF);

  return tagCall(CB, translateTaggedFunction(OrigF));
}

/// Sometimes a program does not call a dynamic memory allocation function
/// directly, but rather via a allocation wrapper function. For these programs,
/// we must tag the calls to the allocation wrapper function (the `OrigF`
/// argument), rather than the underlying \p malloc / \p calloc / \p realloc
/// call.
///
/// This means that the call site identifier is now associated with the call to
/// the allocation wrapper function, rather than the underlying \p malloc /
/// \p calloc / \p realloc call. When \p malloc / \p calloc / \p realloc is
/// eventually (if at all) called by the allocation wrapper function, the
/// already-generated tag is passed through to the appropriate fuzzalloc
/// function.
Function *TagDynamicAllocs::tagFunction(Function *OrigF) {
  LLVM_DEBUG(dbgs() << "tagging function " << OrigF->getName() << '\n');

  // Make a new version of the allocation wrapper function, with "__tagged_"
  // preprended to the name and that accepts a tag as the first argument to the
  // function
  Function *TaggedF = translateTaggedFunction(OrigF);

  // We can only replace the function body if it is defined in this module
  if (!OrigF->isDeclaration()) {
    // Map the original function arguments to the new version of the allocation
    // wrapper function. Skip the tag argument (i.e., first argument)
    ValueToValueMapTy VMap;
    auto NewFuncArgIt = TaggedF->arg_begin() + 1;
    for (auto &Arg : OrigF->args()) {
      VMap[&Arg] = &(*NewFuncArgIt++);
    }

    SmallVector<ReturnInst *, 8> Returns;
    CloneFunctionInto(TaggedF, OrigF, VMap, /* ModuleLevelChanges */ true,
                      Returns);

    // Update allocsize attribute (if it exists). Just move the allocsize index
    // up one (to take into account the tag being inserted as the first function
    // parameter)
    if (TaggedF->hasFnAttribute(Attribute::AllocSize)) {
      Attribute Attr = TaggedF->getFnAttribute(Attribute::AllocSize);
      std::pair<unsigned, Optional<unsigned>> Args = Attr.getAllocSizeArgs();
      Args.first += 1;
      Args.second = Args.second.hasValue()
                        ? Optional<unsigned>(Args.second.getValue() + 1)
                        : None;

      TaggedF->removeFnAttr(Attribute::AllocSize);
      TaggedF->addFnAttr(Attribute::getWithAllocSizeArgs(
          TaggedF->getContext(), Args.first, Args.second));
    }

    // Update the contents of the function (i.e., the instructions) when we
    // update the users of the dynamic memory allocation function (i.e., in
    // tagUser)

    TaggedFunctions.insert(TaggedF);
    NumOfTaggedFunctions++;
  }

  return TaggedF;
}

/// A dynamic memory allocation function could be assigned to a global
/// variable (which is different to a global alias). If so, the global variable
/// must be updated to point to a tagged version of the dynamic memory
/// allocation function.
///
/// Which global variables get tagged is determined by stores of whitelisted
/// functions.
GlobalVariable *TagDynamicAllocs::tagGlobalVariable(GlobalVariable *OrigGV) {
  LLVM_DEBUG(dbgs() << "tagging global variable " << *OrigGV << '\n');

  // Cache users
  SmallVector<User *, 16> Users(OrigGV->user_begin(), OrigGV->user_end());

  // Translate the global variable to get a tagged version. Since it is a globa;
  // variable casting to a pointer type is safe (all globals are pointers)
  GlobalVariable *TaggedGV = translateTaggedGlobalVariable(OrigGV);
  PointerType *TaggedGVTy = cast<PointerType>(TaggedGV->getValueType());

  // Replace the initializer (if it exists) with a tagged version
  if (OrigGV->hasInitializer()) {
    auto *OrigInitializer = OrigGV->getInitializer();

    if (auto *InitializerF = dyn_cast<Function>(OrigInitializer)) {
      // Tag the initializer function
      TaggedGV->setInitializer(translateTaggedFunction(InitializerF));
    } else if (isa<ConstantPointerNull>(OrigInitializer)) {
      // Retype the null pointer initializer
      TaggedGV->setInitializer(ConstantPointerNull::get(TaggedGVTy));
    } else {
      assert(false && "Unsupported global variable initializer");
    }
  }

  // Replace all the users of the global variable
  for (auto *U : Users) {
    if (auto *Load = dyn_cast<LoadInst>(U)) {
      // Cache users
      SmallVector<User *, 8> LoadUsers(Load->user_begin(), Load->user_end());

      // Load the global variable containing the tagged function
      auto *NewLoad =
          new LoadInst(TaggedGVTy, TaggedGV,
                       Load->hasName() ? "__tagged_" + Load->getName() : "",
                       Load->isVolatile(), Align(Load->getAlignment()),
                       Load->getOrdering(), Load->getSyncScopeID(), Load);

      for (auto *LU : LoadUsers) {
        if (auto *CB = dyn_cast<CallBase>(LU)) {
          // Replace a call to the function stored in the original global
          // variable with a call to the tagged version
          tagCall(CB, NewLoad);
        } else if (auto *PHI = dyn_cast<PHINode>(LU)) {
          // Replace the loaded global variable with the tagged version
          PHI->replaceUsesOfWith(Load, NewLoad);

          // We can replace the PHI node once all of the PHI node values are of
          // the same type as the tagged global variable
          if (std::all_of(PHI->value_op_begin(), PHI->value_op_end(),
                          [TaggedGVTy](const Value *V) {
                            return V->getType() == TaggedGVTy;
                          })) {
            // Replace the PHI node with an equivalent node of the correct
            // type (i.e., so that it matches the type of the tagged global
            // variable)
            auto *NewPHI = PHINode::Create(
                TaggedGVTy, PHI->getNumIncomingValues(),
                PHI->hasName() ? "__tagged_" + PHI->getName() : "", PHI);
            for (unsigned I = 0; I < PHI->getNumIncomingValues(); ++I) {
              NewPHI->addIncoming(PHI->getIncomingValue(I),
                                  PHI->getIncomingBlock(I));
            }

            // Cannot use `replaceAllUsesWith` because the PHI nodes have
            // different types
            for (auto &U : PHI->uses()) {
              U.set(NewPHI);
            }

            // Nothing uses the PHI node now. Delete it
            PHI->eraseFromParent();

            for (auto *PU : NewPHI->users()) {
              // TODO only deal with call instructions for now
              assert(isa<CallBase>(PU));

              // Replace a call to the function stored in the original global
              // variable with a call to the tagged version
              tagCall(cast<CallBase>(PU), NewPHI);
            }
          }
        } else {
          // Warn on unsupported load user and replace with an undef
          std::string LUStr;
          raw_string_ostream OS(LUStr);
          OS << *LU;

          WARNF("[%s] Replacing unsupported load user %s with an undef value",
                this->Mod->getName().str().c_str(), LUStr.c_str());
          LU->replaceUsesOfWith(Load, UndefValue::get(Load->getType()));
        }
      }

      Load->eraseFromParent();
    } else if (auto *Store = dyn_cast<StoreInst>(U)) {
      // The only things that should be written to a tagged global variable are
      // functions that are going to be tagged
      if (auto *F = dyn_cast<Function>(Store->getValueOperand())) {
        if (!isTaggableFunction(F)) {
          assert(false && "Must store taggable function");
        }

        auto *NewStore =
            new StoreInst(translateTaggedFunction(F), TaggedGV,
                          Store->isVolatile(), Align(Store->getAlignment()),
                          Store->getOrdering(), Store->getSyncScopeID(), Store);
        Store->replaceAllUsesWith(NewStore);
        Store->eraseFromParent();
      } else {
        // We cannot determine anything about the value being stored - just
        // replace it with the abort function and hope for the best
        std::string ValOpStr;
        raw_string_ostream OS(ValOpStr);
        OS << *Store->getValueOperand();

        WARNF("[%s] Replacing store of %s to %s with an abort",
              this->Mod->getName().str().c_str(), ValOpStr.c_str(),
              OrigGV->getName().str().c_str());
        Store->replaceUsesOfWith(OrigGV, castAbort(OrigGV->getType()));
      }
    } else if (auto *BitCast = dyn_cast<BitCastOperator>(U)) {
      // Cache users
      SmallVector<User *, 16> BitCastUsers(BitCast->user_begin(),
                                           BitCast->user_end());

      for (auto *BCU : BitCastUsers) {
        assert(isa<Instruction>(BCU));
        auto *NewBitCast = CastInst::CreateBitOrPointerCast(
            TaggedGV, BitCast->getDestTy(), "", cast<Instruction>(BCU));
        BCU->replaceUsesOfWith(BitCast, NewBitCast);
      }
      BitCast->deleteValue();
    } else {
      // TODO handle other users
      assert(false && "Unsupported global variable user");
    }
  }

  NumOfTaggedGlobalVariables++;

  return TaggedGV;
}

/// A dynamic memory allocation function could be assigned to a global alias.
/// If so, the global alias must be updated to point to a tagged version of the
/// dynamic memory allocation function.
GlobalAlias *TagDynamicAllocs::tagGlobalAlias(GlobalAlias *OrigGA) {
  LLVM_DEBUG(dbgs() << "tagging global alias " << *OrigGA << '\n');

  Constant *OrigAliasee = OrigGA->getAliasee();
  Constant *TaggedAliasee = nullptr;

  if (auto *AliaseeF = dyn_cast<Function>(OrigAliasee)) {
    TaggedAliasee = translateTaggedFunction(AliaseeF);
  } else if (auto *AliaseeGV = dyn_cast<GlobalVariable>(OrigAliasee)) {
    TaggedAliasee = translateTaggedGlobalVariable(AliaseeGV);
  } else {
    assert(false &&
           "Global alias aliasee must be a function or global variable");
  }

  auto *TaggedGA = GlobalAlias::create(
      TaggedAliasee->getType()->getPointerElementType(),
      TaggedAliasee->getType()->getPointerAddressSpace(), OrigGA->getLinkage(),
      OrigGA->hasName() ? "__tagged_" + OrigGA->getName() : "", TaggedAliasee,
      OrigGA->getParent());

  // TODO handle users
  assert(OrigGA->hasNUses(0) && "Not supported");

  NumOfTaggedGlobalAliases++;

  return TaggedGA;
}

void TagDynamicAllocs::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<TargetLibraryInfoWrapperPass>();
}

bool TagDynamicAllocs::doInitialization(Module &M) {
  LLVMContext &C = M.getContext();
  const DataLayout &DL = M.getDataLayout();

  this->Mod = &M;
  this->TagTy = Type::getIntNTy(C, NUM_TAG_BITS);
  this->SizeTTy = DL.getIntPtrType(C);

  return false;
}

bool TagDynamicAllocs::runOnModule(Module &M) {
  assert(ClDefSiteTagMin >= FUZZALLOC_TAG_MIN && "Invalid minimum tag value");
  assert(ClDefSiteTagMax <= FUZZALLOC_TAG_MAX && "Invalid maximum tag value");

  LLVMContext &C = M.getContext();
  PointerType *Int8PtrTy = Type::getInt8PtrTy(C);
  Type *VoidTy = Type::getVoidTy(C);

  this->ReturnAddrF = Intrinsic::getDeclaration(&M, Intrinsic::returnaddress);
  this->AbortF =
      checkFuzzallocFunc(M.getOrInsertFunction(AbortFuncName, VoidTy));
  this->AbortF->setDoesNotReturn();
  this->AbortF->setDoesNotThrow();

  // Create the tagged memory allocation functions. These functions take the
  // take the same arguments as the original dynamic memory allocation
  // function, except that the first argument is a tag that identifies the
  // allocation site
  this->FuzzallocMallocF = checkFuzzallocFunc(M.getOrInsertFunction(
      FuzzallocMallocFuncName, Int8PtrTy, this->TagTy, this->SizeTTy));
  this->FuzzallocMallocF->addFnAttr(
      Attribute::getWithAllocSizeArgs(C, 1, None));

  this->FuzzallocCallocF = checkFuzzallocFunc(
      M.getOrInsertFunction(FuzzallocCallocFuncName, Int8PtrTy, this->TagTy,
                            this->SizeTTy, this->SizeTTy));
  this->FuzzallocCallocF->addFnAttr(Attribute::getWithAllocSizeArgs(C, 1, 2));

  this->FuzzallocReallocF = checkFuzzallocFunc(
      M.getOrInsertFunction(FuzzallocReallocFuncName, Int8PtrTy, this->TagTy,
                            Int8PtrTy, this->SizeTTy));
  this->FuzzallocReallocF->addFnAttr(
      Attribute::getWithAllocSizeArgs(C, 2, None));

  // Figure out what we need to tag
  getTagSites();

  // Tag all the things

  // Rewrite custom allocation functions (so they accept a def site tag)
  for (auto *F : this->FunctionsToTag) {
    if (isCustomAllocationFunction(F)) {
      tagFunction(F);
    }
  }

  for (auto *F : this->FunctionsToTag) {
    const TargetLibraryInfo *TLI =
        &getAnalysis<TargetLibraryInfoWrapperPass>().getTLI(*F);

    // Cache users
    SmallVector<User *, 16> Users(F->user_begin(), F->user_end());

    for (auto *U : Users) {
      tagUser(U, F, TLI);
    }
  }

  for (auto *GV : this->GlobalVariablesToTag) {
    tagGlobalVariable(GV);
  }

  for (auto *GA : this->GlobalAliasesToTag) {
    tagGlobalAlias(GA);
  }

  if (ClEnableIndirectCallTag) {
    for (auto &F : M.functions()) {
      for (auto *IndirectCall : findIndirectCalls(F)) {
        tagPossibleIndirectCall(IndirectCall);
      }
    }
  }

  // Delete all the things that have been tagged

  for (auto *GA : this->GlobalAliasesToTag) {
    assert(GA->hasNUses(0) && "Global alias still has uses");
    GA->eraseFromParent();
  }

  for (auto *GV : this->GlobalVariablesToTag) {
    assert(GV->hasNUses(0) && "Global variable still has uses");
    GV->eraseFromParent();
  }

  for (auto *F : this->FunctionsToTag) {
    assert(F->hasNUses(0) && "Function still has uses");
    F->eraseFromParent();
  }

  // Finished!

  printStatistic(M, NumOfTaggedDirectCalls);
  printStatistic(M, NumOfTaggedIndirectCalls);
  printStatistic(M, NumOfTaggedFunctions);
  printStatistic(M, NumOfTaggedGlobalVariables);
  printStatistic(M, NumOfTaggedGlobalAliases);
  printStatistic(M, NumOfTrampolines);

  return true;
}

static RegisterPass<TagDynamicAllocs>
    X("fuzzalloc-tag-dyn-allocs",
      "Tag dynamic allocation function calls and replace them with a call to "
      "the appropriate fuzzalloc function",
      false, false);

static void registerTagDynamicAllocsPass(const PassManagerBuilder &,
                                         legacy::PassManagerBase &PM) {
  PM.add(new TagDynamicAllocs());
}

static RegisterStandardPasses
    RegisterTagDynamicAllocsPass(PassManagerBuilder::EP_OptimizerLast,
                                 registerTagDynamicAllocsPass);

static RegisterStandardPasses
    RegisterTagDynamicAllocsPass0(PassManagerBuilder::EP_EnabledOnOptLevel0,
                                  registerTagDynamicAllocsPass);
