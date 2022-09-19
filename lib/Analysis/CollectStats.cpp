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
  O << "  num. local arrays: " << NumLocalArrays << "\n";
  O << "  num. local structs: " << NumLocalStructs << "\n";
  O << "  num. global arrays: " << NumGlobalArrays << "\n";
  O << "  num. global structs: " << NumGlobalStructs << "\n";
  O << "  num. tagged local arrays: " << NumTaggedLocalArrays << "\n";
  O << "  num. tagged local structs: " << NumTaggedLocalStructs << "\n";
  O << "  num. tagged local arrays: " << NumTaggedGlobalArrays << "\n";
  O << "  num. tagged local structs: " << NumTaggedGlobalStructs << "\n";
  O << "  num. instrumented use sites: " << NumInstrumentedUseSites << "\n";
}

bool CollectStats::doInitialization(Module &M) {
  this->NumBasicBlocks = 0;

  this->NumLocalArrays = 0;
  this->NumLocalStructs = 0;
  this->NumGlobalArrays = 0;
  this->NumGlobalStructs = 0;

  this->NumTaggedLocalArrays = 0;
  this->NumTaggedLocalStructs = 0;
  this->NumTaggedGlobalArrays = 0;
  this->NumTaggedGlobalStructs = 0;

  this->NumInstrumentedUseSites = 0;

  return false;
}

bool CollectStats::runOnModule(Module &M) {
  const auto &Vars = getAnalysis<VariableRecovery>().getVariables();
  for (const auto &[_, VI] : Vars) {
    const auto *Ty = VI.getType();
    if (isa<DILocalVariable>(VI.getDbgVar())) {
      if (isa<ArrayType>(Ty)) {
        NumLocalArrays++;
      } else if (isa<StructType>(Ty)) {
        NumLocalStructs++;
      }
    } else if (isa<DIGlobalVariable>(VI.getDbgVar())) {
      if (isa<ArrayType>(Ty)) {
        NumGlobalArrays++;
      } else if (isa<StructType>(Ty)) {
        NumGlobalStructs++;
      }
    }
  }

  for (auto &F : M.functions()) {
    for (auto &BB : F) {
      NumBasicBlocks++;

      for (auto &I : BB) {
        if (I.getMetadata(M.getMDKindID(kFuzzallocTagVarMD))) {
          const auto *Ty = Vars.lookup(&I).getType();
          if (isa<ArrayType>(Ty)) {
            NumTaggedLocalArrays++;
          } else if (isa<StructType>(Ty)) {
            NumTaggedLocalStructs++;
          }
        } else if (I.getMetadata(
                       M.getMDKindID(kFuzzallocInstrumentedUseSiteMD))) {
          NumInstrumentedUseSites++;
        }
      }
    }
  }

  for (auto &G : M.globals()) {
    if (auto *GV = dyn_cast<GlobalVariable>(&G)) {
      if (GV->getMetadata(M.getMDKindID(kFuzzallocTagVarMD))) {
        const auto *Ty = Vars.lookup(GV).getType();
        if (isa<ArrayType>(Ty)) {
          NumTaggedGlobalArrays++;
        } else if (isa<StructType>(Ty)) {
          NumTaggedGlobalStructs++;
        }
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
