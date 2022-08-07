//===-- UseSiteIdentify.h - Identify use sites to track ---------*- C++ -*-===//
///
/// \file
/// Identify use sites to track
///
//===----------------------------------------------------------------------===//

#ifndef USE_SITE_IDENTIFY_H
#define USE_SITE_IDENTIFY_H

#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/Pass.h>

namespace llvm {
class Value;
} // namespace llvm

/// Identify use sites
class UseSiteIdentify : public llvm::FunctionPass {
public:
  using UseSites = llvm::SmallPtrSet<const llvm::Value *, 32>;

  /// Trackable use sites
  enum UseSiteTypes {
    Read,
    Write,
    Access,
  };

  static char ID;
  UseSiteIdentify() : llvm::FunctionPass(ID) {}

  virtual void getAnalysisUsage(llvm::AnalysisUsage &) const override;
  virtual bool runOnFunction(llvm::Function &) override;

  const UseSites &getUseSites() const { return ToTrack; }

private:
  UseSites ToTrack;
};

#endif // USE_SITE_IDENTIFY_H
