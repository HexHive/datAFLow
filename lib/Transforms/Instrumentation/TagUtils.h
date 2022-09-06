//===-- TagUtils.h - Common tagging functionality ---------------*- C++ -*-===//
///
/// \file
/// Common tagging functionality
///
//===----------------------------------------------------------------------===//

#ifndef TAG_UTILS_H
#define TAG_UTILS_H

#include <llvm/IR/IRBuilder.h>

namespace llvm {
class ConstantInt;
class DIVariable;
class IntegerType;
class Instruction;
class Module;
class Type;
class TypeSize;
class Value;
} // namespace llvm

/// Randomly generate a def site tag
llvm::ConstantInt *generateTag(llvm::IntegerType *);

/// Compute the adjusted size for a tagged variable
size_t getTaggedVarSize(const llvm::TypeSize &, size_t);

/// Insert a call to `malloc`
llvm::Instruction *insertMalloc(llvm::Type *, llvm::Value *,
                                llvm::Instruction *, bool = true);

/// Insert a call to `free`
llvm::Instruction *insertFree(llvm::Type *, llvm::Value *, llvm::Instruction *);

#endif // TAG_UTILS_H
