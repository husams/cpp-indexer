#include "clangx_lt/storage_symbol_sink.hpp"

#include "clangx_lt/kind_map.hpp"

#include "storage/storage.hpp"

namespace cidx::lt {

StorageSymbolSink::StorageSymbolSink(cidx::Storage &db) : db_(db) {}

void StorageSymbolSink::set_current_file_id(int64_t file_id) {
  current_file_id_ = file_id;
}

void StorageSymbolSink::emit(const SymbolRecord &s) {
  const char *kind_name = cidx_kind_name_from_int(s.kind);
  if (kind_name == nullptr)
    return;

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
  sym.linkage = s.linkage;
  sym.access = s.access;
  sym.parent_usr = s.parent_usr;
  sym.resolved = s.resolved;
  db_.add_symbol(sym);
}

} // namespace cidx::lt
