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
class TypeSize;
} // llvm namespace

/// Randomly generate a def site tag
llvm::ConstantInt *generateTag(llvm::IntegerType *);

size_t getTaggedVarSize(const llvm::TypeSize &);

#endif // VARIABLE_TAG_H