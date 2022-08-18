//===-- DebugUseSite.cpp - Instrument use sites -----------------*- C++ -*-===//
///
/// \file
/// Instrument use sites using a debug function
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
#include "fuzzalloc/Runtime/BaggyBounds.h"

using namespace llvm;

#define DEBUG_TYPE "fuzzalloc-dbg-use-site"

namespace {
static unsigned NumInstrumentedWrites = 0;
static unsigned NumInstrumentedReads = 0;
} // anonymous namespace

/// Instrument use sites
class DebugUseSite : public ModulePass {
public:
  static char ID;
  DebugUseSite() : ModulePass(ID) {}

  virtual void getAnalysisUsage(AnalysisUsage &) const override;
  virtual bool runOnModule(Module &) override;

private:
  void doInstrument(InterestingMemoryOperand *);

  Module *Mod;
  LLVMContext *Ctx;
  const DataLayout *DL;

  FunctionCallee BBDebugUseFn;

  PointerType *Int8PtrTy;
  IntegerType *IntPtrTy;
};

char DebugUseSite::ID = 0;

void DebugUseSite::doInstrument(InterestingMemoryOperand *Op) {
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

  auto *PtrCast = IRB.CreatePointerCast(Ptr, Int8PtrTy);
  auto *PtrElemTy = Ptr->getType()->getPointerElementType();
  auto *Size = ConstantInt::get(IntPtrTy, DL->getTypeStoreSize(PtrElemTy));
  IRB.CreateCall(BBDebugUseFn, {PtrCast, Size});
}

void DebugUseSite::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<UseSiteIdentify>();
}

bool DebugUseSite::runOnModule(Module &M) {
  bool Changed = false;

  // Initialize stuff
  this->Mod = &M;
  this->Ctx = &M.getContext();
  this->DL = &M.getDataLayout();

  this->IntPtrTy = DL->getIntPtrType(*Ctx);
  this->Int8PtrTy = Type::getInt8PtrTy(*Ctx);

  this->BBDebugUseFn = Mod->getOrInsertFunction(
      "__bb_dbg_use", Type::getVoidTy(*Ctx), Int8PtrTy, IntPtrTy);

  for (auto &F : M) {
    if (F.isDeclaration() || F.getName().startswith("fuzzalloc.")) {
      continue;
    }

    auto *UseSiteOps = getAnalysis<UseSiteIdentify>().getUseSites(F);
    if (!UseSiteOps || UseSiteOps->empty()) {
      continue;
    }

    for (auto &Op : *UseSiteOps) {
      doInstrument(&Op);
    }
    Changed = true;
  }

  return Changed;
}

//
// Pass registration
//

static RegisterPass<DebugUseSite> X(DEBUG_TYPE, "Instrument use sites (debug)",
                                    false, false);

static void registerDebugUseSitePass(const PassManagerBuilder &,
                                     legacy::PassManagerBase &PM) {
  PM.add(new DebugUseSite());
}

static RegisterStandardPasses
    RegisterDebugUseSitePass(PassManagerBuilder::EP_OptimizerLast,
                             registerDebugUseSitePass);

static RegisterStandardPasses
    RegisterDebugUseSitePass0(PassManagerBuilder::EP_EnabledOnOptLevel0,
                              registerDebugUseSitePass);
