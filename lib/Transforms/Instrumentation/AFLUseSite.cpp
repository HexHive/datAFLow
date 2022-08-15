//===-- AFLUseSite.cpp - Instrument use sites -------------------*- C++ -*-===//
///
/// \file
/// Instrument use sites using an AFL-style bitmap
///
//===----------------------------------------------------------------------===//

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/InlineAsm.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/Pass.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/Debug.h>
#include <llvm/Transforms/IPO/PassManagerBuilder.h>

#include "fuzzalloc/Analysis/UseSiteIdentify.h"
#include "fuzzalloc/Metadata.h"
#include "fuzzalloc/baggy_bounds.h"
#include "fuzzalloc/fuzzalloc.h"

using namespace llvm;

#define DEBUG_TYPE "fuzzalloc-afl-use-site"

namespace {
//
// Command-line options
//

static cl::opt<bool> ClUseOffset("fuzzalloc-use-offset",
                                 cl::desc("Capture offsets in the use site"),
                                 cl::Hidden, cl::init(true));

//
// Global variables
//

static unsigned NumInstrumentedReads = 0;
static unsigned NumInstrumentedWrites = 0;
} // anonymous namespace

/// Instrument use sites
class AFLUseSite : public ModulePass {
public:
  static char ID;
  AFLUseSite() : ModulePass(ID) {}

  virtual void getAnalysisUsage(AnalysisUsage &) const override;
  virtual bool runOnModule(Module &) override;

private:
  void doInstrument(InterestingMemoryOperand *);

  Module *Mod;
  LLVMContext *Ctx;
  const DataLayout *DL;

  FunctionCallee ReadPCAsmFn;
  FunctionCallee BBLookupFn;

  GlobalVariable *AFLMapPtr;
  GlobalVariable *BaggyBoundsPtr;

  IntegerType *IntPtrTy;
  IntegerType *TagTy;
  ConstantInt *DefaultTag;
};

char AFLUseSite::ID = 0;

void AFLUseSite::doInstrument(InterestingMemoryOperand *Op) {
  if (Op->IsWrite) {
    NumInstrumentedWrites++;
  } else {
    NumInstrumentedReads++;
  }

  auto *Inst = Op->getInsn();
  auto *Ptr = Op->getPtr();

  LLVM_DEBUG(dbgs() << "Instrumenting " << *Inst << " (ptr=" << *Ptr << ")\n");

  // This metadata can be used by the static pointer analysis
  Inst->setMetadata(Mod->getMDKindID(kFuzzallocInstrumentedUseSiteMD),
                    MDNode::get(*Ctx, None));

  IRBuilder<> IRB(Inst);

  // Get the def site tag
  auto *Base = IRB.CreateAlloca(IntPtrTy, /*ArraySize=*/nullptr, "def.base");
  auto *PtrCast = IRB.CreatePointerCast(Ptr, IRB.getInt8PtrTy());
  auto *Tag = IRB.CreateCall(BBLookupFn, {PtrCast, Base}, "tag");

  // Compute the use site offset (the same size as the tag). This is just the
  // difference between the pointer and the previously-computed base address
  auto *Offset = [&]() -> Value * {
    if (ClUseOffset) {
      auto *P = IRB.CreatePtrToInt(Ptr, IntPtrTy);
      auto *BaseLoad = IRB.CreateLoad(Base);
      BaseLoad->setMetadata(Mod->getMDKindID(kNoSanitizeMD),
                            MDNode::get(*Ctx, None));

      // If the tag is just the default tag, then the subtraction will produce
      // an invalid result. So just use zero instead
      auto *Offset = IRB.CreateSelect(
          IRB.CreateICmpEQ(Tag, DefaultTag), Constant::getNullValue(IntPtrTy),
          IRB.CreateSub(P, BaseLoad, Ptr->getName() + ".offset"));
      return IRB.CreateSExtOrTrunc(Offset, TagTy);
    } else {
      return Constant::getNullValue(TagTy);
    }
  }();
  Offset->setName(Ptr->getName() + ".offset");

  // Use the PC as the use site identifier
  auto *UseSite =
      IRB.CreateIntCast(IRB.CreateCall(ReadPCAsmFn), TagTy,
                        /*isSigned=*/false, Ptr->getName() + ".use");

  // Incorporate the memory access offset into the use site
  UseSite = IRB.CreateAdd(UseSite, Offset);

  // Load the AFL bitmap
  auto *AFLMap = IRB.CreateLoad(AFLMapPtr);

  // Hash the allocation site and use site to index into the bitmap
  //
  // zext is necessary otherwise we end up using signed indices
  //
  // Hash algorithm: ((31 * (def_site - DEFAULT_TAG)) ^ use_site) - use_site
  auto *HashMul = ConstantInt::get(TagTy, 31);
  auto *Hash = IRB.CreateSub(
      IRB.CreateXor(IRB.CreateMul(HashMul, IRB.CreateSub(Tag, DefaultTag)),
                    UseSite),
      UseSite, Ptr->getName() + ".def_use_hash");
  auto *AFLMapIdx = IRB.CreateGEP(
      AFLMap, IRB.CreateZExt(Hash, IRB.getInt32Ty()), "__afl_area_ptr_idx");

  // Update the bitmap only if the def site is not the default tag
  auto *CounterLoad = IRB.CreateLoad(AFLMapIdx);
  auto *Incr = IRB.CreateAdd(CounterLoad, IRB.getInt8(1));
  auto *CounterStore = IRB.CreateStore(Incr, AFLMapIdx);

  AFLMap->setMetadata(Mod->getMDKindID(kNoSanitizeMD), MDNode::get(*Ctx, None));
  CounterLoad->setMetadata(Mod->getMDKindID(kNoSanitizeMD),
                           MDNode::get(*Ctx, None));
  CounterStore->setMetadata(Mod->getMDKindID(kNoSanitizeMD),
                            MDNode::get(*Ctx, None));
}

void AFLUseSite::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<UseSiteIdentify>();
}

bool AFLUseSite::runOnModule(Module &M) {
  bool Changed = false;

  // Initialize stuff
  this->Mod = &M;
  this->Ctx = &M.getContext();
  this->DL = &M.getDataLayout();

  {
    auto *ReadPCAsmTy =
        FunctionType::get(Type::getInt64Ty(*Ctx), /*isVarArg=*/false);
    this->ReadPCAsmFn = FunctionCallee(
        ReadPCAsmTy, InlineAsm::get(ReadPCAsmTy, "leaq (%rip), $0",
                                    /*Constraints=*/"=r",
                                    /*hasSideEffects=*/false));
  }

  this->TagTy = Type::getIntNTy(*Ctx, kNumTagBits);
  this->IntPtrTy = DL->getIntPtrType(*Ctx);
  this->DefaultTag = ConstantInt::get(TagTy, kFuzzallocDefaultTag);

  {
    auto *Int8PtrTy = Type::getInt8PtrTy(*Ctx);
    this->AFLMapPtr = new GlobalVariable(
        *Mod, Int8PtrTy, /*isConstant=*/false, GlobalValue::ExternalLinkage,
        /*Initializer=*/nullptr, "__afl_area_ptr");
    this->BaggyBoundsPtr = new GlobalVariable(
        *Mod, Int8PtrTy, /*isConstant=*/false, GlobalValue::ExternalLinkage,
        /*Initializer=*/nullptr, "__baggy_bounds_table");

    this->BBLookupFn = Mod->getOrInsertFunction("__bb_lookup", TagTy, Int8PtrTy,
                                                IntPtrTy->getPointerTo());
  }

  for (auto &F : M) {
    if (F.isDeclaration() || F.getName().startswith("fuzzalloc.")) {
      continue;
    }

    auto &UseSiteOps = getAnalysis<UseSiteIdentify>(F).getUseSites();
    if (UseSiteOps.empty()) {
      continue;
    }

    for (auto &Op : UseSiteOps) {
      doInstrument(&Op);
    }
    Changed = true;
  }

  return Changed;
}

//
// Pass registration
//

static RegisterPass<AFLUseSite> X(DEBUG_TYPE, "Instrument use sites (AFL)",
                                  false, false);

static void registerAFLUseSitePass(const PassManagerBuilder &,
                                   legacy::PassManagerBase &PM) {
  PM.add(new AFLUseSite());
}

static RegisterStandardPasses
    RegisterAFLUseSitePass(PassManagerBuilder::EP_OptimizerLast,
                           registerAFLUseSitePass);

static RegisterStandardPasses
    RegisterAFLUseSitePass0(PassManagerBuilder::EP_EnabledOnOptLevel0,
                            registerAFLUseSitePass);
