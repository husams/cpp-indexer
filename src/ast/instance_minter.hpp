// InstanceMinter: mints concrete `X<B>` class-template-specialization instance
// entities. Gated to non-system primaries; emits instance -> primary
// instantiates(5) plus template_arg rows through the canonical encoder.
#pragma once

#include "clang/AST/Type.h"

#include <cstdint>

namespace clang {
class ASTContext;
class ClassTemplateSpecializationDecl;
class NamedDecl;
class TypedefNameDecl;
} // namespace clang

namespace cidx::ast {

class EdgeSink;
class MintBuilder;
class TemplateArgumentEncoder;

class InstanceMinter {
public:
  InstanceMinter(const clang::ASTContext &context, EdgeSink &sink,
                 const MintBuilder &mint,
                 const TemplateArgumentEncoder &targ_encoder);

  // `X<B> field;` / `X<B> v;` / alias-underlying types (pointer/ref/array
  // layers peeled).
  void mint_instance_from_type(clang::QualType type) const;

  // `using Y = X<B>;` / `typedef X<B> Y;`.
  void mint_named_instance(const clang::TypedefNameDecl *alias) const;

private:
  const clang::NamedDecl *mintable_primary(
      const clang::ClassTemplateSpecializationDecl *spec) const;
  int64_t
  mint_instance_pair(const clang::ClassTemplateSpecializationDecl *spec,
                     const clang::NamedDecl *primary,
                     clang::QualType written_type) const;

  const clang::ASTContext &context_;
  EdgeSink &sink_;
  const MintBuilder &mint_;
  const TemplateArgumentEncoder &targ_encoder_;
};

} // namespace cidx::ast
