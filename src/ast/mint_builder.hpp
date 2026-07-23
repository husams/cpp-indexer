// MintBuilder: builds a MintRequest (USR-keyed stub payload) from a NamedDecl,
// mirroring the libclang edge pass's mint_symbol_id call sites: spelling,
// qualified/display names, stub kind name, and the referenced declaration's
// location resolved against registered files (ref_decl_loc).
#pragma once

#include "ast/edge_records.hpp"

#include <optional>

namespace clang {
class ASTContext;
class NamedDecl;
} // namespace clang

namespace cidx::ast {

class EdgeSink;

class MintBuilder {
public:
  MintBuilder(const clang::ASTContext &context, EdgeSink &sink);

  // nullopt when the decl has no USR.
  std::optional<MintRequest> build(const clang::NamedDecl *decl) const;
  [[nodiscard]] const clang::ASTContext &context() const { return context_; }

private:
  const clang::ASTContext &context_;
  EdgeSink &sink_;
};

} // namespace cidx::ast
