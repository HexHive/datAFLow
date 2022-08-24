//===-- VariableTag.h - Common tagging functionality ------------*- C++ -*-===//
///
/// \file
/// Common tagging functionality
///
//===----------------------------------------------------------------------===//

#ifndef VARIABLE_TAG_H
#define VARIABLE_TAG_H

namespace llvm {
class ConstantInt;
class IntegerType;
class Instruction;
class Type;
class TypeSize;
class Value;
} // namespace llvm

/// Randomly generate a def site tag
llvm::ConstantInt *generateTag(llvm::IntegerType *);

/// Compute the adjusted size for a tagged variable
size_t getTaggedVarSize(const llvm::TypeSize &);

/// Insert a call to `malloc`
llvm::Instruction *insertMalloc(llvm::Type *, llvm::Value *,
                                llvm::Instruction *, bool = true);

/// Insert a call to `free`
llvm::Instruction *insertFree(llvm::Type *, llvm::Value *, llvm::Instruction *);

#endif // VARIABLE_TAG_H
