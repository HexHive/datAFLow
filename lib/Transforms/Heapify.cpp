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
#include <llvm/Transforms/Utils/ModuleUtils.h>

#include "fuzzalloc/Analysis/DefSiteIdentify.h"
#include "fuzzalloc/Analysis/VariableRecovery.h"
#include "fuzzalloc/Metadata.h"
#include "fuzzalloc/Streams.h"
#include "fuzzalloc/Transforms/Utils.h"

using namespace llvm;

#define DEBUG_TYPE "fuzzalloc-heapify"

STATISTIC(NumHeapifiedAllocas, "Number of heapified allocas");
STATISTIC(NumHeapifiedGlobals, "Number of heapified globals");

namespace {
static Use *ptrUse(Use &U) { return &U; }
} // anonymous namespace

class Heapify : public ModulePass {
public:
  static char ID;
  Heapify() : ModulePass(ID) {}

  virtual void getAnalysisUsage(AnalysisUsage &) const override;
  virtual bool runOnModule(Module &M) override;

private:
  Instruction *insertMalloc(Type *, Value *, Instruction *);
  Instruction *insertFree(Type *, Value *, Instruction *);

  void createHeapifyCtor(GlobalVariable *, IRBuilder<> &);
  void createHeapifyDtor(GlobalVariable *, IRBuilder<> &);
  GlobalVariable *heapifyGlobal(GlobalVariable *);

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

/// Create constructor for heapified global variable
///
/// The IRBuilder's insertion point is set to where a `malloc` should be
/// inserted.
void Heapify::createHeapifyCtor(GlobalVariable *GV, IRBuilder<> &IRB) {
  auto *GlobalCtorTy =
      FunctionType::get(Type::getVoidTy(*Ctx), /*isVarArg=*/false);
  auto *GlobalCtorF =
      Function::Create(GlobalCtorTy, GlobalValue::InternalLinkage,
                       "fuzzalloc.ctor." + GV->getName(), *Mod);
  appendToGlobalCtors(*Mod, GlobalCtorF, /*Priority=*/0);

  auto *EntryBB = BasicBlock::Create(*Ctx, "entry", GlobalCtorF);
  IRB.SetInsertPoint(EntryBB);

  if (GV->getLinkage() == GlobalValue::LinkOnceAnyLinkage ||
      GV->getLinkage() == GlobalValue::LinkOnceODRLinkage) {
    // Weak linkage means the same constructor may be inserted in multiple
    // modules, causing the global to be allocated multiple times. To prevent
    // this, we generate code to check if the global has already been allocated.
    // If it has, just return.

    // Create the basic block when the global has already been allocated:
    // nothing to do in this case
    auto *TrueBB = BasicBlock::Create(*Ctx, "true", GlobalCtorF);
    ReturnInst::Create(*Ctx, TrueBB);

    // Create the basic block when the global has not been allocated: load and
    // allocate the variable
    auto *FalseBB = BasicBlock::Create(*Ctx, "false", GlobalCtorF);
    ReturnInst::Create(*Ctx, FalseBB);

    // Load the global
    auto *GVLoad = IRB.CreateLoad(GV);
    GVLoad->setMetadata(Mod->getMDKindID(kFuzzallocNoInstrumentMD),
                        MDNode::get(*Ctx, None));
    GVLoad->setMetadata(Mod->getMDKindID(kNoSanitizeMD),
                        MDNode::get(*Ctx, None));

    // Check if the global has already been allocated
    auto *Check =
        IRB.CreateICmpNE(GVLoad, Constant::getNullValue(GVLoad->getType()));
    IRB.CreateCondBr(Check, TrueBB, FalseBB);

    // Only allocate the global if it has not already been allocated
    IRB.SetInsertPoint(FalseBB->getTerminator());
  } else {
    // No branching - just return from the block
    auto *RetVoid = IRB.CreateRetVoid();
    IRB.SetInsertPoint(RetVoid);
  }
}

/// Create destructor for heapified global variable
///
/// The IRBuilder's insertion point is set to where a `free` should be inserted.
void Heapify::createHeapifyDtor(GlobalVariable *GV, IRBuilder<> &IRB) {
  auto *GlobalDtorTy =
      FunctionType::get(Type::getVoidTy(*Ctx), /*isVarArg=*/false);
  auto *GlobalDtorF =
      Function::Create(GlobalDtorTy, GlobalValue::InternalLinkage,
                       "fuzzalloc.dtor." + GV->getName(), *Mod);
  appendToGlobalDtors(*Mod, GlobalDtorF, /*Priority=*/0);

  auto *EntryBB = BasicBlock::Create(*Ctx, "entry", GlobalDtorF);
  IRB.SetInsertPoint(EntryBB);

  if (GV->getLinkage() == GlobalValue::LinkOnceAnyLinkage ||
      GV->getLinkage() == GlobalValue::LinkOnceODRLinkage) {
    // Weak linkage means the same destructor may be inserted in multiple
    // modules, causing the global to be freed multiple times. To prevent this,
    // we generate code to check if the global has already been freed. If it
    // has, just return.

    // Create the basic block when the global has already been freed: nothing
    // to do in this case
    auto *TrueBB = BasicBlock::Create(*Ctx, "true", GlobalDtorF);
    ReturnInst::Create(*Ctx, TrueBB);

    // Create the basic block when the global has not been freed: load and free
    // the variable
    auto *FalseBB = BasicBlock::Create(*Ctx, "false", GlobalDtorF);
    ReturnInst::Create(*Ctx, FalseBB);

    // Load the global
    auto *GVLoad = IRB.CreateLoad(GV);
    GVLoad->setMetadata(Mod->getMDKindID(kFuzzallocNoInstrumentMD),
                        MDNode::get(*Ctx, None));
    GVLoad->setMetadata(Mod->getMDKindID(kNoSanitizeMD),
                        MDNode::get(*Ctx, None));

    // Check if the global has already been free
    auto *Check =
        IRB.CreateICmpEQ(GVLoad, Constant::getNullValue(GVLoad->getType()));
    IRB.CreateCondBr(Check, TrueBB, FalseBB);

    // Set the global to NULL
    IRB.SetInsertPoint(FalseBB->getTerminator());
    auto *NullStore =
        IRB.CreateStore(Constant::getNullValue(GVLoad->getType()), GV);
    NullStore->setMetadata(Mod->getMDKindID(kFuzzallocNoInstrumentMD),
                           MDNode::get(*Ctx, None));
    NullStore->setMetadata(Mod->getMDKindID(kNoSanitizeMD),
                           MDNode::get(*Ctx, None));

    // Free the global variable before setting it to NULL
    IRB.SetInsertPoint(NullStore);
  } else {
    // No branching - just return from the block
    auto *RetVoid = IRB.CreateRetVoid();
    IRB.SetInsertPoint(RetVoid);
  }
}

GlobalVariable *Heapify::heapifyGlobal(GlobalVariable *OrigGV) {

  IRBuilder<> IRB(*Ctx);
  auto *ValueTy = [&]() -> Type * {
    if (isa<ArrayType>(OrigGV->getValueType())) {
      return OrigGV->getValueType()->getArrayElementType()->getPointerTo();
    }
    return OrigGV->getValueType();
  }();

  // Create new global
  auto *NewGV = new GlobalVariable(
      *Mod, ValueTy, /*isConstant=*/false, OrigGV->getLinkage(),
      /* If the original global had an initializer, replace it with the null
         pointer initializer (it will be initialized later) */
      !OrigGV->isDeclaration() ? Constant::getNullValue(ValueTy) : nullptr,
      OrigGV->getName(), OrigGV, OrigGV->getThreadLocalMode(),
      OrigGV->getType()->getAddressSpace(), OrigGV->isExternallyInitialized());
  NewGV->takeName(OrigGV);
  NewGV->copyAttributesFrom(OrigGV);
  NewGV->setAlignment(MaybeAlign(0));

  // Copy debug info
  SmallVector<DIGlobalVariableExpression *, 2> GVEs;
  OrigGV->getDebugInfo(GVEs);
  for (auto *GVE : GVEs) {
    NewGV->addDebugInfo(GVE);
  }

  // Allocate and initialize the global
  if (!OrigGV->isDeclaration()) {
    createHeapifyCtor(NewGV, IRB);

    auto *ValueTy = OrigGV->getValueType();
    auto *MallocCall = insertMalloc(ValueTy, NewGV, &*IRB.GetInsertPoint());

    if (OrigGV->hasInitializer()) {
      auto *Initializer = OrigGV->getInitializer();
      auto *InitializerTy = Initializer->getType();
      auto *InitializerGV =
          new GlobalVariable(*Mod, InitializerTy, /*isConstant=*/true,
                             GlobalValue::PrivateLinkage, Initializer);
      InitializerGV->setUnnamedAddr(GlobalValue::UnnamedAddr::Global);
      InitializerGV->setAlignment(MaybeAlign(OrigGV->getAlignment()));

      IRB.CreateMemCpy(MallocCall, MaybeAlign(NewGV->getAlignment()),
                       InitializerGV, MaybeAlign(InitializerGV->getAlignment()),
                       DL->getTypeAllocSize(InitializerTy));
    }

    auto *MallocStore = IRB.CreateStore(MallocCall, NewGV);
    MallocStore->setMetadata(Mod->getMDKindID(kFuzzallocNoInstrumentMD),
                             MDNode::get(*Ctx, None));
    MallocStore->setMetadata(Mod->getMDKindID(kNoSanitizeMD),
                             MDNode::get(*Ctx, None));
  }

  // Cache uses and update
  SmallVector<Use *, 16> Uses(map_range(OrigGV->uses(), ptrUse));
  for (auto *U : Uses) {
    auto *User = U->getUser();

    if (isa<Instruction>(User)) {
      // Load the new global from the heap before we can do anything with it
      auto *InsertPt = phiSafeInsertPt(U);
      auto *LoadNewGV =
          new LoadInst(NewGV->getValueType(), NewGV, "", InsertPt);
      auto *BitCastNewGV = CastInst::CreatePointerCast(
          LoadNewGV, OrigGV->getType(), "", InsertPt);
      User->replaceUsesOfWith(OrigGV, BitCastNewGV);
    } else {
      // Constant expressions should have been lowered
      llvm_unreachable("Unsupported global variable user");
    }
  }

  // Deallocate the global
  if (!NewGV->isDeclaration()) {
    createHeapifyDtor(NewGV, IRB);
    insertFree(NewGV->getValueType(), NewGV, &*IRB.GetInsertPoint());
  }

  OrigGV->eraseFromParent();
  return NewGV;
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

  // Cache uses and update
  SmallVector<Use *, 16> Uses(map_range(OrigAlloca->uses(), ptrUse));
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

  for (auto *Def : DefSites) {
    if (auto *Alloca = dyn_cast<AllocaInst>(Def)) {
      status_stream() << "heapifying " << Vars.lookup(Alloca) << '\n';
      heapifyAlloca(Alloca);
      NumHeapifiedAllocas++;
    } else if (auto *GV = dyn_cast<GlobalVariable>(Def)) {
      status_stream() << "heapifying " << Vars.lookup(GV) << '\n';
      heapifyGlobal(GV);
      NumHeapifiedGlobals++;
    } else {
      llvm_unreachable("Unsupported def site");
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
