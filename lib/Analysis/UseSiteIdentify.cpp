//===-- UseSiteIdentify.cpp - Identify use sites to track -------*- C++ -*-===//
///
/// \file
/// Identify use sites to track
///
//===----------------------------------------------------------------------===//

#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/Pass.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Transforms/IPO/PassManagerBuilder.h>
#include <llvm/Transforms/Instrumentation/AddressSanitizerCommon.h>
#include <llvm/Transforms/Utils/PromoteMemToReg.h>

#include "fuzzalloc/Analysis/UseSiteIdentify.h"
#include "fuzzalloc/Metadata.h"
#include "fuzzalloc/Streams.h"

using namespace llvm;

#define DEBUG_TYPE "fuzzalloc-use-site-identify"

namespace {
//
// Command-line options
//

static cl::bits<UseSiteIdentify::UseSiteTypes> ClUseSitesToTrack(
    cl::desc("Use site type (to track)"),
    cl::values(clEnumValN(UseSiteIdentify::UseSiteTypes::Read,
                          "fuzzalloc-use-read", "Track reads (uses)"),
               clEnumValN(UseSiteIdentify::UseSiteTypes::Write,
                          "fuzzalloc-use-write", "Track writes (uses)")));

static cl::opt<bool> ClOpt("fuzzalloc-opt", cl::desc("Optimize instrumentat"),
                           cl::Hidden, cl::init(true));
static cl::opt<bool>
    ClTrackAtomics("fuzzalloc-track-atomics",
                   cl::desc("Track atomic instructions (rmw, cmpxhg)"),
                   cl::Hidden, cl::init(true));
static cl::opt<bool> ClTrackByval("fuzzalloc-track-byvals",
                                  cl::desc("Track byval call arguments"),
                                  cl::Hidden, cl::init(true));

//
// Global variables
//

static unsigned NumUsesToTrack = 0;
static unsigned NumReadUseSites = 0;
static unsigned NumWriteUseSites = 0;

//
// Helper functions
//

// Adapted from AddressSanitizer
static size_t getAllocaSizeInBytes(const AllocaInst *Alloca) {
  const auto &ArraySize = [&]() -> size_t {
    if (Alloca->isArrayAllocation()) {
      const auto *CI = dyn_cast<ConstantInt>(Alloca->getArraySize());
      assert(CI && "Non-constant array size");
      return CI->getZExtValue();
    } else {
      return 1;
    }
  }();
  auto *Ty = Alloca->getAllocatedType();
  auto SizeInBytes = Alloca->getModule()->getDataLayout().getTypeAllocSize(Ty);

  return SizeInBytes * ArraySize;
}

} // anonymous namespace

char UseSiteIdentify::ID = 0;

// Adapted from AddressSanitizer
bool UseSiteIdentify::isInterestingAlloca(const AllocaInst *Alloca) {
  auto PreviouslySeenAllocaInfo = ProcessedAllocas.find(Alloca);
  if (PreviouslySeenAllocaInfo != ProcessedAllocas.end()) {
    return PreviouslySeenAllocaInfo->second;
  }

  bool IsInteresting =
      (Alloca->getAllocatedType()->isSized() &&
       // alloca() may be called with 0 size, ignore it.
       ((!Alloca->isStaticAlloca()) || getAllocaSizeInBytes(Alloca) > 0) &&
       // We are only interested in allocas not promotable to registers.
       // Promotable allocas are common under -O0.
       !isAllocaPromotable(Alloca) &&
       // inalloca allocas are not treated as static, and we don't want
       // dynamic alloca instrumentation for them as well.
       Alloca->isUsedWithInAlloca() &&
       // swifterror allocas are register promoted by ISel
       !Alloca->isSwiftError());

  ProcessedAllocas.insert({Alloca, IsInteresting});
  return IsInteresting;
}

// Adapted from AddressSanitizer
bool UseSiteIdentify::ignoreAccess(const Value *Ptr) {
  // Do not instrument accesses from different address spaces; we cannot deal
  // with them
  auto *PtrTy = cast<PointerType>(Ptr->getType()->getScalarType());
  if (PtrTy->getPointerAddressSpace() != 0) {
    return true;
  }

  // Ignore swifterror addresses.
  // swifterror memory addresses are mem2reg promoted by instruction
  // selection. As such they cannot have regular uses like an instrumentation
  // function and it makes no sense to track them as memory.
  if (Ptr->isSwiftError()) {
    return true;
  }

  // Treat memory accesses to promotable allocas as non-interesting since they
  // will not cause memory violations. This greatly speeds up the instrumented
  // executable at -O0.
  if (auto *Alloca = dyn_cast_or_null<AllocaInst>(Ptr)) {
    if (!isInterestingAlloca(Alloca)) {
      return true;
    }
  }

  return false;
}

// Adapted from AddressSanitizer
void UseSiteIdentify::getInterestingMemoryOperands(
    Instruction *Inst, UseSiteOperands &InterestingOperands) {
  if (Inst->hasMetadata(kFuzzallocNoInstrumentMD)) {
    return;
  }

  if (auto *Load = dyn_cast<LoadInst>(Inst)) {
    if (ClUseSitesToTrack.isSet(UseSiteTypes::Read) &&
        !ignoreAccess(Load->getPointerOperand())) {
      InterestingOperands.emplace_back(Load, Load->getPointerOperandIndex(),
                                       false, Load->getType(),
                                       Load->getAlign());
      NumReadUseSites++;
    }
  } else if (auto *Store = dyn_cast<StoreInst>(Inst)) {
    if (ClUseSitesToTrack.isSet(UseSiteTypes::Write) &&
        !ignoreAccess(Store->getPointerOperand())) {
      InterestingOperands.emplace_back(
          Store, Store->getPointerOperandIndex(), true,
          Store->getValueOperand()->getType(), Store->getAlign());
      NumWriteUseSites++;
    }
  } else if (auto *RMW = dyn_cast<AtomicRMWInst>(Inst)) {
    if (ClTrackAtomics && !ignoreAccess(RMW->getPointerOperand())) {
      InterestingOperands.emplace_back(RMW, RMW->getPointerOperandIndex(), true,
                                       RMW->getValOperand()->getType(), None);
      NumWriteUseSites++;
    }
  } else if (auto *XCHG = dyn_cast<AtomicCmpXchgInst>(Inst)) {
    if (ClTrackAtomics && !ignoreAccess(XCHG->getPointerOperand())) {
      InterestingOperands.emplace_back(
          XCHG, XCHG->getPointerOperandIndex(), true,
          XCHG->getCompareOperand()->getType(), None);
      NumWriteUseSites++;
    }
  } else if (auto *Call = dyn_cast<CallInst>(Inst)) {
    auto *F = Call->getCalledFunction();
    if (F && (F->getIntrinsicID() == Intrinsic::masked_load ||
              F->getIntrinsicID() == Intrinsic::masked_store)) {
      bool IsWrite = F->getIntrinsicID() == Intrinsic::masked_store;
      // Masked store has an initial operand for the value.
      unsigned OpOffset = IsWrite ? 1 : 0;
      if (IsWrite ? !ClUseSitesToTrack.isSet(UseSiteTypes::Write)
                  : !ClUseSitesToTrack.isSet(UseSiteTypes::Read)) {
        return;
      }

      auto *BasePtr = Call->getOperand(OpOffset);
      if (ignoreAccess(BasePtr)) {
        return;
      }
      auto *Ty = cast<PointerType>(BasePtr->getType())->getElementType();
      MaybeAlign Alignment = Align(1);
      // Otherwise no alignment guarantees. We probably got Undef.
      if (auto *Op = dyn_cast<ConstantInt>(Call->getOperand(1 + OpOffset))) {
        Alignment = Op->getMaybeAlignValue();
      }
      auto *Mask = Call->getOperand(2 + OpOffset);
      InterestingOperands.emplace_back(Call, OpOffset, IsWrite, Ty, Alignment,
                                       Mask);
      if (IsWrite) {
        NumWriteUseSites++;
      } else {
        NumReadUseSites++;
      }
    } else {
      for (unsigned ArgNo = 0; ArgNo < Call->arg_size(); ++ArgNo) {
        if (!ClTrackByval || !Call->isByValArgument(ArgNo) ||
            ignoreAccess(Call->getArgOperand(ArgNo))) {
          return;
        }
        auto *Ty = Call->getParamByValType(ArgNo);
        InterestingOperands.emplace_back(Call, ArgNo, false, Ty, Align(1));
        NumReadUseSites++;
      }
    }
  }
}

bool UseSiteIdentify::runOnFunction(Function &F) {
  // Don't instrument our own functions
  if (F.getName().startswith("fuzzalloc.")) {
    return false;
  }

  UseSiteOperands InterestingOperands;
  SmallPtrSet<Value *, 16> TempsToTrack;

  for (auto &BB : F) {
    TempsToTrack.clear();
    for (auto &I : BB) {
      InterestingOperands.clear();
      getInterestingMemoryOperands(&I, InterestingOperands);

      for (auto &Operand : InterestingOperands) {
        if (ClOpt) {
          auto *Ptr = Operand.getPtr();
          // If we have a mask, skip instrumentation if we've already
          // instrumented the full object. But don't add to TempsToTrack
          // because we might get another load/store with a different mask.
          if (Operand.MaybeMask) {
            if (TempsToTrack.count(Ptr)) {
              continue; // We've seen this (whole) temp in the current BB.
            }
          } else {
            if (!TempsToTrack.insert(Ptr).second) {
              continue; // We've seen this temp in the current BB.
            }
          }
        }
        ToTrack[&F].push_back(Operand);
        NumUsesToTrack++;
      }
    }
  }

  return false;
}

UseSiteIdentify::UseSiteOperands *UseSiteIdentify::getUseSites(Function &F) {
  auto It = ToTrack.find(&F);
  if (It == ToTrack.end()) {
    return nullptr;
  }

  return &It->second;
}

void UseSiteIdentify::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.setPreservesAll();
}

bool UseSiteIdentify::runOnModule(Module &M) {
  bool Changed = false;

  if (ClUseSitesToTrack.isSet(UseSiteIdentify::Read)) {
    status_stream() << "[" << M.getName() << "] Tracking read use sites\n";
  }
  if (ClUseSitesToTrack.isSet(UseSiteIdentify::Write)) {
    status_stream() << "[" << M.getName() << "] Tracking write use sites\n";
  }

  for (auto &F : M) {
    Changed = runOnFunction(F);
  }

  return Changed;
}

//
// Pass registration
//

static RegisterPass<UseSiteIdentify> X(DEBUG_TYPE, "Identify use sites", true,
                                       true);

static void registerUseSiteIdentifyPass(const PassManagerBuilder &,
                                        legacy::PassManagerBase &PM) {
  PM.add(new UseSiteIdentify());
}

static RegisterStandardPasses
    RegisterUseSiteIdentifyPass(PassManagerBuilder::EP_OptimizerLast,
                                registerUseSiteIdentifyPass);

static RegisterStandardPasses
    RegisterUseSiteIdentifyPass0(PassManagerBuilder::EP_EnabledOnOptLevel0,
                                 registerUseSiteIdentifyPass);
