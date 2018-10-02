//===-- FuzzallocUtils.cpp - fuzzalloc utils ------------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Common functionality.
///
//===----------------------------------------------------------------------===//

#include <cxxabi.h>

#include "llvm/ADT/Statistic.h"
#include "llvm/Analysis/InstructionSimplify.h"
#include "llvm/Analysis/MemoryLocation.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/GlobalAlias.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Operator.h"

#include "FuzzallocUtils.h"
#include "debug.h"     // from afl
#include "fuzzalloc.h" // from fuzzalloc

using namespace llvm;

// Common command-line arguments
cl::opt<unsigned> ClDefSiteTagMin("fuzzalloc-tag-min",
                                  cl::desc("Minimum tag value"),
                                  cl::init(FUZZALLOC_TAG_MIN), cl::Hidden);
cl::opt<unsigned> ClDefSiteTagMax("fuzzalloc-tag-max",
                                  cl::desc("Maximum tag value"),
                                  cl::init(FUZZALLOC_TAG_MAX), cl::Hidden);

void setNoSanitizeMetadata(Instruction *I) {
  Module *M = I->getModule();
  LLVMContext &C = M->getContext();

  I->setMetadata(M->getMDKindID("nosanitize"), MDNode::get(C, None));
}

void printStatistic(const Module &M, const Statistic &Stat) {
  if (Stat > 0) {
    OKF("[%s] %u %s - %s", M.getName().str().c_str(), Stat.getValue(),
        Stat.getName(), Stat.getDesc());
  }
}

bool isVTableOrTypeInfo(const Value *V) {
  if (!isa<GlobalVariable>(V)) {
    return false;
  }

  int DemangleStatus;
  char *DemangleNameCStr = abi::__cxa_demangle(
      V->getName().str().c_str(), nullptr, nullptr, &DemangleStatus);
  if (DemangleStatus == 0) {
    StringRef DemangleName = StringRef(DemangleNameCStr);

    if (DemangleName.startswith_lower("vtable for ") ||
        DemangleName.startswith_lower("vtt for ") ||
        DemangleName.startswith_lower("typeinfo for ") ||
        DemangleName.startswith_lower("typeinfo name for ")) {
      return true;
    }
  }

  return false;
}

Value *GetUnderlyingObjectThroughLoads(Value *V, const DataLayout &DL,
                                       unsigned MaxLookup) {
  if (!V->getType()->isPointerTy()) {
    return V;
  }

  for (unsigned Count = 0; MaxLookup == 0 || Count < MaxLookup; ++Count) {
    if (GEPOperator *GEP = dyn_cast<GEPOperator>(V)) {
      V = GEP->getPointerOperand();
    } else if (Operator::getOpcode(V) == Instruction::BitCast ||
               Operator::getOpcode(V) == Instruction::AddrSpaceCast ||
               Operator::getOpcode(V) == Instruction::Load) {
      V = cast<Operator>(V)->getOperand(0);
    } else if (GlobalAlias *GA = dyn_cast<GlobalAlias>(V)) {
      if (GA->isInterposable()) {
        return V;
      }
      V = GA->getAliasee();
    } else if (isa<AllocaInst>(V)) {
      // An alloca can't be further simplified.
      return V;
    } else {
      if (auto *Call = dyn_cast<CallBase>(V)) {
        // CaptureTracking can know about special capturing properties of some
        // intrinsics like launder.invariant.group, that can't be expressed with
        // the attributes, but have properties like returning aliasing pointer.
        // Because some analysis may assume that nocaptured pointer is not
        // returned from some special intrinsic (because function would have to
        // be marked with returns attribute), it is crucial to use this function
        // because it should be in sync with CaptureTracking. Not using it may
        // cause weird miscompilations where 2 aliasing pointers are assumed to
        // noalias.
        if (auto *RP = getArgumentAliasingToReturnedPointer(Call, false)) {
          V = RP;
          continue;
        }
      }

      // See if InstructionSimplify knows any relevant tricks.
      if (Instruction *I = dyn_cast<Instruction>(V)) {
        // TODO: Acquire a DominatorTree and AssumptionCache and use them.
        if (Value *Simplified = SimplifyInstruction(I, {DL, I})) {
          V = Simplified;
          continue;
        }
      }

      return V;
    }
    assert(V->getType()->isPointerTy() && "Unexpected operand type!");
  }

  return V;
}

Optional<StructOffset> getStructOffset(const StructType *StructTy,
                                       unsigned ByteOffset,
                                       const DataLayout &DL) {
  if (StructTy->isOpaque()) {
    return None;
  }

  const StructLayout *SL =
      DL.getStructLayout(const_cast<StructType *>(StructTy));

  if (ByteOffset > SL->getSizeInBytes()) {
    return None;
  }

  unsigned StructIdx = SL->getElementContainingOffset(ByteOffset);
  Type *ElemTy = StructTy->getElementType(StructIdx);

  // Handle nested structs. The recursion will eventually bottom out at some
  // primitive type (ideally, a function pointer).
  //
  // The idea is that the byte offset may point to some inner struct. If this is
  // the case, then we want to record the element in the inner struct so that we
  // can tag calls to it later
  if (auto *ElemStructTy = dyn_cast<StructType>(ElemTy)) {
    assert(!ElemStructTy->isOpaque());
    return getStructOffset(ElemStructTy,
                           ByteOffset - SL->getElementOffset(StructIdx), DL);
  } else {
    return Optional<StructOffset>({StructTy, StructIdx});
  }
}
