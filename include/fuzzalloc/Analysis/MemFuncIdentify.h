//===-- MemFuncIdentify.h - Identify memory allocation funcs ----*- C++ -*-===//
///
/// \file
/// Identify dynamic memory allocation function calls
///
//===----------------------------------------------------------------------===//

#ifndef MEM_FUNC_IDENTIFY_H
#define MEM_FUNC_IDENTIFY_H

#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/IR/ValueMap.h>
#include <llvm/Pass.h>

namespace llvm {
class Function;
class User;
class Value;
} // namespace llvm

/// Identify dynamic memory allocation function calls
class MemFuncIdentify : public llvm::ModulePass {
public:
  using DynMemoryFunctionUsers =
      llvm::ValueMap</* Memory allocation function */ const llvm::Function *,
                     /* Users */ llvm::SmallPtrSet<const llvm::Value *, 16>>;

  static char ID;
  MemFuncIdentify() : llvm::ModulePass(ID) {}

  virtual void getAnalysisUsage(llvm::AnalysisUsage &) const override;
  virtual bool runOnModule(llvm::Module &) override;

  const DynMemoryFunctionUsers &getFuncs() { return MemFuncUsers; }

private:
  void getMemoryBuiltins(const llvm::Function &);

  llvm::SmallPtrSet<const llvm::Function *, 8> MemFuncs;
  DynMemoryFunctionUsers MemFuncUsers;
};

#endif // MEM_FUNC_IDENTIFY_H
