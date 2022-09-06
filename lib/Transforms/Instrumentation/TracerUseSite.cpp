//===-- TracerUseSite.cpp - Instrument use sites ----------------*- C++ -*-===//
///
/// \file
/// Instrument use sites using the def-use tracer
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
#include "fuzzalloc/Streams.h"

#include "TracerUtils.h"

using namespace llvm;

#define DEBUG_TYPE "fuzzalloc-tracer-use-site"

namespace {
static unsigned NumInstrumentedWrites = 0;
static unsigned NumInstrumentedReads = 0;
} // anonymous namespace

/// Instrument use sites
class TracerUseSite : public ModulePass {
public:
  static char ID;
  TracerUseSite() : ModulePass(ID) {}

  virtual void getAnalysisUsage(AnalysisUsage &) const override;
  virtual bool runOnModule(Module &) override;

private:
  void doInstrument(InterestingMemoryOperand *);

  Module *Mod;
  LLVMContext *Ctx;
  const DataLayout *DL;

  StructType *TracerSrcLocationTy;
  FunctionCallee TracerUseFn;

  PointerType *Int8PtrTy;
  IntegerType *IntPtrTy;
};

char TracerUseSite::ID = 0;

void TracerUseSite::doInstrument(InterestingMemoryOperand *Op) {
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

  assert(Inst->getNextNode());
  IRBuilder<> IRB(Inst->getNextNode());

  auto *PtrCast = IRB.CreatePointerCast(Ptr, Int8PtrTy);
  auto *PtrElemTy = Ptr->getType()->getPointerElementType();
  auto *Size = ConstantInt::get(IntPtrTy, DL->getTypeStoreSize(PtrElemTy));
  auto *UseLoc = IRB.CreatePointerCast(tracerCreateUse(Inst, Mod),
                                       TracerSrcLocationTy->getPointerTo());
  IRB.CreateCall(TracerUseFn, {PtrCast, Size, UseLoc});
}

void TracerUseSite::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<UseSiteIdentify>();
}

bool TracerUseSite::runOnModule(Module &M) {
  bool Changed = false;

  // Initialize stuff
  this->Mod = &M;
  this->Ctx = &M.getContext();
  this->DL = &M.getDataLayout();

  this->IntPtrTy = DL->getIntPtrType(*Ctx);
  this->Int8PtrTy = Type::getInt8PtrTy(*Ctx);

  this->TracerSrcLocationTy =
      StructType::create({Int8PtrTy, Int8PtrTy, IntPtrTy, IntPtrTy},
                         "fuzzalloc.SrcLocation", /*isPacked=*/true);
  this->TracerUseFn =
      Mod->getOrInsertFunction("__tracer_use", Type::getVoidTy(*Ctx), Int8PtrTy,
                               IntPtrTy, TracerSrcLocationTy->getPointerTo());

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

  success_stream() << "Num. instrumented reads: " << NumInstrumentedReads
                   << '\n';
  success_stream() << "Num. instrumented writes: " << NumInstrumentedWrites
                   << '\n';

  return Changed;
}

//
// Pass registration
//

static RegisterPass<TracerUseSite>
    X(DEBUG_TYPE, "Instrument use sites (tracer)", false, false);

static void registerTracerUseSitePass(const PassManagerBuilder &,
                                      legacy::PassManagerBase &PM) {
  PM.add(new TracerUseSite());
}

static RegisterStandardPasses
    RegisterTracerUseSitePass(PassManagerBuilder::EP_OptimizerLast,
                              registerTracerUseSitePass);

static RegisterStandardPasses
    RegisterTracerUseSitePass0(PassManagerBuilder::EP_EnabledOnOptLevel0,
                               registerTracerUseSitePass);
