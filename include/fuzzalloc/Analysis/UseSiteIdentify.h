//===-- UseSiteIdentify.h - Identify use sites to track ---------*- C++ -*-===//
///
/// \file
/// Identify use sites to track
///
//===----------------------------------------------------------------------===//

#ifndef USE_SITE_IDENTIFY_H
#define USE_SITE_IDENTIFY_H

#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/ValueMap.h>
#include <llvm/Pass.h>
#include <llvm/Transforms/Instrumentation/AddressSanitizerCommon.h>

namespace llvm {
class AllocaInst;
class Value;
} // namespace llvm

/// Identify use sites
///
/// This is mostly based on how AddressSanitizer selects instrumentation sites
class UseSiteIdentify : public llvm::ModulePass {
public:
  using UseSiteOperands = llvm::SmallVector<llvm::InterestingMemoryOperand, 32>;

  /// Trackable use sites
  enum UseSiteTypes {
    Read,
    Write,
  };

  static char ID;
  UseSiteIdentify() : llvm::ModulePass(ID) {}

  virtual void getAnalysisUsage(llvm::AnalysisUsage &) const override;
  virtual bool runOnModule(llvm::Module &) override;

  UseSiteOperands *getUseSites(llvm::Function &F);

private:
  bool isInterestingAlloca(const llvm::AllocaInst *);
  bool ignoreAccess(const llvm::Value *);
  void getInterestingMemoryOperands(llvm::Instruction *, UseSiteOperands &);

  bool runOnFunction(llvm::Function &);

  llvm::ValueMap<llvm::Function *, UseSiteOperands> ToTrack;
  llvm::ValueMap<const llvm::AllocaInst *, bool> ProcessedAllocas;
};

#endif // USE_SITE_IDENTIFY_H
