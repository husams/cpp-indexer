// Small shims over Clang/LLVM API differences between the versions cidx builds
// against (LLVM 21 on RHEL 9.8, LLVM 22 on macOS Homebrew). The C++ API is NOT
// source-stable across majors — this header localizes the divergences.
#pragma once

#include "clang/AST/TemplateBase.h"
#include "llvm/ADT/APSInt.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Config/llvm-config.h"

#include <string>

namespace cidx::ast::compat {

// llvm::toString(APSInt, radix) is an LLVM 22 addition; APSInt::toString is
// present in both.
inline std::string integral_to_string(const llvm::APSInt &value) {
  llvm::SmallString<32> buf;
  value.toString(buf, /*Radix=*/10);
  return std::string(buf);
}

} // namespace cidx::ast::compat
