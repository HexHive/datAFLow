//===-- Streams.h - Output stream wrappers ----------------------*- C++ -*-===//
///
/// \file
/// Colored wrappers around LLVM output streams.
///
//===----------------------------------------------------------------------===//

#ifndef STREAMS_H
#define STREAMS_H

#include <llvm/Support/WithColor.h>

llvm::raw_ostream &error_stream() {
  return llvm::WithColor{llvm::errs(), llvm::HighlightColor::Error} << "[!] ";
}

llvm::raw_ostream &status_stream() {
  return llvm::WithColor{llvm::outs(), llvm::HighlightColor::Remark} << "[*] ";
}

llvm::raw_ostream &success_stream() {
  return llvm::WithColor{llvm::outs(), llvm::HighlightColor::String} << "[+] ";
}

llvm::raw_ostream &warning_stream() {
  return llvm::WithColor{llvm::errs(), llvm::HighlightColor::Warning} << "[!] ";
}

#endif // STREAMS_H