//===-- LowerConstantExpr.cpp - Lower constant expressions ------*- C++ -*-===//
///
/// \file
/// Lower constant expressions.
///
//===----------------------------------------------------------------------===//

#include <llvm/IR/InstIterator.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/Pass.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Transforms/IPO/PassManagerBuilder.h>

#include "fuzzalloc/Transforms/Utils.h"

using namespace llvm;

#define DEBUG_TYPE "fuzzalloc-lower-cexpr"

namespace {
//
// Global variables
//

static unsigned NumLoweredCExprs = 0;

//
// Helper functions
//

static bool expandInstruction(Instruction *);

static Instruction *expandCExpr(ConstantExpr *CE, Instruction *InsertPt) {
  auto *NewInst = CE->getAsInstruction();
  NewInst->insertBefore(InsertPt);
  expandInstruction(NewInst);
  return NewInst;
}

static bool expandInstruction(Instruction *Inst) {
  if (isa<LandingPadInst>(Inst)) {
    return false;
  }

  bool Changed = false;

  for (unsigned Op = 0; Op < Inst->getNumOperands(); ++Op) {
    if (auto *CE = dyn_cast<ConstantExpr>(Inst->getOperand(Op))) {
      auto *U = &Inst->getOperandUse(Op);
      phiSafeReplaceUses(U, expandCExpr(CE, phiSafeInsertPt(U)));

      if (!CE->isConstantUsed()) {
        CE->destroyConstant();
      }

      NumLoweredCExprs++;
      Changed = true;
    }
  }

  return Changed;
}
} // anonymous namespace

/// Lower constant expressions to instructions
class LowerCExpr : public FunctionPass {
public:
  static char ID;
  LowerCExpr() : FunctionPass(ID) {}
  virtual bool runOnFunction(Function &) override;
};

char LowerCExpr::ID = 0;

bool LowerCExpr::runOnFunction(Function &F) {
  bool Changed = false;

  for (auto &I : instructions(F)) {
    Changed = expandInstruction(&I);
  }

  return Changed;
}

//
// Pass registration
//

static RegisterPass<LowerCExpr> X(DEBUG_TYPE, "Lower constant expressions",
                                  false, false);

static void registerLowerCExprPass(const PassManagerBuilder &,
                                   legacy::PassManagerBase &PM) {
  PM.add(new LowerCExpr());
}

static RegisterStandardPasses
    RegisterLowerCExprPass(PassManagerBuilder::EP_OptimizerLast,
                           registerLowerCExprPass);

static RegisterStandardPasses
    RegisterLowerCExprPass0(PassManagerBuilder::EP_EnabledOnOptLevel0,
                            registerLowerCExprPass);
