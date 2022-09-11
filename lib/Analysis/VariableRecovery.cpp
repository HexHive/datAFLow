//===-- VariableRecovery.cpp - Recover source-level variables -------------===//
///
/// \file
/// Recover source-level variables through debug information.
///
//===----------------------------------------------------------------------===//

#include <llvm/IR/DebugInfo.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/SpecialCaseList.h>
#include <llvm/Transforms/IPO/PassManagerBuilder.h>

#include "fuzzalloc/Analysis/VariableRecovery.h"

#define DEBUG_TYPE "fuzzalloc-variable-recovery"

using namespace llvm;

namespace {
//
// Classes
//

class FuncIgnoreList {
private:
  std::unique_ptr<SpecialCaseList> SCL;

public:
  FuncIgnoreList() = default;
  FuncIgnoreList(std::unique_ptr<SpecialCaseList> List)
      : SCL(std::move(List)) {}

  bool isIn(const Function &F) const {
    return SCL && SCL->inSection("", "fun", F.getName());
  }
};

//
// Command-line options
//

static cl::opt<std::string>
    ClFuncIgnoreList("fuzzalloc-def-ignore-funcs",
                     cl::desc("Special case list of functions to ignore"),
                     cl::value_desc("path"));

//
// Global variables
//

static unsigned NumLocalVars = 0;
static unsigned NumGlobalVars = 0;

//
// Helper functions
//

static FuncIgnoreList getFuncIgnoreList() {
  if (ClFuncIgnoreList.empty()) {
    return FuncIgnoreList();
  }

  if (!sys::fs::exists(ClFuncIgnoreList)) {
    std::string Err;
    raw_string_ostream OS(Err);
    OS << "fuzzalloc def ignore function list does not exist at "
       << ClFuncIgnoreList;
    OS.flush();
    report_fatal_error(Err);
  }

  return FuncIgnoreList(SpecialCaseList::createOrDie(
      {ClFuncIgnoreList}, *vfs::getRealFileSystem()));
}
} // anonymous namespace

raw_ostream &operator<<(raw_ostream &OS, const VarInfo &VI) {
  VI.dump(OS);
  return OS;
}

char VariableRecovery::ID = 0;

void VariableRecovery::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.setPreservesAll();
}

bool VariableRecovery::runOnModule(Module &M) {
  const auto &FuncIgnores = getFuncIgnoreList();

  // STEP 1: Local variables
  for (auto &F : M) {
    if (FuncIgnores.isIn(F)) {
      continue;
    }

    for (auto &I : instructions(&F)) {
      // We only care about llvm.dbg.* variable declarations
      if (!isa<DbgVariableIntrinsic>(&I)) {
        continue;
      }

      auto *Var = cast<DbgVariableIntrinsic>(&I)->getVariable();
      const auto *Loc = I.getDebugLoc() ? &I.getDebugLoc() : nullptr;
      LLVM_DEBUG(dbgs() << "Handling local variable `" << Var->getName()
                        << "`\n");

      if (auto *DbgVal = dyn_cast<DbgValueInst>(&I)) {
        auto *V = DbgVal->getValue();
        if (!V) {
          continue;
        }
        auto *Ty = V->getType();
        VarInfo VI(Var, Loc, Ty);
        if (Vars.insert({V, VI}).second) {
          NumLocalVars++;
        }
      } else if (auto *DbgDecl = dyn_cast<DbgDeclareInst>(&I)) {
        auto *Addr = DbgDecl->getAddress();
        if (!Addr) {
          continue;
        }
        auto *Ty = Addr->getType()->isPointerTy()
                       ? Addr->getType()->getPointerElementType()
                       : Addr->getType();
        VarInfo VI(Var, Loc, Ty);
        if (Vars.insert({Addr, VI}).second) {
          NumLocalVars++;
        }
      } else if (auto *DbgAddr = dyn_cast<DbgAddrIntrinsic>(&I)) {
        llvm_unreachable("llvm.dbg.addr not supported");
      }
    }
  }

  // STEP 2: Global variables
  SmallVector<DIGlobalVariableExpression *, 16> GVExprs;
  for (auto &GV : M.globals()) {
    GVExprs.clear();
    GV.getDebugInfo(GVExprs);
    for (auto *GVExpr : GVExprs) {
      if (auto *Var = GVExpr->getVariable()) {
        LLVM_DEBUG(dbgs() << "Handling global variable `" << Var->getName()
                          << "`\n");
        VarInfo VI(Var, GV.getValueType());
        if (Vars.insert({&GV, VI}).second) {
          NumGlobalVars++;
        }
      }
    }
  }

  return false;
}

//
// Pass registration
//

static RegisterPass<VariableRecovery>
    X(DEBUG_TYPE, "Recover source-level variables", true, true);

static void registerVariableRecoveryPass(const PassManagerBuilder &,
                                         legacy::PassManagerBase &PM) {
  PM.add(new VariableRecovery());
}

static RegisterStandardPasses
    RegisterVariableRecoveryPass(PassManagerBuilder::EP_ModuleOptimizerEarly,
                                 registerVariableRecoveryPass);

static RegisterStandardPasses RegisterInstrumentVariableRecoveryPass0(
    PassManagerBuilder::EP_EnabledOnOptLevel0, registerVariableRecoveryPass);
