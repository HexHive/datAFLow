//===-- HeapifyAllocas.cpp - Heapify alloca arrays ------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
///
/// \file
/// This pass heapifies stack-based (i.e., allocas) static arrays to
/// dynamically-allocated arrays via \p malloc.
///
//===----------------------------------------------------------------------===//

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Analysis/CaptureTracking.h"
#include "llvm/IR/DIBuilder.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/Pass.h"
#include "llvm/Support/Debug.h"
#include "llvm/Transforms/IPO/PassManagerBuilder.h"

#include "HeapifyUtils.h"
#include "Utils/FuzzallocUtils.h"

using namespace llvm;

#define DEBUG_TYPE "fuzzalloc-heapify-allocas"

static cl::opt<int> ClMinArraySize(
    "fuzzalloc-min-alloca-array-size",
    cl::desc("The minimum size of a static alloca array to heapify to malloc"),
    cl::init(1));

static cl::opt<bool> ClHeapifyStructs(
    "fuzzalloc-heapify-structs",
    cl::desc("Heapify alloca structs that have their address taken"),
    cl::init(false), cl::Hidden);

STATISTIC(NumOfAllocaHeapification, "Number of alloca heapifications.");
STATISTIC(NumOfFreeInsert, "Number of calls to free inserted.");

namespace {

/// HeapifyAllocas: instrument the code in a module to heapify static,
/// fixed-size arrays on the stack (i.e., allocas) to dynamically allocated
/// arrays via \p malloc.
class HeapifyAllocas : public ModulePass {
private:
  DataLayout *DL;
  DIBuilder *DBuilder;

  Instruction *insertMalloc(const AllocaInst *, AllocaInst *,
                            Instruction *) const;

  void copyDebugInfo(const AllocaInst *, AllocaInst *) const;

  AllocaInst *heapifyAlloca(AllocaInst *,
                            const ArrayRef<IntrinsicInst *> &) const;

public:
  static char ID;
  HeapifyAllocas() : ModulePass(ID) {}

  bool doInitialization(Module &) override;
  bool doFinalization(Module &) override;
  bool runOnModule(Module &) override;
};

} // end anonymous namespace

char HeapifyAllocas::ID = 0;

// This defines our "heapification policy"; i.e., which allocas who's def/use
// chains will be tracked at runtime
static bool isHeapifiableAlloca(AllocaInst *Alloca) {
  Type *AllocatedTy = Alloca->getAllocatedType();

  // Check if the allocated value is of a heapifiable type (i.e., an array)
  if (isHeapifiableType(AllocatedTy)) {
    return true;
  }

  // Otherwise, heapify structs/classes (not from libstdc++) that escape the
  // function in which they are defined
  StructType *AllocatedStructTy = dyn_cast<StructType>(AllocatedTy);
  if (!AllocatedStructTy || !ClHeapifyStructs) {
    return false;
  }

  // Check if the struct escapes
  bool AllocaEscapes = PointerMayBeCaptured(Alloca, /* ReturnCaptures */ false,
                                            /*StoreCaptures */ true);
  if (!AllocaEscapes) {
    return false;
  }

  // Literal structs don't have names
  if (AllocatedStructTy->isLiteral()) {
    return false;
  }

  // Ignore libstdc++ structs/classes
  StringRef StructName = AllocatedTy->getStructName();
  if (StructName.startswith("class.__gnu_cxx::") ||
      StructName.startswith("class.__gnu_debug::") ||
      StructName.startswith("class.__gnu_internal::") ||
      StructName.startswith("class.__gnu_parallel::") ||
      StructName.startswith("class.__gnu_pbds::") ||
      StructName.startswith("class.__gnu_profile::") ||
      StructName.startswith("class.__gnu_sequential::") ||
      StructName.startswith("class.__abi::") ||
      StructName.startswith("class.std::")) {
    return false;
  }

  return true;
}

/// Insert a call to `malloc` before the `InsertPt` instruction. The result of
/// the `malloc` call is stored in `NewAlloca`.
Instruction *HeapifyAllocas::insertMalloc(const AllocaInst *OrigAlloca,
                                          AllocaInst *NewAlloca,
                                          Instruction *InsertPt) const {
  const Module *M = OrigAlloca->getModule();
  LLVMContext &C = M->getContext();
  Type *AllocatedTy = OrigAlloca->getAllocatedType();
  Instruction *MallocCall = nullptr;

  IRBuilder<> IRB(InsertPt);

  if (auto *ArrayTy = dyn_cast<ArrayType>(AllocatedTy)) {
    // Insert array malloc call
    Type *ElemTy = ArrayTy->getArrayElementType();
    uint64_t ArrayNumElems = ArrayTy->getNumElements();

    MallocCall = createArrayMalloc(C, *this->DL, IRB, ElemTy, ArrayNumElems,
                                   NewAlloca->getName() + "_malloccall");
  } else {
    // Insert non-array malloc call
    MallocCall = createMalloc(C, *this->DL, IRB, AllocatedTy,
                              NewAlloca->getName() + "_malloccall");
  }

  assert(MallocCall && "malloc call should have been created");
  auto *MallocStore = IRB.CreateStore(MallocCall, NewAlloca);
  MallocStore->setMetadata(M->getMDKindID("fuzzalloc.noinstrument"),
                           MDNode::get(C, None));

  return MallocCall;
}

void HeapifyAllocas::copyDebugInfo(const AllocaInst *OrigAlloca,
                                   AllocaInst *NewAlloca) const {
  auto *F = OrigAlloca->getFunction();

  for (auto I = inst_begin(F); I != inst_end(F); ++I) {
    if (auto *DbgDeclare = dyn_cast<DbgDeclareInst>(&*I)) {
      if (DbgDeclare->getAddress() == OrigAlloca) {
        this->DBuilder->insertDeclare(NewAlloca, DbgDeclare->getVariable(),
                                      DbgDeclare->getExpression(),
                                      DbgDeclare->getDebugLoc(),
                                      const_cast<DbgDeclareInst *>(DbgDeclare));
      }
    }
  }
}

AllocaInst *HeapifyAllocas::heapifyAlloca(
    AllocaInst *Alloca, const ArrayRef<IntrinsicInst *> &LifetimeStarts) const {
  LLVM_DEBUG(dbgs() << "heapifying " << *Alloca << " in function "
                    << Alloca->getFunction()->getName() << "\n");

  const Module *M = Alloca->getModule();
  LLVMContext &C = M->getContext();

  // Cache uses
  SmallVector<User *, 8> Users(Alloca->user_begin(), Alloca->user_end());

  // This will transform something like this:
  //
  // %1 = alloca [NumElements x Ty]
  //
  // into something like this:
  //
  // %1 = alloca Ty*
  // %2 = call i8* @malloc(PtrTy Size)
  // %3 = bitcast i8* %2 to Ty*
  // store Ty* %3, Ty** %1
  //
  // Where:
  //
  //  - `Ty` is the array element type
  //  - `NumElements` is the array number of elements
  //  - `PtrTy` is the target's pointer type
  //  - `Size` is the size of the allocated buffer (equivalent to
  //    `NumElements * sizeof(Ty)`)

  const Type *AllocatedTy = Alloca->getAllocatedType();
  PointerType *NewAllocaTy = nullptr;

  if (AllocatedTy->isArrayTy()) {
    NewAllocaTy = AllocatedTy->getArrayElementType()->getPointerTo();
  } else {
    NewAllocaTy = AllocatedTy->getPointerTo();
  }

  assert(NewAllocaTy && "New alloca must have a type");
  auto *NewAlloca = new AllocaInst(NewAllocaTy, this->DL->getAllocaAddrSpace(),
                                   Alloca->getName(), Alloca);
  NewAlloca->setMetadata(M->getMDKindID("fuzzalloc.heapified_alloca"),
                         MDNode::get(C, None));
  NewAlloca->takeName(Alloca);
  copyDebugInfo(Alloca, NewAlloca);

  // Decide where to insert the call to malloc.
  //
  // If there are lifetime.start intrinsics, then we must allocate memory at
  // these intrinsics. Otherwise, we can just perform the allocation after the
  // alloca instruction.
  bool FoundLifetimeStart = false;
  for (auto *LifetimeStart : LifetimeStarts) {
    if (GetUnderlyingObjectThroughLoads(LifetimeStart->getOperand(1),
                                        *this->DL) == Alloca) {
      auto *Ptr = LifetimeStart->getOperand(1);
      assert(isa<Instruction>(Ptr));

      insertMalloc(Alloca, NewAlloca, cast<Instruction>(Ptr));
      FoundLifetimeStart = true;
    }
  }

  if (!FoundLifetimeStart) {
    insertMalloc(Alloca, NewAlloca, NewAlloca->getNextNode());
  }

  // Update all the users of the original alloca to use the new heap-based
  // alloca
  for (auto *U : Users) {
    if (auto *GEP = dyn_cast<GetElementPtrInst>(U)) {
      // Ensure GEPs are correctly typed
      updateGEP(GEP, NewAlloca, NewAlloca->getAllocatedType());
    } else if (auto *PHI = dyn_cast<PHINode>(U)) {
      // PHI nodes are a special case because they must always be the first
      // instruction in a basic block. To ensure this property is true we insert
      // the load instruction at the end of the appropriate predecessor block(s)
      for (unsigned I = 0; I < PHI->getNumIncomingValues(); ++I) {
        Value *IncomingValue = PHI->getIncomingValue(I);
        BasicBlock *IncomingBlock = PHI->getIncomingBlock(I);

        if (IncomingValue == Alloca) {
          auto *LoadNewAlloca =
              new LoadInst(NewAlloca->getAllocatedType(), NewAlloca, "",
                           IncomingBlock->getTerminator());
          auto *BitCastNewAlloca = CastInst::CreatePointerCast(
              LoadNewAlloca, IncomingValue->getType(), "",
              IncomingBlock->getTerminator());
          PHI->setIncomingValue(I, BitCastNewAlloca);
        }
      }
    } else if (auto *Inst = dyn_cast<Instruction>(U)) {
      // We must load the new alloca from the heap before we do anything with it
      auto *LoadNewAlloca =
          new LoadInst(NewAlloca->getAllocatedType(), NewAlloca, "", Inst);
      auto *BitCastNewAlloca = CastInst::CreatePointerCast(
          LoadNewAlloca, Alloca->getType(), "", Inst);
      Inst->replaceUsesOfWith(Alloca, BitCastNewAlloca);
    } else {
      assert(false && "Unsupported alloca user");
    }
  }

  NumOfAllocaHeapification++;
  return NewAlloca;
}

bool HeapifyAllocas::doInitialization(Module &M) {
  this->DL = new DataLayout(M.getDataLayout());
  this->DBuilder = new DIBuilder(M, /* AllowUnresolved */ false);

  return false;
}

bool HeapifyAllocas::doFinalization(Module &) {
  delete this->DL;

  this->DBuilder->finalize();
  delete this->DBuilder;

  return false;
}

bool HeapifyAllocas::runOnModule(Module &M) {
  // Static array allocations to heapify
  SmallVector<AllocaInst *, 8> AllocasToHeapify;

  // lifetime.start intrinsics that will require calls to mallic to be inserted
  // before them
  SmallVector<IntrinsicInst *, 4> LifetimeStarts;

  // lifetime.end intrinsics that will require calls to free to be inserted
  // before them
  SmallVector<IntrinsicInst *, 4> LifetimeEnds;

  // Return instructions that may require calls to free to be inserted before
  // them
  SmallVector<ReturnInst *, 4> Returns;

  for (auto &F : M.functions()) {
    AllocasToHeapify.clear();
    LifetimeStarts.clear();
    LifetimeEnds.clear();
    Returns.clear();

    // Collect all the things!
    for (auto I = inst_begin(F); I != inst_end(F); ++I) {
      // Collect heapifaible allocas
      if (auto *Alloca = dyn_cast<AllocaInst>(&*I)) {
        if (isHeapifiableAlloca(Alloca)) {
          AllocasToHeapify.push_back(Alloca);
        }
        // Lifetime start/end intrinsics are required for placing mallocs/frees
      } else if (auto *Intrinsic = dyn_cast<IntrinsicInst>(&*I)) {
        if (Intrinsic->getIntrinsicID() == Intrinsic::lifetime_start) {
          LifetimeStarts.push_back(Intrinsic);
        } else if (Intrinsic->getIntrinsicID() == Intrinsic::lifetime_end) {
          LifetimeEnds.push_back(Intrinsic);
        }
        // Return instructions are required for placing frees
      } else if (auto *Return = dyn_cast<ReturnInst>(&*I)) {
        Returns.push_back(Return);
      }
    }

    // Heapify static arrays to dynamically allocated arrays and insert calls
    // to free at the appropriate locations (either at lifetime.end intrinsics
    // or at return instructions)
    for (auto *Alloca : AllocasToHeapify) {
      // Heapify the alloca. After this function call all users of the original
      // alloca are invalid
      auto *NewAlloca = heapifyAlloca(Alloca, LifetimeStarts);

      // Check if any of the original allocas (which have now been replaced by
      // the new alloca) are used in any lifetime.end intrinsics. If they are,
      // insert the free before the lifetime.end intrinsic and NOT at function
      // return, otherwise we may end up with a double free :(
      if (LifetimeEnds.empty()) {
        // If no lifetime.end intrinsics were found, just free the allocation
        // when the function returns
        for (auto *Return : Returns) {
          insertFree(NewAlloca->getAllocatedType(), NewAlloca, Return);
          NumOfFreeInsert++;
        }
      } else {
        // Otherwise insert the free before each lifetime.end
        for (auto *LifetimeEnd : LifetimeEnds) {
          if (GetUnderlyingObjectThroughLoads(LifetimeEnd->getOperand(1),
                                              *this->DL) == NewAlloca) {
            insertFree(NewAlloca->getAllocatedType(), NewAlloca, LifetimeEnd);
            NumOfFreeInsert++;
          }
        }
      }

      Alloca->eraseFromParent();
    }
  }

  printStatistic(M, NumOfAllocaHeapification);
  printStatistic(M, NumOfFreeInsert);

  return NumOfAllocaHeapification > 0;
}

static RegisterPass<HeapifyAllocas>
    X("fuzzalloc-heapify-allocas",
      "Heapify static array allocas to malloc calls", false, false);

static void registerHeapifyAllocasPass(const PassManagerBuilder &,
                                       legacy::PassManagerBase &PM) {
  PM.add(new HeapifyAllocas());
}

static RegisterStandardPasses
    RegisterHeapifyAllocasPass(PassManagerBuilder::EP_ModuleOptimizerEarly,
                               registerHeapifyAllocasPass);

static RegisterStandardPasses
    RegisterHeapifyAllocasPass0(PassManagerBuilder::EP_EnabledOnOptLevel0,
                                registerHeapifyAllocasPass);
