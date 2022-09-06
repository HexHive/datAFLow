//===-- AFLUseSite.cpp - Instrument use sites -------------------*- C++ -*-===//
///
/// \file
/// Instrument use sites using an AFL-style bitmap
///
//===----------------------------------------------------------------------===//

#include <llvm/IR/IRBuilder.h>
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

#include "TagUtils.h"

using namespace llvm;

#define DEBUG_TYPE "fuzzalloc-afl-use-site"

namespace {
enum UseSiteCapture {
  UseOnly,
  UseWithOffset,
  UseWithValue,
};

//
// Command-line options
//

static cl::opt<UseSiteCapture> ClUseCapture(
    cl::desc("What to capture at use site"),
    cl::values(clEnumValN(UseSiteCapture::UseOnly, "fuzzalloc-capture-use",
                          "Record a def was used"),
               clEnumValN(UseSiteCapture::UseWithOffset,
                          "fuzzalloc-capture-offset",
                          "Record the offset a def was used"),
               clEnumValN(UseSiteCapture::UseWithValue,
                          "fuzzalloc-capture-value",
                          "Record the value of the def")));

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

  FunctionCallee HashFn;

  IntegerType *TagTy;
  PointerType *Int8PtrTy;
  IntegerType *IntPtrTy;
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

  LLVM_DEBUG(dbgs() << "Instrumenting " << *Inst << " in "
                    << Inst->getFunction()->getName() << " (ptr=" << *Ptr
                    << ")\n");

  // This metadata can be used by the static pointer analysis
  Inst->setMetadata(Mod->getMDKindID(kFuzzallocInstrumentedUseSiteMD),
                    MDNode::get(*Ctx, None));

  assert(Inst->getNextNode());
  IRBuilder<> IRB(Inst->getNextNode());

  // Compute the AFL coverage bitmap index based on the def-use chain and update
  // the AFL coverage bitmap (done inside the hash function)
  auto *PtrCast = IRB.CreatePointerCast(Ptr, Int8PtrTy);
  auto *PtrElemTy = Ptr->getType()->getPointerElementType();
  auto *Size = ConstantInt::get(IntPtrTy, DL->getTypeStoreSize(PtrElemTy));
  auto *UseSite = generateTag(TagTy);

  IRB.CreateCall(HashFn, {UseSite, PtrCast, Size});
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

  this->TagTy = Type::getIntNTy(*Ctx, kNumTagBits);
  this->IntPtrTy = DL->getIntPtrType(*Ctx);
  this->Int8PtrTy = Type::getInt8PtrTy(*Ctx);

  // Select the hash function to use
  {
    auto *VoidTy = Type::getVoidTy(*Ctx);
    auto HashFnName = [&]() {
      if (ClUseCapture == UseOnly) {
        return "__afl_hash_def_use";
      } else if (ClUseCapture == UseWithOffset) {
        return "__afl_hash_def_use_offset";
      } else if (ClUseCapture == UseWithValue) {
        return "__afl_hash_def_use_value";
      }
      llvm_unreachable("Invalid use capture option");
    }();
    this->HashFn = Mod->getOrInsertFunction(HashFnName, VoidTy, TagTy,
                                            Int8PtrTy, IntPtrTy);
  }

  // Instrument all the things
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

  status_stream() << "[" << M.getName() << "] Use site capture: " << [&]() {
    switch (ClUseCapture) {
    case UseSiteCapture::UseOnly:
      return "use";
    case UseSiteCapture::UseWithOffset:
      return "offset";
    case UseSiteCapture::UseWithValue:
      return "value";
    }
  }() << '\n';
  success_stream() << "[" << M.getName()
                   << "] Num. instrumented reads: " << NumInstrumentedReads
                   << '\n';
  success_stream() << "[" << M.getName()
                   << "] Num. instrumented writes: " << NumInstrumentedWrites
                   << '\n';

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
