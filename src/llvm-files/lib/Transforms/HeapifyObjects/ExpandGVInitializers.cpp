//===-- ExpandGVInitializers.cpp - Expand global variable initializers ----===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Expand global variables with constant initializers.
///
//===----------------------------------------------------------------------===//

#include "llvm/ADT/Statistic.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/Pass.h"
#include "llvm/Support/Debug.h"
#include "llvm/Transforms/IPO/PassManagerBuilder.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"

#include "HeapifyUtils.h"
#include "Utils/FuzzallocUtils.h"

using namespace llvm;

#define DEBUG_TYPE "fuzzalloc-expand-gv-initializers"

STATISTIC(NumOfExpandedGlobalVariables,
          "Number of expanded global variable initializers");

namespace {

/// ExpandGVInitializers: rewrite global variable static initializers to
/// dynamic initializers in the module's constructor.
class ExpandGVInitializers : public ModulePass {
private:
  SmallPtrSet<Constant *, 8> DeadConstants;

  Function *expandInitializer(GlobalVariable *);

public:
  static char ID;
  ExpandGVInitializers() : ModulePass(ID) {}

  bool runOnModule(Module &M) override;
};

} // end anonymous namespace

char ExpandGVInitializers::ID = 0;

static void getGVsToExpand(const GlobalVariable *OrigGV,
                           SmallPtrSet<GlobalVariable *, 8> &GVs) {
  SmallVector<const User *, 16> Worklist(OrigGV->user_begin(),
                                         OrigGV->user_end());

  while (!Worklist.empty()) {
    auto *U = Worklist.pop_back_val();
    if (!isa<Constant>(U)) {
      continue;
    }

    if (auto *GV = dyn_cast<GlobalVariable>(U)) {
      if (GV->hasInitializer()) {
        auto Res = GVs.insert(const_cast<GlobalVariable *>(GV));
        if (!Res.second) {
          continue;
        }
      }
    }

    Worklist.append(U->user_begin(), U->user_end());
  }
}

static bool constantStructContainsArray(const ConstantStruct *ConstStruct) {
  for (auto &Op : ConstStruct->operands()) {
    if (isa<ArrayType>(Op->getType())) {
      return true;
    } else if (auto *GEP = dyn_cast<GEPOperator>(Op)) {
      if (isa<ArrayType>(GEP->getSourceElementType())) {
        return true;
      }
    } else if (auto *StructOp = dyn_cast<ConstantStruct>(Op)) {
      return constantStructContainsArray(StructOp);
    }
  }

  return false;
}

/// Create the constructor
///
/// The constructor must be executed after the heapified global variable's
/// constructor, hence the higher priority
static IRBuilder<> createInitCtor(GlobalVariable *GV) {
  Module *M = GV->getParent();
  LLVMContext &C = M->getContext();

  FunctionType *GlobalCtorTy =
      FunctionType::get(Type::getVoidTy(C), /* isVarArg */ false);
  Function *GlobalCtorF =
      Function::Create(GlobalCtorTy, GlobalValue::LinkageTypes::InternalLinkage,
                       "fuzzalloc.init_" + GV->getName(), M);
  appendToGlobalCtors(*M, GlobalCtorF, kHeapifyGVCtorAndDtorPriority + 1);

  // Create the entry basic block
  BasicBlock *BB = BasicBlock::Create(C, "entry", GlobalCtorF);
  ReturnInst::Create(C, BB);

  return IRBuilder<>(BB->getTerminator());
}

/// Recursively expand `ConstantAggregate`s by generating equivalent
/// instructions in a module constructor.
static void expandConstantAggregate(IRBuilder<> &IRB, GlobalVariable *GV,
                                    ConstantAggregate *CA,
                                    std::vector<unsigned> &Idxs) {
  Module *M = GV->getParent();
  LLVMContext &C = M->getContext();
  IntegerType *Int32Ty = Type::getInt32Ty(C);

  // Converts an unsigned integer to something IRBuilder understands
  auto UnsignedToInt32 = [Int32Ty](const unsigned &N) {
    return ConstantInt::get(Int32Ty, N);
  };

  for (unsigned I = 0; I < CA->getNumOperands(); ++I) {
    auto *Op = CA->getOperand(I);

    if (auto *AggOp = dyn_cast<ConstantAggregate>(Op)) {
      // Expand the nested ConstantAggregate
      Idxs.push_back(I);
      expandConstantAggregate(IRB, GV, AggOp, Idxs);
      Idxs.pop_back();
    } else {
      std::vector<Value *> IdxValues(Idxs.size());
      std::transform(Idxs.begin(), Idxs.end(), IdxValues.begin(),
                     UnsignedToInt32);
      IdxValues.push_back(UnsignedToInt32(I));

      auto *Store = IRB.CreateStore(Op, IRB.CreateInBoundsGEP(GV, IdxValues));
      Store->setMetadata(M->getMDKindID("fuzzalloc.noinstrument"),
                         MDNode::get(C, None));
      setNoSanitizeMetadata(Store);
    }
  }
}

/// Move global variable's who have a `ConstantAggregate` initializer into a
/// constructor function.
Function *ExpandGVInitializers::expandInitializer(GlobalVariable *GV) {
  LLVM_DEBUG(dbgs() << "expanding initializer for global variable " << *GV
                    << '\n');

  Module *M = GV->getParent();
  LLVMContext &C = M->getContext();

  // Create the constructor
  IRBuilder<> IRB = createInitCtor(GV);
  Constant *Initializer = GV->getInitializer();

  if (isa<ConstantAggregate>(Initializer)) {
    for (unsigned I = 0; I < Initializer->getNumOperands(); ++I) {
      auto *Op = Initializer->getOperand(I);

      if (auto *AggregateOp = dyn_cast<ConstantAggregate>(Op)) {
        std::vector<unsigned> Idxs = {0, I};
        expandConstantAggregate(IRB, GV, AggregateOp, Idxs);
      } else {
        auto *Store = IRB.CreateStore(
            Op, IRB.CreateConstInBoundsGEP2_32(/* Ty */ nullptr, GV, 0, I));
        Store->setMetadata(M->getMDKindID("fuzzalloc.noinstrument"),
                           MDNode::get(C, None));
        setNoSanitizeMetadata(Store);
      }
    }
  } else if (auto *ConstExpr = dyn_cast<ConstantExpr>(Initializer)) {
    auto *Store = IRB.CreateStore(ConstExpr, GV);
    Store->setMetadata(M->getMDKindID("fuzzalloc.noinstrument"),
                       MDNode::get(C, None));
    setNoSanitizeMetadata(Store);
  } else if (auto *GlobalVal = dyn_cast<GlobalValue>(Initializer)) {
    auto *Store = IRB.CreateStore(GlobalVal, GV);
    Store->setMetadata(M->getMDKindID("fuzzalloc.noinstrument"),
                       MDNode::get(C, None));
    setNoSanitizeMetadata(Store);
  } else {
    assert(false && "Unsupported initializer to expand");
  }

  // Reset the initializer and make sure the global doesn't get placed into
  // readonly memory
  GV->setInitializer(Constant::getNullValue(GV->getValueType()));
  GV->setConstant(false);

  this->DeadConstants.insert(Initializer);

  NumOfExpandedGlobalVariables++;

  return IRB.GetInsertBlock()->getParent();
}

bool ExpandGVInitializers::runOnModule(Module &M) {
  SmallPtrSet<GlobalVariable *, 8> GVsToExpand;

  for (auto &GV : M.globals()) {
    // Skip LLVM intrinsics
    if (GV.getName().startswith("llvm.")) {
      continue;
    }

    // Skip C++ junk
    if (isVTableOrTypeInfo(&GV) || isFromLibCpp(&GV)) {
      continue;
    }

    // If the global variable is constant and cannot be exported, skip it
    if (GV.isConstant() &&
        (GV.hasPrivateLinkage() || GV.hasInternalLinkage())) {
      continue;
    }

    // The global variable may not be expandable itself, but other globals that
    // use this one may require expansion
    if (isHeapifiableType(GV.getValueType())) {
      getGVsToExpand(&GV, GVsToExpand);
    }

    // Nothing to expand
    if (!GV.hasInitializer()) {
      continue;
    }

    const Constant *Initializer = GV.getInitializer();
    if (!isa<ConstantAggregate>(Initializer)) {
      continue;
    }

    if (isa<ConstantArray>(Initializer)) {
      // Arrays are always expandable
      GVsToExpand.insert(&GV);
    } else if (auto *ConstStruct = dyn_cast<ConstantStruct>(Initializer)) {
      // Structs are expandable if they contain an array
      if (constantStructContainsArray(ConstStruct)) {
        GVsToExpand.insert(&GV);
      }
    } else {
      assert(false && "Unsupported constant initializer");
    }
  }

  for (auto *GV : GVsToExpand) {
    expandInitializer(GV);
  }

  for (auto *C : this->DeadConstants) {
    C->removeDeadConstantUsers();
  }

  printStatistic(M, NumOfExpandedGlobalVariables);

  return NumOfExpandedGlobalVariables > 0;
}

static RegisterPass<ExpandGVInitializers>
    X("fuzzalloc-expand-gv-initializers",
      "Expand global variable static initializers", false, false);

static void registerExpandGVInitializersPass(const PassManagerBuilder &,
                                             legacy::PassManagerBase &PM) {
  PM.add(new ExpandGVInitializers());
}

static RegisterStandardPasses RegisterExpandGVInitializersPass(
    PassManagerBuilder::EP_ModuleOptimizerEarly,
    registerExpandGVInitializersPass);

static RegisterStandardPasses
    RegisterExpandGVInitializersPass0(PassManagerBuilder::EP_EnabledOnOptLevel0,
                                      registerExpandGVInitializersPass);
