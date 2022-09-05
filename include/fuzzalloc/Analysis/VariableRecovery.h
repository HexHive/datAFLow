//===-- VariableRecovery.h - Recover source-level variables -----*- C++ -*-===//
///
/// \file
/// Recover source-level variables through debug information.
///
//===----------------------------------------------------------------------===//

#ifndef VARIABLE_RECOVERY_H
#define VARIABLE_RECOVERY_H

#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/ValueMap.h>
#include <llvm/Pass.h>

namespace llvm {
class DebugLoc;
class Type;
} // namespace llvm

/// Information on a source-level variable
class VarInfo {
public:
  VarInfo() = default;
  VarInfo(const llvm::DIVariable *V, const llvm::DebugLoc *DL,
          const llvm::Type *T)
      : DbgVar(V), Loc(DL), Ty(T) {}
  VarInfo(const llvm::DIVariable *V, const llvm::Type *T)
      : VarInfo(V, nullptr, T) {}

  const llvm::DIVariable *getDbgVar() const { return DbgVar; }
  const llvm::Type *getType() const { return Ty; }
  const llvm::DebugLoc *getLoc() const { return Loc; }

  void dump(llvm::raw_ostream &OS) const {
    if (!DbgVar || !Ty) {
      return;
    }

    if (llvm::isa<llvm::DILocalVariable>(DbgVar)) {
      OS << "local variable `" << DbgVar->getName() << "` (type=" << *Ty << ')';
    } else if (llvm::isa<llvm::DIGlobalVariable>(DbgVar)) {
      OS << "global variable `" << DbgVar->getName() << "` (type=" << *Ty
         << ')';
    }
  }

private:
  const llvm::DIVariable *DbgVar; ///< The debug variable
  const llvm::DebugLoc *Loc;      ///< The debug location
  const llvm::Type *Ty;           ///< The actual type
};

llvm::raw_ostream &operator<<(llvm::raw_ostream &OS, const VarInfo &VI);

/// Recover source-level debug variables
class VariableRecovery : public llvm::ModulePass {
public:
  using SrcVariables = llvm::ValueMap<llvm::Value *, VarInfo>;

  static char ID;
  VariableRecovery() : llvm::ModulePass(ID) {}

  virtual void getAnalysisUsage(llvm::AnalysisUsage &) const override;
  virtual bool runOnModule(llvm::Module &) override;

  const SrcVariables &getVariables() const { return Vars; }

private:
  SrcVariables Vars;
};

#endif // VARIABLE_RECOVERY_H
