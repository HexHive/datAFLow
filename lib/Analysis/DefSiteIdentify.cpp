//===-- DefSiteIdentify.cpp - Identify def sites to track -------*- C++ -*-===//
///
/// \file
/// Identify def sites to track
///
//===----------------------------------------------------------------------===//

#include <llvm/Analysis/MemoryBuiltins.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/Pass.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Transforms/IPO/PassManagerBuilder.h>

#include "fuzzalloc/Analysis/DefSiteIdentify.h"
#include "fuzzalloc/Analysis/VariableRecovery.h"
#include "fuzzalloc/Streams.h"

using namespace llvm;

#define DEBUG_TYPE "fuzzalloc-def-site-identify"

namespace {
//
// Command-line options
//

static cl::bits<DefSiteIdentify::DefSiteTypes> ClDefSitesToTrack(
    cl::desc("Def site type (to track)"),
    cl::values(clEnumValN(DefSiteIdentify::DefSiteTypes::Array,
                          "fuzzalloc-def-array", "Track arrays (defs)"),
               clEnumValN(DefSiteIdentify::DefSiteTypes::Struct,
                          "fuzzalloc-def-struct", "Track structs (defs)")));
static cl::opt<bool>
    ClIgnoreGlobalConstants("fuzzalloc-def-ignore-constant-globals",
                            cl::desc("Ignore constant globals"), cl::Hidden,
                            cl::init(false));

//
// Global variables
//

static unsigned NumDefSites = 0;
} // anonymous namespace

char DefSiteIdentify::ID = 0;

void DefSiteIdentify::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<VariableRecovery>();
  AU.setPreservesAll();
}

bool DefSiteIdentify::runOnModule(Module &M) {
  const auto &Vars = getAnalysis<VariableRecovery>().getVariables();

  if (ClDefSitesToTrack.isSet(DefSiteTypes::Array)) {
    status_stream() << "[" << M.getName() << "] Tracking array def sites\n";
  }
  if (ClDefSitesToTrack.isSet(DefSiteTypes::Struct)) {
    status_stream() << "[" << M.getName() << "] Tracking struct def sites\n";
  }

  for (const auto &[V, VI] : Vars) {
    const auto *Ty = VI.getType();
    if (const auto *GV = dyn_cast<GlobalVariable>(V)) {
      if (ClIgnoreGlobalConstants && GV->isConstant()) {
        continue;
      }
    }

    // Save the variable's definition if it's one we want to track
    if (ClDefSitesToTrack.isSet(DefSiteTypes::Array) && isa<ArrayType>(Ty)) {
      ToTrack.insert(V);
    } else if (ClDefSitesToTrack.isSet(DefSiteTypes::Struct) &&
               isa<StructType>(Ty)) {
      ToTrack.insert(V);
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
