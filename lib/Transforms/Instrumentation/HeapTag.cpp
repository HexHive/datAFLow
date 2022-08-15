//===-- HeapTag.cpp - Instrument dynamic memory funcs -----------*- C++ -*-===//
///
/// \file
/// Instrument dynamic memory allocation functions
///
//===----------------------------------------------------------------------===//

#include <llvm/ADT/Statistic.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/ValueMap.h>
#include <llvm/Pass.h>
#include <llvm/Support/Debug.h>
#include <llvm/Transforms/IPO/PassManagerBuilder.h>
#include <llvm/Transforms/Utils/Cloning.h>

#include "fuzzalloc/Analysis/MemFuncIdentify.h"
#include "fuzzalloc/Metadata.h"
#include "fuzzalloc/Streams.h"
#include "fuzzalloc/Transforms/Utils.h"
#include "fuzzalloc/fuzzalloc.h"

using namespace llvm;

#define DEBUG_TYPE "fuzzalloc-tag-heap"

class HeapTag : public ModulePass {
public:
  static char ID;
  HeapTag() : ModulePass(ID) {}

  virtual void getAnalysisUsage(AnalysisUsage &) const override;
  virtual bool runOnModule(Module &) override;

private:
  ConstantInt *generateTag() const;
  FunctionType *getTaggedFunctionType(const FunctionType *) const;
  Function *getTaggedFunction(const Function *) const;
  Function *tagFunction(const Function *) const;
  Instruction *tagCall(CallBase *, FunctionCallee) const;
  void tagUser(User *, Function *) const;

  Module *Mod;
  LLVMContext *Ctx;
  IntegerType *TagTy;

  SmallPtrSet<Function *, 8> TaggedFuncs;
  ValueMap</* Original function */ Function *, /* Tagged function */ Function *>
      TaggedFuncMap;
};

char HeapTag::ID = 0;

ConstantInt *HeapTag::generateTag() const {
  return ConstantInt::get(
      TagTy, static_cast<uint64_t>(RAND(kFuzzallocTagMin, kFuzzallocTagMax)));
}

FunctionType *HeapTag::getTaggedFunctionType(const FunctionType *Ty) const {
  SmallVector<Type *, 4> TaggedFuncParams = {TagTy};
  TaggedFuncParams.append(Ty->param_begin(), Ty->param_end());
  return FunctionType::get(Ty->getReturnType(), TaggedFuncParams,
                           Ty->isVarArg());
}

Function *HeapTag::getTaggedFunction(const Function *OrigF) const {
  const auto &Name = "__bb_" + OrigF->getName().str();
  auto *TaggedFTy = getTaggedFunctionType(OrigF->getFunctionType());
  auto TaggedCallee = Mod->getOrInsertFunction(Name, TaggedFTy);
  assert(TaggedCallee && "Tagged function not inserted");
  auto *TaggedF = cast<Function>(TaggedCallee.getCallee()->stripPointerCasts());

  TaggedF->getArg(0)->setName("tag");
  for (unsigned I = 0; I < OrigF->arg_size(); ++I) {
    TaggedF->getArg(I + 1)->setName(OrigF->getArg(I)->getName());
  }

  return TaggedF;
}

Function *HeapTag::tagFunction(const Function *OrigF) const {
  LLVM_DEBUG(dbgs() << "tagging function " << OrigF->getName() << '\n');

  // Insert (or get, if it already exists) the tagged function declaration
  auto *TaggedF = getTaggedFunction(OrigF);

  // If this function is defined outside this module, nothing more to do
  if (OrigF->isDeclaration()) {
    return TaggedF;
  }

  // Map the original function arguments to the new version of the allocation
  // wrapper function. Skip the tag argument (i.e., first argument)
  ValueToValueMapTy VMap;
  auto NewFuncArgIt = TaggedF->arg_begin() + 1;
  for (auto &Arg : OrigF->args()) {
    VMap[&Arg] = &(*NewFuncArgIt++);
  }

  // Insert the tagged function body
  SmallVector<ReturnInst *, 8> Returns;
  CloneFunctionInto(TaggedF, OrigF, VMap, /*ModuleLevelChanges=*/true, Returns);

  // Update allocsize attribute (if it exists). Just move the allocsize index up
  // one (to take into account the tag being inserted as the first function
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

  return TaggedF;
}

Instruction *HeapTag::tagCall(CallBase *CB, FunctionCallee TaggedF) const {
  LLVM_DEBUG(dbgs() << "tagging call " << *CB << " (in function "
                    << CB->getFunction()->getName() << ")\n");

  // The tag values depends on where the function call _is_. If the (tagged)
  // function is being called from within another tagged function, then just
  // pass the first argument (which is always the tag) straight through.
  // Otherwise, generate a new tag
  auto *Tag = [&]() -> Value * {
    auto *ParentF = CB->getFunction();
    if (TaggedFuncs.count(ParentF) > 0) {
      return ParentF->arg_begin();
    }
    return generateTag();
  }();

  // Make the tag the first argument and copy the original call's arguments
  // after
  SmallVector<Value *, 4> TaggedCallArgs = {Tag};
  TaggedCallArgs.append(CB->arg_begin(), CB->arg_end());

  // Create the tagged call
  auto *TaggedCall = [&]() -> CallBase * {
    const auto &Name = CB->getName() + ".tagged";
    if (isa<CallInst>(CB)) {
      return CallInst::Create(TaggedF, TaggedCallArgs, Name, CB);
    } else if (auto *Invoke = dyn_cast<InvokeInst>(CB)) {
      return InvokeInst::Create(TaggedF, Invoke->getNormalDest(),
                                Invoke->getUnwindDest(), TaggedCallArgs, Name,
                                CB);
    }
    llvm_unreachable("Unsupported call instruction");
  }();
  TaggedCall->takeName(CB);
  TaggedCall->setCallingConv(CB->getCallingConv());
  TaggedCall->setAttributes(CB->getAttributes());
  TaggedCall->setDebugLoc(CB->getDebugLoc());
  TaggedCall->copyMetadata(*CB);
  TaggedCall->setMetadata(Mod->getMDKindID(kFuzzallocBBAllocMD),
                          MDNode::get(*Ctx, None));

  // Replace the callee
  CB->replaceAllUsesWith(TaggedCall);
  CB->eraseFromParent();

  return TaggedCall;
}

/// Replace the use of a memory allocation function with the tagged version
void HeapTag::tagUser(User *U, Function *F) const {
  LLVM_DEBUG(dbgs() << "replacing user " << *U << " of function "
                    << F->getName() << '\n');

  auto *TaggedF = TaggedFuncMap.lookup(F);

  if (auto *CB = dyn_cast<CallBase>(U)) {
    tagCall(CB, FunctionCallee(TaggedF));
  } else if (auto *BC = dyn_cast<BitCastInst>(U)) {
    // The underlying type (i.e., behind the bitcast) must be a `FunctionType`
    // (because the use is a function)
    auto *SrcBitCastTy = BC->getDestTy()->getPointerElementType();
    assert(isa<FunctionType>(SrcBitCastTy) && "Requires a function bitcast");

    // (a) Add the tag (i.e., the call site identifier) as the first argument
    // to the cast function type and (b) cast the tagged function so that the
    // type includes the tag argument
    auto *DstBitCastTy =
        getTaggedFunctionType(cast<FunctionType>(SrcBitCastTy));
    auto *NewBC =
        new BitCastInst(TaggedF, DstBitCastTy->getPointerTo(), "", BC);
    NewBC->takeName(BC);
    NewBC->setDebugLoc(BC->getDebugLoc());
    NewBC->copyMetadata(*BC);

    // All the bitcast users should be calls. So tag the calls
    SmallVector<User *, 16> BCUsers(BC->users());
    for (auto *BCU : BCUsers) {
      if (auto *CB = dyn_cast<CallBase>(BCU)) {
        tagCall(CB, FunctionCallee(DstBitCastTy, NewBC));
      } else {
        llvm_unreachable("All bitcast users must be calls");
      }
    }
    BC->eraseFromParent();
  } else {
    llvm_unreachable("User not supported");
  }
}

void HeapTag::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<MemFuncIdentify>();
}

bool HeapTag::runOnModule(Module &M) {
  auto &MemFuncs = getAnalysis<MemFuncIdentify>().getFuncs();

  if (MemFuncs.empty()) {
    return false;
  }

  this->Mod = &M;
  this->Ctx = &M.getContext();
  this->TagTy = Type::getIntNTy(*Ctx, kNumTagBits);

  // Create the tagged memory allocation functions. These functions take the
  // same arguments as the original dynamic memory allocation function, except
  // the first argument is a tag identifying the allocation site
  for (auto *F : MemFuncs) {
    auto *TaggedF = tagFunction(F);

    TaggedFuncs.insert(TaggedF);
    TaggedFuncMap.insert({F, TaggedF});
  }

  // Tag the users of these functions
  for (auto [F, _] : TaggedFuncMap) {
    SmallVector<User *, 16> Users(F->users());
    for (auto *U : Users) {
      tagUser(U, F);
    }
  }

  // Erase the now-unused memory functions
  assert(
      all_of(MemFuncs, [](const Function *F) { return F->getNumUses() == 0; }));
  for (auto *F : MemFuncs) {
    F->eraseFromParent();
  }
  MemFuncs.clear();

  // Finally, replace calls to `free` with `__bb_free`
  {
    auto *FreeFTy =
        FunctionType::get(Type::getVoidTy(*Ctx), {Type::getInt8PtrTy(*Ctx)},
                          /*isVarArg=*/false);
    auto FreeCallee = Mod->getOrInsertFunction("free", FreeFTy);
    if (FreeCallee) {
      auto BBFreeF = Mod->getOrInsertFunction("__bb_free", FreeFTy);
      assert(BBFreeF && "Unable to insert __bb_free function");
      assert(isa<Function>(FreeCallee.getCallee()));
      auto *FreeF = cast<Function>(FreeCallee.getCallee());

      SmallVector<User *, 16> FreeUsers(FreeF->users());
      for (auto *U : FreeUsers) {
        U->replaceUsesOfWith(FreeF, BBFreeF.getCallee());
      }
      FreeF->eraseFromParent();
    }
  }

  return true;
}

//
// Pass registration
//

static RegisterPass<HeapTag> X(DEBUG_TYPE, "Tag heap variables", false,
                                  false);

static void registerHeapTagPass(const PassManagerBuilder &,
                                   legacy::PassManagerBase &PM) {
  PM.add(new HeapTag());
}

static RegisterStandardPasses
    RegisterHeapTagPass(PassManagerBuilder::EP_OptimizerLast,
                           registerHeapTagPass);

static RegisterStandardPasses
    RegisterHeapTagPass0(PassManagerBuilder::EP_EnabledOnOptLevel0,
                            registerHeapTagPass);
