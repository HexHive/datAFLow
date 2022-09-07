//===-- Utils.h - Common instrumentation functionality ----------*- C++ -*-===//
///
/// \file
/// Common instrumentation functionality
///
//===----------------------------------------------------------------------===//

#ifndef UTILS_H
#define UTILS_H

#include <llvm/Support/CommandLine.h>

#include "fuzzalloc/Analysis/VariableRecovery.h"

namespace llvm {
class Constant;
class ConstantInt;
class DIVariable;
class Instruction;
class IntegerType;
class Module;
class Type;
class TypeSize;
class Value;
} // namespace llvm

enum InstType {
  InstNone,
  InstAFL,
  InstTrace,
};

extern llvm::cl::opt<InstType> ClInstType;

/// Insert code to read the current value of the program counter
llvm::Value *readPC(llvm::Instruction *);

/// Randomly generate a def site tag
llvm::ConstantInt *generateTag(llvm::IntegerType *);

/// Compute the adjusted size for a tagged variable
size_t getTaggedVarSize(const llvm::TypeSize &, size_t);

/// Insert a call to `malloc`
llvm::Instruction *insertMalloc(llvm::Type *, llvm::Value *,
                                llvm::Instruction *, bool = true);

/// Insert a call to `free`
llvm::Instruction *insertFree(llvm::Type *, llvm::Value *, llvm::Instruction *);

//
// Tracer functionality
//

/// Log a variable definition in the def-use tracer
llvm::Instruction *tracerLogDef(llvm::Constant *, llvm::Instruction *);

/// Create a constant `SrcDef` struct for tracing variable definitions
llvm::Constant *tracerCreateDef(const VarInfo &, llvm::Module *);

/// Create a constant `SrcLocation` struct for tracing variable uses
llvm::Constant *tracerCreateUse(llvm::Instruction *, llvm::Module *);

#endif // UTILS_H
