#include "ast/names.hpp"

#include "ast/location.hpp"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/AST/PrettyPrinter.h"
#include "llvm/Support/raw_ostream.h"

namespace cidx::lt {

std::string spelling(const clang::NamedDecl *decl) {
  return decl->getDeclName().getAsString();
}

std::string qualified_name(const clang::ASTContext &context,
                           const clang::NamedDecl *decl) {
  // Mirror cidx qualified_name (ast_cursor.cpp): walk semantic parents to the
  // TU, joining non-empty spellings with "::" (anonymous levels skipped).
  // libclang spells a lambda's closure class "(lambda at <file>:<line>:<col>)",
  // so lambda scopes survive in the chain.
  std::vector<std::string> parts;
  const clang::Decl *d = decl;
  while (d != nullptr && !llvm::isa<clang::TranslationUnitDecl>(d)) {
    if (const auto *nd = llvm::dyn_cast<clang::NamedDecl>(d)) {
      std::string s;
      const auto *rec = llvm::dyn_cast<clang::CXXRecordDecl>(nd);
      if (rec != nullptr && rec->isLambda()) {
        const ExpansionLoc loc = expansion_loc(context, rec->getLocation());
        llvm::raw_string_ostream os(s);
        os << "(lambda at " << loc.file << ':' << loc.line << ':' << loc.col
           << ')';
      } else {
        s = nd->getDeclName().getAsString();
      }
      if (!s.empty())
        parts.push_back(std::move(s));
    }
    const clang::DeclContext *dc = d->getDeclContext();
    d = dc != nullptr ? llvm::dyn_cast<clang::Decl>(dc) : nullptr;
  }
  std::string out;
  for (auto it = parts.rbegin(); it != parts.rend(); ++it) {
    if (!out.empty())
      out += "::";
    out += *it;
  }
  return out;
}

std::optional<std::string> display_name(const clang::ASTContext &context,
                                        const clang::NamedDecl *decl) {
  // Mirror clang_getCursorDisplayName (CIndex.cpp): functions get their
  // parameter TYPE list (and template-specialization args); class template
  // specializations get their template args; everything else falls back to
  // the plain spelling.
  const clang::PrintingPolicy &policy = context.getPrintingPolicy();
  std::string out;
  llvm::raw_string_ostream os(out);

  if (const auto *ft = llvm::dyn_cast<clang::FunctionTemplateDecl>(decl)) {
    // libclang displays a function template like its templated function:
    // name(param types).
    return display_name(context, ft->getTemplatedDecl());
  }
  if (const auto *ct = llvm::dyn_cast<clang::ClassTemplateDecl>(decl)) {
    // libclang displays a class template as Name<Param1, Param2, ...> using
    // the template PARAMETER names.
    os << ct->getName() << '<';
    bool first = true;
    for (const clang::NamedDecl *p : *ct->getTemplateParameters()) {
      if (!first)
        os << ", ";
      first = false;
      os << p->getName();
    }
    os << '>';
    return out;
  }
  if (const auto *fd = llvm::dyn_cast<clang::FunctionDecl>(decl)) {
    fd->getDeclName().print(os, policy);
    if (fd->getTemplateSpecializationArgs() != nullptr) {
      // libclang prints the AS-WRITTEN spec args: an implicit instantiation
      // shows empty <> (make_shared<>), an explicit specialization its
      // written args (describe<bool>).
      os << '<';
      if (const clang::ASTTemplateArgumentListInfo *written =
              fd->getTemplateSpecializationArgsAsWritten()) {
        bool first_arg = true;
        for (const clang::TemplateArgumentLoc &tal : written->arguments()) {
          if (!first_arg)
            os << ", ";
          first_arg = false;
          tal.getArgument().print(policy, os, /*IncludeType=*/false);
        }
      }
      os << '>';
    }
    os << '(';
    bool first = true;
    for (const clang::ParmVarDecl *p : fd->parameters()) {
      if (!first)
        os << ", ";
      first = false;
      os << p->getType().getAsString(policy);
    }
    if (fd->isVariadic()) {
      if (!first)
        os << ", ";
      os << "...";
    }
    os << ')';
  } else if (const auto *spec =
                 llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(decl)) {
    spec->getDeclName().print(os, policy);
    // Partial specializations print their args AS WRITTEN (libclang shows
    // 'RuleTemplate<int, NameType>', not canonical type-parameter-0-0).
    if (const auto *partial =
            llvm::dyn_cast<clang::ClassTemplatePartialSpecializationDecl>(
                decl)) {
      if (const clang::ASTTemplateArgumentListInfo *written =
              partial->getTemplateArgsAsWritten()) {
        os << '<';
        bool first = true;
        for (const clang::TemplateArgumentLoc &tal : written->arguments()) {
          if (!first)
            os << ", ";
          first = false;
          tal.getArgument().print(policy, os, /*IncludeType=*/false);
        }
        os << '>';
        return out;
      }
    }
    clang::printTemplateArgumentList(os, spec->getTemplateArgs().asArray(),
                                     policy);
  } else {
    std::string s = spelling(decl);
    if (s.empty())
      return std::nullopt;
    return s;
  }
  if (out.empty())
    return std::nullopt;
  return out;
}

std::optional<std::string> type_info(const clang::ASTContext &context,
                                     const clang::NamedDecl *decl) {
  // Typedef/alias -> underlying type; otherwise the cursor's own type
  // (clang_getCursorType): value decls their type, functions their prototype,
  // tag decls the tag type.
  const clang::PrintingPolicy &policy = context.getPrintingPolicy();
  clang::QualType type;
  if (const auto *td = llvm::dyn_cast<clang::TypedefNameDecl>(decl)) {
    type = td->getUnderlyingType();
  } else if (const auto *ft = llvm::dyn_cast<clang::FunctionTemplateDecl>(decl)) {
    // libclang's cursor type of a function template is the templated
    // function's prototype (e.g. "T (T)"). Class templates have none.
    type = ft->getTemplatedDecl()->getType();
  } else if (const auto *vd = llvm::dyn_cast<clang::ValueDecl>(decl)) {
    type = vd->getType();
  } else if (const auto *tag = llvm::dyn_cast<clang::TagDecl>(decl)) {
    // LLVM 22 deleted the TagDecl overload; go through the TypeDecl base.
    type = context.getTypeDeclType(static_cast<const clang::TypeDecl *>(tag));
  } else {
    return std::nullopt;
  }
  if (type.isNull())
    return std::nullopt;
  std::string s = type.getAsString(policy);
  if (s.empty())
    return std::nullopt;
  return s;
}

} // namespace cidx::lt
