#include "ast/storage_symbol_sink.hpp"

#include "ast/kind_map.hpp"

#include "storage/storage.hpp"

namespace cidx::ast {

StorageSymbolSink::StorageSymbolSink(cidx::Storage &db) : db_(db) {}

void StorageSymbolSink::set_current_file_id(int64_t file_id) {
  current_file_id_ = file_id;
}

void StorageSymbolSink::reset_counters() { stored_ = 0; }

int StorageSymbolSink::stored_count() const { return stored_; }

void StorageSymbolSink::emit(const SymbolRecord &s) {
  const char *kind_name = cidx_kind_name_from_int(s.kind);
  if (kind_name == nullptr) {
    return;
  }

  cidx::Symbol sym;
  sym.usr = s.usr;
  sym.spelling = s.spelling;
  sym.kind = kind_name;
  sym.qual_name = s.qual_name;
  sym.display_name = s.display_name;
  sym.type_info = s.type_info;
  sym.file_id = current_file_id_;
  sym.line = s.line;
  sym.col = s.col;
  sym.end_line = s.end_line;
  sym.end_col = s.end_col;
  if (s.decl_line) { // declarations record their own site (to_symbol)
    sym.decl_file_id = current_file_id_;
    sym.decl_line = s.decl_line;
    sym.decl_col = s.decl_col;
  }
  sym.is_definition = s.is_definition;
  sym.is_pure = s.is_pure;
  sym.is_static = s.is_static;
  sym.is_instantiation = s.is_instantiation;
  sym.linkage = s.linkage;
  sym.access = s.access;
  sym.parent_usr = s.parent_usr;
  sym.const_value = s.const_value;
  sym.resolved = s.resolved;
  const std::optional<cidx::Symbol> existing = db_.lookup_symbol(sym.usr);
  db_.add_symbol(sym);
  if (!(existing && existing->resolved)) {
    ++stored_; // AstIndexer::store: true = counted as "stored"
  }
}

} // namespace cidx::ast
