//===-- DefSiteIdentify.cpp - Identify def sites to track -------*- C++ -*-===//
///
/// \file
/// Identify def sites to track
///
//===----------------------------------------------------------------------===//

#include <llvm/ADT/Statistic.h>
#include <llvm/Analysis/MemoryBuiltins.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/Pass.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Transforms/IPO/PassManagerBuilder.h>

#include "fuzzalloc/Analysis/DefSiteIdentify.h"
#include "fuzzalloc/Analysis/MemFuncIdentify.h"
#include "fuzzalloc/Analysis/VariableRecovery.h"

using namespace llvm;

#define DEBUG_TYPE "fuzzalloc-def-site-identify"

STATISTIC(NumDefSites, "Number of def sites identified");

namespace {

//
// Command-line options
//

static cl::bits<DefSiteIdentify::DefSiteTypes> ClDefSitesToTrack(
    cl::desc("Def site type (to track)"),
    cl::values(clEnumValN(DefSiteIdentify::DefSiteTypes::Array,
                          "fuzzalloc-def-array", "Track arrays (defs)"),
               clEnumValN(DefSiteIdentify::DefSiteTypes::Struct,
                          "fuzzalloc-def-struct", "Track structs (defs)"),
               clEnumValN(DefSiteIdentify::DefSiteTypes::DynAlloc,
                          "fuzzalloc-def-dyn-alloc",
                          "Track dynamic memory allocations (defs)")));
static cl::opt<bool>
    ClIgnoreGlobalConstants("fuzzalloc-ignore-constant-globals",
                            cl::desc("Ignore constant globals"));
} // anonymous namespace

char DefSiteIdentify::ID = 0;

void DefSiteIdentify::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.setPreservesAll();
  AU.addRequired<MemFuncIdentify>();
  AU.addRequired<VariableRecovery>();
}

bool DefSiteIdentify::runOnModule(Module &M) {
  const auto &MemFuncs = getAnalysis<MemFuncIdentify>().getFuncs();
  const auto &Vars = getAnalysis<VariableRecovery>().getVariables();

  for (const auto &[V, VI] : Vars) {
    const auto *Ty = VI.getType();
    if (const auto *GV = dyn_cast<GlobalVariable>(V)) {
      if (ClIgnoreGlobalConstants && GV->isConstant()) {
        continue;
      }
    }

    // Save the variable's definition if it's one we want to track
    if (trackArrays() && isa<ArrayType>(Ty)) {
      ToTrack.insert(V);
    } else if (trackStructs() && isa<StructType>(Ty)) {
      ToTrack.insert(V);
    } else if (trackDynAllocs() && isa<PointerType>(Ty) &&
               isa<CallBase>(V->stripPointerCasts())) {
      auto *CB = cast<CallBase>(V->stripPointerCasts());
      if (MemFuncs.count(CB->getCalledFunction()) > 0) {
        ToTrack.insert(CB);
      }
    }
  }

  NumDefSites = ToTrack.size();

  return false;
}

void DefSiteIdentify::print(raw_ostream &OS, const Module *M) const {
  const auto &Vars = getAnalysis<VariableRecovery>().getVariables();
  for (const auto *Def : ToTrack) {
    OS << Vars.lookup(const_cast<Value *>(Def)) << " def: `" << *Def << "`\n";
  }
}

bool DefSiteIdentify::trackArrays() {
  return ClDefSitesToTrack.isSet(DefSiteTypes::Array);
}

bool DefSiteIdentify::trackStructs() {
  return ClDefSitesToTrack.isSet(DefSiteTypes::Struct);
}

bool DefSiteIdentify::trackDynAllocs() {
  return ClDefSitesToTrack.isSet(DefSiteTypes::DynAlloc);
}

//
// Pass registration
//

static RegisterPass<DefSiteIdentify> X(DEBUG_TYPE, "Identify def sites", true,
                                       true);

static void registerDefSiteIdentifyPass(const PassManagerBuilder &,
                                        legacy::PassManagerBase &PM) {
  PM.add(new DefSiteIdentify());
}

static RegisterStandardPasses
    RegisterDefSiteIdentifyPass(PassManagerBuilder::EP_OptimizerLast,
                                registerDefSiteIdentifyPass);

static RegisterStandardPasses
    RegisterDefSiteIdentifyPass0(PassManagerBuilder::EP_EnabledOnOptLevel0,
                                 registerDefSiteIdentifyPass);
