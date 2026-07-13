// TemplateArgumentEncoder: the ONE conversion from clang::TemplateArgument to
// a template_arg row (docs/improvements/template-arg-contract.md). Every
// extraction path — class-spec edges, callable call sites, minted instances,
// instantiation owners, local-variable declarations — encodes through this
// class, so packs, non-type and template-template arguments store the same
// arg_kind regardless of where they were seen:
//
//   1 = type            (literal = printed spelling, ref_id = typed TagDecl)
//   2 = non-type value  (Declaration/NullPtr/Integral/StructuralValue/Expression)
//   3 = template-template (Template/TemplateExpansion)
//   4 = pack            (one row for the pack; elements not expanded)
//   Null encodes no row (an unfilled slot).
#pragma once

#include "ast/edge_records.hpp"

#include "clang/AST/Type.h"

#include <cstdint>
#include <optional>
#include <string>

namespace clang {
class ASTContext;
class TemplateArgument;
} // namespace clang

namespace cidx::lt {

class EdgeSink;

class TemplateArgumentEncoder {
public:
  TemplateArgumentEncoder(const clang::ASTContext &context, EdgeSink &sink);

  // Encode one argument for owner/position. `written` overrides the printed
  // spelling of a Type argument (the as-written sugared type when the caller
  // has one). Returns nullopt for a Null argument.
  std::optional<TemplateArgRecord>
  encode(int64_t owner_id, int64_t position, const clang::TemplateArgument &arg,
         clang::QualType written = clang::QualType()) const;

  // encode + sink.add_template_arg. Returns the encoded record when emitted.
  std::optional<TemplateArgRecord>
  emit(int64_t owner_id, int64_t position, const clang::TemplateArgument &arg,
       clang::QualType written = clang::QualType()) const;

  // Display fragment for template display names: the stored literal for
  // type/value kinds that carry one, "?" otherwise (suppresses the rewrite).
  static std::string display_text(const TemplateArgRecord &record);

private:
  const clang::ASTContext &context_;
  EdgeSink &sink_;
};

} // namespace cidx::lt
