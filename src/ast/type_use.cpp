#include "ast/type_use.hpp"

#include "ast/edge_sink.hpp"
#include "ast/location.hpp"
#include "ast/usr.hpp"

#include "clang/AST/Decl.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/AST/TemplateName.h"
#include "clang/AST/Type.h"

namespace cidx::ast {

const clang::NamedDecl *named_type_decl(clang::QualType type) {
  for (int i = 0; i < 32; ++i) {
    if (type.isNull()) {
      return nullptr;
    }
    // Strip as WRITTEN: non-canonical pointee/element steps so a typedef of
    // the pointee level is preserved exactly like libclang's walk.
    if (type->isPointerType() || type->isReferenceType()) {
      type = type->getPointeeType();
    } else if (const clang::ArrayType *at = type->getAsArrayTypeUnsafe()) {
      type = at->getElementType();
    } else {
      break;
    }
  }
  if (type.isNull()) {
    return nullptr;
  }
  // A typedef spelled at this level resolves to the ALIAS decl (libclang's
  // clang_getTypeDeclaration on the written type), looking through
  // elaboration sugar only.
  if (const auto *td = type->getAs<clang::TypedefType>()) {
    return td->getDecl();
  }
  if (const clang::TagDecl *tag = type->getAsTagDecl()) {
    return tag;
  }
  // Dependent Wrapper<T>: clang_getTypeDeclaration resolves to the class
  // TEMPLATE decl.
  if (const auto *tst = type->getAs<clang::TemplateSpecializationType>()) {
    if (const clang::TemplateDecl *tmpl =
            tst->getTemplateName().getAsTemplateDecl()) {
      return tmpl;
    }
  }
  return nullptr;
}

void emit_type_use(EdgeSink &sink, int64_t src_id, clang::QualType type,
                   int64_t file_id, const ExpansionLoc &loc, int conditional,
                   int64_t edge_kind) {
  const clang::NamedDecl *decl = named_type_decl(type);
  if (decl == nullptr) {
    return;
  }
  const std::string usr = usr_for_decl(decl);
  if (usr.empty()) {
    return;
  }
  const auto dst = sink.lookup_symbol_id(usr, loc.file);
  if (!dst || *dst == src_id) {
    return;
  }
  EdgeRecord e;
  e.src_id = src_id;
  e.dst_id = *dst;
  e.kind = edge_kind;
  const int64_t edge_id = sink.add_edge(e);
  if (loc.line != 0) {
    EdgeSiteRecord site;
    site.edge_id = edge_id;
    site.file_id = file_id;
    site.line = loc.line;
    site.col = loc.col;
    site.conditional = conditional;
    sink.add_edge_site(site);
  }
}

} // namespace cidx::ast
