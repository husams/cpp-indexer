// Decl::Kind -> CXCursorKind integer mapping.
//
// cidx stores the raw libclang CXCursorKind integer in symbol.kind (frozen
// 17-entry map, storage.cpp:463-485). The Clang C++ API exposes clang::Decl::Kind
// instead, so this translates one to the other, keeping the on-disk schema
// byte-identical. Returns -1 for decls cidx does not persist.
#pragma once

namespace clang {
class Decl;
}

namespace cidx::lt {

int cidx_symbol_kind(const clang::Decl *decl);

} // namespace cidx::lt
