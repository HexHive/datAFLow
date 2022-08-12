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
  using DefSites = llvm::SmallPtrSet<llvm::Value *, 32>;

  /// Trackable def sites
  enum DefSiteTypes {
    Array,
    Struct,
  };

  static char ID;
  DefSiteIdentify() : llvm::ModulePass(ID) {}

  virtual void getAnalysisUsage(llvm::AnalysisUsage &) const override;
  virtual bool runOnModule(llvm::Module &) override;
  virtual void print(llvm::raw_ostream &, const llvm::Module *) const override;

  const DefSites &getDefSites() const { return ToTrack; }

  static bool trackArrays();
  static bool trackStructs();
  static bool trackDynAllocs();

private:
  DefSites ToTrack;
};

#endif // DEF_SITE_IDENTIFY_H
