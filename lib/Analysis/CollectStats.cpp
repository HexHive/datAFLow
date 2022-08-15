//===-- CollectStats.cpp - Collect useful statistics ------------*- C++ -*-===//
///
/// \file
/// This pass simply collects a number of useful statistics.
///
//===----------------------------------------------------------------------===//

#include <llvm/IR/Instructions.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/Pass.h>
#include <llvm/Transforms/IPO/PassManagerBuilder.h>

#include "fuzzalloc/Analysis/CollectStats.h"
#include "fuzzalloc/Analysis/VariableRecovery.h"
#include "fuzzalloc/Metadata.h"

using namespace llvm;

#define DEBUG_TYPE "fuzzalloc-collect-stats"

char CollectStats::ID = 0;

void CollectStats::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<VariableRecovery>();
  AU.setPreservesAll();
}

void CollectStats::print(raw_ostream &O, const Module *) const {
  O << "  num. basic blocks: " << NumBasicBlocks << "\n";
  O << "  num. local variables: " << NumLocalVars << "\n";
  O << "  num. global variables: " << NumGlobalVars << "\n";
  O << "  num. tagged local variables: " << NumTaggedLocalVars << "\n";
  O << "  num. tagged local variables: " << NumTaggedGlobalVars << "\n";
  O << "  num. instrumented use sites: " << NumInstrumentedUseSites << "\n";
}

bool CollectStats::doInitialization(Module &M) {
  this->NumBasicBlocks = 0;
  this->NumLocalVars = 0;
  this->NumTaggedLocalVars = 0;
  this->NumGlobalVars = 0;
  this->NumTaggedGlobalVars = 0;
  this->NumInstrumentedUseSites = 0;

  return false;
}

bool CollectStats::runOnModule(Module &M) {
  const auto &Vars = getAnalysis<VariableRecovery>().getVariables();
  for (const auto &[_, Var] : Vars) {
    if (isa<DILocalVariable>(Var.getDbgVar())) {
      NumLocalVars++;
    } else {
      NumGlobalVars++;
    }
  }

  for (const auto &F : M.functions()) {
    for (auto &BB : F) {
      NumBasicBlocks++;

      for (auto &I : BB) {
        if (I.getMetadata(M.getMDKindID(kFuzzallocTaggVarMD))) {
          NumTaggedLocalVars++;
        } else if (I.getMetadata(
                       M.getMDKindID(kFuzzallocInstrumentedUseSiteMD))) {
          NumInstrumentedUseSites++;
        }
      }
    }
  }

  for (const auto &G : M.globals()) {
    if (auto *GV = dyn_cast<GlobalVariable>(&G)) {
      if (GV->getMetadata(M.getMDKindID(kFuzzallocTaggVarMD))) {
        NumTaggedGlobalVars++;
      }
    }
  }

  return false;
}

static RegisterPass<CollectStats>
    X(DEBUG_TYPE, "Collect some useful statistics", true, true);

static void registerCollectStatsPass(const PassManagerBuilder &,
                                     legacy::PassManagerBase &PM) {
  PM.add(new CollectStats());
}

static RegisterStandardPasses
    RegisterCollectStatsPass(PassManagerBuilder::EP_ModuleOptimizerEarly,
                             registerCollectStatsPass);

static RegisterStandardPasses
    RegisterCollectStatsPass0(PassManagerBuilder::EP_EnabledOnOptLevel0,
                              registerCollectStatsPass);
