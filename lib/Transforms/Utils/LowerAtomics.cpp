//===-- LowerAtomics.cpp - Lower atomic instructions ----------------------===//
///
/// \file
/// Wrapper around LLVM's LowerAtomic pass.
///
//===----------------------------------------------------------------------===//

#include <llvm/IR/LegacyPassManager.h>
#include <llvm/Pass.h>
#include <llvm/Transforms/IPO/PassManagerBuilder.h>
#include <llvm/Transforms/Scalar/LowerAtomic.h>

using namespace llvm;

#define DEBUG_TYPE "fuzzalloc-lower-atomics"

class LowerAtomics : public FunctionPass {
public:
  static char ID;
  LowerAtomics() : FunctionPass(ID) {}
  virtual bool runOnFunction(Function &) override;

private:
  LowerAtomicPass Impl;
};

char LowerAtomics::ID = 0;

// Adapted from `LowerAtomicLegacyPass`
bool LowerAtomics::runOnFunction(Function &F) {
  FunctionAnalysisManager DummyFAM;
  auto PA = Impl.run(F, DummyFAM);
  return !PA.areAllPreserved();
}

//
// Pass registration
//

static RegisterPass<LowerAtomics> X(DEBUG_TYPE, "Lower atomics", false, false);

static void registerLowerAtomicsPass(const PassManagerBuilder &,
                                     legacy::PassManagerBase &PM) {
  PM.add(new LowerAtomics());
}

static RegisterStandardPasses
    RegisterLowerAtomicsPass(PassManagerBuilder::EP_OptimizerLast,
                             registerLowerAtomicsPass);

static RegisterStandardPasses
    RegisterLowerAtomicsPass0(PassManagerBuilder::EP_EnabledOnOptLevel0,
                              registerLowerAtomicsPass);
