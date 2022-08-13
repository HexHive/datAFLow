//===-- LowerDbgDeclare.cpp - Lower DbgDeclare instructions -----*- C++ -*-===//
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

class LowerDbgDeclareIntrinsic : public FunctionPass {
public:
  static char ID;
  LowerDbgDeclareIntrinsic() : FunctionPass(ID) {}
  virtual bool runOnFunction(Function &) override;
};

char LowerDbgDeclareIntrinsic::ID = 0;

bool LowerDbgDeclareIntrinsic::runOnFunction(Function &F) {
  return LowerDbgDeclare(F);
}

//
// Pass registration
//

static RegisterPass<LowerDbgDeclareIntrinsic>
    X(DEBUG_TYPE, "Lower llvm.dbg.declare intrinsics", false, false);

static void registerLowerDbgDeclareIntrinsicsPass(const PassManagerBuilder &,
                                                  legacy::PassManagerBase &PM) {
  PM.add(new LowerDbgDeclareIntrinsic());
}

static RegisterStandardPasses RegisterLowerDbgDeclareIntrinsicsPass(
    PassManagerBuilder::EP_OptimizerLast,
    registerLowerDbgDeclareIntrinsicsPass);

static RegisterStandardPasses RegisterLowerDbgDeclareIntrinsicsPass0(
    PassManagerBuilder::EP_EnabledOnOptLevel0,
    registerLowerDbgDeclareIntrinsicsPass);
