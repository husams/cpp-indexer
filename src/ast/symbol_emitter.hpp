// Symbol sink interface.
//
// The visitor emits SymbolRecords through this interface and knows nothing
// about where they go. The probe uses a TSV-to-stream implementation; the real
// layer will implement a Storage-backed sink writing the `symbol` table.
// Keeping this abstraction is what lets the visitor stay decoupled from I/O and
// storage.
#pragma once

#include "ast/symbol_record.hpp"

namespace cidx::ast {

class SymbolFactEmitter {
public:
  virtual ~SymbolFactEmitter() = default;
  virtual void emit(const SymbolRecord &symbol) = 0;
};

class SymbolEmitter : public SymbolFactEmitter {
public:
  ~SymbolEmitter() override = default;
};

// Focused compatibility adapter for production visitors that still accept the
// historical SymbolEmitter name while publishing to a bounded fact emitter.
class SymbolEmitterAdapter final : public SymbolEmitter {
public:
  explicit SymbolEmitterAdapter(SymbolFactEmitter &emitter)
      : emitter_(emitter) {}

  void emit(const SymbolRecord &symbol) override { emitter_.emit(symbol); }

private:
  SymbolFactEmitter &emitter_;
};

} // namespace cidx::ast
