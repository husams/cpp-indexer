#include "ast/template_argument_encoder.hpp"

#include "ast/clang_compat.hpp"
#include "ast/edge_sink.hpp"
#include "ast/names.hpp"
#include "ast/value_provenance.hpp"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/TemplateBase.h"

namespace cidx::ast {

TemplateArgumentEncoder::TemplateArgumentEncoder(
    const clang::ASTContext &context, EdgeSink &sink)
    : context_(context), sink_(sink) {}

std::optional<TemplateArgRecord>
TemplateArgumentEncoder::encode(int64_t owner_id, int64_t position,
                                const clang::TemplateArgument &arg,
                                clang::QualType written) const {
  TemplateArgRecord ta;
  ta.owner_id = owner_id;
  ta.position = position;
  // Exhaustive over clang::TemplateArgument::ArgKind — no default, so a new
  // Clang argument kind fails the build instead of silently mis-storing.
  switch (arg.getKind()) {
  case clang::TemplateArgument::Null:
    return std::nullopt; // unfilled slot: no row
  case clang::TemplateArgument::Type: {
    ta.arg_kind = 1;
    const clang::QualType t = written.isNull() ? arg.getAsType() : written;
    const std::string sp = t.getAsString(printing_policy(context_));
    if (!sp.empty()) {
      ta.literal = sp;
    }
    // The referenced record resolves through pointer/reference wrappers
    // (Box<Foo *> links to Foo); record_usr_of_type strips them typedly.
    const std::string ref_usr = record_usr_of_type(t);
    if (!ref_usr.empty()) {
      ta.ref_id = sink_.lookup_symbol_id(ref_usr);
    }
    break;
  }
  case clang::TemplateArgument::Integral:
    ta.arg_kind = 2;
    ta.literal = compat::integral_to_string(arg.getAsIntegral());
    break;
  case clang::TemplateArgument::Declaration:
  case clang::TemplateArgument::NullPtr:
  case clang::TemplateArgument::StructuralValue:
  case clang::TemplateArgument::Expression:
    ta.arg_kind = 2;
    break;
  case clang::TemplateArgument::Template:
  case clang::TemplateArgument::TemplateExpansion:
    ta.arg_kind = 3;
    break;
  case clang::TemplateArgument::Pack:
    ta.arg_kind = 4;
    break;
  }
  return ta;
}

std::optional<TemplateArgRecord>
TemplateArgumentEncoder::emit(int64_t owner_id, int64_t position,
                              const clang::TemplateArgument &arg,
                              clang::QualType written) const {
  const auto record = encode(owner_id, position, arg, written);
  if (record) {
    sink_.add_template_arg(*record);
  }
  return record;
}

std::string TemplateArgumentEncoder::display_text(
    const TemplateArgRecord &record) {
  if ((record.arg_kind == 1 || record.arg_kind == 2) && record.literal) {
    return *record.literal;
  }
  return "?";
}

} // namespace cidx::ast
