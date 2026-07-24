#include "ast/template_argument_encoder.hpp"

#include "ast/clang_compat.hpp"
#include "ast/edge_sink.hpp"
#include "ast/location.hpp"
#include "ast/names.hpp"
#include "ast/type_use.hpp"
#include "ast/usr.hpp"
#include "ast/value_provenance.hpp"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/AST/TemplateBase.h"

namespace cidx::ast {

TemplateArgumentEncoder::TemplateArgumentEncoder(clang::ASTContext &context,
                                                 EdgeSink &sink)
    : context_(context), sink_(sink), types_(context, sink) {}

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
    ta.type_id = types_.intern(t);
    const std::string sp = t.getAsString(printing_policy(context_));
    if (!sp.empty()) {
      ta.literal = sp;
    }
    // The referenced record resolves through pointer/reference wrappers
    // (Box<Foo *> links to Foo); record_usr_of_type strips them typedly.
    const std::string ref_usr = record_usr_of_type(t);
    if (!ref_usr.empty()) {
      const clang::NamedDecl *ref_decl = named_type_decl(t);
      ta.ref_id = sink_.lookup_symbol_id(
          ref_usr,
          ref_decl != nullptr
              ? std::optional<std::string>(
                    expansion_loc(context_, ref_decl->getLocation()).file)
              : std::nullopt);
    }
    break;
  }
  case clang::TemplateArgument::Integral:
    ta.arg_kind = 2;
    if (const auto *enum_type = arg.getIntegralType()->getAs<clang::EnumType>();
        enum_type != nullptr) {
      const clang::EnumDecl *enum_decl = enum_type->getDecl();
      for (const clang::EnumConstantDecl *constant : enum_decl->enumerators()) {
        if (constant->getInitVal() == arg.getAsIntegral()) {
          ta.literal =
              enum_decl->getNameAsString() + "::" + constant->getNameAsString();
          break;
        }
      }
    }
    if (!ta.literal) {
      ta.literal = compat::integral_to_string(arg.getAsIntegral());
    }
    ta.type_id = types_.intern(arg.getIntegralType());
    break;
  case clang::TemplateArgument::Declaration:
  case clang::TemplateArgument::NullPtr:
  case clang::TemplateArgument::StructuralValue:
  case clang::TemplateArgument::Expression:
    ta.arg_kind = 2;
    break;
  case clang::TemplateArgument::Template:
    ta.arg_kind = 3;
    if (const auto *td = arg.getAsTemplate().getAsTemplateDecl();
        td != nullptr) {
      ta.literal = td->getNameAsString();
      ta.ref_id = sink_.lookup_symbol_id(
          usr_for_decl(td), expansion_loc(context_, td->getLocation()).file);
    }
    break;
  case clang::TemplateArgument::TemplateExpansion:
    ta.arg_kind = 3;
    if (const auto *td =
            arg.getAsTemplateOrTemplatePattern().getAsTemplateDecl();
        td != nullptr) {
      ta.literal = td->getNameAsString();
      ta.ref_id = sink_.lookup_symbol_id(
          usr_for_decl(td), expansion_loc(context_, td->getLocation()).file);
    }
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
  if (arg.getKind() == clang::TemplateArgument::Pack) {
    std::optional<TemplateArgRecord> first;
    const auto &pack = arg.getPackAsArray();
    if (pack.empty()) {
      const auto record = encode(owner_id, position, arg);
      if (record) {
        sink_.add_template_arg(*record);
      }
      return record;
    }
    for (unsigned i = 0; i < pack.size(); ++i) {
      auto record = encode(owner_id, position, pack[i]);
      if (!record) {
        continue;
      }
      record->pack_index = static_cast<int64_t>(i);
      if (!first) {
        first = record;
      }
      sink_.add_template_arg(*record);
    }
    return first;
  }
  const auto record = encode(owner_id, position, arg, written);
  if (record) {
    sink_.add_template_arg(*record);
  }
  return record;
}

std::string
TemplateArgumentEncoder::display_text(const TemplateArgRecord &record) {
  if ((record.arg_kind == 1 || record.arg_kind == 2) && record.literal) {
    return *record.literal;
  }
  return "?";
}

} // namespace cidx::ast
