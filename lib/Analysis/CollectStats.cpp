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
#include "fuzzalloc/Metadata.h"

using namespace llvm;

#define DEBUG_TYPE "fuzzalloc-collect-stats"

char CollectStats::ID = 0;

void CollectStats::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.setPreservesAll();
}

void CollectStats::print(raw_ostream &O, const Module *) const {
  O << "  num. basic blocks: " << this->NumBasicBlocks << "\n";
  O << "  num. allocas: " << this->NumAllocas << "\n";
  O << "  num. global variables: " << this->NumGlobalVars << "\n";
  O << "  num. tagged allocas: " << this->NumTaggedAllocas << "\n";
  O << "  num. instrumented use sites: " << this->NumInstrumentedUseSites
    << "\n";
}

bool CollectStats::doInitialization(Module &M) {
  this->NumBasicBlocks = 0;
  this->NumAllocas = 0;
  this->NumTaggedAllocas = 0;
  this->NumGlobalVars = 0;
  this->NumTaggedGlobalVars = 0;
  this->NumInstrumentedUseSites = 0;

  return false;
}

bool CollectStats::runOnModule(Module &M) {
  for (const auto &F : M.functions()) {
    for (auto &BB : F) {
      this->NumBasicBlocks++;

      for (auto &I : BB) {
        if (isa<AllocaInst>(I)) {
          this->NumAllocas++;
        }

        if (I.getMetadata(M.getMDKindID(kFuzzallocTaggVarMD))) {
          this->NumTaggedAllocas++;
        } else if (I.getMetadata(
                       M.getMDKindID(kFuzzallocInstrumentedUseSiteMD))) {
          this->NumInstrumentedUseSites++;
        }
      }
    }
  }

  for (const auto &G : M.globals()) {
    if (auto *GV = dyn_cast<GlobalVariable>(&G)) {
      if (!GV->isDeclaration()) {
        this->NumGlobalVars++;
      }
      if (GV->getMetadata(M.getMDKindID(kFuzzallocTaggVarMD))) {
        this->NumTaggedGlobalVars++;
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
