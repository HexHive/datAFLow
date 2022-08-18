//===-- LowerAtomic.cpp - Lower atomic instructions -------------*- C++ -*-===//
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

#define DEBUG_TYPE "fuzzalloc-lower-atomic"

class LowerAtomic : public FunctionPass {
public:
  static char ID;
  LowerAtomic() : FunctionPass(ID) {}
  virtual bool runOnFunction(Function &) override;

private:
  LowerAtomicPass Impl;
};

char LowerAtomic::ID = 0;

// Adapted from `LowerAtomicLegacyPass`
bool LowerAtomic::runOnFunction(Function &F) {
  FunctionAnalysisManager DummyFAM;
  auto PA = Impl.run(F, DummyFAM);
  return !PA.areAllPreserved();
}

//
// Pass registration
//

static RegisterPass<LowerAtomic> X(DEBUG_TYPE, "Lower atomics", false, false);

static void registerLowerAtomicPass(const PassManagerBuilder &,
                                    legacy::PassManagerBase &PM) {
  PM.add(new LowerAtomic());
}

static RegisterStandardPasses
    RegisterLowerAtomicPass(PassManagerBuilder::EP_OptimizerLast,
                            registerLowerAtomicPass);

static RegisterStandardPasses
    RegisterLowerAtomicPass0(PassManagerBuilder::EP_EnabledOnOptLevel0,
                             registerLowerAtomicPass);
