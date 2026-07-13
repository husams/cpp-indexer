// Name/type spellings that reproduce libclang's conventions.
//
//  - spelling:        clang_getCursorSpelling (DeclarationName as written)
//  - qualified_name:  cidx's semantic-parent join, skipping anonymous levels
//                     (NOT clang's getQualifiedNameAsString, which prints
//                     "(anonymous namespace)" levels)
//  - display_name:    clang_getCursorDisplayName (function name + parameter
//                     type list, template-specialization args)
//  - type_info:       clang_getTypeSpelling of the cursor type; underlying
//                     type for typedef/alias decls
#pragma once

#include <optional>
#include <string>

namespace clang {
class ASTContext;
class NamedDecl;
class PrintingPolicy;
} // namespace clang

namespace cidx::lt {

// The single PrintingPolicy for every stored type spelling (template-argument
// literals, display text, type_info). Configure printing here and nowhere
// else.
const clang::PrintingPolicy &printing_policy(const clang::ASTContext &context);

std::string spelling(const clang::NamedDecl *decl);

// Empty string when every level is anonymous. Lambda scopes print as
// "(lambda at <file>:<line>:<col>)" like libclang's cursor spelling.
std::string qualified_name(const clang::ASTContext &context,
                           const clang::NamedDecl *decl);

std::optional<std::string> display_name(const clang::ASTContext &context,
                                        const clang::NamedDecl *decl);

std::optional<std::string> type_info(const clang::ASTContext &context,
                                     const clang::NamedDecl *decl);

} // namespace cidx::lt
