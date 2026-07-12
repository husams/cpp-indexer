#include "clangx_lt/tsv_symbol_emitter.hpp"

#include "llvm/Support/raw_ostream.h"

namespace cidx::lt {

namespace {

constexpr const char *kNull = "\\N"; // matches `sqlite3 -nullvalue '\N'` dumps

void put(llvm::raw_ostream &os, const std::optional<std::string> &v) {
  os << '\t' << (v ? v->c_str() : kNull);
}

void put(llvm::raw_ostream &os, const std::optional<int64_t> &v) {
  os << '\t';
  if (v)
    os << *v;
  else
    os << kNull;
}

} // namespace

TsvSymbolEmitter::TsvSymbolEmitter(llvm::raw_ostream &os) : os_(os) {}

void TsvSymbolEmitter::emit(const SymbolRecord &s) {
  // Column order mirrors the `symbol` table dump used by the parity harness
  // (scripts/lt_symbol_diff.sh) — keep the two in lockstep. Leading `file`
  // column is consumed by lt_merge.py (file_id analogue), not compared.
  os_ << s.file << '\t' << s.usr << '\t' << s.spelling << '\t' << s.kind;
  put(os_, s.qual_name);
  put(os_, s.display_name);
  put(os_, s.type_info);
  os_ << '\t' << s.line << '\t' << s.col << '\t' << s.end_line << '\t'
      << s.end_col;
  put(os_, s.decl_line);
  put(os_, s.decl_col);
  os_ << '\t' << (s.is_definition ? 1 : 0) << '\t' << (s.is_pure ? 1 : 0)
      << '\t' << (s.is_static ? 1 : 0);
  put(os_, s.linkage);
  put(os_, s.access);
  put(os_, s.parent_usr);
  os_ << '\t' << (s.resolved ? 1 : 0) << '\n';
}

} // namespace cidx::lt
