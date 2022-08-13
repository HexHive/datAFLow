//===-- LowerNewDelete.cpp - Lower C++ new/delete functions -----*- C++ -*-===//
///
/// \file
/// Lower C++ new/delete functions to malloc/free calls
///
//===----------------------------------------------------------------------===//

#include <llvm/ADT/Statistic.h>
#include <llvm/Analysis/TargetLibraryInfo.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/Pass.h>
#include <llvm/Support/Debug.h>
#include <llvm/Transforms/IPO/PassManagerBuilder.h>
#include <llvm/Transforms/Utils/Local.h>

#include "fuzzalloc/Analysis/MemFuncIdentify.h"
#include "fuzzalloc/Metadata.h"

using namespace llvm;

#define DEBUG_TYPE "fuzzalloc-lower-new-delete"

STATISTIC(NumLoweredNews, "Number of lowered news");
STATISTIC(NumLoweredDeletes, "Number of lowered deletes");

namespace {
static bool isNewFn(const Function *F, const TargetLibraryInfo *TLI) {
  StringRef FnName = F->getName();
  LibFunc TLIFn;
  if (!TLI || !TLI->getLibFunc(FnName, TLIFn) || !TLI->has(TLIFn)) {
    return false;
  }

  if (TLIFn == LibFunc_Znwj || TLIFn == LibFunc_ZnwjRKSt9nothrow_t ||
      TLIFn == LibFunc_Znwm || TLIFn == LibFunc_ZnwmRKSt9nothrow_t ||
      TLIFn == LibFunc_Znaj || TLIFn == LibFunc_ZnajRKSt9nothrow_t ||
      TLIFn == LibFunc_Znam || TLIFn == LibFunc_ZnamRKSt9nothrow_t) {
    return true;
  }

  return false;
}

static bool isDeleteFn(const Function *F, const TargetLibraryInfo *TLI) {
  StringRef FnName = F->getName();
  LibFunc TLIFn;
  if (!TLI || !TLI->getLibFunc(FnName, TLIFn) || !TLI->has(TLIFn)) {
    return false;
  }

  if (TLIFn == LibFunc_ZdlPv || TLIFn == LibFunc_ZdaPv) {
    return true;
  }

  return false;
}

static CallInst *lowerInvoke(InvokeInst *Invoke) {
  auto *NewCall = createCallMatchingInvoke(Invoke);
  NewCall->takeName(Invoke);
  NewCall->insertBefore(Invoke);
  Invoke->replaceAllUsesWith(NewCall);

  // Follow the call by a branch to the normal destination
  auto *NormalDestBB = Invoke->getNormalDest();
  BranchInst::Create(NormalDestBB, Invoke);

  // Update PHI nodes in the unwind destination
  auto *BB = Invoke->getParent();
  auto *UnwindDestBB = Invoke->getUnwindDest();
  UnwindDestBB->removePredecessor(BB);
  Invoke->eraseFromParent();

  return NewCall;
}
} // anonymous namespace

class LowerNewDelete : public ModulePass {
public:
  static char ID;
  LowerNewDelete() : ModulePass(ID) {}
  virtual void getAnalysisUsage(AnalysisUsage &) const override;
  virtual bool runOnModule(Module &) override;

private:
  void lowerNew(User *, Function *) const;
  void lowerDelete(User *, Function *) const;

  Module *Mod;
  LLVMContext *Ctx;

  Function *MallocFn;
  Function *FreeFn;
};

char LowerNewDelete::ID = 0;

void LowerNewDelete::lowerNew(User *U, Function *NewFn) const {
  LLVM_DEBUG(dbgs() << "rewriting new call " << *U << '\n');

  // Lower invoke to call, as we don't deal with `new`'s exceptions anyway
  if (auto *Invoke = dyn_cast<InvokeInst>(U)) {
    U = lowerInvoke(Invoke);
  }

  if (auto *CB = dyn_cast<CallBase>(U)) {
    auto *AllocSize = CB->getArgOperand(0);
    auto *Malloc = CallInst::CreateMalloc(
        CB, AllocSize->getType(), CB->getType()->getPointerElementType(),
        AllocSize, nullptr, nullptr);
    Malloc->takeName(CB);
    Malloc->setDebugLoc(CB->getDebugLoc());
    Malloc->copyMetadata(*CB);
    Malloc->setMetadata(Mod->getMDKindID(kFuzzallocLoweredNewMD),
                        MDNode::get(*Ctx, None));

    if (auto *MallocCall = dyn_cast<CallBase>(Malloc)) {
      MallocCall->setCallingConv(CB->getCallingConv());
      MallocCall->setAttributes(CB->getAttributes());
    }

    CB->replaceAllUsesWith(Malloc);
    CB->eraseFromParent();
    NumLoweredNews++;
  } else {
    U->replaceUsesOfWith(NewFn, MallocFn);
  }
}

void LowerNewDelete::lowerDelete(User *U, Function *DeleteFn) const {
  LLVM_DEBUG(dbgs() << "rewriting delete call " << *U << '\n');

  // Lower invoke to call, as we dont' deal with `delete`'s exceptions anyway
  if (auto *Invoke = dyn_cast<InvokeInst>(U)) {
    U = lowerInvoke(Invoke);
  }

  if (auto *CB = dyn_cast<CallBase>(U)) {
    auto *Ptr = CB->getArgOperand(CB->getNumArgOperands() - 1);

    auto *Free = CallInst::CreateFree(Ptr, CB);
    Free->takeName(CB);
    Free->setDebugLoc(CB->getDebugLoc());
    Free->copyMetadata(*CB);
    Free->setMetadata(Mod->getMDKindID(kFuzzallocLoweredDeleteMD),
                      MDNode::get(*Ctx, None));

    if (auto *FreeCall = dyn_cast<CallBase>(Free)) {
      FreeCall->setCallingConv(CB->getCallingConv());
      FreeCall->setAttributes(CB->getAttributes());
    }

    CB->replaceAllUsesWith(Free);
    CB->eraseFromParent();

    NumLoweredDeletes++;
  } else {
    U->replaceUsesOfWith(DeleteFn, FreeFn);
  }
}

void LowerNewDelete::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<TargetLibraryInfoWrapperPass>();
  AU.addRequired<MemFuncIdentify>();
}

bool LowerNewDelete::runOnModule(Module &M) {
  this->Mod = &M;
  this->Ctx = &M.getContext();

  {
    auto &DL = M.getDataLayout();
    auto *IntPtrTy = DL.getIntPtrType(*Ctx);
    auto *Int8PtrTy = Type::getInt8PtrTy(*Ctx);

    auto Malloc = M.getOrInsertFunction("malloc", Int8PtrTy, IntPtrTy);
    assert(Malloc && isa<Function>(Malloc.getCallee()) &&
           "Unable to get malloc function");
    this->MallocFn = cast<Function>(Malloc.getCallee());

    auto Free = M.getOrInsertFunction("free", Type::getVoidTy(*Ctx), Int8PtrTy);
    assert(Free && isa<Function>(Free.getCallee()) &&
           "Unable to get free function");
    this->FreeFn = cast<Function>(Free.getCallee());
  }

  bool Changed = false;
  const auto &MemFuncs = getAnalysis<MemFuncIdentify>().getFuncs();

  // Get calls to `new`
  auto NewFns = make_filter_range(MemFuncs, [&](const Function *F) {
    const auto TLI = getAnalysis<TargetLibraryInfoWrapperPass>().getTLI(*F);
    return isNewFn(F, &TLI);
  });

  // Lower calls to `new`
  for (auto *F : NewFns) {
    SmallVector<User *, 16> Users(F->users());
    for (auto *U : Users) {
      lowerNew(U, F);
      Changed = true;
    }
  }

  // Get calls to `delete`
  auto DeleteFns = make_filter_range(M.functions(), [&](const Function &F) {
    const auto TLI = getAnalysis<TargetLibraryInfoWrapperPass>().getTLI(F);
    return isDeleteFn(&F, &TLI);
  });

  // Lower calls to `delete`
  for (auto &F : DeleteFns) {
    SmallVector<User *, 16> Users(F.users());
    for (auto *U : Users) {
      lowerDelete(U, &F);
      Changed = true;
    }
  }

  return Changed;
}

//
// Pass registration
//

static RegisterPass<LowerNewDelete>
    X(DEBUG_TYPE, "Lower new/delete functions to malloc/free", false, false);

static void registerLowerNewDeletePass(const PassManagerBuilder &,
                                       legacy::PassManagerBase &PM) {
  PM.add(new LowerNewDelete());
}

static RegisterStandardPasses
    RegisterLowerNewDeletePass(PassManagerBuilder::EP_OptimizerLast,
                               registerLowerNewDeletePass);

static RegisterStandardPasses
    RegisterLowerNewDeletePass0(PassManagerBuilder::EP_EnabledOnOptLevel0,
                                registerLowerNewDeletePass);
