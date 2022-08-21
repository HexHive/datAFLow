//===-- GlobalVarTag.cpp - Tag global variables -----------------*- C++ -*-===//
///
/// \file
/// Tag global variables
///
//===----------------------------------------------------------------------===//

#include <llvm/IR/Constants.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/Pass.h>
#include <llvm/Transforms/IPO/PassManagerBuilder.h>
#include <llvm/Transforms/Utils/ModuleUtils.h>

#include "fuzzalloc/Analysis/DefSiteIdentify.h"
#include "fuzzalloc/Metadata.h"
#include "fuzzalloc/Runtime/BaggyBounds.h"
#include "fuzzalloc/Streams.h"
#include "fuzzalloc/Transforms/Utils.h"

#include "VariableTag.h"

using namespace llvm;

#define DEBUG_TYPE "fuzzalloc-tag-global-variable"

namespace {
static unsigned NumTaggedGVs = 0;
static unsigned NumHeapifiedGVs = 0;
} // anonymous namespace

class GlobalVarTag : public ModulePass {
public:
  static char ID;
  GlobalVarTag() : ModulePass(ID) {}

  virtual void getAnalysisUsage(AnalysisUsage &) const override;
  virtual bool runOnModule(Module &) override;

private:
  void createHeapifyCtor(GlobalVariable *, IRBuilder<> &);
  void createHeapifyDtor(GlobalVariable *, IRBuilder<> &);

  GlobalVariable *heapify(GlobalVariable *);
  GlobalVariable *tagGlobalVariable(GlobalVariable *, BasicBlock *);

  Module *Mod;
  LLVMContext *Ctx;
  const DataLayout *DL;

  IntegerType *TagTy;
  IntegerType *IntPtrTy;
  FunctionCallee BBRegisterFn;
};

char GlobalVarTag::ID = 0;

/// Create constructor for heapified global variable
///
/// The IRBuilder's insertion point is set to where a `malloc` should be
/// inserted.
void GlobalVarTag::createHeapifyCtor(GlobalVariable *GV, IRBuilder<> &IRB) {
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
void GlobalVarTag::createHeapifyDtor(GlobalVariable *GV, IRBuilder<> &IRB) {
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

GlobalVariable *GlobalVarTag::heapify(GlobalVariable *OrigGV) {
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
  SmallVector<Use *, 16> Uses(
      map_range(OrigGV->uses(), [](Use &U) { return &U; }));
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
    } else if (auto *GV = dyn_cast<GlobalVariable>(User)) {
      // If the user is another global variable then the `use` must be an
      // assignment initializer. Here, we need to replace the initializer rather
      // then call `handleOperandChange`
      assert(GV->hasInitializer());
      assert(GV->getInitializer() == OrigGV);
      auto *NewInit = ConstantExpr::getPointerCast(NewGV, OrigGV->getType());
      GV->setInitializer(NewInit);
    } else if (auto *C = dyn_cast<Constant>(User)) {
      auto *BitCast = ConstantExpr::getPointerCast(NewGV, OrigGV->getType());
      C->handleOperandChange(OrigGV, BitCast);
    } else {
      llvm_unreachable("Unsupported global variable user");
    }
  }

  // Deallocate the global
  if (!NewGV->isDeclaration()) {
    createHeapifyDtor(NewGV, IRB);
    insertFree(NewGV->getValueType(), NewGV, &*IRB.GetInsertPoint());
  }

  OrigGV->eraseFromParent();
  NumHeapifiedGVs++;

  return NewGV;
}

GlobalVariable *GlobalVarTag::tagGlobalVariable(GlobalVariable *OrigGV,
                                                BasicBlock *CtorBB) {
  auto *Zero = ConstantInt::getNullValue(IntegerType::getInt32Ty(*Ctx));
  auto *OrigTy = OrigGV->getValueType();
  auto OrigSize = DL->getTypeAllocSize(OrigTy);
  auto NewAllocSize = getTaggedVarSize(DL->getTypeAllocSize(OrigTy));
  if (NewAllocSize > IntegerType::MAX_INT_BITS) {
    warning_stream() << "Unable to tag global variable `" << OrigGV->getName()
                     << "`: new allocation size " << NewAllocSize
                     << " is greater than the max. Heapifying instead.\n";
    return heapify(OrigGV);
  }

  auto PaddingSize = NewAllocSize - OrigSize - kMetaSize;
  if (PaddingSize > kMaxPaddingSize) {
    warning_stream() << "Unable to tag globa variable `" << OrigGV->getName()
                     << "`: padding size " << PaddingSize
                     << " is greater than the max. Heapifying instead.\n";
    return heapify(OrigGV);
  }

  auto *PaddingTy = ArrayType::get(Type::getInt8Ty(*Ctx), PaddingSize);
  auto *Tag = generateTag(TagTy);

  auto *NewGVTy =
      StructType::get(*Ctx, {OrigTy, PaddingTy, TagTy}, /*isPacked=*/true);
  auto *NewInit = ConstantStruct::get(
      NewGVTy, {OrigGV->hasInitializer() ? OrigGV->getInitializer()
                                         : UndefValue::get(OrigTy),
                UndefValue::get(PaddingTy), Tag});

  // Create a tagged version of the global variable (only visible in this
  // module)
  auto *NewGV = new GlobalVariable(
      *Mod, NewGVTy, OrigGV->isConstant(), GlobalValue::PrivateLinkage, NewInit,
      OrigGV->hasName() ? OrigGV->getName().str() + ".tagged" : "", OrigGV,
      OrigGV->getThreadLocalMode(), OrigGV->getType()->getPointerAddressSpace(),
      OrigGV->isExternallyInitialized());
  NewGV->copyAttributesFrom(OrigGV);
  NewGV->setMetadata(Mod->getMDKindID(kFuzzallocTaggVarMD),
                     MDNode::get(*Ctx, None));
  NewGV->setAlignment(MaybeAlign(NewAllocSize));
  if (NewGV->isImplicitDSOLocal()) {
    NewGV->setDSOLocal(true);
  }

  // Copy debug info
  SmallVector<DIGlobalVariableExpression *, 2> GVEs;
  OrigGV->getDebugInfo(GVEs);
  for (auto *GVE : GVEs) {
    NewGV->addDebugInfo(GVE);
  }

  // Cache and update users
  SmallVector<Use *, 16> Uses(
      map_range(OrigGV->uses(), [](Use &U) { return &U; }));
  for (auto *U : Uses) {
    auto *User = U->getUser();

    if (isa<Instruction>(User)) {
      auto *InsertPt = phiSafeInsertPt(U);
      auto *GEP = GetElementPtrInst::CreateInBounds(NewGVTy, NewGV,
                                                    {Zero, Zero}, "", InsertPt);
      User->replaceUsesOfWith(OrigGV, GEP);
    } else if (auto *GV = dyn_cast<GlobalVariable>(User)) {
      // If the user is another global variable then the `use` must be an
      // assignment initializer. Here, we need to replace the initializer rather
      // then call `handleOperandChange`
      assert(GV->hasInitializer());
      assert(GV->getInitializer() == OrigGV);
      auto *NewInit = ConstantExpr::getInBoundsGetElementPtr(
          NewGVTy, NewGV, ArrayRef<Constant *>({Zero, Zero}));
      GV->setInitializer(NewInit);
    } else if (auto *C = dyn_cast<Constant>(User)) {
      auto *GEP = ConstantExpr::getInBoundsGetElementPtr(
          NewGVTy, NewGV, ArrayRef<Constant *>({Zero, Zero}));
      C->handleOperandChange(OrigGV, GEP);
    } else {
      llvm_unreachable("Unsupported global variable user");
    }
  }

  // Register the allocation in the baggy bounds table
  auto *Int8PtrTy = Type::getInt8PtrTy(*Ctx);
  auto *NewGVCasted = new BitCastInst(NewGV, Int8PtrTy, "", CtorBB);
  CallInst::Create(BBRegisterFn,
                   {NewGVCasted, ConstantInt::get(IntPtrTy, NewAllocSize)}, "",
                   CtorBB);

  // If the original global variable is externally visible, replace it with an
  // alias that points to the original data in the new global variable
  if (!OrigGV->hasPrivateLinkage()) {
    auto *Aliasee = ConstantExpr::getInBoundsGetElementPtr(
        NewGVTy, NewGV, ArrayRef<Constant *>({Zero, Zero}));
    auto *GVAlias = GlobalAlias::create(
        OrigTy, OrigGV->getType()->getPointerAddressSpace(),
        OrigGV->getLinkage(), OrigGV->getName(), Aliasee, Mod);
    GVAlias->takeName(OrigGV);
    GVAlias->copyAttributesFrom(OrigGV);
  }

  OrigGV->eraseFromParent();
  return NewGV;
}

void GlobalVarTag::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<DefSiteIdentify>();
}

bool GlobalVarTag::runOnModule(Module &M) {
  this->Mod = &M;
  this->Ctx = &M.getContext();
  this->DL = &M.getDataLayout();

  this->TagTy = Type::getIntNTy(*Ctx, kNumTagBits);
  this->IntPtrTy = DL->getIntPtrType(*Ctx);
  this->BBRegisterFn =
      M.getOrInsertFunction("__bb_register", Type::getVoidTy(*Ctx),
                            Type::getInt8PtrTy(*Ctx), IntPtrTy);

  const auto &DefSites = getAnalysis<DefSiteIdentify>().getDefSites();

  if (DefSites.empty()) {
    return false;
  }

  // Create a constructor to register the tagged globals in the baggy bounds
  // table
  auto *GlobalCtorTy =
      FunctionType::get(Type::getVoidTy(*Ctx), /*isVarArg=*/false);
  auto *GlobalCtorF = Function::Create(
      GlobalCtorTy, GlobalValue::InternalLinkage, "fuzzalloc.ctor", *Mod);
  appendToGlobalCtors(*Mod, GlobalCtorF, /*Priority=*/0);

  auto *CtorEntryBB = BasicBlock::Create(*Ctx, "entry", GlobalCtorF);

  for (auto *Def : DefSites) {
    if (auto *GV = dyn_cast<GlobalVariable>(Def)) {
      tagGlobalVariable(GV, CtorEntryBB);
      NumTaggedGVs++;
    }
  }

  ReturnInst::Create(*Ctx, CtorEntryBB);

  success_stream() << "Num. tagged global variables: " << NumTaggedGVs << '\n';
  success_stream() << "Num. heapified global variables: " << NumHeapifiedGVs
                   << '\n';

  return true;
}

//
// Pass registration
//

static RegisterPass<GlobalVarTag> X(DEBUG_TYPE, "Tag global variables", false,
                                    false);

static void registerGlobalVarTagPass(const PassManagerBuilder &,
                                     legacy::PassManagerBase &PM) {
  PM.add(new GlobalVarTag());
}

static RegisterStandardPasses
    RegisterGlobalVarTagPass(PassManagerBuilder::EP_OptimizerLast,
                             registerGlobalVarTagPass);

static RegisterStandardPasses
    RegisterGlobalVarTagPass0(PassManagerBuilder::EP_EnabledOnOptLevel0,
                              registerGlobalVarTagPass);
