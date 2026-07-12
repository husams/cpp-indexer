// Probe-only SymbolEmitter: writes `<usr>\t<kind>\t<qualified-name>` per symbol.
//
// This is the throwaway sink for the Phase-0 probe. The real layer will provide
// a Storage-backed SymbolEmitter writing the `symbol` table instead; nothing in
// the visitor changes when that sink is swapped in.
#pragma once

#include "clangx_lt/symbol_emitter.hpp"

namespace llvm {
class raw_ostream;
}

namespace cidx::lt {

class TsvSymbolEmitter : public SymbolEmitter {
public:
  explicit TsvSymbolEmitter(llvm::raw_ostream &os);

  void emit(const SymbolRecord &symbol) override;

private:
  llvm::raw_ostream &os_;
};

} // namespace cidx::lt
