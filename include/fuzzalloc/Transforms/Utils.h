//===-- Utils.h - Transform utils -------------------------------*- C++ -*-===//
///
/// \file
/// Transformation utilities
///
//===----------------------------------------------------------------------===//

#ifndef TRANSFORM_UTILS_H
#define TRANSFORM_UTILS_H

#include <llvm/IR/Instructions.h>

/// Find a safe insertion point, ensuring PHI nodes are not broken (as they must
/// always be the first instruction in a block)
llvm::Instruction *phiSafeInsertPt(llvm::Use *U) {
  auto *InsertPt = llvm::cast<llvm::Instruction>(U->getUser());
  if (auto *PN = llvm::dyn_cast<llvm::PHINode>(InsertPt)) {
    InsertPt = PN->getIncomingBlock(*U)->getTerminator();
  }
  return InsertPt;
}

void phiSafeReplaceUses(llvm::Use *U, llvm::Value *NewVal) {
  if (auto *PN = llvm::dyn_cast<llvm::PHINode>(U->getUser())) {
    // A PHI node can have multiple incoming edges from the same
    // block, in which case all these edges must have the same
    // incoming value.
    auto *BB = PN->getIncomingBlock(*U);
    for (unsigned I = 0; I < PN->getNumIncomingValues(); ++I) {
      if (PN->getIncomingBlock(I) == BB) {
        PN->setIncomingValue(I, NewVal);
      }
    }
  } else {
    U->getUser()->replaceUsesOfWith(U->get(), NewVal);
  }
}

#endif
