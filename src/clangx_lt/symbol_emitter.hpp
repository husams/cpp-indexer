// Symbol sink interface.
//
// The visitor emits SymbolRecords through this interface and knows nothing about
// where they go. The probe uses a TSV-to-stream implementation; the real layer
// will implement a Storage-backed sink writing the `symbol` table. Keeping this
// abstraction is what lets the visitor stay decoupled from I/O and storage.
#pragma once

#include "clangx_lt/symbol_record.hpp"

namespace cidx::lt {

class SymbolEmitter {
public:
  virtual ~SymbolEmitter() = default;
  virtual void emit(const SymbolRecord &symbol) = 0;
};

} // namespace cidx::lt
