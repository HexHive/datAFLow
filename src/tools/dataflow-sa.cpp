//===-- dataflow-sa.cpp - Static def/use chain analysis -------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Perform a static def/use chain analysis.
///
//===----------------------------------------------------------------------===//

#include <vector>

#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/JSON.h>
#include <llvm/Support/WithColor.h>

#include "SVF-FE/LLVMModule.h"
#include "SVF-FE/SVFIRBuilder.h"
#include "Util/config.h"
#include "WPA/Andersen.h"

#include "Config.h"

using namespace llvm;
using namespace SVF;

namespace {
//
// Constants
//

static constexpr const char *kTaggedMalloc = "__tagged_malloc";
static constexpr const char *kTaggedCalloc = "__tagged_calloc";
static constexpr const char *kTaggedRealloc = "__tagged_realloc";

static const auto &error_stream = []() -> raw_ostream & {
  return WithColor{errs(), HighlightColor::Error} << "[!] ";
};
static const auto &status_stream = []() -> raw_ostream & {
  return WithColor{outs(), HighlightColor::Remark} << "[*] ";
};
static const auto &success_stream = []() -> raw_ostream & {
  return WithColor{outs(), HighlightColor::String} << "[+] ";
};

//
// Command-line options
//

static cl::OptionCategory Cat("Static def/use chain analysis");
static cl::opt<std::string> BCFilename(cl::Positional, cl::desc("<BC file>"),
                                       cl::value_desc("path"), cl::Required,
                                       cl::cat(Cat));
static cl::opt<std::string> OutJSON("json", cl::desc("Output JSON"),
                                    cl::value_desc("path"), cl::cat(Cat));

//
// Helper functions
//

} // anonymous namespace

int main(int argc, char *argv[]) {
  cl::HideUnrelatedOptions(Cat);
  cl::ParseCommandLineOptions(argc, argv, "Static def/use chain analysis");

  // Parse bitcode
  status_stream() << "Parsing " << BCFilename << "...\n";
  LLVMContext Ctx;
  SMDiagnostic Err;
  auto Mod = parseIRFile(BCFilename, Err, Ctx);
  if (!Mod) {
    error_stream() << "Failed to parse `" << BCFilename
                   << "`: " << Err.getMessage() << '\n';
    ::exit(1);
  }

  status_stream() << "Doing pointer analysis...\n";

  // Initialize external API
  auto *Externals = ExtAPI::getExtAPI(kExtAPIPath);

  auto *SVFMod = LLVMModuleSet::getLLVMModuleSet()->buildSVFModule(*Mod);
  SVFMod->buildSymbolTableInfo();

  // Build SVF IR
  auto *IR = [&SVFMod]() {
    SVFIRBuilder Builder;
    return Builder.build(SVFMod);
  }();

  // Build pointer analysis
  auto *Ander = AndersenWaveDiff::createAndersenWaveDiff(IR);
  auto *VFG = [&Ander]() {
    SVFGBuilder Builder(/*WithIndCall=*/true);
    return Builder.buildFullSVFG(Ander);
  }();

  ValueMap<const Value *, Set<const Value *>> DefUseChains;

  // Get definitions
  SmallVector<const VFGNode *, 0> Defs;

  status_stream() << "Collecting definitions...\n";
  for (const auto *CS : IR->getCallSiteSet()) {
    const auto *F = SVFUtil::getCallee(CS->getCallSite());
    if (!F) {
      continue;
    }
    if (F->getName() == kTaggedMalloc || F->getName() == kTaggedCalloc ||
        F->getName() == kTaggedRealloc) {
      assert((Externals->is_alloc(F) || Externals->is_realloc(F)) &&
             "Tagged function must (re)allocate");
      const auto *PAGNode = IR->getGNode(IR->getValueNode(CS->getCallSite()));
      const auto *VNode = VFG->getDefSVFGNode(PAGNode);
      Defs.push_back(VNode);
    }
  }
  if (Defs.empty()) {
    error_stream() << "Failed to collect any def sites\n";
    ::exit(1);
  }
  success_stream() << "Collected " << Defs.size() << " def sites\n";

  // Collect uses
  FIFOWorkList<const VFGNode *> Worklist;
  Set<const VFGNode *> Visited;

  status_stream() << "Collecting uses...\n";
  for (const auto *Def : Defs) {
    Worklist.clear();
    Visited.clear();

    Worklist.push(Def);
    while (!Worklist.empty()) {
      const auto *Node = Worklist.pop();
      for (const auto *Edge : Node->getOutEdges()) {
        const auto *Succ = Edge->getDstNode();
        if (Visited.insert(Succ).second) {
          Worklist.push(Succ);
        }
      }
    }

    for (const auto *Use : Visited) {
      const auto *PAGNode = VFG->getLHSTopLevPtr(Use);
      if (PAGNode) {
        const auto *V = PAGNode->getValue();
        DefUseChains[Def->getValue()].insert(V);
      }
    }
  }

  // Save Output JSON
  if (!OutJSON.empty()) {
    std::string S;
    raw_string_ostream SS{S};

    std::vector<json::Value> J, JUses;
    J.reserve(DefUseChains.size());

    status_stream() << "Writing to " << OutJSON << "...\n";
    for (const auto &[Def, Uses] : DefUseChains) {
      JUses.clear();
      JUses.reserve(Uses.size());

      for (const auto *Use : Uses) {
        Use->print(SS, /*IsForDebug=*/true);
        JUses.push_back(SS.str());
        S.clear();
      }

      Def->print(SS, /*IsForDebug=*/true);
      J.push_back({SS.str(), JUses});
      S.clear();
    }

    std::error_code EC;
    raw_fd_ostream OS{OutJSON, EC, sys::fs::OF_Text};
    if (EC) {
      error_stream() << "Unable to open " << OutJSON << '\n';
      ::exit(1);
    }

    OS << json::Value{J};
    OS.flush();
    OS.close();
  }

  // Cleanup
  AndersenWaveDiff::releaseAndersenWaveDiff();
  SVFIR::releaseSVFIR();
  LLVMModuleSet::releaseLLVMModuleSet();
  llvm_shutdown();

  return 0;
}
