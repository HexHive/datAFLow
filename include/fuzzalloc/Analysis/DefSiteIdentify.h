//===-- DefSiteIdentify.h - Identify def sites to track ---------*- C++ -*-===//
///
/// \file
/// Identify def sites to track
///
//===----------------------------------------------------------------------===//

#ifndef DEF_SITE_IDENTIFY_H
#define DEF_SITE_IDENTIFY_H

#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/Pass.h>

namespace llvm {
class Value;
} // namespace llvm

/// Identify def sites
class DefSiteIdentify : public llvm::ModulePass {
public:
  using DefSites = llvm::SmallPtrSet<const llvm::Value *, 16>;

  /// Trackable def sites
  enum DefSiteTypes {
    Array,
    Struct,
    DynAlloc,
  };

  static char ID;
  DefSiteIdentify() : llvm::ModulePass(ID) {}

  virtual void getAnalysisUsage(llvm::AnalysisUsage &) const override;
  virtual bool runOnModule(llvm::Module &) override;

  const DefSites &getDefSites() const { return ToTrack; }

  static bool trackArrays();
  static bool trackStructs();
  static bool trackDynAllocs();

private:
  DefSites ToTrack;
};

#endif // DEF_SITE_IDENTIFY_H
