//===-- LowerDebugDeclare.cpp - Lower DbgDeclare instructions ---*- C++ -*-===//
///
/// \file
/// Lowers llvm.dbg.declare intrinsics into an appropriate set of llvm.dbg.value
/// intrinsics.
///
//===----------------------------------------------------------------------===//

#include <llvm/IR/LegacyPassManager.h>
#include <llvm/Pass.h>
#include <llvm/Transforms/IPO/PassManagerBuilder.h>
#include <llvm/Transforms/Utils/Local.h>

using namespace llvm;

#define DEBUG_TYPE "fuzzalloc-lower-dbg-declare"

class LowerDebugDeclare : public FunctionPass {
public:
  static char ID;
  LowerDebugDeclare() : FunctionPass(ID) {}
  virtual bool runOnFunction(Function &) override;
};

char LowerDebugDeclare::ID = 0;

bool LowerDebugDeclare::runOnFunction(Function &F) {
  return LowerDbgDeclare(F);
}

//
// Pass registration
//

static RegisterPass<LowerDebugDeclare>
    X(DEBUG_TYPE, "Lower llvm.dbg.declare intrinsics", false, false);

static void registerLowerDbgDeclarePass(const PassManagerBuilder &,
                                        legacy::PassManagerBase &PM) {
  PM.add(new LowerDebugDeclare());
}

static RegisterStandardPasses
    RegisterLowerDbgDeclarePass(PassManagerBuilder::EP_OptimizerLast,
                                registerLowerDbgDeclarePass);

static RegisterStandardPasses
    RegisterLowerDbgDeclarePass0(PassManagerBuilder::EP_EnabledOnOptLevel0,
                                 registerLowerDbgDeclarePass);
