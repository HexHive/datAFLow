//===-- Heapify.cpp - Heapify static allocations ----------------*- C++ -*-===//
///
/// \file
/// Heapify static allocations
///
//===----------------------------------------------------------------------===//

#include <llvm/ADT/Statistic.h>
#include <llvm/IR/DIBuilder.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/Pass.h>
#include <llvm/Transforms/IPO/PassManagerBuilder.h>
#include <llvm/Transforms/Utils/EscapeEnumerator.h>
#include <llvm/Transforms/Utils/Local.h>
#include <llvm/Transforms/Utils/ModuleUtils.h>

#include "fuzzalloc/Analysis/DefSiteIdentify.h"
#include "fuzzalloc/Analysis/VariableRecovery.h"
#include "fuzzalloc/Metadata.h"
#include "fuzzalloc/Streams.h"
#include "fuzzalloc/Transforms/Utils.h"

using namespace llvm;

#define DEBUG_TYPE "fuzzalloc-heapify"

STATISTIC(NumHeapifiedAllocas, "Number of heapified allocas");

class Heapify : public ModulePass {
public:
  static char ID;
  Heapify() : ModulePass(ID) {}

  virtual void getAnalysisUsage(AnalysisUsage &) const override;
  virtual bool runOnModule(Module &M) override;

private:
  Instruction *insertMalloc(Type *, Value *, Instruction *);
  Instruction *insertFree(Type *, Value *, Instruction *);

  AllocaInst *heapifyAlloca(AllocaInst *);

  Module *Mod;
  LLVMContext *Ctx;
  const DataLayout *DL;
  std::unique_ptr<DIBuilder> DbgBuilder;
};

char Heapify::ID = 0;

Instruction *Heapify::insertMalloc(Type *Ty, Value *Ptr,
                                   Instruction *InsertPt) {
  auto *IntPtrTy = DL->getIntPtrType(*Ctx);
  auto *MallocCall = [&]() -> Instruction * {
    if (auto *ArrayTy = dyn_cast<ArrayType>(Ty)) {
      // Insert array malloc call
      auto *ElemTy = ArrayTy->getArrayElementType();
      auto TySize = DL->getTypeAllocSize(ElemTy);
      auto NumElems = ArrayTy->getNumElements();
      return CallInst::CreateMalloc(InsertPt, IntPtrTy, ElemTy,
                                    ConstantInt::get(IntPtrTy, TySize),
                                    ConstantInt::get(IntPtrTy, NumElems),
                                    nullptr, Ptr->getName() + "_malloccall");
    } else {
      // Insert non-array malloc call
      return CallInst::CreateMalloc(InsertPt, IntPtrTy, Ty,
                                    ConstantExpr::getSizeOf(Ty), nullptr,
                                    nullptr, Ptr->getName() + "_malloccall");
    }
  }();

  auto *MallocStore = new StoreInst(MallocCall, Ptr, InsertPt);
  MallocStore->setMetadata(Mod->getMDKindID(kFuzzallocNoInstrumentMD),
                           MDNode::get(*Ctx, None));
  MallocStore->setMetadata(Mod->getMDKindID(kNoSanitizeMD),
                           MDNode::get(*Ctx, None));

  return MallocCall;
}

Instruction *Heapify::insertFree(Type *Ty, Value *Ptr, Instruction *InsertPt) {
  // Load the pointer to the dynamically allocated memory and free it
  auto *Load = new LoadInst(Ty, Ptr, "", InsertPt);
  Load->setMetadata(Mod->getMDKindID(kFuzzallocNoInstrumentMD),
                    MDNode::get(*Ctx, None));
  Load->setMetadata(Mod->getMDKindID(kNoSanitizeMD), MDNode::get(*Ctx, None));

  return CallInst::CreateFree(Load, InsertPt);
}

AllocaInst *Heapify::heapifyAlloca(AllocaInst *OrigAlloca) {
  unsigned NumMallocs = 0;
  unsigned NumFrees = 0;

  auto *AllocaTy = OrigAlloca->getAllocatedType();
  auto *NewAllocaTy = [&]() -> PointerType * {
    if (AllocaTy->isArrayTy()) {
      return AllocaTy->getArrayElementType()->getPointerTo();
    } else {
      return AllocaTy->getPointerTo();
    }
  }();

  // Create the new alloca
  auto *NewAlloca =
      new AllocaInst(NewAllocaTy, OrigAlloca->getType()->getAddressSpace(),
                     OrigAlloca->getName(), OrigAlloca);
  NewAlloca->setMetadata(Mod->getMDKindID(kFuzzallocHeapifiedAllocaMD),
                         MDNode::get(*Ctx, None));
  NewAlloca->takeName(OrigAlloca);
  NewAlloca->copyMetadata(*OrigAlloca);

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

  // Cache and update users
  SmallVector<Use *, 16> Uses(
      map_range(OrigAlloca->uses(), [](Use &U) { return &U; }));
  for (auto *U : Uses) {
    auto *User = U->getUser();

    if (IsLifetimeStart(User)) {
      // A lifetime.start intrinsic indicates the variable is now "live". So
      // allocate it
      insertMalloc(AllocaTy, NewAlloca, cast<Instruction>(User));
      User->replaceUsesOfWith(OrigAlloca, NewAlloca);
      NumMallocs++;
    } else if (IsLifetimeEnd(User)) {
      // A lifetime.end intrinsic indicates the variable is now "dead". So
      // deallocate it
      insertFree(NewAlloca->getAllocatedType(), NewAlloca,
                 cast<Instruction>(User));
      User->replaceUsesOfWith(OrigAlloca, NewAlloca);
      NumFrees++;
    } else if (isa<Instruction>(User)) {
      // Load the new alloca from the heap before we can do anything with it
      auto *InsertPt = phiSafeInsertPt(U);
      auto *LoadNewAlloca =
          new LoadInst(NewAlloca->getAllocatedType(), NewAlloca, "", InsertPt);
      auto *BitCastNewAlloca = CastInst::CreatePointerCast(
          LoadNewAlloca, OrigAlloca->getType(), "", InsertPt);
      User->replaceUsesOfWith(OrigAlloca, BitCastNewAlloca);
    } else {
      llvm_unreachable("Unsupported alloca user");
    }
  }

  // Place the malloc call after the new alloca if we did not encounter any
  // lifetime.start intrinsics
  if (NumMallocs == 0) {
    insertMalloc(AllocaTy, NewAlloca, OrigAlloca);
    NumMallocs++;
  }

  // Insert free calls at function exit if we did not encounter any
  // lifetime.end intrinsics
  if (NumFrees == 0) {
    EscapeEnumerator EE(*OrigAlloca->getFunction());
    while (auto *AtExit = EE.Next()) {
      insertFree(NewAlloca->getAllocatedType(), NewAlloca,
                 &*AtExit->GetInsertPoint());
    }
  }

  // Update debug users
  replaceDbgDeclare(OrigAlloca, NewAlloca, *DbgBuilder,
                    DIExpression::ApplyOffset, 0);
  replaceDbgValueForAlloca(OrigAlloca, NewAlloca, *DbgBuilder);

  OrigAlloca->eraseFromParent();
  return NewAlloca;
}

void Heapify::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<VariableRecovery>();
  AU.addRequired<DefSiteIdentify>();
}

bool Heapify::runOnModule(Module &M) {
  this->Mod = &M;
  this->Ctx = &M.getContext();
  this->DL = &M.getDataLayout();
  this->DbgBuilder = std::make_unique<DIBuilder>(M);

  const auto &Vars = getAnalysis<VariableRecovery>().getVariables();
  const auto &DefSites = getAnalysis<DefSiteIdentify>().getDefSites();

  if (DefSites.empty()) {
    return false;
  }

  for (auto *Def : DefSites) {
    if (auto *Alloca = dyn_cast<AllocaInst>(Def)) {
      status_stream() << "heapifying " << Vars.lookup(Alloca) << '\n';
      heapifyAlloca(Alloca);
      NumHeapifiedAllocas++;
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
