#include "clangx_lt/symbol_action.hpp"

#include "clangx_lt/symbol_consumer.hpp"
#include "clangx_lt/tsv_symbol_emitter.hpp"

#include "llvm/Support/raw_ostream.h"

namespace cidx::lt {

SymbolAction::SymbolAction()
    : emitter_(std::make_unique<TsvSymbolEmitter>(llvm::outs())) {}

SymbolAction::~SymbolAction() = default;

std::unique_ptr<clang::ASTConsumer>
SymbolAction::CreateASTConsumer(clang::CompilerInstance & /*ci*/,
                                llvm::StringRef /*file*/) {
  return std::make_unique<SymbolConsumer>(*emitter_);
}

} // namespace cidx::lt
