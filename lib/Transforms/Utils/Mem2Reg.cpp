//===-- Mem2Reg.cpp - Mem2Reg wrapper -------------------------------------===//
///
/// \file
/// Promote memory to registers
///
//===----------------------------------------------------------------------===//

#include <llvm/ADT/SmallVector.h>
#include <llvm/Analysis/AssumptionCache.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/Pass.h>
#include <llvm/Transforms/IPO/PassManagerBuilder.h>
#include <llvm/Transforms/Utils/PromoteMemToReg.h>

using namespace llvm;

#define DEBUG_TYPE "fuzzalloc-mem2reg"

namespace {
static unsigned NumPromoted = 0;
} // anonymous namespace

class Mem2Reg : public FunctionPass {
public:
  static char ID;
  Mem2Reg() : FunctionPass(ID) {}
  virtual void getAnalysisUsage(AnalysisUsage &) const override;
  virtual bool runOnFunction(Function &) override;
};

char Mem2Reg::ID = 0;

void Mem2Reg::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<AssumptionCacheTracker>();
  AU.addRequired<DominatorTreeWrapperPass>();
  AU.setPreservesCFG();
}

bool Mem2Reg::runOnFunction(Function &F) {
  auto &DT = getAnalysis<DominatorTreeWrapperPass>().getDomTree();
  auto &AC = getAnalysis<AssumptionCacheTracker>().getAssumptionCache(F);
  SmallVector<AllocaInst *, 8> Allocas;
  auto &BB = F.getEntryBlock();
  bool Changed = false;

  while (true) {
    Allocas.clear();

    for (auto I = BB.begin(), E = --BB.end(); I != E; ++I) {
      if (auto *Alloca = dyn_cast<AllocaInst>(I)) {
        if (isAllocaPromotable(Alloca)) {
          Allocas.push_back(Alloca);
        }
      }
    }

    if (Allocas.empty()) {
      break;
    }

    PromoteMemToReg(Allocas, DT, &AC);
    NumPromoted += Allocas.size();
    Changed = true;
  }

  return Changed;
}

//
// Pass registration
//

static RegisterPass<Mem2Reg> X(DEBUG_TYPE, "Lower constant expressions", true,
                               false);

static void registerMem2RegPass(const PassManagerBuilder &,
                                legacy::PassManagerBase &PM) {
  PM.add(new Mem2Reg());
}

static RegisterStandardPasses
    RegisterMem2RegPass(PassManagerBuilder::EP_OptimizerLast,
                        registerMem2RegPass);

static RegisterStandardPasses
    RegisterMem2RegPass0(PassManagerBuilder::EP_EnabledOnOptLevel0,
                         registerMem2RegPass);
