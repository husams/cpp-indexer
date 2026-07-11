// Storage-backed SymbolEmitter: writes SymbolRecords through the real
// cidx::Storage (add_symbol upsert, which also records decl_site rows) —
// the LT analogue of AstIndexer::store. The orchestrator sets the file_id of
// the file currently being walked (to_symbol's file_id parameter).
#pragma once

#include "clangx_lt/symbol_emitter.hpp"

#include <cstdint>

namespace cidx {
class Storage;
}

namespace cidx::lt {

class StorageSymbolSink : public SymbolEmitter {
public:
  explicit StorageSymbolSink(cidx::Storage &db);

  void set_current_file_id(int64_t file_id);

  void emit(const SymbolRecord &symbol) override;

private:
  cidx::Storage &db_;
  int64_t current_file_id_ = -1;
};

} // namespace cidx::lt
