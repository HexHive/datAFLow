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
  unsigned long getNumAllocas() const { return NumAllocas; }
  unsigned long getNumTaggedAllocas() const { return NumTaggedAllocas; }
  unsigned long getNumGlobalVars() const { return NumGlobalVars; }
  unsigned long getNumTaggedGlobalVars() const { return NumTaggedGlobalVars; }
  unsigned long getNumInstrumenteduseSites() const {
    return NumInstrumentedUseSites;
  }

private:
  unsigned long NumBasicBlocks;
  unsigned long NumAllocas;
  unsigned long NumTaggedAllocas;
  unsigned long NumGlobalVars;
  unsigned long NumTaggedGlobalVars;
  unsigned long NumInstrumentedUseSites;
};

#endif // COLLECT_STATS_H