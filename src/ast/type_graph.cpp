#include "ast/type_graph.hpp"

#include "ast/edge_sink.hpp"
#include "ast/usr.hpp"

#include "clang/AST/ASTContext.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/AST/TemplateBase.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/raw_ostream.h"

#include <utility>
#include <vector>

namespace cidx::ast {

namespace {

// Bound on structural recursion (layers of pointers/arrays/functions/args).
// Real code never approaches this; a hit falls back to an opaque node.
constexpr int kMaxDepth = 48;

// type_key grammar (deterministic; NOT for display):
//   node   := [ 'q:' cvr '(' node ')' ]        -- local const/volatile/restrict
//           | 'b:' canonical-spelling          -- builtin
//           | 't:' usr-or-qualname             -- record/enum (incl. spec decl)
//           | 'a:' usr-or-qualname             -- typedef / using alias
//           | 'at:' usr '(' node,* ')'         -- alias-template specialization
//           | 'T:' depth '.' index             -- template type parameter
//           | 'p(' node ')'                    -- pointer
//           | 'l(' node ')' | 'r(' node ')'    -- lvalue / rvalue reference
//           | 'A' [size] '(' node ')'          -- array
//           | 'f(' node ';' node,* [',...'] ')' ['#' flags]
//                                              -- function proto: ret; params;
//                                                 '...' = variadic; flags:
//                                                 c/v method quals, l/r ref-
//                                                 qualifier, n = cannot throw,
//                                                 d = dependent throw
//           | 'fnp(' node ')'                  -- function without prototype
//           | 'o:' canonical-spelling          -- anything else
// USRs make named layers unique (a specialization USR encodes its arguments);
// canonical spellings are stable for builtins/opaque shapes.

std::string cvr_flags(clang::Qualifiers quals) {
  std::string s;
  if (quals.hasConst()) {
    s += 'c';
  }
  if (quals.hasVolatile()) {
    s += 'v';
  }
  if (quals.hasRestrict()) {
    s += 'r';
  }
  return s;
}

std::string usr_or_qualname(const clang::NamedDecl *decl) {
  std::string usr = usr_for_decl(decl);
  if (!usr.empty()) {
    return usr;
  }
  return decl->getQualifiedNameAsString();
}

} // namespace

TypeInterner::TypeInterner(clang::ASTContext &context, EdgeSink &sink)
    : context_(context), sink_(sink) {}

std::optional<int64_t> TypeInterner::intern(clang::QualType qt) {
  const std::optional<Result> r = build(qt, 0);
  if (!r) {
    return std::nullopt;
  }
  return r->id;
}

// Intern `rec` (key/kind/decl_usr/children already decided), attaching the
// spelling, local qualifier flags, and the canonical link.
TypeInterner::Result TypeInterner::emit_node(clang::QualType qt,
                                             TypeNodeRecord rec, int depth) {
  const clang::Qualifiers quals = qt.getLocalQualifiers();
  const std::string flags = cvr_flags(quals);
  if (!flags.empty()) {
    rec.is_const = quals.hasConst();
    rec.is_volatile = quals.hasVolatile();
    rec.is_restrict = quals.hasRestrict();
    rec.type_key = "q:" + flags + "(" + rec.type_key + ")";
  }
  rec.spelling = qt.getAsString(context_.getPrintingPolicy());
  // Canonical link: a sugared shape (alias layers anywhere inside) points at
  // its canonical shape so alias-insensitive queries can unify them. A
  // distinct sugared QualType can still encode to the SAME key (e.g. an
  // elaborated TagType vs its canonical TagType both key on the decl USR);
  // comparing keys keeps such self-canonical rows at NULL instead of writing
  // a self-loop.
  const clang::QualType canon = context_.getCanonicalType(qt);
  if (canon.getAsOpaquePtr() != qt.getAsOpaquePtr() && depth < kMaxDepth) {
    if (const std::optional<Result> c = build(canon, depth + 1);
        c && c->key != rec.type_key) {
      rec.canonical_id = c->id;
    }
  }
  Result out;
  out.key = rec.type_key;
  out.id = sink_.intern_type_node(rec);
  memo_[qt.getAsOpaquePtr()] = out;
  return out;
}

std::optional<TypeInterner::Result> TypeInterner::build(clang::QualType qt,
                                                        int depth) {
  if (qt.isNull()) {
    return std::nullopt;
  }
  if (const auto hit = memo_.find(qt.getAsOpaquePtr()); hit != memo_.end()) {
    return hit->second;
  }
  const clang::Type *t = qt.getTypePtr();
  TypeNodeRecord rec;

  if (depth >= kMaxDepth) {
    const clang::QualType canon = context_.getCanonicalType(qt);
    rec.kind = kTypeOther;
    rec.type_key = "o:" + canon.getAsString(context_.getPrintingPolicy());
    return emit_node(qt, std::move(rec), depth);
  }

  // -- named alias layers (identity-preserving sugar) --------------------------
  if (const auto *tt = llvm::dyn_cast<clang::TypedefType>(t)) {
    rec.kind = kTypeAlias;
    rec.decl_usr = usr_for_decl(tt->getDecl());
    rec.type_key = "a:" + usr_or_qualname(tt->getDecl());
    const Result self = emit_node(qt, std::move(rec), depth);
    if (const auto target = build(tt->desugar(), depth + 1)) {
      sink_.add_type_edge(self.id, kTypeEdgeAliasOfK, 0, target->id);
    }
    return self;
  }
  if (const auto *tst = llvm::dyn_cast<clang::TemplateSpecializationType>(t);
      tst != nullptr && tst->isTypeAlias()) {
    // Alias-template specialization (`X<int>` where X is a `using` template):
    // keep the alias identity, link one desugar step, and expose the args.
    const clang::TemplateDecl *td = tst->getTemplateName().getAsTemplateDecl();
    rec.kind = kTypeAlias;
    if (td != nullptr) {
      rec.decl_usr = usr_for_decl(td);
    }
    std::string key = "at:";
    key += td != nullptr ? usr_or_qualname(td) : "?";
    key += "(";
    std::vector<std::pair<int64_t, std::optional<Result>>> args;
    bool first = true;
    for (unsigned i = 0; i < tst->template_arguments().size(); ++i) {
      const clang::TemplateArgument &arg = tst->template_arguments()[i];
      if (!first) {
        key += ",";
      }
      first = false;
      if (arg.getKind() == clang::TemplateArgument::Type) {
        auto r = build(arg.getAsType(), depth + 1);
        key += r ? r->key : "?";
        args.emplace_back(i, std::move(r));
      } else {
        std::string printed;
        llvm::raw_string_ostream os(printed);
        arg.print(context_.getPrintingPolicy(), os, /*IncludeType=*/true);
        key += "#" + printed;
      }
    }
    key += ")";
    rec.type_key = std::move(key);
    const Result self = emit_node(qt, std::move(rec), depth);
    for (const auto &[pos, r] : args) {
      if (r) {
        sink_.add_type_edge(self.id, kTypeEdgeTemplateArgK, pos, r->id);
      }
    }
    if (const auto target = build(tst->desugar(), depth + 1)) {
      sink_.add_type_edge(self.id, kTypeEdgeAliasOfK, 0, target->id);
    }
    return self;
  }

  // -- transparent sugar (elaborated, paren, using, subst, decltype, ...) ------
  {
    const clang::QualType step = qt.getSingleStepDesugaredType(context_);
    if (step.getAsOpaquePtr() != qt.getAsOpaquePtr() &&
        !llvm::isa<clang::TemplateSpecializationType>(t)) {
      return build(step, depth + 1);
    }
  }

  if (const auto *bt = llvm::dyn_cast<clang::BuiltinType>(t)) {
    rec.kind = kTypeBuiltin;
    rec.type_key =
        "b:" + clang::QualType(bt, 0).getAsString(context_.getPrintingPolicy());
    return emit_node(qt, std::move(rec), depth);
  }

  if (const auto *tp = llvm::dyn_cast<clang::TemplateTypeParmType>(t)) {
    rec.kind = kTypeTemplateParam;
    rec.type_key = "T:" + std::to_string(tp->getDepth()) + "." +
                   std::to_string(tp->getIndex());
    return emit_node(qt, std::move(rec), depth);
  }

  if (const clang::TagDecl *tag = t->getAsTagDecl()) {
    rec.kind = llvm::isa<clang::EnumDecl>(tag) ? kTypeEnum : kTypeRecord;
    rec.decl_usr = usr_for_decl(tag);
    rec.type_key = "t:" + usr_or_qualname(tag);
    const Result self = emit_node(qt, std::move(rec), depth);
    // A class-template specialization exposes its type arguments so
    // "accepts vector<Foo>" style closures reach Foo.
    if (const auto *spec =
            llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(tag)) {
      const clang::TemplateArgumentList &args = spec->getTemplateArgs();
      for (unsigned i = 0; i < args.size(); ++i) {
        if (args[i].getKind() != clang::TemplateArgument::Type) {
          continue;
        }
        if (const auto r = build(args[i].getAsType(), depth + 1)) {
          sink_.add_type_edge(self.id, kTypeEdgeTemplateArgK, i, r->id);
        }
      }
    }
    return self;
  }

  if (const auto *pt = llvm::dyn_cast<clang::PointerType>(t)) {
    const auto inner = build(pt->getPointeeType(), depth + 1);
    rec.kind = kTypePointer;
    rec.type_key = "p(" + (inner ? inner->key : "?") + ")";
    const Result self = emit_node(qt, std::move(rec), depth);
    if (inner) {
      sink_.add_type_edge(self.id, kTypeEdgePointeeK, 0, inner->id);
    }
    return self;
  }

  if (const auto *rt = llvm::dyn_cast<clang::ReferenceType>(t)) {
    const bool rvalue = llvm::isa<clang::RValueReferenceType>(t);
    const auto inner = build(rt->getPointeeTypeAsWritten(), depth + 1);
    rec.kind = rvalue ? kTypeRValueRef : kTypeLValueRef;
    rec.type_key =
        (rvalue ? "r(" : "l(") + (inner ? inner->key : "?") + ")";
    const Result self = emit_node(qt, std::move(rec), depth);
    if (inner) {
      sink_.add_type_edge(self.id, kTypeEdgePointeeK, 0, inner->id);
    }
    return self;
  }

  if (const auto *at = llvm::dyn_cast<clang::ArrayType>(t)) {
    const auto inner = build(at->getElementType(), depth + 1);
    std::string size;
    if (const auto *ca = llvm::dyn_cast<clang::ConstantArrayType>(t)) {
      llvm::SmallString<16> buf;
      ca->getSize().toString(buf, 10, /*Signed=*/false);
      size = std::string(buf);
    }
    rec.kind = kTypeArray;
    rec.type_key = "A" + size + "(" + (inner ? inner->key : "?") + ")";
    const Result self = emit_node(qt, std::move(rec), depth);
    if (inner) {
      sink_.add_type_edge(self.id, kTypeEdgeElementK, 0, inner->id);
    }
    return self;
  }

  if (const auto *fp = llvm::dyn_cast<clang::FunctionProtoType>(t)) {
    const auto ret = build(fp->getReturnType(), depth + 1);
    std::string key = "f(" + (ret ? ret->key : "?") + ";";
    std::vector<std::pair<int64_t, std::optional<Result>>> params;
    for (unsigned i = 0; i < fp->getNumParams(); ++i) {
      if (i != 0) {
        key += ",";
      }
      auto r = build(fp->getParamType(i), depth + 1);
      key += r ? r->key : "?";
      params.emplace_back(i, std::move(r));
    }
    if (fp->isVariadic()) {
      key += fp->getNumParams() != 0 ? ",..." : "...";
    }
    key += ")";
    // Return + parameters alone under-identify a function type: variadicness,
    // whether the type can throw, method cv-qualifiers, and the ref-qualifier
    // are all part of the shape (`void(int)` != `void(int, ...)` !=
    // `void(int) noexcept`). Encode them as a '#'-suffixed flag string so
    // distinct shapes never intern to one node. canThrow() is used instead of
    // the written exception-spec kind so `noexcept(false)`/`throw(...)`
    // spellings unify with their semantic equivalents.
    std::string flags;
    const clang::Qualifiers mq = fp->getMethodQuals();
    if (mq.hasConst()) {
      flags += 'c';
    }
    if (mq.hasVolatile()) {
      flags += 'v';
    }
    if (fp->getRefQualifier() == clang::RQ_LValue) {
      flags += 'l';
    } else if (fp->getRefQualifier() == clang::RQ_RValue) {
      flags += 'r';
    }
    if (fp->canThrow() == clang::CT_Cannot) {
      flags += 'n';
    } else if (fp->canThrow() == clang::CT_Dependent) {
      flags += 'd';
    }
    if (!flags.empty()) {
      key += "#" + flags;
    }
    rec.kind = kTypeFunction;
    rec.type_key = std::move(key);
    const Result self = emit_node(qt, std::move(rec), depth);
    if (ret) {
      sink_.add_type_edge(self.id, kTypeEdgeReturnK, 0, ret->id);
    }
    for (const auto &[pos, r] : params) {
      if (r) {
        sink_.add_type_edge(self.id, kTypeEdgeParamK, pos, r->id);
      }
    }
    return self;
  }
  if (const auto *fn = llvm::dyn_cast<clang::FunctionNoProtoType>(t)) {
    const auto ret = build(fn->getReturnType(), depth + 1);
    rec.kind = kTypeFunction;
    rec.type_key = "fnp(" + (ret ? ret->key : "?") + ")";
    const Result self = emit_node(qt, std::move(rec), depth);
    if (ret) {
      sink_.add_type_edge(self.id, kTypeEdgeReturnK, 0, ret->id);
    }
    return self;
  }

  // Everything else (member pointers, dependent shapes, packs, atomics, ...):
  // one opaque node keyed by the canonical print. No children.
  const clang::QualType canon = context_.getCanonicalType(qt);
  rec.kind = kTypeOther;
  rec.type_key = "o:" + canon.getAsString(context_.getPrintingPolicy());
  return emit_node(qt, std::move(rec), depth);
}

} // namespace cidx::ast
