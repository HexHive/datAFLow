//===-- AFLUseSite.cpp - Instrument use sites -------------------*- C++ -*-===//
///
/// \file
/// Instrument use sites using an AFL-style bitmap
///
//===----------------------------------------------------------------------===//

#include <llvm/ADT/Statistic.h>
#include <llvm/Analysis/MemoryBuiltins.h>
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

STATISTIC(NumInstrumentedWrites, "Number of instrumented writes");
STATISTIC(NumInstrumentedReads, "Number of instrumented reads");

namespace {
static cl::opt<bool> ClUseOffset("fuzzalloc-use-offset",
                                 cl::desc("Capture offsets in the use site"),
                                 cl::Hidden, cl::init(true));
static cl::opt<bool> ClUseBBLookupFunc(
    "fuzzalloc-use-lookup-func",
    cl::desc("Use __bb_lookup function, rather than explicitly generating IR"),
    cl::Hidden, cl::init(false));
} // anonymous namespace

/// Instrument use sites
class AFLUseSite : public ModulePass {
public:
  static char ID;
  AFLUseSite() : ModulePass(ID) {}

  virtual void getAnalysisUsage(AnalysisUsage &) const override;
  virtual bool runOnModule(Module &) override;

private:
  Value *getDefSite(Value *, IRBuilder<> &);
  void doInstrument(InterestingMemoryOperand *, ObjectSizeOffsetEvaluator *);

  Module *Mod;
  LLVMContext *Ctx;
  const DataLayout *DL;

  FunctionCallee ReadPCAsm;
  FunctionCallee BBLookup;
  GlobalVariable *AFLMapPtr;
  GlobalVariable *BaggyBoundsPtr;

  IntegerType *TagTy;
  ConstantInt *DefaultTag;
};

char AFLUseSite::ID = 0;

Value *AFLUseSite::getDefSite(Value *Ptr, IRBuilder<> &IRB) {
  if (BBLookup) {
    auto *Cast = IRB.CreatePointerCast(Ptr, IRB.getInt8PtrTy());
    return IRB.CreateCall(BBLookup, {Cast}, "def_lookup");
  } else {
    auto IntPtrTy = IRB.getIntPtrTy(*DL);
    auto *One = ConstantInt::get(IntPtrTy, 1);
    auto *TagSize = ConstantInt::get(IntPtrTy, kMetaSize);

    auto *P = IRB.CreatePtrToInt(Ptr, IntPtrTy);
    auto *Index = IRB.CreateLShr(P, static_cast<uint64_t>(kSlotSizeLog2));

    auto *BaggyBoundsTable = IRB.CreateLoad(BaggyBoundsPtr);
    BaggyBoundsTable->setMetadata(Mod->getMDKindID(kNoSanitizeMD),
                                  MDNode::get(*Ctx, None));

    auto *BaggyBoundsIdx = IRB.CreateGEP(BaggyBoundsTable, Index);
    auto *BaggyBoundsIdxLoad = IRB.CreateLoad(BaggyBoundsIdx);
    BaggyBoundsIdxLoad->setMetadata(Mod->getMDKindID(kNoSanitizeMD),
                                    MDNode::get(*Ctx, None));

    auto *E = IRB.CreateZExt(BaggyBoundsIdxLoad, IntPtrTy);

    auto *AllocSize = IRB.CreateShl(One, E);
    auto *Base = IRB.CreateAnd(P, IRB.CreateNeg(IRB.CreateSub(AllocSize, One)));

    auto *TagAddr = IRB.CreateSub(IRB.CreateAdd(Base, AllocSize), TagSize);
    return IRB.CreateSelect(
        IRB.CreateICmpEQ(E, ConstantInt::getNullValue(IntPtrTy)),
        ConstantInt::getNullValue(TagTy), IRB.CreateLoad(TagAddr));

    assert(false && "Not yet implemented");
  }
}

void AFLUseSite::doInstrument(InterestingMemoryOperand *Op,
                              ObjectSizeOffsetEvaluator *ObjSizeEval) {
  if (Op->IsWrite) {
    NumInstrumentedWrites++;
  } else {
    NumInstrumentedReads++;
  }

  auto *Inst = Op->getInsn();
  auto *Ptr = Op->getPtr();

  LLVM_DEBUG(dbgs() << "Instrumenting " << *Inst << " (ptr=" << *Ptr << ")\n");

  // This metadata can be used by the static pointer analysis
  Inst->setMetadata(Mod->getMDKindID(kFuzzallocInstrumentedDerefMD),
                    MDNode::get(*Ctx, None));

  // Get the def site
  IRBuilder<> IRB(Inst);
  auto *DefSite = getDefSite(Ptr, IRB);

  // Compute the use site offset (the same size as the tag). If this cannot be
  // determined statically, build code that can determine it dynamically
  auto *UseOffset = [&]() -> Value * {
    auto *Zero = Constant::getNullValue(TagTy);
    if (ObjSizeEval) {
      auto SizeOffset = ObjSizeEval->compute(Ptr);
      return SizeOffset == ObjSizeEval->unknown()
                 ? Zero
                 : IRB.CreateIntCast(SizeOffset.second, TagTy,
                                     /*isSigned=*/true);
    }
    return Zero;
  }();
  UseOffset->setName(Ptr->getName() + ".offset");

  // Use the PC as the use site identifier
  auto *UseSite =
      IRB.CreateIntCast(IRB.CreateCall(ReadPCAsm), TagTy,
                        /*isSigned=*/false, Ptr->getName() + ".use_site");

  // Incorporate the memory access offset into the use site
  UseSite = IRB.CreateAdd(UseSite, UseOffset);

  // Load the AFL bitmap
  auto *AFLMap = IRB.CreateLoad(AFLMapPtr);

  // Hash the allocation site and use site to index into the bitmap
  //
  // zext is necessary otherwise we end up using signed indices
  //
  // Hash algorithm: ((31 * (def_site - DEFAULT_TAG)) ^ use_site) - use_site
  auto *HashMul = ConstantInt::get(TagTy, 31);
  auto *Hash = IRB.CreateSub(
      IRB.CreateXor(IRB.CreateMul(HashMul, IRB.CreateSub(DefSite, DefaultTag)),
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
  AU.addRequired<TargetLibraryInfoWrapperPass>();
  AU.addRequired<UseSiteIdentify>();
}

bool AFLUseSite::runOnModule(Module &M) {
  // Initialize stuff
  this->Mod = &M;
  this->Ctx = &M.getContext();
  this->DL = &M.getDataLayout();

  auto *Int8PtrTy = Type::getInt8PtrTy(*Ctx);

  {
    auto *ReadPCAsmTy =
        FunctionType::get(Type::getInt64Ty(*Ctx), /*isVarArg=*/false);
    this->ReadPCAsm = FunctionCallee(
        ReadPCAsmTy, InlineAsm::get(ReadPCAsmTy, "leaq (%rip), $0",
                                    /*Constraints=*/"=r",
                                    /*hasSideEffects=*/false));
  }

  this->TagTy = Type::getIntNTy(*Ctx, kNumTagBits);
  this->DefaultTag = ConstantInt::get(TagTy, kFuzzallocDefaultTag);

  this->AFLMapPtr = new GlobalVariable(
      *Mod, Int8PtrTy, /*isConstant=*/false, GlobalValue::ExternalLinkage,
      /*Initializer=*/nullptr, "__afl_area_ptr");
  this->BaggyBoundsPtr = new GlobalVariable(
      *Mod, Int8PtrTy, /*isConstant=*/false, GlobalValue::ExternalLinkage,
      /*Initializer=*/nullptr, "__baggy_bounds_table");

  this->BBLookup =
      ClUseBBLookupFunc
          ? Mod->getOrInsertFunction("__bb_lookup", TagTy, Int8PtrTy)
          : nullptr;

  for (auto &F : M) {
    if (F.isDeclaration()) {
      continue;
    }

    const auto &TLI = getAnalysis<TargetLibraryInfoWrapperPass>().getTLI(F);
    auto &UseSiteOps = getAnalysis<UseSiteIdentify>(F).getUseSites();

    if (UseSiteOps.empty()) {
      return false;
    }

    auto ObjSizeEval = [&]() -> ObjectSizeOffsetEvaluator * {
      if (ClUseOffset) {
        ObjectSizeOpts EvalOpts;
        EvalOpts.RoundToAlign = true;
        return new ObjectSizeOffsetEvaluator(*DL, &TLI, *Ctx, EvalOpts);
      }
      return nullptr;
    }();

    for (auto &Op : UseSiteOps) {
      doInstrument(&Op, ObjSizeEval);
    }

    if (ObjSizeEval) {
      delete ObjSizeEval;
    }
  }

  return true;
}

//
// Pass registration
//

static RegisterPass<AFLUseSite> X(DEBUG_TYPE, "Instrument use sites", false,
                                  false);

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
