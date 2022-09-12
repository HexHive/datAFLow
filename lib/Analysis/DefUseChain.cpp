//===-- DefUseChain.cpp - Static def-use analysis ---------------*- C++ -*-===//
///
/// \file
/// Perform a static def-use chain analysis
///
//===----------------------------------------------------------------------===//

#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/Transforms/IPO/PassManagerBuilder.h>

#include "Graphs/VFG.h"
#include "MemoryModel/PointerAnalysis.h"
#include "SVF-FE/LLVMModule.h"
#include "SVF-FE/SVFIRBuilder.h"
#include "Util/Options.h"
#include "WPA/Andersen.h"
#include "WPA/AndersenSFR.h"
#include "WPA/Steensgaard.h"
#include "WPA/TypeAnalysis.h"

#include "fuzzalloc/Analysis/DefUseChain.h"
#include "fuzzalloc/Analysis/MemFuncIdentify.h"
#include "fuzzalloc/Analysis/VariableRecovery.h"
#include "fuzzalloc/Metadata.h"
#include "fuzzalloc/Streams.h"
#include "fuzzalloc/fuzzalloc.h"

#define DEBUG_TYPE "fuzzalloc-def-use-chain"

using namespace llvm;
using namespace SVF;

namespace {
//
// Helper functions
//

static std::string getNameOrAsOperand(const Value *V) {
  if (!V->getName().empty()) {
    return std::string{V->getName()};
  }

  std::string Name;
  raw_string_ostream OS{Name};
  V->printAsOperand(OS, false);
  return OS.str();
}

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

//
// Definition site
//

DefSite::DefSite(const VFGNode *N, const DIVariable *DIVar, const DebugLoc *DL)
    : Node(N), Val(Node->getValue()), DIVar(DIVar), Loc(DL) {}

//
// Use site
//

UseSite::UseSite(const VFGNode *N)
    : Node(N), Val(N->getValue()), Loc(cast<Instruction>(Val)->getDebugLoc()) {}

//
// Def-use chains
//

char DefUseChain::ID = 0;

DefUseChain::~DefUseChain() {
  delete WPA;
  SVFIR::releaseSVFIR();
  LLVMModuleSet::releaseLLVMModuleSet();
}

void DefUseChain::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<VariableRecovery>();
  AU.addRequired<MemFuncIdentify>();
  AU.setPreservesAll();
}

bool DefUseChain::runOnModule(Module &M) {
  const auto &SrcVars = getAnalysis<VariableRecovery>().getVariables();
  const auto &MemFuncs = getAnalysis<MemFuncIdentify>().getFuncs();

  // Initialize external API
  auto *Externals = ExtAPI::getExtAPI();
  for (const auto *MemFn : MemFuncs) {
    const auto &Name = MemFn->getName();
    if (Externals->get_type(Name.str()) != ExtAPI::extType::EFT_NULL) {
      continue;
    }

    // XXX This is very hacky
    if (StringRef(Name.lower()).contains("malloc") ||
        StringRef(Name.lower()).contains("calloc")) {
      Externals->add_entry(Name.str().c_str(), ExtAPI::extType::EFT_ALLOC,
                           true);
    } else if (StringRef(Name.lower()).contains("realloc")) {
      Externals->add_entry(Name.str().c_str(), ExtAPI::extType::EFT_REALLOC,
                           true);
    } else if (StringRef(Name.lower()).contains("strdup")) {
      Externals->add_entry(Name.str().c_str(),
                           ExtAPI::extType::EFT_NOSTRUCT_ALLOC, true);
    }
  }

  auto *SVFMod = LLVMModuleSet::getLLVMModuleSet()->buildSVFModule(M);
  SVFMod->buildSymbolTableInfo();

  // Build SVF IR
  auto *IR = [&SVFMod]() {
    SVFIRBuilder Builder;
    return Builder.build(SVFMod);
  }();

  // Build and run pointer analysis

  // SVF allows multiple pointer analyses to be specified. We only accept the
  // first
  status_stream() << "Doing pointer analysis (";
  WPA = [&]() -> BVDataPTAImpl * {
    if (Options::PASelected.isSet(PointerAnalysis::Andersen_WPA)) {
      outs() << "Standard inclusion-based";
      return new Andersen(IR);
    } else if (Options::PASelected.isSet(PointerAnalysis::AndersenSCD_WPA)) {
      outs() << "Selective cycle detection inclusion-based";
      return new AndersenSCD(IR);
    } else if (Options::PASelected.isSet(PointerAnalysis::AndersenSFR_WPA)) {
      outs() << "Stride-based field representation inclusion-based";
      return new AndersenSFR(IR);
    } else if (Options::PASelected.isSet(
                   PointerAnalysis::AndersenWaveDiff_WPA)) {
      outs() << "Diff wave propagation inclusion-based";
      return new AndersenWaveDiff(IR);
    } else if (Options::PASelected.isSet(PointerAnalysis::Steensgaard_WPA)) {
      outs() << "Steensgaard";
      return new Steensgaard(IR);
    } else if (Options::PASelected.isSet(PointerAnalysis::FSSPARSE_WPA)) {
      outs() << "Sparse flow-sensitive";
      return new FlowSensitive(IR);
    } else if (Options::PASelected.isSet(PointerAnalysis::VFS_WPA)) {
      outs() << "Versioned sparse flow-sensitive";
      return new VersionedFlowSensitive(IR);
    } else if (Options::PASelected.isSet(PointerAnalysis::TypeCPP_WPA)) {
      outs() << "Type-based fast";
      return new TypeAnalysis(IR);
    } else {
      llvm_unreachable("Unsupported pointer analysis");
    }
  }();
  outs() << ")...\n";

  WPA->analyze();
  auto *VFG = [&]() {
    SVFGBuilder Builder(/*WithIndCall=*/true);
    return Builder.buildFullSVFG(WPA);
  }();

  //
  // Get definitions
  //

  DefSet Defs;

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
  UseSet Uses;
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

      // Save uses
      Uses.emplace(Use);
      if (DefUses[Def].emplace(Use).second) {
        NumDefUseChains++;
      }
    }
  }

  success_stream() << "Collected " << Uses.size() << " unique uses\n";
  success_stream() << "Collected " << NumDefUseChains << " def-use chains\n";

  return false;
}

//
// JSON helpers
//

json::Value toJSON(const DefSite &Def) {
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

json::Value toJSON(const UseSite &Use) {
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

json::Value toJSON(const DefUseChain::UseSet &Uses) {
  std::vector<json::Value> J;
  J.reserve(Uses.size());

  for (const auto &U : Uses) {
    J.push_back(U);
  }

  return J;
}

//
// Pass registration
//

static RegisterPass<DefUseChain> X(DEBUG_TYPE, "Def-use chain analysis", true,
                                   true);

static void registerDefUseChainPass(const PassManagerBuilder &,
                                    legacy::PassManagerBase &PM) {
  PM.add(new DefUseChain());
}

static RegisterStandardPasses
    RegisterDefUseChainPass(PassManagerBuilder::EP_OptimizerLast,
                            registerDefUseChainPass);

static RegisterStandardPasses
    RegisterDefUseChainPass0(PassManagerBuilder::EP_EnabledOnOptLevel0,
                             registerDefUseChainPass);
