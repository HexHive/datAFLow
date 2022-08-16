//===-- GlobalVarTag.cpp - Tag global variables -----------------*- C++ -*-===//
///
/// \file
/// Tag global variables
///
//===----------------------------------------------------------------------===//

#include <llvm/IR/Constants.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/Pass.h>
#include <llvm/Transforms/IPO/PassManagerBuilder.h>
#include <llvm/Transforms/Utils/ModuleUtils.h>

#include "fuzzalloc/Analysis/DefSiteIdentify.h"
#include "fuzzalloc/Analysis/VariableRecovery.h"
#include "fuzzalloc/Metadata.h"
#include "fuzzalloc/Runtime/BaggyBounds.h"
#include "fuzzalloc/Transforms/Utils.h"

using namespace llvm;

#define DEBUG_TYPE "fuzzalloc-tag-global-variable"

namespace {
static unsigned NumTaggedGVs = 0;
} // anonymous namespace

class GlobalVarTag : public ModulePass {
public:
  static char ID;
  GlobalVarTag() : ModulePass(ID) {}

  virtual void getAnalysisUsage(AnalysisUsage &) const override;
  virtual bool runOnModule(Module &) override;

private:
  ConstantInt *generateTag() const;
  size_t getTaggedVarSize(Type *) const;
  GlobalVariable *tagGlobalVariable(GlobalVariable *, BasicBlock *);

  Module *Mod;
  LLVMContext *Ctx;
  const DataLayout *DL;

  IntegerType *TagTy;
  IntegerType *IntPtrTy;
  FunctionCallee BBRegisterFn;
};

char GlobalVarTag::ID = 0;

ConstantInt *GlobalVarTag::generateTag() const {
  return ConstantInt::get(
      TagTy, static_cast<uint64_t>(RAND(kFuzzallocTagMin, kFuzzallocTagMax)));
}

size_t GlobalVarTag::getTaggedVarSize(Type *Ty) const {
  auto AdjustedSize = DL->getTypeAllocSize(Ty) + kMetaSize;
  if (AdjustedSize < kSlotSize) {
    AdjustedSize = kSlotSize;
  }
  return bb_nextPow2(AdjustedSize);
}

GlobalVariable *GlobalVarTag::tagGlobalVariable(GlobalVariable *OrigGV,
                                                BasicBlock *CtorBB) {
  auto *Zero = ConstantInt::getNullValue(IntegerType::getInt32Ty(*Ctx));
  auto *OrigTy = OrigGV->getValueType();
  auto OrigSize = DL->getTypeAllocSize(OrigTy);
  auto NewAllocSize = getTaggedVarSize(OrigTy);

  auto PaddingSize = NewAllocSize - OrigSize - kMetaSize;
  auto *PaddingTy = ArrayType::get(Type::getInt8Ty(*Ctx), PaddingSize);
  auto *Tag = generateTag();

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
      OrigGV->hasName() ? OrigGV->getName() + ".tagged" : "", OrigGV,
      OrigGV->getThreadLocalMode(), OrigGV->getType()->getPointerAddressSpace(),
      OrigGV->isExternallyInitialized());
  NewGV->copyAttributesFrom(OrigGV);
  NewGV->setMetadata(Mod->getMDKindID(kFuzzallocTaggVarMD),
                     MDNode::get(*Ctx, None));
  NewGV->setAlignment(MaybeAlign(NewAllocSize));

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
    } else if (auto *CE = dyn_cast<ConstantExpr>(User)) {
      auto *GEP = ConstantExpr::getInBoundsGetElementPtr(
          NewGVTy, NewGV, ArrayRef<Constant *>({Zero, Zero}));
      CE->handleOperandChange(OrigGV, GEP);
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
  AU.addRequired<VariableRecovery>();
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

  const auto &Vars = getAnalysis<VariableRecovery>().getVariables();
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
