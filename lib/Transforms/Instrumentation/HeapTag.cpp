//===-- HeapTag.cpp - Instrument dynamic memory funcs -----------*- C++ -*-===//
///
/// \file
/// Instrument dynamic memory allocation functions
///
//===----------------------------------------------------------------------===//

#include <llvm/ADT/Statistic.h>
#include <llvm/IR/IRBuilder.h>
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

#include "Utils.h"

using namespace llvm;

#define DEBUG_TYPE "fuzzalloc-tag-heap"

namespace {
static unsigned NumTaggedFuncs = 0;
static unsigned NumTaggedFuncUsers = 0;
static unsigned NumTrampolines = 0;
} // anonymous namespace

class HeapTag : public ModulePass {
public:
  static char ID;
  HeapTag() : ModulePass(ID) {}

  virtual void getAnalysisUsage(AnalysisUsage &) const override;
  virtual bool runOnModule(Module &) override;

private:
  Function *createTrampoline(const Function *) const;

  FunctionType *getTaggedFunctionType(const FunctionType *) const;
  Function *getTaggedFunction(const Function *) const;
  Function *tagFunction(const Function *) const;
  Instruction *tagCall(CallBase *, FunctionCallee) const;
  void tagUse(Use *) const;
  void doAFLTag(MemFuncIdentify::DynamicMemoryFunctions &);

  Module *Mod;
  LLVMContext *Ctx;
  IntegerType *TagTy;
  IntegerType *IntPtrTy;

  Function *ReturnAddrFn;

  SmallPtrSet<Function *, 8> TaggedFuncs;
  ValueMap</* Original function */ Function *, /* Tagged function */ Function *>
      TaggedFuncMap;
};

char HeapTag::ID = 0;

Function *HeapTag::createTrampoline(const Function *OrigF) const {
  const auto &TrampolineName = "fuzzalloc.trampoline." + OrigF->getName().str();
  auto *TrampolineFn = Mod->getFunction(TrampolineName);
  if (TrampolineFn) {
    return TrampolineFn;
  }

  // Create the trampoline function
  TrampolineFn =
      Function::Create(OrigF->getFunctionType(), GlobalValue::WeakAnyLinkage,
                       TrampolineName, *Mod);
  auto *EntryBB = BasicBlock::Create(*Ctx, "entry", TrampolineFn);

  IRBuilder<> IRB(EntryBB);

  // Use the trampoline's return address (modulo the max tag) as the allocation
  // site tag
  auto *MaxTag = ConstantInt::get(TagTy, kFuzzallocTagMax);
  auto *RetAddr = IRB.CreateCall(ReturnAddrFn, IRB.getInt32(0));
  auto *Tag = IRB.CreateURem(
      IRB.CreateZExtOrTrunc(IRB.CreatePtrToInt(RetAddr, IntPtrTy), TagTy),
      MaxTag);

  // Call a tagged version of the dynamic memory allocation function and return
  // its result
  auto *TaggedFn = getTaggedFunction(OrigF);
  SmallVector<Value *, 4> Args = {Tag};
  for (auto &Arg : TrampolineFn->args()) {
    Args.push_back(&Arg);
  }
  IRB.CreateRet(IRB.CreateCall(TaggedFn, Args));

  NumTrampolines++;
  return TrampolineFn;
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
  CloneFunctionInto(TaggedF, OrigF, VMap,
#if LLVM_VERSION_MAJOR > 12
                    CloneFunctionChangeType::GlobalChanges,
#else
                    /*ModuleLevelChanges=*/true,
#endif
                    Returns);

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
  TaggedF->setMetadata(Mod->getMDKindID(kFuzzallocDynAllocFnMD),
                       MDNode::get(*Ctx, None));

  NumTaggedFuncs++;
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
    return generateTag(TagTy);
  }();

  // Make the tag the first argument and copy the original call's arguments
  // after
  SmallVector<Value *, 4> TaggedCallArgs = {Tag};
  TaggedCallArgs.append(CB->arg_begin(), CB->arg_end());

  // Create the tagged call
  auto *TaggedCall = [&]() -> CallBase * {
    const auto &Name = CB->getName().str() + ".tagged";
    if (isa<CallInst>(CB)) {
      return CallInst::Create(TaggedF, TaggedCallArgs, Name, CB);
    } else if (auto *Invoke = dyn_cast<InvokeInst>(CB)) {
      return InvokeInst::Create(TaggedF, Invoke->getNormalDest(),
                                Invoke->getUnwindDest(), TaggedCallArgs, Name,
                                CB);
    }
    llvm_unreachable("Unsupported call instruction");
  }();

  // Copy parameter attributes
  const auto &AL = CB->getAttributes();
  for (unsigned I = 0; I < CB->arg_size(); ++I) {
    for (auto &Attr : AL.getAttributes(AttributeList::FirstArgIndex + I)) {
      TaggedCall->addParamAttr(I + 1, Attr);
    }
  }

  TaggedCall->takeName(CB);
  TaggedCall->setCallingConv(CB->getCallingConv());
  TaggedCall->setDebugLoc(CB->getDebugLoc());
  TaggedCall->copyMetadata(*CB);
  TaggedCall->setMetadata(Mod->getMDKindID(kFuzzallocTagVarMD),
                          MDNode::get(*Ctx, None));

  // Replace the callee
  CB->replaceAllUsesWith(TaggedCall);
  CB->eraseFromParent();

  return TaggedCall;
}

/// Replace the use of a memory allocation function with the tagged version
void HeapTag::tagUse(Use *U) const {
  auto *User = U->getUser();
  auto *Fn = dyn_cast<Function>(U->get());
  assert(Fn && "Use must be a function");

  LLVM_DEBUG(dbgs() << "replacing user " << *User << " of function "
                    << Fn->getName() << '\n');

  auto *TaggedFn = TaggedFuncMap.lookup(Fn);
  auto *TrampolineFn = createTrampoline(Fn);

  if (auto *CB = dyn_cast<CallBase>(User)) {
    if (CB->isArgOperand(U)) {
      // The use is a function call argument
      phiSafeReplaceUses(U, TrampolineFn);
    } else {
      // The use is the function callee
      tagCall(CB, FunctionCallee(TaggedFn));
    }
  } else if (auto *BC = dyn_cast<BitCastInst>(User)) {
    // Get the underlying type behind the bitcast. If it is a function type,
    // adjust the function signature to accept the tag argument. Otherwise, we
    // create a trampoline
    auto *SrcBitCastTy = BC->getDestTy()->getPointerElementType();

    if (isa<FunctionType>(SrcBitCastTy)) {
      // (a) Add the tag (i.e., the call site identifier) as the first argument
      // to the cast function type and (b) cast the tagged function so that the
      // type includes the tag argument
      auto *DstBitCastTy =
          getTaggedFunctionType(cast<FunctionType>(SrcBitCastTy));
      auto *NewBC =
          new BitCastInst(TaggedFn, DstBitCastTy->getPointerTo(), "", BC);
      NewBC->takeName(BC);
      NewBC->setDebugLoc(BC->getDebugLoc());
      NewBC->copyMetadata(*BC);

      // All the bitcast users should be calls. So tag the calls (or insert
      // trampolines if the calls use the bitcast as an argument)
      SmallVector<llvm::Use *, 16> BCUses(
          map_range(BC->uses(), [](Use &U) { return &U; }));
      for (auto *BCU : BCUses) {
        auto *BCUser = BCU->getUser();
        if (auto *CB = dyn_cast<CallBase>(BCUser)) {
          if (CB->isArgOperand(BCU)) {
            phiSafeReplaceUses(BCU, TrampolineFn);
          } else {
            tagCall(CB, FunctionCallee(DstBitCastTy, NewBC));
          }
        } else {
          llvm_unreachable("All bitcast users must be calls");
        }
      }
      BC->eraseFromParent();
    } else {
      phiSafeReplaceUses(U, TrampolineFn);
    }
  } else if (auto *GV = dyn_cast<GlobalVariable>(User)) {
    // If the user is another global variable then the `use` must be an
    // assignment initializer. Here, we need to replace the initializer rather
    // then call `handleOperandChange`
    assert(GV->hasInitializer());
    assert(GV->getInitializer() == Fn);
    GV->setInitializer(TrampolineFn);
  } else if (auto *C = dyn_cast<Constant>(User)) {
    C->handleOperandChange(Fn, TrampolineFn);
  } else {
    phiSafeReplaceUses(U, TrampolineFn);
  }

  NumTaggedFuncUsers++;
}

void HeapTag::doAFLTag(MemFuncIdentify::DynamicMemoryFunctions &MemFuncs) {
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
    SmallVector<Use *, 16> Uses(
        map_range(F->uses(), [](Use &U) { return &U; }));
    for (auto *U : Uses) {
      tagUse(U);
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
}

void HeapTag::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<MemFuncIdentify>();
}

bool HeapTag::runOnModule(Module &M) {
  auto &MemFuncs = getAnalysis<MemFuncIdentify>().getFuncs();

  if (MemFuncs.empty()) {
    return false;
  }

  // Initialize stuff
  this->Mod = &M;
  this->Ctx = &M.getContext();
  this->TagTy = Type::getIntNTy(*Ctx, kNumTagBits);
  this->IntPtrTy = Mod->getDataLayout().getIntPtrType(*Ctx);
  this->ReturnAddrFn = Intrinsic::getDeclaration(Mod, Intrinsic::returnaddress);

  if (ClInstType == InstType::InstAFL) {
    doAFLTag(MemFuncs);
  } else {
    for (auto *F : MemFuncs) {
      // Tag the function as a memory allocation routine
      F->setMetadata(Mod->getMDKindID(kFuzzallocDynAllocFnMD),
                     MDNode::get(*Ctx, None));
      NumTaggedFuncs++;

      // Tag calls as producing a new def site
      SmallVector<User *, 16> Users(F->users());
      while (!Users.empty()) {
        auto *U = Users.pop_back_val();
        if (auto *CB = dyn_cast<CallBase>(U)) {
          CB->setMetadata(Mod->getMDKindID(kFuzzallocTagVarMD),
                          MDNode::get(*Ctx, None));
          NumTaggedFuncs++;
        } else if (isa<BitCastInst>(U)) {
          Users.append(U->users().begin(), U->users().end());
        }
      }
    }
  }

  success_stream() << "[" << M.getName()
                   << "] Num. tagged memory funcs.: " << NumTaggedFuncs << '\n';
  success_stream() << "[" << M.getName()
                   << "] Num. tagged memory func. users: " << NumTaggedFuncUsers
                   << '\n';
  success_stream() << "[" << M.getName()
                   << "] Num. memory func. trampolines: " << NumTrampolines
                   << '\n';

  return true;
}

//
// Pass registration
//

static RegisterPass<HeapTag> X(DEBUG_TYPE, "Tag heap variables", false, false);

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
