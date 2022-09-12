//===-- UseSite.cpp - Instrument use sites ----------------------*- C++ -*-===//
///
/// \file
/// Instrument use sites
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

#include "Utils.h"

using namespace llvm;

#define DEBUG_TYPE "fuzzalloc-use-site"

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
class UseSite : public ModulePass {
public:
  static char ID;
  UseSite() : ModulePass(ID) {}

  virtual void getAnalysisUsage(AnalysisUsage &) const override;
  virtual bool runOnModule(Module &) override;

private:
  void doInstrument(InterestingMemoryOperand *);

  Module *Mod;
  LLVMContext *Ctx;
  const DataLayout *DL;

  FunctionCallee InstFn;

  IntegerType *TagTy;
  PointerType *Int8PtrTy;
  IntegerType *IntPtrTy;
  StructType *TracerSrcLocationTy;
};

char UseSite::ID = 0;

void UseSite::doInstrument(InterestingMemoryOperand *Op) {
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

  auto *PtrCast = IRB.CreatePointerCast(Ptr, Int8PtrTy);
  auto *PtrElemTy = Ptr->getType()->getPointerElementType();
  auto *Size = ConstantInt::get(IntPtrTy, DL->getTypeStoreSize(PtrElemTy));

  if (ClInstType == InstType::InstAFL) {
    auto *Metadata = generateTag(TagTy);
    IRB.CreateCall(InstFn, {Metadata, PtrCast, Size});
  } else if (ClInstType == InstType::InstTrace) {
    auto *Metadata = ConstantExpr::getPointerCast(
        tracerCreateUse(Inst, Mod), TracerSrcLocationTy->getPointerTo());
    IRB.CreateCall(InstFn, {Metadata, PtrCast, Size});
  }
}

void UseSite::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<UseSiteIdentify>();
}

bool UseSite::runOnModule(Module &M) {
  bool Changed = false;

  // Initialize stuff
  this->Mod = &M;
  this->Ctx = &M.getContext();
  this->DL = &M.getDataLayout();

  this->TagTy = Type::getIntNTy(*Ctx, kNumTagBits);
  this->IntPtrTy = DL->getIntPtrType(*Ctx);
  this->Int8PtrTy = Type::getInt8PtrTy(*Ctx);
  this->TracerSrcLocationTy =
      StructType::create({Int8PtrTy, Int8PtrTy, IntPtrTy, IntPtrTy},
                         "fuzzalloc.SrcLocation", /*isPacked=*/true);

  // Select the instrumentation to use
  this->InstFn = [&]() -> FunctionCallee {
    auto *VoidTy = Type::getVoidTy(*Ctx);

    if (ClInstType == InstType::InstAFL) {
      auto InstFnName = [&]() -> std::string {
        if (ClUseCapture == UseOnly) {
          return "__afl_hash_def_use";
        } else if (ClUseCapture == UseWithOffset) {
          return "__afl_hash_def_use_offset";
        } else if (ClUseCapture == UseWithValue) {
          return "__afl_hash_def_use_value";
        }
        llvm_unreachable("Invalid use capture option");
      }();
      auto Fn = Mod->getOrInsertFunction(InstFnName, VoidTy, TagTy, Int8PtrTy,
                                         IntPtrTy);
      return Fn;
    } else if (ClInstType == InstType::InstTrace) {
      auto Fn = Mod->getOrInsertFunction("__tracer_use", VoidTy,
                                         TracerSrcLocationTy->getPointerTo(),
                                         Int8PtrTy, IntPtrTy);
      assert(isa_and_nonnull<Function>(Fn.getCallee()));
      cast<Function>(Fn.getCallee())->setDoesNotThrow();
      cast<Function>(Fn.getCallee())->addParamAttr(0, Attribute::NonNull);
      cast<Function>(Fn.getCallee())->addParamAttr(0, Attribute::ReadOnly);

      return Fn;
    } else {
      return FunctionCallee();
    }
  }();

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
    llvm_unreachable("Invalid use site capture");
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

static RegisterPass<UseSite> X(DEBUG_TYPE, "Instrument use sites", false,
                               false);

static void registerUseSitePass(const PassManagerBuilder &,
                                legacy::PassManagerBase &PM) {
  PM.add(new UseSite());
}

static RegisterStandardPasses
    RegisterUseSitePass(PassManagerBuilder::EP_OptimizerLast,
                        registerUseSitePass);

static RegisterStandardPasses
    RegisterUseSitePass0(PassManagerBuilder::EP_EnabledOnOptLevel0,
                         registerUseSitePass);
