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

// Storage kind NAME ("class", "function", ...) or nullptr when unmapped.
const char *cidx_symbol_kind_name(const clang::Decl *decl);

// Kind name for a minted stub: the mapped name, else "function" (stub_kind).
const char *cidx_stub_kind_name(const clang::Decl *decl);

// Storage kind name for a CXCursorKind integer, nullptr if unmapped.
const char *cidx_kind_name_from_int(int kind);

} // namespace cidx::lt
