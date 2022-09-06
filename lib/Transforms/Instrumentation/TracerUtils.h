//===-- TracerUtils.h - Common tracer functionality -------------*- C++ -*-===//
///
/// \file
/// Common def-use tracer functionality
///
//===----------------------------------------------------------------------===//

#ifndef TRACER_UTILS_H
#define TRACER_UTILS_H

#include <llvm/Support/CommandLine.h>

#include "fuzzalloc/Analysis/VariableRecovery.h"

namespace llvm {
class Constant;
class Instruction;
class Module;
class Value;
} // namespace llvm

extern llvm::cl::opt<bool> ClUseTracer;

llvm::Value *readPC(llvm::Instruction *);

/// Log a variable definition in the def-use tracer
llvm::Instruction *tracerLogDef(llvm::Constant *, llvm::Instruction *);

/// Create a constant `SrcDef` struct for tracing variable definitions
llvm::Constant *tracerCreateDef(const VarInfo &, llvm::Module *);

/// Create a constant `SrcLocation` struct for tracing variable uses
llvm::Constant *tracerCreateUse(llvm::Instruction *, llvm::Module *);

#endif // TRACER_UTILS_H
