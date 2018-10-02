//===-- CollectStats.cpp - Collects useful statsistics --------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
///
/// \file
/// This pass simply collects a number of useful statistics.
///
//===----------------------------------------------------------------------===//

#include "llvm/IR/Instructions.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/Pass.h"
#include "llvm/Transforms/IPO/PassManagerBuilder.h"

#include "Utils/FuzzallocUtils.h"

using namespace llvm;

#define DEBUG_TYPE "fuzzalloc-collect-stats"

namespace {

/// Collect useful statistics
class CollectStats : public ModulePass {
private:
  unsigned long NumOfBasicBlocks;
  unsigned long NumOfAllocas;
  unsigned long NumOfGlobalVars;
  unsigned long NumOfTaggedAllocs;
  unsigned long NumOfInstrumentedDerefs;
  unsigned long NumOfHeapifiedAllocas;

public:
  static char ID;
  CollectStats() : ModulePass(ID) {}

  void getAnalysisUsage(AnalysisUsage &) const override;
  void print(llvm::raw_ostream &, const Module *) const override;
  bool doInitialization(Module &M) override;
  bool runOnModule(Module &) override;
};

} // anonymous namespace

char CollectStats::ID = 0;

void CollectStats::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.setPreservesAll();
}

void CollectStats::print(raw_ostream &O, const Module *M) const {
  O << "  num. basic blocks: " << this->NumOfBasicBlocks << "\n";
  O << "  num. allocas: " << this->NumOfAllocas << "\n";
  O << "  num. global variables: " << this->NumOfGlobalVars << "\n";
  O << "  num. tagged allocs: " << this->NumOfTaggedAllocs << "\n";
  O << "  num. of dereferenced pointers: " << this->NumOfInstrumentedDerefs
    << "\n";
  O << "  num. of heapified allocas: " << this->NumOfHeapifiedAllocas << "\n";
}

bool CollectStats::doInitialization(Module &M) {
  this->NumOfBasicBlocks = 0;
  this->NumOfAllocas = 0;
  this->NumOfGlobalVars = 0;
  this->NumOfTaggedAllocs = 0;
  this->NumOfInstrumentedDerefs = 0;
  this->NumOfHeapifiedAllocas = 0;

  return false;
}

bool CollectStats::runOnModule(Module &M) {
  for (const auto &F : M.functions()) {
    for (auto &BB : F) {
      this->NumOfBasicBlocks++;

      for (auto &I : BB) {
        if (isa<AllocaInst>(I)) {
          this->NumOfAllocas++;
        }

        if (I.getMetadata(M.getMDKindID("fuzzalloc.tagged_alloc"))) {
          this->NumOfTaggedAllocs++;
        } else if (I.getMetadata(
                       M.getMDKindID("fuzzalloc.instrumented_deref"))) {
          this->NumOfInstrumentedDerefs++;
        } else if (I.getMetadata(M.getMDKindID("fuzzalloc.heapified_alloca"))) {
          this->NumOfHeapifiedAllocas++;
        }
      }
    }
  }

  for (const auto &G : M.globals()) {
    if (auto *GV = dyn_cast<GlobalVariable>(&G)) {
      if (!GV->isDeclaration()) {
        this->NumOfGlobalVars++;
      }
    }
  }

  return false;
}

static RegisterPass<CollectStats> X("fuzzalloc-collect-stats",
                                    "Collect some useful statistics", false,
                                    false);

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
