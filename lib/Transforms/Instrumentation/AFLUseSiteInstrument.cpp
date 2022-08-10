//===-- AFLUseSiteInstrument.cpp - Instrument use sites ---------*- C++ -*-===//
///
/// \file
/// Instrument use sites using an AFL-style bitmap
///
//===----------------------------------------------------------------------===//

#include <llvm/ADT/Statistic.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/Pass.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/Debug.h>
#include <llvm/Transforms/IPO/PassManagerBuilder.h>

#include "fuzzalloc/Analysis/UseSiteIdentify.h"
#include "fuzzalloc/Metadata.h"

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
  void doInstrument(InterestingMemoryOperand *);

  Module *Mod;
  LLVMContext *Ctx;
  const DataLayout *DL;
};

char AFLUseSiteInstrument::ID = 0;

Value *AFLUseSiteInstrument::getDefSite(Value *Ptr, IRBuilder<> &IRB) const {
  return nullptr;
}

void AFLUseSiteInstrument::doInstrument(InterestingMemoryOperand *Op) {
  auto *Inst = Op->getInsn();
  auto *Ptr = Op->getPtr();
  IRBuilder<> IRB(Inst);

  if (Op->IsWrite) {
    NumInstrumentedWrites++;
  } else {
    NumInstrumentedReads++;
  }

  LLVM_DEBUG(dbgs() << "Instrumenting " << Inst << " (ptr=" << *Ptr << ")\n");

  // This metadata can be used by the static pointer analysis
  Inst->setMetadata(Mod->getMDKindID(kFuzzallocInstrumentedDerefMD),
                    MDNode::get(*Ctx, None));

  // Get the def site
  auto *DefSite = getDefSite(Ptr, IRB);

  // Get the use site offset. Default to zero if we can't determine the offset
  auto *UseSiteOffset = Constant::getNullValue(IRB.getInt64Ty());
  if (ClUseOffset) {
    auto *UseSiteGEP = getUseSiteGEP(Ptr, DL);
    if (UseSiteGEP) {
      UseSiteOffset = EmitGEPOffset(&IRB, DL, UseSiteGEP);
    }
  }
  UseSiteOffset->setName(Ptr->getName() + ".offset");
  auto *UseSiteOffsetInt64 =
      IRB.CreateSExtOrTrunc(UseSiteOffset, IRB.getInt64Ty());

  // Use the PC as the use site identifier
  auto *UseSite =
      IRB.CreateIntCast(IRB.CreateCall(this->ReadPCAsm), this->TagTy,
                        /*isSigned=*/false, Ptr->getName() + ".use_site");

  // Incorporate the memory access offset into the use site
  if (ClUseOffset) {
    UseSite = IRB.CreateAdd(UseSite,
                            IRB.CreateIntCast(UseSiteOffsetInt64, this->TagTy,
                                              /*isSigned=*/true));
  }

  // Load the AFL bitmap
  auto *AFLMap = IRB.CreateLoad(this->AFLMapPtr);

  // Hash the allocation site and use site to index into the bitmap
  //
  // zext is necessary otherwise we end up using signed indices
  //
  // Hash algorithm: ((31 * (def_site - DEFAULT_TAG)) ^ use_site) - use_site
  auto *Hash = IRB.CreateSub(
      IRB.CreateXor(IRB.CreateMul(this->HashMul,
                                  IRB.CreateSub(DefSite, this->DefaultTag)),
                    UseSite),
      UseSite, Ptr->getName() + ".def_use_hash");
  auto *AFLMapIdx = IRB.CreateGEP(
      AFLMap, IRB.CreateZExt(Hash, IRB.getInt32Ty()), "__afl_area_ptr_idx");

  // Update the bitmap only if the def site is not the default tag
  auto *CounterLoad = IRB.CreateLoad(AFLMapIdx);
  auto *Incr = IRB.CreateAdd(CounterLoad, this->AFLInc);
  auto *CounterStore = IRB.CreateStore(Incr, AFLMapIdx);

  AFLMap->setMetadata(Mod->getMDKindID(kNoSanitizeMD), MDNode::get(*Ctx, None));
  CounterLoad->setMetadata(Mod->getMDKindID(kNoSanitizeMD),
                           MDNode::get(*Ctx, None));
  CounterStore->setMetadata(Mod->getMDKindID(kNoSanitizeMD),
                            MDNode::get(*Ctx, None));
}

void AFLUseSiteInstrument::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<UseSiteIdentify>();
}

bool AFLUseSiteInstrument::runOnFunction(Function &F) {
  this->Mod = F.getParent();
  this->Ctx = &Mod->getContext();
  this->DL = &Mod->getDataLayout();

  auto &UseSiteOps = getAnalysis<UseSiteIdentify>().getUseSites();
  if (UseSiteOps.empty()) {
    return false;
  }

  for (auto &Op : UseSiteOps) {
    doInstrument(&Op);
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
