//===-- DebugUseSite.cpp - Instrument use sites -----------------*- C++ -*-===//
///
/// \file
/// Instrument use sites using a debug function
///
//===----------------------------------------------------------------------===//

#include <llvm/ADT/Statistic.h>
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

#define DEBUG_TYPE "fuzzalloc-dbg-use-site"

STATISTIC(NumInstrumentedWrites, "Number of instrumented writes");
STATISTIC(NumInstrumentedReads, "Number of instrumented reads");

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

  FunctionCallee BBLookupFn;
  FunctionCallee BBDebugUseFn;

  IntegerType *IntPtrTy;
  IntegerType *TagTy;
  ConstantInt *DefaultTag;
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
  Inst->setMetadata(Mod->getMDKindID(kFuzzallocInstrumentedDerefMD),
                    MDNode::get(*Ctx, None));

  IRBuilder<> IRB(Inst);

  // Get the def site tag
  auto *Base = IRB.CreateAlloca(IntPtrTy, /*ArraySize=*/nullptr, "def.base");
  auto *PtrCast = IRB.CreatePointerCast(Ptr, IRB.getInt8PtrTy());
  auto *Tag = IRB.CreateCall(BBLookupFn, {PtrCast, Base}, "tag");

  // Compute the use site offset (the same size as the tag). This is just the
  // difference between the pointer and the previously-computed base address
  auto *P = IRB.CreatePtrToInt(Ptr, IntPtrTy);
  auto *BaseLoad = IRB.CreateLoad(Base);
  BaseLoad->setMetadata(Mod->getMDKindID(kNoSanitizeMD),
                        MDNode::get(*Ctx, None));

  // If the tag is just the default tag, then the subtraction will produce an
  // invalid result. So just use zero instead
  auto *Offset = IRB.CreateSelect(
      IRB.CreateICmpEQ(Tag, DefaultTag), Constant::getNullValue(IntPtrTy),
      IRB.CreateSub(P, BaseLoad, Ptr->getName() + ".offset"));

  IRB.CreateCall(BBDebugUseFn, {Tag, Offset});
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

  this->TagTy = Type::getIntNTy(*Ctx, kNumTagBits);
  this->IntPtrTy = DL->getIntPtrType(*Ctx);
  this->DefaultTag = ConstantInt::get(TagTy, kFuzzallocDefaultTag);

  {
    auto *Int8PtrTy = Type::getInt8PtrTy(*Ctx);
    this->BBLookupFn = Mod->getOrInsertFunction("__bb_lookup", TagTy, Int8PtrTy,
                                                IntPtrTy->getPointerTo());
    this->BBDebugUseFn = Mod->getOrInsertFunction(
        "__bb_dbg_use", Type::getVoidTy(*Ctx), TagTy, IntPtrTy);
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
