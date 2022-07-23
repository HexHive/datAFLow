//===-- VariableRecovery.cpp - Recover source-level variables -------------===//
///
/// \file
/// Recover source-level variables through debug information.
///
//===----------------------------------------------------------------------===//

#include <llvm/IR/DebugInfo.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Transforms/IPO/PassManagerBuilder.h>

#include "Analysis/VariableRecovery.h"

#define DEBUG_TYPE "variable-recovery"

using namespace llvm;

char VariableRecovery::ID = 0;

bool VariableRecovery::runOnModule(Module &M) {
  // STEP 1: Local variables
  for (const auto &F : M) {
    for (const auto &I : instructions(&F)) {
      // We only care about llvm.dbg.* variable declarations
      if (!isa<DbgVariableIntrinsic>(&I)) {
        continue;
      }

      const auto *Var = cast<DbgVariableIntrinsic>(&I)->getVariable();
      LLVM_DEBUG(dbgs() << "Handling local variable `" << Var->getName()
                        << "`\n");

      if (const auto *DbgVal = dyn_cast<DbgValueInst>(&I)) {
        const auto *V = DbgVal->getValue();
        if (V) {
          Vars.insert({V, Var});
        }
      } else if (const auto *DbgDecl = dyn_cast<DbgDeclareInst>(&I)) {
        const auto *Addr = DbgDecl->getAddress();
        if (Addr) {
          Vars.insert({Addr, Var});
        }
      } else if (const auto *DbgAddr = dyn_cast<DbgAddrIntrinsic>(&I)) {
        assert(false && "llvm.dbg.addr not yet supported");
      }
    }
  }

  // STEP 2: Global variables
  SmallVector<DIGlobalVariableExpression *, 16> GVExprs;
  for (auto &GV : M.globals()) {
    GVExprs.clear();
    GV.getDebugInfo(GVExprs);
    for (const auto *GVExpr : GVExprs) {
      if (auto *Var = GVExpr->getVariable()) {
        LLVM_DEBUG(dbgs() << "Handling global variable `" << Var->getName()
                          << "`\n");
        Vars.insert({&GV, Var});
      }
    }
  }

  return false;
}

//===----------------------------------------------------------------------===//

static RegisterPass<VariableRecovery> X("fuzzalloc-variable-recovery",
                                        "Recover source-level variables", true,
                                        true);

static void registerVariableRecoveryPass(const PassManagerBuilder &,
                                         legacy::PassManagerBase &PM) {
  PM.add(new VariableRecovery());
}

static RegisterStandardPasses
    RegisterVariableRecoveryPass(PassManagerBuilder::EP_ModuleOptimizerEarly,
                                 registerVariableRecoveryPass);

static RegisterStandardPasses RegisterInstrumentVariableRecoveryPass0(
    PassManagerBuilder::EP_EnabledOnOptLevel0, registerVariableRecoveryPass);
