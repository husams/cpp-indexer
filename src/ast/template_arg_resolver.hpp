// TemplateArgResolver: best-effort ref_id resolution for a template argument's
// literal spelling (resolve_template_arg_ref_id, ast_templates.cpp:363).
// Strips elaborations/cv/refs from the literal, then tries the qualified name
// against enclosing scopes before falling back to the unqualified tail.
#pragma once

#include <optional>
#include <string>
#include <vector>

namespace clang {
class ASTContext;
class Decl;
} // namespace clang

namespace cidx::lt {

class EdgeSink;

class TemplateArgResolver {
public:
  TemplateArgResolver(const clang::ASTContext &context, EdgeSink &sink);

  // scope_decl: the specialization decl whose enclosing scopes qualify a bare
  // name (cursor_scope_qual_names analogue).
  std::optional<int64_t> resolve(const std::optional<std::string> &literal,
                                 const clang::Decl *scope_decl) const;

private:
  const clang::ASTContext &context_;
  EdgeSink &sink_;
};

} // namespace cidx::lt
