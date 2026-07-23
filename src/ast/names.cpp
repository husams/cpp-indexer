#include "ast/names.hpp"

#include <ranges>

#include "ast/location.hpp"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/AST/PrettyPrinter.h"
#include "llvm/Support/raw_ostream.h"

namespace cidx::ast {

const clang::PrintingPolicy &printing_policy(const clang::ASTContext &context) {
  return context.getPrintingPolicy();
}

std::string spelling(const clang::NamedDecl *decl) {
  return decl->getDeclName().getAsString();
}

namespace {

// The AS-WRITTEN template argument list, comma-joined inside <>.
void print_written_args(llvm::raw_string_ostream &os,
                        const clang::PrintingPolicy &policy,
                        const clang::ASTTemplateArgumentListInfo *written) {
  os << '<';
  if (written != nullptr) {
    bool first = true;
    for (const clang::TemplateArgumentLoc &tal : written->arguments()) {
      if (!first) {
        os << ", ";
      }
      first = false;
      tal.getArgument().print(policy, os, /*IncludeType=*/false);
    }
  }
  os << '>';
}

// Function display: name, as-written spec args (an implicit instantiation
// shows empty <>, an explicit specialization its written args), and the
// parameter TYPE list.
void print_function_display(llvm::raw_string_ostream &os,
                            const clang::PrintingPolicy &policy,
                            const clang::FunctionDecl *fd) {
  if (const auto *method = llvm::dyn_cast<clang::CXXMethodDecl>(fd);
      method != nullptr && llvm::isa<clang::CXXConstructorDecl>(method)) {
    os << method->getParent()->getName();
  } else {
    fd->getDeclName().print(os, policy);
  }
  if (fd->getTemplateSpecializationArgs() != nullptr) {
    if (const auto *written = fd->getTemplateSpecializationArgsAsWritten()) {
      print_written_args(os, policy, written);
    } else {
      clang::printTemplateArgumentList(
          os, fd->getTemplateSpecializationArgs()->asArray(), policy);
    }
  } else if (const auto *ft = fd->getDescribedFunctionTemplate()) {
    os << '<';
    bool first = true;
    for (const clang::NamedDecl *param : *ft->getTemplateParameters()) {
      if (!first) {
        os << ", ";
      }
      first = false;
      os << param->getNameAsString();
      if (const auto *type = llvm::dyn_cast<clang::TemplateTypeParmDecl>(param);
          type != nullptr && type->isParameterPack()) {
        os << "...";
      }
    }
    os << '>';
  }
  os << '(';
  bool first = true;
  for (const clang::ParmVarDecl *p : fd->parameters()) {
    if (!first) {
      os << ", ";
    }
    first = false;
    os << p->getType().getAsString(policy);
  }
  if (fd->isVariadic()) {
    if (!first) {
      os << ", ";
    }
    os << "...";
  }
  os << ')';
}

// Class-template specialization display: partial specializations print their
// args AS WRITTEN ('RuleTemplate<int, NameType>', not canonical
// type-parameter-0-0); full specializations print the deduced list.
void print_class_spec_display(
    llvm::raw_string_ostream &os, const clang::PrintingPolicy &policy,
    const clang::ClassTemplateSpecializationDecl *spec) {
  spec->getDeclName().print(os, policy);
  if (const auto *partial =
          llvm::dyn_cast<clang::ClassTemplatePartialSpecializationDecl>(spec)) {
    if (const clang::ASTTemplateArgumentListInfo *written =
            partial->getTemplateArgsAsWritten()) {
      print_written_args(os, policy, written);
      return;
    }
  }
  clang::printTemplateArgumentList(os, spec->getTemplateArgs().asArray(),
                                   policy);
}

// The FunctionDecl whose signature identifies this decl: the decl itself, or a
// function template's templated function. Non-callables return null.
const clang::FunctionDecl *function_for_signature(const clang::NamedDecl *nd) {
  if (const auto *fd = llvm::dyn_cast<clang::FunctionDecl>(nd)) {
    return fd;
  }
  if (const auto *ft = llvm::dyn_cast<clang::FunctionTemplateDecl>(nd)) {
    return ft->getTemplatedDecl();
  }
  return nullptr;
}

// The leaf function/method's qualified-name rendering: its display form (name,
// as-written spec args, parameter TYPE list) plus the member cv/ref qualifiers
// that belong to the C++ signature, so a const overload keeps a qualified name
// distinct from its non-const twin.
void print_function_signature(llvm::raw_string_ostream &os,
                              const clang::PrintingPolicy &policy,
                              const clang::FunctionDecl *fd) {
  print_function_display(os, policy, fd);
  const auto *method = llvm::dyn_cast<clang::CXXMethodDecl>(fd);
  if (method == nullptr) {
    return;
  }
  if (method->isConst()) {
    os << " const";
  }
  if (method->isVolatile()) {
    os << " volatile";
  }
  switch (method->getRefQualifier()) {
  case clang::RQ_LValue:
    os << " &";
    break;
  case clang::RQ_RValue:
    os << " &&";
    break;
  case clang::RQ_None:
    break;
  }
}

// Mirror cidx qualified_name (ast_cursor.cpp): walk semantic parents to the TU,
// joining non-empty spellings with "::" (anonymous levels skipped). libclang
// spells a lambda's closure class "(lambda at <file>:<line>:<col>)", so lambda
// scopes survive in the chain. With with_signature, the leaf function/method
// carries its full signature so overloads stay distinct.
std::string join_qualified(const clang::ASTContext &context,
                           const clang::NamedDecl *decl, bool with_signature) {
  std::vector<std::string> parts;
  const clang::Decl *d = decl;
  while (d != nullptr && !llvm::isa<clang::TranslationUnitDecl>(d)) {
    if (const auto *nd = llvm::dyn_cast<clang::NamedDecl>(d)) {
      std::string s;
      const auto *rec = llvm::dyn_cast<clang::CXXRecordDecl>(nd);
      const auto *spec =
          llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(nd);
      if (rec != nullptr && rec->isLambda()) {
        const ExpansionLoc loc = expansion_loc(context, rec->getLocation());
        llvm::raw_string_ostream os(s);
        os << "(lambda at " << loc.file << ':' << loc.line << ':' << loc.col
           << ')';
      } else if (spec != nullptr) {
        // A specialization level carries its argument list, so MyClass<int>
        // and MyClass<double> (and their members) never collapse onto the
        // pattern's qualified name. Partial specializations print as written.
        llvm::raw_string_ostream os(s);
        print_class_spec_display(os, printing_policy(context), spec);
      } else if (const clang::FunctionDecl *fd =
                     with_signature && d == decl ? function_for_signature(nd)
                                                 : nullptr;
                 fd != nullptr) {
        // The leaf function/method carries its full signature (parameter types
        // plus member cv/ref qualifiers) so overloads never collapse onto one
        // qualified name. Enclosing function scopes stay bare names.
        llvm::raw_string_ostream os(s);
        print_function_signature(os, printing_policy(context), fd);
      } else if (rec != nullptr &&
                 rec->getDescribedClassTemplate() != nullptr) {
        s = rec->getNameAsString();
        s += '<';
        bool first = true;
        for (const clang::NamedDecl *param :
             *rec->getDescribedClassTemplate()->getTemplateParameters()) {
          if (!first) {
            s += ", ";
          }
          first = false;
          s += param->getNameAsString();
          if (const auto *type =
                  llvm::dyn_cast<clang::TemplateTypeParmDecl>(param);
              type != nullptr && type->isParameterPack()) {
            s += "...";
          }
        }
        s += '>';
      } else if (const auto *ct =
                     llvm::dyn_cast<clang::ClassTemplateDecl>(nd)) {
        s = ct->getNameAsString() + "<";
        bool first = true;
        for (const clang::NamedDecl *param : *ct->getTemplateParameters()) {
          if (!first)
            s += ", ";
          first = false;
          s += param->getNameAsString();
          if (const auto *type =
                  llvm::dyn_cast<clang::TemplateTypeParmDecl>(param);
              type != nullptr && type->isParameterPack())
            s += "...";
        }
        s += '>';
      } else {
        s = nd->getDeclName().getAsString();
      }
      if (!s.empty()) {
        parts.push_back(std::move(s));
      }
    }
    const clang::DeclContext *dc = d->getDeclContext();
    d = dc != nullptr ? llvm::dyn_cast<clang::Decl>(dc) : nullptr;
  }
  std::string out;
  for (const auto &part : std::views::reverse(parts)) {
    if (!out.empty()) {
      out += "::";
    }
    out += part;
  }
  return out;
}

} // namespace

std::string qualified_name(const clang::ASTContext &context,
                           const clang::NamedDecl *decl) {
  return join_qualified(context, decl, /*with_signature=*/true);
}

std::string qualified_name_bare(const clang::ASTContext &context,
                                const clang::NamedDecl *decl) {
  return join_qualified(context, decl, /*with_signature=*/false);
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
      if (!first) {
        os << ", ";
      }
      first = false;
      os << p->getName();
    }
    os << '>';
    return out;
  }
  if (const auto *fd = llvm::dyn_cast<clang::FunctionDecl>(decl)) {
    print_function_display(os, policy, fd);
  } else if (const auto *spec =
                 llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(decl)) {
    print_class_spec_display(os, policy, spec);
  } else {
    std::string s = spelling(decl);
    if (s.empty()) {
      return std::nullopt;
    }
    return s;
  }
  if (out.empty()) {
    return std::nullopt;
  }
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
  } else if (const auto *ft =
                 llvm::dyn_cast<clang::FunctionTemplateDecl>(decl)) {
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
  if (type.isNull()) {
    return std::nullopt;
  }
  std::string s = type.getAsString(policy);
  if (s.empty()) {
    return std::nullopt;
  }
  return s;
}

} // namespace cidx::ast
