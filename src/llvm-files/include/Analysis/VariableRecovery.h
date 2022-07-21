//===-- VariableRecovery.h - Recover source-level variables ---------------===//
///
/// \file
/// Recover source-level variables through debug information.
///
//===----------------------------------------------------------------------===//

#ifndef VARIABLE_RECOVERY_H
#define VARIABLE_RECOVERY_H

#include <llvm/ADT/ValueMap.h>
#include <llvm/Pass.h>

class VariableRecovery : public llvm::ModulePass {
public:
  using SrcVariables =
      llvm::ValueMap<const llvm::Value *, const llvm::DIVariable *>;

  static char ID;
  VariableRecovery() : llvm::ModulePass(ID) {}
  virtual void releaseMemory() override { Vars.clear(); }
  virtual bool runOnModule(llvm::Module &) override;

  const SrcVariables &getVariables() const { return Vars; }

private:
  SrcVariables Vars;
};

#endif // VARIABLE_RECOVERY_H
