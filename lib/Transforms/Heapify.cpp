//===-- Heapify.cpp - Heapify static allocations ----------------*- C++ -*-===//
///
/// \file
/// Heapify static allocations
///
//===----------------------------------------------------------------------===//

#include <llvm/ADT/Statistic.h>
#include <llvm/IR/DIBuilder.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/Pass.h>
#include <llvm/Transforms/IPO/PassManagerBuilder.h>
#include <llvm/Transforms/Utils/EscapeEnumerator.h>
#include <llvm/Transforms/Utils/Local.h>

#include "fuzzalloc/Analysis/DefSiteIdentify.h"
#include "fuzzalloc/Metadata.h"

using namespace llvm;

#define DEBUG_TYPE "fuzzalloc-heapify"

STATISTIC(NumHeapifiedAllocas, "Number of heapified allocas");
STATISTIC(NumHeapifiedGlobals, "Number of heapified globals");

namespace {
static Instruction *phiSafeInsertPt(Use &U) {
  auto *InsertPt = cast<Instruction>(U.getUser());
  if (auto *PN = dyn_cast<PHINode>(InsertPt)) {
    InsertPt = PN->getIncomingBlock(U)->getTerminator();
  }
  return InsertPt;
}

static Instruction *insertFree(AllocaInst *Alloca, Instruction *InsertPt) {
  // Load the pointer to the dynamically allocated memory and free it
  auto *Load = new LoadInst(Alloca->getAllocatedType(), Alloca, "", InsertPt);
  return CallInst::CreateFree(Load, InsertPt);
}
} // anonymous namespace

class Heapify : public ModulePass {
public:
  static char ID;
  Heapify() : ModulePass(ID) {}

  virtual void getAnalysisUsage(AnalysisUsage &) const override;
  virtual bool runOnModule(Module &M) override;

private:
  Instruction *insertMalloc(const AllocaInst *, AllocaInst *,
                            Instruction *InsertPt);
  AllocaInst *heapifyAlloca(AllocaInst *);
  GlobalVariable *heapifyGlobal(GlobalVariable *);

  const Module *Mod;                     ///< Module being instrumented
  LLVMContext *Ctx;                      ///< Context
  const DataLayout *DL;                  ///< Data layout
  std::unique_ptr<DIBuilder> DbgBuilder; ///< Debug builder
};

char Heapify::ID = 0;

Instruction *Heapify::insertMalloc(const AllocaInst *OrigAlloca,
                                   AllocaInst *NewAlloca,
                                   Instruction *InsertPt) {
  Type *AllocaTy = OrigAlloca->getAllocatedType();
  auto *IntPtrTy = DL->getIntPtrType(*Ctx);

  auto *MallocCall = [&]() -> Instruction * {
    if (auto *ArrayTy = dyn_cast<ArrayType>(AllocaTy)) {
      // Insert array malloc call
      auto *ElemTy = ArrayTy->getArrayElementType();
      auto TySize = DL->getTypeAllocSize(ElemTy);
      auto NumElems = ArrayTy->getNumElements();
      return CallInst::CreateMalloc(
          InsertPt, IntPtrTy, ElemTy, ConstantInt::get(IntPtrTy, TySize),
          ConstantInt::get(IntPtrTy, NumElems), nullptr,
          NewAlloca->getName() + "_malloccall");
    } else {
      // Insert non-array malloc call
      return CallInst::CreateMalloc(
          InsertPt, IntPtrTy, AllocaTy, ConstantExpr::getSizeOf(AllocaTy),
          nullptr, nullptr, NewAlloca->getName() + "_malloccall");
    }
  }();
  auto *MallocStore = new StoreInst(MallocCall, NewAlloca, InsertPt);
  MallocStore->setMetadata(Mod->getMDKindID(kFuzzallocNoInstrumentMD),
                           MDNode::get(*Ctx, None));

  return MallocCall;
}

AllocaInst *Heapify::heapifyAlloca(AllocaInst *Alloca) {
  unsigned NumMallocs = 0;
  unsigned NumFrees = 0;

  const auto *AllocaTy = Alloca->getAllocatedType();
  auto *NewAllocaTy = [&]() -> PointerType * {
    if (AllocaTy->isArrayTy()) {
      return AllocaTy->getArrayElementType()->getPointerTo();
    } else {
      return AllocaTy->getPointerTo();
    }
  }();

  // Create the new alloca
  auto *NewAlloca =
      new AllocaInst(NewAllocaTy, Alloca->getType()->getAddressSpace(),
                     Alloca->getName(), Alloca);
  NewAlloca->setMetadata(Mod->getMDKindID(kFuzzallocHeapifiedAllocaMD),
                         MDNode::get(*Ctx, None));
  NewAlloca->takeName(Alloca);
  NewAlloca->copyMetadata(*Alloca);

  // Helpers for finding lifetime intrinsics
  const auto &LifetimePred = [](Intrinsic::ID ID) {
    return [=](const User *U) {
      if (const auto *II = dyn_cast<IntrinsicInst>(U)) {
        return II->getIntrinsicID() == ID;
      }
      return false;
    };
  };
  const auto &IsLifetimeStart = LifetimePred(Intrinsic::lifetime_start);
  const auto &IsLifetimeEnd = LifetimePred(Intrinsic::lifetime_end);

  // Update users
  for (auto &U : Alloca->uses()) {
    auto *User = U.getUser();

    if (IsLifetimeStart(User)) {
      // A lifetime.start intrinsic indicates the variable is now "live". So
      // allocate it
      insertMalloc(Alloca, NewAlloca, cast<Instruction>(User));
      User->replaceUsesOfWith(Alloca, NewAlloca);
      NumMallocs++;
    } else if (IsLifetimeEnd(User)) {
      // A lifetime.end intrinsic indicates the variable is now "dead". So
      // deallocate it
      insertFree(NewAlloca, cast<Instruction>(User));
      User->replaceUsesOfWith(Alloca, NewAlloca);
      NumFrees++;
    } else if (auto *I = dyn_cast<Instruction>(User)) {
      // Load the new alloca from the heap before we can do anything with it
      auto *InsertPt = phiSafeInsertPt(U);
      auto *LoadNewAlloca =
          new LoadInst(NewAlloca->getAllocatedType(), NewAlloca, "", InsertPt);
      auto *BitCastNewAlloca = CastInst::CreatePointerCast(
          LoadNewAlloca, Alloca->getType(), "", InsertPt);
      User->replaceUsesOfWith(Alloca, BitCastNewAlloca);
    }
  }

  // Place the malloc call after the new alloca if we did not encounter any
  // lifetime.start intrinsics
  if (NumMallocs == 0) {
    insertMalloc(Alloca, NewAlloca, Alloca);
    NumMallocs++;
  }

  // Insert free calls at function exit if we did not encounter any
  // lifetime.end intrinsics
  if (NumFrees == 0) {
    EscapeEnumerator EE(*Alloca->getFunction());
    while (auto *AtExit = EE.Next()) {
      insertFree(NewAlloca, &*AtExit->GetInsertPoint());
    }
  }

  // Update debug users
  replaceDbgDeclare(Alloca, NewAlloca, *DbgBuilder, DIExpression::ApplyOffset,
                    0);
  replaceDbgValueForAlloca(Alloca, NewAlloca, *DbgBuilder);

  Alloca->eraseFromParent();

  return NewAlloca;
}

GlobalVariable *Heapify::heapifyGlobal(GlobalVariable *GV) {
}

void Heapify::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<DefSiteIdentify>();
}

bool Heapify::runOnModule(Module &M) {
  this->Mod = &M;
  this->Ctx = &M.getContext();
  this->DL = &M.getDataLayout();
  this->DbgBuilder = std::make_unique<DIBuilder>(M);

  const auto &DefSites = getAnalysis<DefSiteIdentify>().getDefSites();
  for (auto *Def : DefSites) {
    if (auto *Alloca = dyn_cast<AllocaInst>(Def)) {
      heapifyAlloca(Alloca);
      NumHeapifiedAllocas++;
    } else if (auto *GV = dyn_cast<GlobalVariable>(Def)) {
      heapifyGlobal(GV);
      NumHeapifiedGlobals++;
    } else {
    }
  }

  return true;
}

//
// Pass registration
//

static RegisterPass<Heapify> X(DEBUG_TYPE, "Heapify static allocations", false,
                               false);

static void registerHeapifyPass(const PassManagerBuilder &,
                                legacy::PassManagerBase &PM) {
  PM.add(new Heapify());
}

static RegisterStandardPasses
    RegisterHeapifyPass(PassManagerBuilder::EP_OptimizerLast,
                        registerHeapifyPass);

static RegisterStandardPasses
    RegisterHeapifyPass0(PassManagerBuilder::EP_EnabledOnOptLevel0,
                         registerHeapifyPass);
