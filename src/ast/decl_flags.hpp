// Decl flag/enum extraction reproducing libclang's C-API semantics:
// clang_isCursorDefinition, clang_CXXMethod_isPureVirtual/isStatic,
// clang_getCursorLinkage, clang_getCXXAccessSpecifier.
#pragma once

#include <optional>
#include <string>

namespace clang {
class Decl;
}

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

} // namespace cidx::ast
