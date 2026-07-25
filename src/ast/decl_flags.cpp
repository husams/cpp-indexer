#include "ast/decl_flags.hpp"

#include "ast/names.hpp"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/Basic/Linkage.h"

namespace cidx::ast {

bool is_definition(const clang::Decl *decl) {
  // Mirror clang_isCursorDefinition (CIndex.cpp isDeclADefinition): tag,
  // function, and variable decls answer "is THIS decl the definition";
  // members, enumerators, typedefs, and namespaces are definitions by nature.
  if (const auto *tag = llvm::dyn_cast<clang::TagDecl>(decl)) {
    return tag->isThisDeclarationADefinition();
  }
  // libclang's definition-of-a-function = THIS decl carries an actual body
  // STATEMENT (clang_getCursorDefinition goes through getBody). Explicitly
  // defaulted/deleted declarations have none, so `= default` / `= delete`
  // are NOT definitions — unlike isThisDeclarationADefinition().
  if (const auto *fn = llvm::dyn_cast<clang::FunctionDecl>(decl)) {
    return fn->doesThisDeclarationHaveABody();
  }
  if (const auto *var = llvm::dyn_cast<clang::VarDecl>(decl)) {
    return var->isThisDeclarationADefinition() == clang::VarDecl::Definition;
  }
  if (const auto *ft = llvm::dyn_cast<clang::FunctionTemplateDecl>(decl)) {
    return ft->getTemplatedDecl()->doesThisDeclarationHaveABody();
  }
  if (const auto *ct = llvm::dyn_cast<clang::ClassTemplateDecl>(decl)) {
    return ct->isThisDeclarationADefinition();
  }
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

bool is_template_instantiation(const clang::Decl *decl) {
  // TemplateSpecializationKind is the single source of truth: implicit and
  // explicit instantiations are instantiations; explicit (full) and partial
  // specializations are authored declarations. ClassTemplatePartialSpec
  // derives from ClassTemplateSpecialization and answers TSK_Undeclared /
  // TSK_ExplicitSpecialization, so it falls out false naturally.
  clang::TemplateSpecializationKind tsk = clang::TSK_Undeclared;
  // CXXRecordDecl::getTemplateSpecializationKind covers class-template
  // specializations AND instantiated member classes (Outer<int>::Inner).
  if (const auto *rec = llvm::dyn_cast<clang::CXXRecordDecl>(decl)) {
    tsk = rec->getTemplateSpecializationKind();
  } else if (const auto *fn = llvm::dyn_cast<clang::FunctionDecl>(decl)) {
    if (!fn->isFunctionTemplateSpecialization()) {
      return false;
    }
    tsk = fn->getTemplateSpecializationKind();
  } else if (const auto *var = llvm::dyn_cast<clang::VarDecl>(decl)) {
    tsk = var->getTemplateSpecializationKind();
  }
  switch (tsk) {
  case clang::TSK_ImplicitInstantiation:
  case clang::TSK_ExplicitInstantiationDeclaration:
  case clang::TSK_ExplicitInstantiationDefinition:
    return true;
  case clang::TSK_Undeclared:
  case clang::TSK_ExplicitSpecialization:
    return false;
  }
  return false;
}

std::optional<std::string> linkage_name(const clang::Decl *decl) {
  const auto *nd = llvm::dyn_cast<clang::NamedDecl>(decl);
  if (nd == nullptr) {
    return std::nullopt;
  }
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

std::optional<std::string> callable_kind_name(const clang::Decl *decl) {
  const auto *named = llvm::dyn_cast<clang::NamedDecl>(decl);
  if (named == nullptr) {
    return std::nullopt;
  }
  const auto *function = llvm::dyn_cast<clang::FunctionDecl>(decl);
  if (function == nullptr) {
    if (const auto *template_decl =
            llvm::dyn_cast<clang::FunctionTemplateDecl>(decl)) {
      function = template_decl->getTemplatedDecl();
    }
  }
  if (function == nullptr) {
    return std::nullopt;
  }
  if (llvm::isa<clang::CXXConstructorDecl>(function)) {
    return std::string("constructor");
  }
  if (llvm::isa<clang::CXXDestructorDecl>(function)) {
    return std::string("destructor");
  }
  return llvm::isa<clang::CXXMethodDecl>(function)
             ? std::optional<std::string>("method")
             : std::optional<std::string>("free-function");
}

std::optional<std::string>
template_origin_name(const clang::ASTContext &context,
                     const clang::Decl *decl) {
  const auto *function = llvm::dyn_cast<clang::FunctionDecl>(decl);
  if (function == nullptr) {
    return std::nullopt;
  }
  if (const auto *method = llvm::dyn_cast<clang::CXXMethodDecl>(function)) {
    const auto *record =
        llvm::dyn_cast<clang::CXXRecordDecl>(method->getDeclContext());
    if (record != nullptr && is_template_instantiation(record)) {
      return std::nullopt;
    }
  }
  const clang::NamedDecl *origin = nullptr;
  if (const auto *primary = function->getPrimaryTemplate()) {
    origin = primary->getTemplatedDecl();
  }
  if (origin == nullptr) {
    origin = function->getTemplateInstantiationPattern();
  }
  if (origin == nullptr || origin == function) {
    return std::nullopt;
  }
  const auto *named = llvm::dyn_cast<clang::NamedDecl>(origin);
  if (named == nullptr) {
    return std::nullopt;
  }
  if (const auto *origin_function =
          llvm::dyn_cast<clang::FunctionDecl>(named)) {
    if (const auto display = display_name(context, origin_function)) {
      std::string normalized = *display;
      for (std::size_t pos = normalized.find("*const");
           pos != std::string::npos; pos = normalized.find("*const", pos)) {
        normalized.replace(pos, 6, "* const");
        pos += 7;
      }
      const auto *parent =
          llvm::dyn_cast<clang::NamedDecl>(origin_function->getDeclContext());
      if (origin_function->getNumParams() > 1) {
        const std::string bare = normalized.substr(0, normalized.find('('));
        if (parent != nullptr) {
          const std::string scope = qualified_name_bare(context, parent);
          return scope.empty() ? bare : scope + "::" + bare;
        }
        return bare;
      }
      if (parent != nullptr) {
        const std::string scope = qualified_name_bare(context, parent);
        return scope.empty() ? normalized : scope + "::" + normalized;
      }
      return normalized;
    }
  }
  std::string name = qualified_name(context, named);
  if (name.empty()) {
    return std::nullopt;
  }
  return name;
}

std::optional<std::string> template_form_name(const clang::Decl *decl) {
  if (const auto *function = llvm::dyn_cast<clang::FunctionDecl>(decl)) {
    if (function->isFunctionTemplateSpecialization()) {
      switch (function->getTemplateSpecializationKind()) {
      case clang::TSK_ImplicitInstantiation:
        return std::string("implicit-instantiation");
      case clang::TSK_ExplicitInstantiationDeclaration:
      case clang::TSK_ExplicitInstantiationDefinition:
        return std::string("explicit-instantiation");
      case clang::TSK_ExplicitSpecialization:
        return std::string("explicit-specialization");
      case clang::TSK_Undeclared:
        break;
      }
    }
    if (llvm::isa<clang::CXXMethodDecl>(function)) {
      const auto *record =
          llvm::dyn_cast<clang::CXXRecordDecl>(function->getDeclContext());
      if (record != nullptr &&
          (record->getDescribedClassTemplate() != nullptr ||
           record->getTemplateSpecializationKind() != clang::TSK_Undeclared)) {
        return is_template_instantiation(record)
                   ? std::optional<std::string>("owner-instance-member")
                   : std::optional<std::string>("owner-pattern-member");
      }
    }
    return std::string("pattern");
  }
  if (llvm::isa<clang::FunctionTemplateDecl>(decl) ||
      llvm::isa<clang::ClassTemplateDecl>(decl)) {
    return std::string("pattern");
  }
  return std::nullopt;
}

} // namespace cidx::ast
