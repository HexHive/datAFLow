//===-- LowerAtomics.cpp - Lower atomic instructions ----------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Wrapper around LLVM's LowerAtomic pass.
///
//===----------------------------------------------------------------------===//

#include "llvm/IR/LegacyPassManager.h"
#include "llvm/Pass.h"
#include "llvm/Transforms/IPO/PassManagerBuilder.h"
#include "llvm/Transforms/Scalar/LowerAtomic.h"

using namespace llvm;

#define DEBUG_TYPE "fuzzalloc-lower-atomics"

class LowerAtomics : public llvm::FunctionPass {
public:
  static char ID;
  LowerAtomics() : llvm::FunctionPass(ID) {}
  virtual bool runOnFunction(llvm::Function &) override;

private:
  llvm::LowerAtomicPass Impl;
};

char LowerAtomics::ID = 0;

// Adapted from `LowerAtomicLegacyPass`
bool LowerAtomics::runOnFunction(Function &F) {
  FunctionAnalysisManager DummyFAM;
  auto PA = Impl.run(F, DummyFAM);
  return !PA.areAllPreserved();
}

//===----------------------------------------------------------------------===//

static RegisterPass<LowerAtomics> X("fuzzalloc-lower-atomics", "Lower atomics",
                                    false, false);

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
