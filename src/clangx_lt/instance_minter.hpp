// InstanceMinter: mints concrete `X<B>` class-template-specialization instance
// entities (Stage 2-4) — the LibTooling analogue of mint_instance_from_type /
// mint_named_instance (ast_templates.cpp:524). Gated to non-system primaries;
// emits instance -> primary instantiates(5) plus TYPE template_arg rows.
#pragma once

#include "clang/AST/Type.h"

namespace clang {
class ASTContext;
class TypedefNameDecl;
} // namespace clang

namespace cidx::lt {

class EdgeSink;
class MintBuilder;
class TemplateArgResolver;

class InstanceMinter {
public:
  InstanceMinter(const clang::ASTContext &context, EdgeSink &sink,
                 const MintBuilder &mint, const TemplateArgResolver &resolver);

  // `X<B> field;` / `X<B> v;` / alias-underlying types (pointer/ref/array
  // layers peeled).
  void mint_instance_from_type(clang::QualType type) const;

  // `using Y = X<B>;` / `typedef X<B> Y;`.
  void mint_named_instance(const clang::TypedefNameDecl *alias) const;

private:
  const clang::ASTContext &context_;
  EdgeSink &sink_;
  const MintBuilder &mint_;
  const TemplateArgResolver &resolver_;
};

} // namespace cidx::lt
