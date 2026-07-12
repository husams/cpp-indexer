#include "clangx_lt/decl_flags.hpp"

#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/Basic/Linkage.h"

namespace cidx::lt {

bool is_definition(const clang::Decl *decl) {
  // Mirror clang_isCursorDefinition (CIndex.cpp isDeclADefinition): tag,
  // function, and variable decls answer "is THIS decl the definition";
  // members, enumerators, typedefs, and namespaces are definitions by nature.
  if (const auto *tag = llvm::dyn_cast<clang::TagDecl>(decl))
    return tag->isThisDeclarationADefinition();
  // libclang's definition-of-a-function = THIS decl carries an actual body
  // STATEMENT (clang_getCursorDefinition goes through getBody). Explicitly
  // defaulted/deleted declarations have none, so `= default` / `= delete`
  // are NOT definitions — unlike isThisDeclarationADefinition().
  if (const auto *fn = llvm::dyn_cast<clang::FunctionDecl>(decl))
    return fn->doesThisDeclarationHaveABody();
  if (const auto *var = llvm::dyn_cast<clang::VarDecl>(decl))
    return var->isThisDeclarationADefinition() == clang::VarDecl::Definition;
  if (const auto *ft = llvm::dyn_cast<clang::FunctionTemplateDecl>(decl))
    return ft->getTemplatedDecl()->doesThisDeclarationHaveABody();
  if (const auto *ct = llvm::dyn_cast<clang::ClassTemplateDecl>(decl))
    return ct->isThisDeclarationADefinition();
  return llvm::isa<clang::FieldDecl>(decl) ||
         llvm::isa<clang::EnumConstantDecl>(decl) ||
         llvm::isa<clang::TypedefNameDecl>(decl) ||
         llvm::isa<clang::NamespaceDecl>(decl);
}

bool is_pure_virtual_method(const clang::Decl *decl) {
  const auto *method = llvm::dyn_cast<clang::CXXMethodDecl>(decl);
  return method != nullptr && method->isPureVirtual();
}

bool is_static_method(const clang::Decl *decl) {
  const auto *method = llvm::dyn_cast<clang::CXXMethodDecl>(decl);
  return method != nullptr && method->isStatic();
}

std::optional<std::string> linkage_name(const clang::Decl *decl) {
  const auto *nd = llvm::dyn_cast<clang::NamedDecl>(decl);
  if (nd == nullptr)
    return std::nullopt;
  // Mirror clang_getCursorLinkage's CXLinkageKind mapping.
  switch (nd->getLinkageInternal()) {
  case clang::Linkage::None:
  case clang::Linkage::VisibleNone:
    return std::string("no-linkage");
  case clang::Linkage::Internal:
    return std::string("internal");
  case clang::Linkage::UniqueExternal:
    return std::string("unique-external");
  case clang::Linkage::Module:
  case clang::Linkage::External:
    return std::string("external");
  case clang::Linkage::Invalid:
    return std::nullopt;
  }
  return std::nullopt;
}

std::optional<std::string> access_name(const clang::Decl *decl) {
  switch (decl->getAccess()) {
  case clang::AS_public:
    return std::string("public");
  case clang::AS_protected:
    return std::string("protected");
  case clang::AS_private:
    return std::string("private");
  case clang::AS_none:
    return std::nullopt;
  }
  return std::nullopt;
}

} // namespace cidx::lt
