//===-- VariableRecovery.h - Recover source-level variables -----*- C++ -*-===//
///
/// \file
/// Recover source-level variables through debug information.
///
//===----------------------------------------------------------------------===//

#ifndef VARIABLE_RECOVERY_H
#define VARIABLE_RECOVERY_H

#include <llvm/IR/ValueMap.h>
#include <llvm/Pass.h>

class VariableRecovery : public llvm::ModulePass {
public:
  using SrcVariables = llvm::ValueMap<llvm::Value *, llvm::DIVariable *>;

  static char ID;
  VariableRecovery() : llvm::ModulePass(ID) {}

  virtual void getAnalysisUsage(llvm::AnalysisUsage &) const override;
  virtual bool runOnModule(llvm::Module &) override;

  const SrcVariables &getVariables() const { return Vars; }

private:
  SrcVariables Vars;
};

#endif // VARIABLE_RECOVERY_H
