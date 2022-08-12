//===-- LowerConstantExprs.cpp - Lower atomic constant expressions --------===//
///
/// \file
/// Lower constant expressions
///
//===----------------------------------------------------------------------===//

#include <llvm/ADT/Statistic.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/Pass.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Transforms/IPO/PassManagerBuilder.h>

#include "fuzzalloc/Transforms/Utils.h"

using namespace llvm;

#define DEBUG_TYPE "fuzzalloc-lower-cexprs"

STATISTIC(NumLoweredCExprs, "Number of lowered constant expressions");

namespace {
bool expandInstruction(Instruction *);

static Instruction *expandCExpr(ConstantExpr *CE, Instruction *InsertPt) {
  auto *NewInst = CE->getAsInstruction();
  NewInst->insertBefore(InsertPt);
  expandInstruction(NewInst);
  return NewInst;
}

static bool expandInstruction(Instruction *Inst) {
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
class LowerCExprs : public FunctionPass {
public:
  static char ID;
  LowerCExprs() : FunctionPass(ID) {}
  virtual bool runOnFunction(Function &) override;
};

char LowerCExprs::ID = 0;

bool LowerCExprs::runOnFunction(Function &F) {
  bool Changed = false;

  for (auto &I : instructions(F)) {
    Changed = expandInstruction(&I);
  }

  return Changed;
}

//
// Pass registration
//

static RegisterPass<LowerCExprs> X(DEBUG_TYPE, "Lower constant expressions",
                                   false, false);

static void registerLowerCExprsPass(const PassManagerBuilder &,
                                    legacy::PassManagerBase &PM) {
  PM.add(new LowerCExprs());
}

static RegisterStandardPasses
    RegisterLowerCExprsPass(PassManagerBuilder::EP_OptimizerLast,
                            registerLowerCExprsPass);

static RegisterStandardPasses
    RegisterLowerCExprsPass0(PassManagerBuilder::EP_EnabledOnOptLevel0,
                             registerLowerCExprsPass);
