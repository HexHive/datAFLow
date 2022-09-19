//===-- CollectStats.h - Collect useful statistics --------------*- C++ -*-===//
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
  unsigned long getNumLocalArrays() const { return NumLocalArrays; }
  unsigned long getNumLocalStructs() const { return NumLocalStructs; }
  unsigned long getNumTaggedLocalArrays() const { return NumTaggedLocalArrays; }
  unsigned long getNumTaggedLocalStructs() const {
    return NumTaggedLocalStructs;
  }
  unsigned long getNumGlobalArrays() const { return NumGlobalArrays; }
  unsigned long getNumGlobalStructs() const { return NumGlobalStructs; }
  unsigned long getNumTaggedGlobalArrays() const {
    return NumTaggedGlobalArrays;
  }
  unsigned long getNumTaggedGlobalStructs() const {
    return NumTaggedGlobalStructs;
  }
  unsigned long getNumTaggedDynAllocs() const { return NumTaggedDynAllocs; }
  unsigned long getNumInstrumenteduseSites() const {
    return NumInstrumentedUseSites;
  }

private:
  unsigned long NumBasicBlocks;

  unsigned long NumLocalArrays;
  unsigned long NumLocalStructs;
  unsigned long NumGlobalArrays;
  unsigned long NumGlobalStructs;

  unsigned long NumTaggedLocalArrays;
  unsigned long NumTaggedLocalStructs;
  unsigned long NumTaggedGlobalArrays;
  unsigned long NumTaggedGlobalStructs;
  unsigned long NumTaggedDynAllocs;

  unsigned long NumInstrumentedUseSites;
};

#endif // COLLECT_STATS_H
