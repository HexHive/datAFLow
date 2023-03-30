//===-- StripLifetime.cpp - Strip lifetime intrinsics -----------*- C++ -*-===//
///
/// \file
/// Strip lifetime.{start, end} intrinsics.
///
//===----------------------------------------------------------------------===//

#include <llvm/IR/InstIterator.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/Pass.h>
#include <llvm/Transforms/IPO/PassManagerBuilder.h>
#include <llvm/Transforms/Utils/Local.h>

using namespace llvm;

#define DEBUG_TYPE "fuzzalloc-strip-lifetime"

class StripLifetime : public ModulePass {
public:
  static char ID;
  StripLifetime() : ModulePass(ID) {}
  virtual bool runOnModule(Module &) override;
};

char StripLifetime::ID = 0;

bool StripLifetime::runOnModule(Module &M) {
  auto &Ctx = M.getContext();
  bool Changed = false;

  SmallVector<User *, 16> ToDelete;

  auto *LifetimeStartFn = Intrinsic::getDeclaration(
      &M, Intrinsic::lifetime_start, Type::getInt8PtrTy(Ctx));
  if (LifetimeStartFn) {
    ToDelete.append(LifetimeStartFn->user_begin(), LifetimeStartFn->user_end());
  }

  auto *LifetimeEndFn = Intrinsic::getDeclaration(&M, Intrinsic::lifetime_end,
                                                  Type::getInt8PtrTy(Ctx));
  if (LifetimeEndFn) {
    ToDelete.append(LifetimeEndFn->user_begin(), LifetimeEndFn->user_end());
  }

  for (auto *U : ToDelete) {
    if (auto *II = dyn_cast<IntrinsicInst>(U)) {
      auto *Ptr = II->getArgOperand(II->getNumOperands() - 1);
      II->eraseFromParent();
      RecursivelyDeleteTriviallyDeadInstructions(Ptr);
      Changed = true;
    }
  }

  if (LifetimeStartFn) {
    LifetimeStartFn->eraseFromParent();
  }
  if (LifetimeEndFn) {
    LifetimeEndFn->eraseFromParent();
  }

  return Changed;
}

//
// Pass registration
//

static RegisterPass<StripLifetime> X(DEBUG_TYPE, "Strip lifetime intrinsics",
                                     false, false);

static void registerStripLifetimePass(const PassManagerBuilder &,
                                      legacy::PassManagerBase &PM) {
  PM.add(new StripLifetime());
}

static RegisterStandardPasses
    RegisterStripLifetimePass(PassManagerBuilder::EP_OptimizerLast,
                              registerStripLifetimePass);

static RegisterStandardPasses
    RegisterStripLifetimePass0(PassManagerBuilder::EP_EnabledOnOptLevel0,
                               registerStripLifetimePass);
