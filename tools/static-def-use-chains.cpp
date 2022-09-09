//===-- static-def-use-chains.cpp - Static dev/use analysis -----*- C++ -*-===//
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

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/InitializePasses.h>
#include <llvm/Pass.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/JSON.h>
#include <llvm/Support/WithColor.h>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"

#include "SVF-FE/LLVMModule.h"
#include "SVF-FE/SVFIRBuilder.h"
#include "Util/config.h"
#include "WPA/Andersen.h"

#include "fuzzalloc/Analysis/MemFuncIdentify.h"
#include "fuzzalloc/Analysis/VariableRecovery.h"
#include "fuzzalloc/Metadata.h"
#include "fuzzalloc/Streams.h"
#include "fuzzalloc/fuzzalloc.h"

using namespace llvm;
using namespace SVF;

namespace {
static std::string getNameOrAsOperand(const Value *V) {
  if (!V->getName().empty()) {
    return std::string{V->getName()};
  }

  std::string Name;
  raw_string_ostream OS{Name};
  V->printAsOperand(OS, false);
  return OS.str();
}
} // anonymous namespace

namespace dataflow {
//
// Classes
//

/// A variable definition
struct Def {
  Def(const VFGNode *Node, const DIVariable *DIVar, const DebugLoc *Loc)
      : Node(Node), Val(Node->getValue()), DIVar(DIVar), Loc(Loc) {}

  bool operator==(const Def &Other) const { return Node == Other.Node; }

  const VFGNode *Node;
  const Value *Val;
  const DIVariable *DIVar;
  const DebugLoc *Loc;
};

/// A variable use
struct Use {
  Use(const VFGNode *Node)
      : Node(Node), Val(Node->getValue()),
        Loc(cast<Instruction>(Val)->getDebugLoc()) {}

  bool operator==(const Use &Other) const { return Node == Other.Node; }

  const VFGNode *Node;
  const Value *Val;
  const DebugLoc &Loc;
};
} // namespace dataflow

namespace std {
template <> struct hash<dataflow::Def> {
  size_t operator()(const dataflow::Def &Def) const {
    return hash<const VFGNode *>()(Def.Node);
  }
};

template <> struct hash<dataflow::Use> {
  size_t operator()(const dataflow::Use &Use) const {
    return hash<const VFGNode *>()(Use.Node);
  }
};
} // namespace std

namespace dataflow {
//
// Aliases
//

using DefSet = absl::flat_hash_set<dataflow::Def>;
using UseSet = absl::flat_hash_set<dataflow::Use>;
using DefUseMap = absl::flat_hash_map<dataflow::Def, UseSet>;

//
// JSON helpers
//

static json::Value toJSON(const dataflow::Def &Def) {
  const auto &VarName = [&]() -> std::string {
    if (const auto *DIVar = Def.DIVar) {
      return DIVar->getName().str();
    }
    return getNameOrAsOperand(Def.Val);
  }();

  const auto &File = [&]() -> Optional<StringRef> {
    if (const auto *DIVar = Def.DIVar) {
      return DIVar->getFilename();
    }
    return None;
  }();

  const auto &Func = [&]() -> Optional<StringRef> {
    if (const auto *Local = dyn_cast_or_null<DILocalVariable>(Def.DIVar)) {
      return getDISubprogram(Local->getScope())->getName();
    } else if (const auto *Inst = dyn_cast<Instruction>(Def.Val)) {
      return Inst->getFunction()->getName();
    }
    return None;
  }();

  const auto &Line = [&]() -> Optional<unsigned int> {
    if (const auto *DIVar = Def.DIVar) {
      return DIVar->getLine();
    }
    return None;
  }();

  const auto &Col = [&]() -> Optional<unsigned int> {
    if (const auto *Loc = Def.Loc) {
      return Loc->get()->getColumn();
    }
    return None;
  }();

  return {VarName, {File, Func, Line, Col}};
}

static json::Value toJSON(const dataflow::Use &Use) {
  const auto &File = [&]() -> Optional<StringRef> {
    if (const auto &Loc = Use.Loc) {
      auto *SP = getDISubprogram(Loc.getScope());
      return SP->getFile()->getFilename();
    }
    return None;
  }();

  const auto &Func = [&]() -> Optional<StringRef> {
    if (const auto &Loc = Use.Loc) {
      return getDISubprogram(Loc->getScope())->getName();
    } else if (const auto *Inst = dyn_cast<Instruction>(Use.Val)) {
      return Inst->getFunction()->getName();
    }
    return None;
  }();

  const auto &Line = [&]() -> Optional<unsigned int> {
    if (const auto &Loc = Use.Loc) {
      return Loc->getLine();
    }
    return None;
  }();

  const auto &Col = [&]() -> Optional<unsigned int> {
    if (const auto &Loc = Use.Loc) {
      return Loc->getColumn();
    }
    return None;
  }();

  return {File, Func, Line, Col};
}

static json::Value toJSON(const UseSet &Uses) {
  std::vector<json::Value> J;
  J.reserve(Uses.size());

  for (const auto &U : Uses) {
    J.push_back(U);
  }

  return J;
}
} // namespace dataflow

namespace {
//
// Command-line options
//

static cl::OptionCategory Cat("Static def/use chain analysis");
static cl::opt<std::string> BCFilename(cl::Positional, cl::desc("<BC file>"),
                                       cl::value_desc("path"), cl::Required,
                                       cl::cat(Cat));
static cl::opt<std::string> OutJSON("out", cl::desc("Output JSON"),
                                    cl::value_desc("path"), cl::cat(Cat));

//
// Helper functions
//

static bool isTaggedVar(const Value *V) {
  if (!V) {
    return false;
  }
  if (const auto *I = dyn_cast<Instruction>(V)) {
    return I->getMetadata(kFuzzallocTagVarMD) != nullptr;
  } else if (const auto *GO = dyn_cast<GlobalObject>(V)) {
    return GO->getMetadata(kFuzzallocTagVarMD) != nullptr;
  }
  return false;
}

static bool isInstrumentedDeref(const Value *V) {
  if (!V) {
    return false;
  }
  if (const auto *I = dyn_cast<Instruction>(V)) {
    return I->getMetadata(kFuzzallocInstrumentedUseSiteMD) != nullptr;
  }
  return false;
}
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

  // Recover source-level variables
  status_stream() << "Running LLVM passes...\n";

  auto &Registry = *PassRegistry::getPassRegistry();
  initializeCore(Registry);
  initializeAnalysis(Registry);

  legacy::PassManager PM;
  auto *RecoverVars = new VariableRecovery;
  auto *MemFuncId = new MemFuncIdentify;
  PM.add(RecoverVars);
  PM.add(MemFuncId);
  PM.run(*Mod);

  const auto &SrcVars = RecoverVars->getVariables();
  const auto &MemFuncs = MemFuncId->getFuncs();

  // Initialize external API
  auto *Externals = ExtAPI::getExtAPI();
  for (const auto *MemFn : MemFuncs) {
    const auto &Name = MemFn->getName();
    if (Externals->get_type(Name.str()) != ExtAPI::extType::EFT_NULL) {
      continue;
    }

    // XXX This is very hacky
    const StringRef NameLower = Name.lower();
    if (NameLower.contains("malloc") || NameLower.contains("calloc")) {
      Externals->add_entry(Name.str().c_str(), ExtAPI::extType::EFT_ALLOC,
                           true);
    } else if (NameLower.contains("realloc")) {
      Externals->add_entry(Name.str().c_str(), ExtAPI::extType::EFT_REALLOC,
                           true);
    } else if (NameLower.contains("strdup")) {
      Externals->add_entry(Name.str().c_str(),
                           ExtAPI::extType::EFT_NOSTRUCT_ALLOC, true);
    }
  }

  status_stream() << "Doing pointer analysis...\n";

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

  //
  // Get definitions
  //

  dataflow::DefSet Defs;

  status_stream() << "Collecting definitions...\n";
  for (const auto &[ID, PAGNode] : *IR) {
    if (!(isa<ValVar>(PAGNode) && PAGNode->hasValue())) {
      continue;
    }

    auto *Val = PAGNode->getValue();
    if (isTaggedVar(Val)) {
      const auto *VNode = VFG->getDefSVFGNode(PAGNode);
      const auto &SrcVar = SrcVars.lookup(const_cast<Value *>(Val));
      Defs.emplace(VNode, SrcVar.getDbgVar(), SrcVar.getLoc());
    }
  }

  if (Defs.empty()) {
    error_stream() << "Failed to collect any def sites\n";
    ::exit(1);
  }

  success_stream() << "Collected " << Defs.size() << " def sites\n";

  // Collect def-use chains
  FIFOWorkList<const VFGNode *> Worklist;
  Set<const VFGNode *> Visited;
  dataflow::DefUseMap DefUseChains;
  dataflow::UseSet Uses;
  size_t NumDefUseChains = 0;

  status_stream() << "Collecting def-use chains...\n";
  for (const auto &Def : Defs) {
    Worklist.clear();
    Visited.clear();

    Worklist.push(Def.Node);
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
      const auto *UseV = Use->getValue();
      if (!isInstrumentedDeref(UseV)) {
        continue;
      }

      // A use must be a load or store
      const auto *V = [&]() -> const Value * {
        if (const auto *Load = dyn_cast<LoadInst>(UseV)) {
          return Load->getPointerOperand();
        } else if (const auto *Store = dyn_cast<StoreInst>(UseV)) {
          return Store->getPointerOperand();
        }
        llvm_unreachable("use must be a load or store");
      }();

      Uses.emplace(Use);
      if (DefUseChains[Def].emplace(Use).second) {
        NumDefUseChains++;
      }
    }
  }
  success_stream() << "Collected " << Uses.size() << " unique uses\n";
  success_stream() << "Collected " << NumDefUseChains << " def-use chains\n";

  // Save Output JSON
  if (!OutJSON.empty()) {
    const auto &NumDefs = DefUseChains.size();
    json::Array J;
    J.reserve(NumDefs);

    status_stream() << "Serializing def/use chains to JSON...\n";
    for (const auto &DUEnum : enumerate(DefUseChains)) {
      const auto &[Def, Uses] = DUEnum.value();

      J.push_back({Def, Uses});

      const auto &Idx = DUEnum.index();
      if (Idx % ((NumDefs + (10 - 1)) / 10) == 0) {
        status_stream() << "  ";
        write_double(outs(), static_cast<float>(Idx) / NumDefs,
                     FloatStyle::Percent);
        outs() << " defs serialized\r";
      }
    }

    std::error_code EC;
    raw_fd_ostream OS(OutJSON, EC, sys::fs::OF_Text);
    if (EC) {
      error_stream() << "Unable to open " << OutJSON << '\n';
      ::exit(1);
    }

    status_stream() << "Writing to " << OutJSON << "...\n";
    OS << std::move(J);
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
