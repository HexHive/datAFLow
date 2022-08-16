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
#include "fuzzalloc/Runtime/BaggyBounds.h"
#include "fuzzalloc/fuzzalloc.h"

// AFL++ headers
#include "config.h"
#include "xxhash.h"

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
    cl::values(clEnumValN(UseSiteCapture::UseOnly, "fuzzalloc-use-only",
                          "Record a def was used"),
               clEnumValN(UseSiteCapture::UseWithOffset,
                          "fuzzalloc-use-with-offset",
                          "Record the offset a def was used"),
               clEnumValN(UseSiteCapture::UseWithValue,
                          "fuzzalloc-use-with-value",
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

  FunctionCallee BBLookupFn;
  FunctionCallee HashFn;

  GlobalVariable *AFLMapPtr;
  GlobalVariable *BaggyBoundsPtr;

  PointerType *Int8PtrTy;
  IntegerType *IntPtrTy;
  IntegerType *HashTy;
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

  // Compute the AFL coverage bitmap index based on the def-use chain
  auto *PtrCast = IRB.CreatePointerCast(Ptr, Int8PtrTy);
  auto *PtrElemTy = Ptr->getType()->getPointerElementType();
  auto *Hash = IRB.CreateCall(
      HashFn,
      {PtrCast, ConstantInt::get(IntPtrTy, DL->getTypeStoreSize(PtrElemTy))},
      Ptr->hasName() ? Ptr->getName() + ".hash" : "hash");

  auto *HashMask = [&]() -> ConstantInt * {
    if (MAP_SIZE_POW2 <= 16) {
      return ConstantInt::get(HashTy, 0xFFFF);
    } else if (MAP_SIZE_POW2 <= 32) {
      return ConstantInt::get(HashTy, 0xFFFFFFFF);
    } else {
      return ConstantInt::get(HashTy, 0xFFFFFFFFFFFFFFFF);
    }
  }();

  // Load the AFL bitmap (first mask out the hash depending on the bitmap size)
  auto *AFLMap = IRB.CreateLoad(AFLMapPtr);
  AFLMap->setMetadata(Mod->getMDKindID(kNoSanitizeMD), MDNode::get(*Ctx, None));

  auto *AFLMapIdx = IRB.CreateGEP(AFLMap, IRB.CreateAnd(Hash, HashMask),
                                  "__afl_area_ptr_idx");

  // Update the bitmap by incrementing the count
  auto *CounterLoad = IRB.CreateLoad(AFLMapIdx);
  CounterLoad->setMetadata(Mod->getMDKindID(kNoSanitizeMD),
                           MDNode::get(*Ctx, None));
  auto *Incr = IRB.CreateAdd(CounterLoad, IRB.getInt8(1));

  auto *CounterStore = IRB.CreateStore(Incr, AFLMapIdx);
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

  this->IntPtrTy = DL->getIntPtrType(*Ctx);
  this->Int8PtrTy = Type::getInt8PtrTy(*Ctx);
  this->HashTy = Type::getIntNTy(*Ctx, sizeof(XXH64_hash_t) * CHAR_BIT);
  this->TagTy = Type::getIntNTy(*Ctx, kNumTagBits);
  this->DefaultTag = ConstantInt::get(TagTy, kFuzzallocDefaultTag);

  {
    this->AFLMapPtr = new GlobalVariable(
        *Mod, Int8PtrTy, /*isConstant=*/false, GlobalValue::ExternalLinkage,
        /*Initializer=*/nullptr, "__afl_area_ptr");
    this->BaggyBoundsPtr = new GlobalVariable(
        *Mod, Int8PtrTy, /*isConstant=*/false, GlobalValue::ExternalLinkage,
        /*Initializer=*/nullptr, "__baggy_bounds_table");

    this->BBLookupFn = Mod->getOrInsertFunction("__bb_lookup", TagTy, Int8PtrTy,
                                                IntPtrTy->getPointerTo());
  }

  {
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
    this->HashFn =
        Mod->getOrInsertFunction(HashFnName, HashTy, Int8PtrTy, IntPtrTy);
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
