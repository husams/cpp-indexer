#include "clangx_lt/tsv_symbol_emitter.hpp"

#include "llvm/Support/raw_ostream.h"

namespace cidx::lt {

TsvSymbolEmitter::TsvSymbolEmitter(llvm::raw_ostream &os) : os_(os) {}

void TsvSymbolEmitter::emit(const SymbolRecord &symbol) {
  os_ << symbol.usr << '\t' << symbol.kind << '\t' << symbol.qualified_name
      << '\n';
}

} // namespace cidx::lt
