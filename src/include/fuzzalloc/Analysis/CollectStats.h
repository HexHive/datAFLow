//===-- CollectStats.h - Collects useful statsistics ----------------------===//
///
/// \file
/// This pass simply collects a number of useful statistics.
///
//===----------------------------------------------------------------------===//

#ifndef COLLECT_STATS_H
#define COLLECT_STATS_H

#include <llvm/Pass.h>

class CollectStats : public llvm::ModulePass {
public:
  static char ID;
  CollectStats() : llvm::ModulePass(ID) {}

  void getAnalysisUsage(llvm::AnalysisUsage &) const override;
  void print(llvm::raw_ostream &, const llvm::Module *) const override;
  bool doInitialization(llvm::Module &) override;
  bool runOnModule(llvm::Module &) override;

  unsigned long getNumBasicBlocks() const { return NumBasicBlocks; }
  unsigned long getNumAllocas() const { return NumAllocas; }
  unsigned long getNumGlobalVars() const { return NumGlobalVars; }
  unsigned long getNumTaggedAllocs() const { return NumTaggedAllocs; }
  unsigned long getNumInstrumentedDerefs() const {
    return NumInstrumentedDerefs;
  }
  unsigned long getNumHeapifiedAllocas() const { return NumHeapifiedAllocas; }

private:
  unsigned long NumBasicBlocks;
  unsigned long NumAllocas;
  unsigned long NumGlobalVars;
  unsigned long NumTaggedAllocs;
  unsigned long NumInstrumentedDerefs;
  unsigned long NumHeapifiedAllocas;
};

#endif
