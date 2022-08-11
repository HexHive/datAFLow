//===-- AFLUseSiteInstrument.cpp - Instrument use sites ---------*- C++ -*-===//
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
#include "fuzzalloc/fuzzalloc.h"

using namespace llvm;

#define DEBUG_TYPE "fuzzalloc-afl-use-site-inst"

STATISTIC(NumInstrumentedWrites, "Number of instrumented writes");
STATISTIC(NumInstrumentedReads, "Number of instrumented reads");

namespace {
static cl::opt<bool> ClUseOffset("fuzzalloc-use-offset",
                                 cl::desc("Capture offsets in the use site"),
                                 cl::Hidden, cl::init(true));
} // anonymous namespace

/// Instrument use sites
class AFLUseSiteInstrument : public FunctionPass {
public:
  static char ID;
  AFLUseSiteInstrument() : FunctionPass(ID) {}

  virtual void getAnalysisUsage(AnalysisUsage &) const override;
  virtual bool runOnFunction(Function &) override;

private:
  Value *getDefSite(Value *, IRBuilder<> &) const;
  void doInstrument(InterestingMemoryOperand *, ObjectSizeOffsetEvaluator *);

  Module *Mod;
  LLVMContext *Ctx;
  const DataLayout *DL;

  FunctionCallee ReadPCAsm;
  GlobalVariable *AFLMapPtr;

  IntegerType *TagTy;
  ConstantInt *HashMul;
  ConstantInt *DefaultTag;
};

char AFLUseSiteInstrument::ID = 0;

Value *AFLUseSiteInstrument::getDefSite(Value *Ptr, IRBuilder<> &IRB) const {
  assert(false && "not yet implemented");
  return nullptr;
}

void AFLUseSiteInstrument::doInstrument(
    InterestingMemoryOperand *Op, ObjectSizeOffsetEvaluator *ObjSizeEval) {
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
    if (ObjSizeEval) {
      auto SizeOffset = ObjSizeEval->compute(Ptr);
      return IRB.CreateIntCast(SizeOffset.second, TagTy, /*isSigned=*/true);
    }
    return Constant::getNullValue(TagTy);
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

void AFLUseSiteInstrument::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<TargetLibraryInfoWrapperPass>();
  AU.addRequired<UseSiteIdentify>();
}

bool AFLUseSiteInstrument::runOnFunction(Function &F) {
  // Initialize stuff
  this->Mod = F.getParent();
  this->Ctx = &Mod->getContext();
  this->DL = &Mod->getDataLayout();

  auto *ReadPCAsmTy =
      FunctionType::get(Type::getInt64Ty(*Ctx), /*isVarArg=*/false);
  this->ReadPCAsm =
      FunctionCallee(ReadPCAsmTy, InlineAsm::get(ReadPCAsmTy, "leaq (%tip), $0",
                                                 /*Constraints=*/"=r",
                                                 /*hasSideEffects=*/false));
  this->AFLMapPtr = new GlobalVariable(
      *Mod, PointerType::getUnqual(Type::getInt8Ty(*Ctx)), /*isConstant=*/false,
      GlobalValue::ExternalLinkage, /*Initializer=*/nullptr, "__afl_area_ptr");

  this->TagTy = Type::getIntNTy(*Ctx, kNumTagBits);
  this->HashMul = ConstantInt::get(TagTy, 31);
  this->DefaultTag = ConstantInt::get(TagTy, kFuzzallocDefaultTag);

  const auto *TLI = &getAnalysis<TargetLibraryInfoWrapperPass>().getTLI(F);
  auto &UseSiteOps = getAnalysis<UseSiteIdentify>().getUseSites();

  if (UseSiteOps.empty()) {
    return false;
  }

  auto ObjSizeEval = [&]() -> ObjectSizeOffsetEvaluator * {
    if (ClUseOffset) {
      ObjectSizeOpts EvalOpts;
      EvalOpts.RoundToAlign = true;
      return new ObjectSizeOffsetEvaluator(*DL, TLI, *Ctx, EvalOpts);
    }
    return nullptr;
  }();

  for (auto &Op : UseSiteOps) {
    doInstrument(&Op, ObjSizeEval);
  }

  if (ObjSizeEval) {
    delete ObjSizeEval;
  }

  return true;
}

//
// Pass registration
//

static RegisterPass<AFLUseSiteInstrument> X(DEBUG_TYPE, "Instrument use sites",
                                            false, false);

static void registerUseSiteInstrumentPass(const PassManagerBuilder &,
                                          legacy::PassManagerBase &PM) {
  PM.add(new AFLUseSiteInstrument());
}

static RegisterStandardPasses
    RegisterUseSiteInstrumentPass(PassManagerBuilder::EP_OptimizerLast,
                                  registerUseSiteInstrumentPass);

static RegisterStandardPasses
    RegisterUseSiteInstrumentPass0(PassManagerBuilder::EP_EnabledOnOptLevel0,
                                   registerUseSiteInstrumentPass);
