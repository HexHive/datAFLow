//===-- RewriteNews.cpp - Rewrite news to mallocs -------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
///
/// \file
/// This pass tags rewrites calls to the `new` operator and replaces them with
/// calls to `malloc` so that they can later be tagged. The objects are
/// initialized via `placement new`.
///
//===----------------------------------------------------------------------===//

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Analysis/MemoryBuiltins.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/Pass.h"
#include "llvm/Support/Debug.h"
#include "llvm/Transforms/IPO/PassManagerBuilder.h"

#include "Utils/FuzzallocUtils.h"

using namespace llvm;

#define DEBUG_TYPE "fuzzalloc-rewrite-news"

STATISTIC(NumOfNewRewrites, "Number of news rewritten.");
STATISTIC(NumOfDeleteRewrites, "Number of deletes rewritten.");

namespace {

/// RewriteNews: Rewrites calls to the `new` operator and replaces them with
/// calls to `malloc`. Objects are initialized via `placement new`.
class RewriteNews : public FunctionPass {
public:
  static char ID;
  RewriteNews() : FunctionPass(ID) {}

  void getAnalysisUsage(AnalysisUsage &) const override;
  bool runOnFunction(Function &) override;
};

} // anonymous namespace

char RewriteNews::ID = 0;

static bool isNewFn(const Value *V, const TargetLibraryInfo *TLI) {
  if (!isa<Function>(V)) {
    return false;
  }

  const Function *Callee = cast<Function>(V);
  StringRef FnName = Callee->getName();
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

static bool isDeleteFn(const Value *V, const TargetLibraryInfo *TLI) {
  if (!isa<Function>(V)) {
    return false;
  }

  const Function *Callee = cast<Function>(V);
  StringRef FnName = Callee->getName();
  LibFunc TLIFn;
  if (!TLI || !TLI->getLibFunc(FnName, TLIFn) || !TLI->has(TLIFn)) {
    return false;
  }

  if (TLIFn == LibFunc_ZdlPv || TLIFn == LibFunc_ZdaPv) {
    return true;
  }

  return false;
}

static Instruction *rewriteNew(CallBase *CB) {
  LLVM_DEBUG(dbgs() << "rewriting new call " << *CB << '\n');

  Value *AllocSize = CB->getArgOperand(0);

  auto *MallocCall = CallInst::CreateMalloc(
      CB, AllocSize->getType(), CB->getType()->getPointerElementType(),
      AllocSize, nullptr, nullptr, "rewrite_new");

  // If new was invoke-d, rather than call-ed, we must branch to the invoke's
  // normal destination.
  //
  // TODO Emulate exception handling (i.e., the invoke's unwind destination)
  if (auto *Invoke = dyn_cast<InvokeInst>(CB)) {
    auto *NormalDest = Invoke->getNormalDest();
    assert(NormalDest && "Invoke has no normal destination");

    BranchInst::Create(NormalDest, CB);

    // Remove the basic block containing the rewritten new in any PHI nodes,
    // otherwise the verifier will fail
    //
    // Copy the PHI nodes into a vectorr so that we don't corrupt the iterator
    // if the PHI node is deleted
    SmallVector<PHINode *, 8> PHIs;
    for (PHINode &PHI : Invoke->getUnwindDest()->phis()) {
      PHIs.push_back(&PHI);
    }

    for (PHINode *PHI : PHIs) {
      int BBIdx = PHI->getBasicBlockIndex(CB->getParent());
      if (BBIdx >= 0) {
        PHI->removeIncomingValue(BBIdx);
      }
    }
  }

  CB->replaceAllUsesWith(MallocCall);
  CB->eraseFromParent();

  NumOfNewRewrites++;

  return MallocCall;
}

static Instruction *rewriteDelete(CallBase *CB) {
  LLVM_DEBUG(dbgs() << "rewriting delete call " << *CB << '\n');

  Value *Ptr = CB->getArgOperand(CB->getNumArgOperands() - 1);

  auto *FreeCall = CallInst::CreateFree(Ptr, CB);
  CB->replaceAllUsesWith(FreeCall);
  CB->eraseFromParent();

  NumOfDeleteRewrites++;

  return FreeCall;
}

void RewriteNews::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<TargetLibraryInfoWrapperPass>();
}

bool RewriteNews::runOnFunction(Function &F) {
  Module *M = F.getParent();
  const DataLayout &DL = M->getDataLayout();
  const TargetLibraryInfo *TLI =
      &getAnalysis<TargetLibraryInfoWrapperPass>().getTLI(F);

  // new calls to rewrite
  SmallVector<CallBase *, 8> NewCalls;

  // delete calls to rewrite
  SmallVector<CallBase *, 8> DeleteCalls;

  // llvm.mem* intrinsics that may require realignment
  SmallVector<MemIntrinsic *, 4> MemIntrinsics;

  // Collect all the things!
  for (auto I = inst_begin(F); I != inst_end(F); ++I) {
    if (auto *CB = dyn_cast<CallBase>(&*I)) {
      Value *Callee = CB->getCalledOperand();

      if (isNewFn(Callee, TLI)) {
        NewCalls.push_back(CB);
      } else if (isDeleteFn(Callee, TLI)) {
        DeleteCalls.push_back(CB);
      } else if (auto *MemI = dyn_cast<MemIntrinsic>(&*I)) {
        MemIntrinsics.push_back(MemI);
      }
    }
  }

  // Rewrite new calls
  for (auto &NewCall : NewCalls) {
    auto *MallocCall = rewriteNew(NewCall);

    for (auto *MemI : MemIntrinsics) {
      if (GetUnderlyingObjectThroughLoads(MemI->getDest(), DL) == MallocCall) {
        MemI->setDestAlignment(1);
      }
    }
  }

  // Rewrite delete calls
  for (auto &DeleteCall : DeleteCalls) {
    rewriteDelete(DeleteCall);
  }

  // Finished!

  printStatistic(*M, NumOfNewRewrites);
  printStatistic(*M, NumOfDeleteRewrites);

  return NewCalls.size() > 0 || DeleteCalls.size() > 0;
}

static RegisterPass<RewriteNews>
    X("fuzzalloc-rewrite-news",
      "Replace news with mallocs so that they can be tagged by a later pass",
      false, false);

static void registerRewriteNewsPass(const PassManagerBuilder &,
                                    legacy::PassManagerBase &PM) {
  PM.add(new RewriteNews());
}

static RegisterStandardPasses
    RegisterRewriteNewsPass(PassManagerBuilder::EP_OptimizerLast,
                            registerRewriteNewsPass);

static RegisterStandardPasses
    RegisterRewriteNewsPass0(PassManagerBuilder::EP_EnabledOnOptLevel0,
                             registerRewriteNewsPass);
