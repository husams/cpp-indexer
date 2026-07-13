// FrontendAction for the Phase-0 symbol probe: wires a TSV emitter to the
// symbol consumer. This is the probe's composition root — swap the emitter here
// (or subclass) to target Storage in the real layer.
#pragma once

#include "clang/Frontend/FrontendAction.h"

#include <memory>

namespace cidx::lt {

class SymbolEmitter;

class SymbolAction : public clang::ASTFrontendAction {
public:
  SymbolAction();
  ~SymbolAction() override;

  std::unique_ptr<clang::ASTConsumer>
  CreateASTConsumer(clang::CompilerInstance &ci, llvm::StringRef file) override;

private:
  std::unique_ptr<SymbolEmitter> emitter_;
};

} // namespace cidx::lt
