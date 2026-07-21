#include "ast/usr.hpp"

#include "clang/Index/USRGeneration.h"
#include "llvm/ADT/SmallString.h"

namespace cidx::ast {

std::string usr_for_decl(const clang::Decl *decl) {
  llvm::SmallString<128> buf;
  if (clang::index::generateUSRForDecl(decl, buf)) {
    return {}; // generation failed
  }
  return std::string(buf);
}

} // namespace cidx::ast
