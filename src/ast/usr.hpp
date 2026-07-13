// USR generation helpers.
//
// Wraps clang::index::generateUSRForDecl so the visitor layer does not depend on
// LLVM index headers directly. These USRs are byte-identical to libclang's
// clang_getCursorUSR (same underlying code), which preserves cidx's cross-TU
// stub resolution and committed index.db identity.
#pragma once

#include <string>

namespace clang {
class Decl;
}

namespace cidx::lt {

// Empty string on generation failure.
std::string usr_for_decl(const clang::Decl *decl);

} // namespace cidx::lt
