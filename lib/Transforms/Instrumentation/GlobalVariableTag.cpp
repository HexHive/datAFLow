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
#include <llvm/Support/CommandLine.h>
#include <llvm/Transforms/IPO/PassManagerBuilder.h>
#include <llvm/Transforms/Utils/ModuleUtils.h>

#include "fuzzalloc/Analysis/DefSiteIdentify.h"
#include "fuzzalloc/Metadata.h"
#include "fuzzalloc/Runtime/BaggyBounds.h"
#include "fuzzalloc/Streams.h"
#include "fuzzalloc/Transforms/Utils.h"

#include "Utils.h"

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
  GlobalVariable *tag(GlobalVariable *, Constant *, BasicBlock *, BasicBlock *);

  Module *Mod;
  LLVMContext *Ctx;
  const DataLayout *DL;

  IntegerType *IntPtrTy;
  PointerType *Int8PtrTy;

  FunctionCallee BBRegisterFn;
  FunctionCallee BBDeregisterFn;
};

char GlobalVarTag::ID = 0;

GlobalVariable *GlobalVarTag::tag(GlobalVariable *OrigGV, Constant *Metadata,
                                  BasicBlock *CtorBB, BasicBlock *DtorBB) {
  auto *OrigTy = OrigGV->getValueType();
  auto *MetaTy = Metadata->getType();
  auto MetaSize = DL->getTypeAllocSize(MetaTy);
  auto NewAllocSize = getTaggedVarSize(DL->getTypeAllocSize(OrigTy), MetaSize);

  auto OrigSize = DL->getTypeAllocSize(OrigTy);
  auto PaddingSize = NewAllocSize - OrigSize - MetaSize;
  auto *PaddingTy = ArrayType::get(Type::getInt8Ty(*Ctx), PaddingSize);

  auto *NewGVTy =
      StructType::get(*Ctx, {OrigTy, PaddingTy, MetaTy}, /*isPacked=*/true);
  auto *NewInit = ConstantStruct::get(
      NewGVTy, {OrigGV->hasInitializer() ? OrigGV->getInitializer()
                                         : ConstantAggregateZero::get(OrigTy),
                ConstantAggregateZero::get(PaddingTy), Metadata});

  // Create a tagged version of the global variable (only visible in this
  // module)
  auto *NewGV = new GlobalVariable(
      *Mod, NewGVTy, OrigGV->isConstant(), OrigGV->getLinkage(), NewInit,
      OrigGV->hasName() ? OrigGV->getName().str() + ".tagged" : "", OrigGV,
      OrigGV->getThreadLocalMode(), OrigGV->getType()->getPointerAddressSpace(),
      OrigGV->isExternallyInitialized());
  NewGV->copyAttributesFrom(OrigGV);
  NewGV->setMetadata(Mod->getMDKindID(kFuzzallocTagVarMD),
                     MDNode::get(*Ctx, None));
  NewGV->setAlignment(MaybeAlign(NewAllocSize));
  NewGV->setLinkage(GlobalValue::PrivateLinkage);
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
  auto *Zero = ConstantInt::getNullValue(IntegerType::getInt32Ty(*Ctx));
  SmallVector<Use *, 16> Uses(
      map_range(OrigGV->uses(), [](Use &U) { return &U; }));
  for (auto *U : Uses) {
    auto *User = U->getUser();

    if (isa<Instruction>(User)) {
      auto *InsertPt = phiSafeInsertPt(U);
      auto *GEP = GetElementPtrInst::CreateInBounds(NewGVTy, NewGV,
                                                    {Zero, Zero}, "", InsertPt);
      phiSafeReplaceUses(U, GEP);
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
  {
    auto *NewGVCasted = new BitCastInst(NewGV, Int8PtrTy, "", CtorBB);
    CallInst::Create(BBRegisterFn,
                     {NewGVCasted, ConstantInt::get(IntPtrTy, NewAllocSize)},
                     "", CtorBB);
  }

  // Deregister the allocation in the baggy bounds table
  {
    auto *NewGVCasted = new BitCastInst(NewGV, Int8PtrTy, "", DtorBB);
    CallInst::Create(BBDeregisterFn, {NewGVCasted}, "", DtorBB);
  }

  // If the original global variable is externally visible, replace it with an
  // alias that points to the original data in the new global variable
  if (!OrigGV->isDeclaration() && !OrigGV->hasPrivateLinkage()) {
    auto *Aliasee = ConstantExpr::getInBoundsGetElementPtr(
        NewGVTy, NewGV, ArrayRef<Constant *>({Zero, Zero}));
    auto *GVAlias = GlobalAlias::create(
        OrigTy, OrigGV->getType()->getPointerAddressSpace(),
        OrigGV->getLinkage(), OrigGV->getName(), Aliasee, Mod);
    GVAlias->takeName(OrigGV);
    GVAlias->setVisibility(OrigGV->getVisibility());
    GVAlias->setUnnamedAddr(OrigGV->getUnnamedAddr());
    GVAlias->setThreadLocalMode(OrigGV->getThreadLocalMode());
    GVAlias->setDLLStorageClass(OrigGV->getDLLStorageClass());
    GVAlias->setDSOLocal(OrigGV->isDSOLocal());
    GVAlias->setPartition(OrigGV->getPartition());
  } else {
    NewGV->takeName(OrigGV);
  }

  OrigGV->eraseFromParent();
  NumTaggedGVs++;
  return NewGV;
}

void GlobalVarTag::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<DefSiteIdentify>();
  AU.addRequired<VariableRecovery>();
}

bool GlobalVarTag::runOnModule(Module &M) {
  this->Mod = &M;
  this->Ctx = &M.getContext();
  this->DL = &M.getDataLayout();

  this->IntPtrTy = DL->getIntPtrType(*Ctx);
  this->Int8PtrTy = Type::getInt8PtrTy(*Ctx);

  auto *TagTy = Type::getIntNTy(*Ctx, kNumTagBits);

  {
    auto *VoidTy = Type::getVoidTy(*Ctx);

    this->BBRegisterFn =
        M.getOrInsertFunction("__bb_register", VoidTy, Int8PtrTy, IntPtrTy);
    assert(isa_and_nonnull<Function>(BBRegisterFn.getCallee()));
    cast<Function>(BBRegisterFn.getCallee())->setDoesNotThrow();
    cast<Function>(BBRegisterFn.getCallee())
        ->addParamAttr(0, Attribute::NonNull);

    this->BBDeregisterFn =
        M.getOrInsertFunction("__bb_deregister", VoidTy, Int8PtrTy);
    assert(isa_and_nonnull<Function>(BBDeregisterFn.getCallee()));
    cast<Function>(BBDeregisterFn.getCallee())->setDoesNotThrow();
    cast<Function>(BBDeregisterFn.getCallee())
        ->addParamAttr(0, Attribute::NonNull);
  }

  const auto &DefSites = getAnalysis<DefSiteIdentify>().getDefSites();
  const auto &SrcVars = getAnalysis<VariableRecovery>().getVariables();

  if (DefSites.empty()) {
    return false;
  }

  // Create a constructor to register the tagged globals in the baggy bounds
  // table
  auto *GlobalCtorTy =
      FunctionType::get(Type::getVoidTy(*Ctx), /*isVarArg=*/false);
  auto *GlobalCtorF = Function::Create(
      GlobalCtorTy, GlobalValue::InternalLinkage, "fuzzalloc.ctor", *Mod);
  auto *CtorEntryBB = BasicBlock::Create(*Ctx, "entry", GlobalCtorF);
  appendToGlobalCtors(*Mod, GlobalCtorF, /*Priority=*/0);

  // Create a destructor to deregister the tagged globals in the baggy bounds
  // table
  auto *GlobalDtorF = Function::Create(
      GlobalCtorTy, GlobalValue::InternalLinkage, "fuzzalloc.dtor", *Mod);
  auto *DtorEntryBB = BasicBlock::Create(*Ctx, "entry", GlobalDtorF);
  appendToGlobalDtors(*Mod, GlobalDtorF, /*Priority=*/0);

  const auto &GVDefs =
      map_range(make_filter_range(
                    DefSites, [](Value *V) { return isa<GlobalVariable>(V); }),
                [](Value *V) { return cast<GlobalVariable>(V); });

  for (auto *GV : GVDefs) {
    if (ClInstType == InstType::InstAFL) {
      auto *Metadata = generateTag(TagTy);
      tag(GV, Metadata, CtorEntryBB, DtorEntryBB);
    } else if (ClInstType == InstType::InstTrace) {
      const auto &SrcVar = SrcVars.lookup(GV);
      auto *Metadata = tracerCreateDef(SrcVar, Mod);
      tag(GV, Metadata, CtorEntryBB, DtorEntryBB);
    } else {
      GV->setMetadata(Mod->getMDKindID(kFuzzallocTagVarMD),
                      MDNode::get(*Ctx, None));
    }
  }

  ReturnInst::Create(*Ctx, CtorEntryBB);
  ReturnInst::Create(*Ctx, DtorEntryBB);

  success_stream() << "[" << M.getName()
                   << "] Num. tagged global variables: " << NumTaggedGVs
                   << '\n';
  success_stream() << "[" << M.getName()
                   << "] Num. heapified global variables: " << NumHeapifiedGVs
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
