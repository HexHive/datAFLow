//===-- DefUseChain.cpp - Static def-use analysis ---------------*- C++ -*-===//
///
/// \file
/// Perform a static def-use chain analysis
///
//===----------------------------------------------------------------------===//

#include <llvm/IR/LegacyPassManager.h>
#include <llvm/Transforms/IPO/PassManagerBuilder.h>

#include "SVF-FE/LLVMModule.h"
#include "SVF-FE/SVFIRBuilder.h"
#include "Util/config.h"
#include "WPA/Andersen.h"

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
// Def-use chains
//

char DefUseChain::ID = 0;

DefUseChain::~DefUseChain() {
  AndersenWaveDiff::releaseAndersenWaveDiff();
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

  auto *SVFMod = LLVMModuleSet::getLLVMModuleSet()->buildSVFModule(M);
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
