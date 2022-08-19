//===-- LocalVariableTag.cpp - Tag local variables --------------*- C++ -*-===//
///
/// \file
/// Tag local variables
///
//===----------------------------------------------------------------------===//

#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DIBuilder.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/ValueMap.h>
#include <llvm/Pass.h>
#include <llvm/Transforms/IPO/PassManagerBuilder.h>
#include <llvm/Transforms/Utils/EscapeEnumerator.h>
#include <llvm/Transforms/Utils/Local.h>

#include "fuzzalloc/Analysis/DefSiteIdentify.h"
#include "fuzzalloc/Metadata.h"
#include "fuzzalloc/Runtime/BaggyBounds.h"
#include "fuzzalloc/Streams.h"
#include "fuzzalloc/Transforms/Utils.h"

#include "VariableTag.h"

using namespace llvm;

#define DEBUG_TYPE "fuzzalloc-tag-local-variable"

namespace {
static unsigned NumTaggedLocals = 0;
static unsigned NumHeapifiedLocals = 0;
} // anonymous namespace

class LocalVarTag : public ModulePass {
public:
  static char ID;
  LocalVarTag() : ModulePass(ID) {}

  virtual void getAnalysisUsage(AnalysisUsage &) const override;
  virtual bool runOnModule(Module &) override;

private:
  AllocaInst *heapify(AllocaInst *);
  AllocaInst *tagAlloca(AllocaInst *);

  Module *Mod;
  LLVMContext *Ctx;
  const DataLayout *DL;
  std::unique_ptr<DIBuilder> DbgBuilder;

  IntegerType *TagTy;
  IntegerType *IntPtrTy;
  FunctionCallee BBRegisterFn;

  ValueMap<AllocaInst *, SmallVector<IntrinsicInst *, 4>> AllocaLifetimes;
};

char LocalVarTag::ID = 0;

AllocaInst *LocalVarTag::heapify(AllocaInst *OrigAlloca) {
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

  // Cache and update users
  SmallVector<Use *, 16> Uses(
      map_range(OrigAlloca->uses(), [](Use &U) { return &U; }));
  for (auto *U : Uses) {
    auto *User = U->getUser();

    if (auto *Inst = dyn_cast<Instruction>(User)) {
      if (isLifetimeStart(Inst)) {
        // A lifetime.start intrinsic indicates the variable is now "live". So
        // allocate it
        insertMalloc(AllocaTy, NewAlloca, Inst);
        Inst->replaceUsesOfWith(OrigAlloca, NewAlloca);
        NumMallocs++;
      } else if (isLifetimeEnd(Inst)) {
        // A lifetime.end intrinsic indicates the variable is now "dead". So
        // deallocate it
        insertFree(NewAlloca->getAllocatedType(), NewAlloca, Inst);
        Inst->replaceUsesOfWith(OrigAlloca, NewAlloca);
        NumFrees++;
      } else {
        // Load the new alloca from the heap before we can do anything with it
        auto *InsertPt = phiSafeInsertPt(U);
        auto *LoadNewAlloca = new LoadInst(NewAlloca->getAllocatedType(),
                                           NewAlloca, "", InsertPt);
        auto *BitCastNewAlloca = CastInst::CreatePointerCast(
            LoadNewAlloca, OrigAlloca->getType(), "", InsertPt);
        Inst->replaceUsesOfWith(OrigAlloca, BitCastNewAlloca);
      }
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
  NumHeapifiedLocals++;

  return NewAlloca;
}

AllocaInst *LocalVarTag::tagAlloca(AllocaInst *OrigAlloca) {
  auto *Int8PtrTy = Type::getInt8PtrTy(*Ctx);
  auto *Zero = ConstantInt::getNullValue(IntegerType::getInt32Ty(*Ctx));
  auto *OrigTy = OrigAlloca->getAllocatedType();
  auto OrigSize = DL->getTypeAllocSize(OrigTy);

  auto NewAllocSize = getTaggedVarSize(DL->getTypeAllocSize(OrigTy));
  if (NewAllocSize > IntegerType::MAX_INT_BITS) {
    warning_stream()
        << "Unable to tag alloca " << OrigAlloca->getName()
        << ": new allocation size " << NewAllocSize
        << " is greater than the max possible size. Heapifying instead\n";
    return heapify(OrigAlloca);
  }

  auto PaddingSize = NewAllocSize - OrigSize - kMetaSize;
  auto *PaddingTy = ArrayType::get(Type::getInt8Ty(*Ctx), PaddingSize);
  auto *Tag = generateTag(TagTy);

  auto *NewAllocaTy =
      StructType::get(*Ctx, {OrigTy, PaddingTy, TagTy}, /*isPacked=*/true);
  auto *NewAlloca = new AllocaInst(
      NewAllocaTy, OrigAlloca->getType()->getPointerAddressSpace(),
      OrigAlloca->hasName() ? OrigAlloca->getName() + ".tagged" : "",
      OrigAlloca);
  NewAlloca->setMetadata(Mod->getMDKindID(kFuzzallocTaggVarMD),
                         MDNode::get(*Ctx, None));
  NewAlloca->copyMetadata(*OrigAlloca);
  NewAlloca->setAlignment(Align(NewAllocSize));
  NewAlloca->takeName(OrigAlloca);

  // Store the tag
  auto *InitVal = ConstantStruct::get(
      NewAllocaTy, {UndefValue::get(OrigTy), UndefValue::get(PaddingTy), Tag});

  const auto &InsertInitStore = [&](Instruction *InsertPt) -> StoreInst * {
    auto *InitStore = new StoreInst(InitVal, NewAlloca, InsertPt);
    InitStore->setMetadata(Mod->getMDKindID(kFuzzallocNoInstrumentMD),
                           MDNode::get(*Ctx, None));
    InitStore->setMetadata(Mod->getMDKindID(kNoSanitizeMD),
                           MDNode::get(*Ctx, None));
    return InitStore;
  };

  // Register the allocation in the baggy bounds table
  const auto &InsertRegisterAlloca = [&](Instruction *InsertPt) -> CallInst * {
    auto *NewAllocaCasted = new BitCastInst(NewAlloca, Int8PtrTy, "", InsertPt);
    return CallInst::Create(
        BBRegisterFn,
        {NewAllocaCasted, ConstantInt::get(IntPtrTy, NewAllocSize)}, "",
        InsertPt);
  };

  const auto &LifetimeIt = AllocaLifetimes.find(OrigAlloca);

  // Fix lifetime.{start, end} intrinsics
  if (LifetimeIt == AllocaLifetimes.end()) {
    InsertInitStore(OrigAlloca);
    InsertRegisterAlloca(OrigAlloca);
  } else {
    for (auto *II : LifetimeIt->second) {
      assert(II->getIntrinsicID() == Intrinsic::lifetime_start ||
             II->getIntrinsicID() == Intrinsic::lifetime_end);
      assert(II->getNumUses() == 0);

      auto *Size = ConstantInt::get(Type::getInt64Ty(*Ctx), NewAllocSize);
      auto *LifetimeFn =
          Intrinsic::getDeclaration(Mod, II->getIntrinsicID(), Int8PtrTy);

      auto *BCNewAlloca = new BitCastInst(NewAlloca, Int8PtrTy, "", II);
      auto *NewLifetime =
          CallInst::Create(LifetimeFn->getFunctionType(), LifetimeFn,
                           {Size, BCNewAlloca}, "", II);
      NewLifetime->takeName(II);

      if (II->getIntrinsicID() == Intrinsic::lifetime_start) {
        InsertInitStore(II);
        InsertRegisterAlloca(II);
      }

      auto *Ptr = II->getArgOperand(II->getNumArgOperands() - 1);
      II->eraseFromParent();
      RecursivelyDeleteTriviallyDeadInstructions(Ptr);
    }
  }

  // Now cache and update the other users
  SmallVector<Use *, 16> Uses(
      map_range(OrigAlloca->uses(), [](Use &U) { return &U; }));
  for (auto *U : Uses) {
    auto *User = U->getUser();

    if (auto *Inst = dyn_cast<Instruction>(User)) {
      auto *InsertPt = phiSafeInsertPt(U);
      auto *GEP = GetElementPtrInst::CreateInBounds(NewAllocaTy, NewAlloca,
                                                    {Zero, Zero}, "", InsertPt);
      Inst->replaceUsesOfWith(OrigAlloca, GEP);
    } else {
      llvm_unreachable("Unsupported alloca user");
    }
  }

  // Update debug users
  replaceDbgDeclare(OrigAlloca, NewAlloca, *DbgBuilder,
                    DIExpression::ApplyOffset, 0);
  replaceDbgValueForAlloca(OrigAlloca, NewAlloca, *DbgBuilder);

  OrigAlloca->eraseFromParent();
  return NewAlloca;
}

void LocalVarTag::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<DefSiteIdentify>();
}

bool LocalVarTag::runOnModule(Module &M) {
  this->Mod = &M;
  this->Ctx = &M.getContext();
  this->DL = &M.getDataLayout();
  this->DbgBuilder = std::make_unique<DIBuilder>(M);

  this->TagTy = Type::getIntNTy(*Ctx, kNumTagBits);
  this->IntPtrTy = DL->getIntPtrType(*Ctx);
  this->BBRegisterFn =
      M.getOrInsertFunction("__bb_register", Type::getVoidTy(*Ctx),
                            Type::getInt8PtrTy(*Ctx), IntPtrTy);

  const auto &DefSites = getAnalysis<DefSiteIdentify>().getDefSites();

  if (DefSites.empty()) {
    return false;
  }

  const auto &AllocaDefs = map_range(
      make_filter_range(DefSites, [](Value *V) { return isa<AllocaInst>(V); }),
      [](Value *V) { return cast<AllocaInst>(V); });
  SmallPtrSet<AllocaInst *, 32> AllocaDefSet(AllocaDefs.begin(),
                                             AllocaDefs.end());

  // Collect any lifetime.{start, end} intrinsic instructions used by the
  // allocas we intend to tag. These intrinsic instructions must be adjusted
  // after tagging
  for (auto &F : M) {
    for (auto &I : instructions(F)) {
      if (isLifetimeStart(&I) || isLifetimeEnd(&I)) {
        auto *II = cast<IntrinsicInst>(&I);
        auto *Addr = II->getArgOperand(II->getNumArgOperands() - 1);
        auto *Alloca = findAllocaForValue(Addr);
        if (Alloca && AllocaDefSet.count(Alloca) > 0) {
          AllocaLifetimes[Alloca].push_back(II);
        }
      }
    }
  }

  for (auto *Alloca : AllocaDefs) {
    tagAlloca(Alloca);
    NumTaggedLocals++;
  }

  return true;
}

//
// Pass registration
//

static RegisterPass<LocalVarTag> X(DEBUG_TYPE, "Tag local variables", false,
                                   false);

static void registerLocalVarTagPass(const PassManagerBuilder &,
                                    legacy::PassManagerBase &PM) {
  PM.add(new LocalVarTag());
}

static RegisterStandardPasses
    RegisterLocalVarTagPass(PassManagerBuilder::EP_OptimizerLast,
                            registerLocalVarTagPass);

static RegisterStandardPasses
    RegisterLocalVarTagPass0(PassManagerBuilder::EP_EnabledOnOptLevel0,
                             registerLocalVarTagPass);
