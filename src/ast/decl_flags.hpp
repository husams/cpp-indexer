// Decl flag/enum extraction reproducing libclang's C-API semantics:
// clang_isCursorDefinition, clang_CXXMethod_isPureVirtual/isStatic,
// clang_getCursorLinkage, clang_getCXXAccessSpecifier.
#pragma once

#include <optional>
#include <string>

namespace clang {
class Decl;
class ASTContext;
} // namespace clang

namespace cidx::ast {

bool is_definition(const clang::Decl *decl);
bool is_pure_virtual_method(const clang::Decl *decl);
bool is_static_method(const clang::Decl *decl);

// The symbol is a template INSTANTIATION (implicit, or an explicit
// instantiation declaration/definition) judged from its
// TemplateSpecializationKind. Explicit specializations and partial
// specializations are authored code, never instantiations.
bool is_template_instantiation(const clang::Decl *decl);

// "no-linkage" | "internal" | "unique-external" | "external" | nullopt.
std::optional<std::string> linkage_name(const clang::Decl *decl);

// "public" | "protected" | "private" | nullopt.
std::optional<std::string> access_name(const clang::Decl *decl);

// Typed callable/provenance facts. These are derived from Clang declaration
// APIs and persisted with the symbol; consumers must not infer them from
// rendered source text or private type encodings.
std::optional<std::string> callable_kind_name(const clang::Decl *decl);
std::optional<std::string>
template_origin_name(const clang::ASTContext &context, const clang::Decl *decl);
std::optional<std::string> template_form_name(const clang::Decl *decl);

} // namespace cidx::ast
