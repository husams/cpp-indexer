// Clang-facing analyzer of cidx-diff (OBJECT library cidx_diff_ast): parse
// one side under its resolved configuration — the exact cidx-astgraph dump_tu
// recipe — and lower the target scope into the plain-data IRs: the syntax
// node tree (kind/label/children fingerprints; whitespace and comments are
// invisible), the clang::CFG-backed Behaviour IR of every callable, and the
// structural profile of every record.
#include "diff/analyze.hpp"

#include <algorithm>
#include <exception>
#include <map>
#include <memory>
#include <regex>
#include <utility>

#include "clang/AST/ASTConsumer.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Attr.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclFriend.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/PrettyPrinter.h"
#include "clang/AST/RecordLayout.h"
#include "clang/AST/Stmt.h"
#include "clang/AST/StmtCXX.h"
#include "clang/Analysis/CFG.h"
#include "clang/Basic/Diagnostic.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendAction.h"
#include "clang/Lex/Lexer.h"
#include "clang/Tooling/ArgumentsAdjusters.h"
#include "clang/Tooling/CompilationDatabase.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/raw_ostream.h"

#include "ast/clang_compat.hpp"
#include "ast/decl_flags.hpp"
#include "ast/location.hpp"
#include "ast/names.hpp"
#include "ast/usr.hpp"
#include "toolchain/toolchain.hpp"
#include "util/errors.hpp"
#include "util/hashing.hpp"

namespace cidx {
namespace diff {

namespace {

SrcRange to_range(const clang::ASTContext &ctx, clang::SourceRange r) {
  const ast::ExpansionLoc s = ast::extent_start(ctx, r);
  const ast::ExpansionLoc e = ast::extent_end(ctx, r);
  return {static_cast<int>(s.line), static_cast<int>(s.col),
          static_cast<int>(e.line), static_cast<int>(e.col)};
}

std::string fmt_range(const SrcRange &r) {
  return std::to_string(r.line) + ":" + std::to_string(r.col) + "-" +
         std::to_string(r.end_line) + ":" + std::to_string(r.end_col);
}

// Unnamed-type spellings embed the side's file path ("(unnamed struct at
// /l/a.cpp:3:1)"), turning identical declarations into false diffs. Drop the
// path, keep the kind and line:col.
std::string scrub_spelling(const std::string &s) {
  static const std::regex re(
      "\\((unnamed [^()]*|anonymous[^()]*|lambda) at [^()]+:(\\d+):(\\d+)\\)");
  return std::regex_replace(s, re, "($1 at $2:$3)");
}

// USRs of file-local entities (anonymous types, statics) embed the main
// file's basename, which differs between the two sides by construction.
// Rewrite it to a stable token so identical file-local code hashes equal.
std::string scrub_usr(const clang::ASTContext &ctx, std::string usr) {
  const clang::SourceManager &sm = ctx.getSourceManager();
  const auto fe = sm.getFileEntryRefForID(sm.getMainFileID());
  if (!fe)
    return usr;
  const std::string name = fe->getName().str();
  const std::size_t slash = name.find_last_of('/');
  const std::string base =
      slash == std::string::npos ? name : name.substr(slash + 1);
  if (base.empty())
    return usr;
  std::size_t pos = 0;
  while ((pos = usr.find(base, pos)) != std::string::npos) {
    usr.replace(pos, base.size(), "{src}");
    pos += 5;
  }
  return usr;
}

// Structural fingerprint over kind/label/children — never source ranges, so
// formatting-only edits hash identically.
void seal(SynNode &n) {
  std::string acc = n.kind;
  acc += '\x1f';
  acc += n.label;
  for (const SynNode &c : n.children) {
    acc += '\x1e';
    acc += c.fingerprint;
  }
  n.fingerprint = sha1_hex(acc);
}

// Entity fingerprint excludes the top node's own label (which carries the
// entity's name) so the heuristic rename tier can match on structure alone.
std::string structural_fingerprint(const SynNode &n) {
  std::string acc = n.kind;
  for (const SynNode &c : n.children) {
    acc += '\x1e';
    acc += c.fingerprint;
  }
  return sha1_hex(acc);
}

std::string entity_kind(const clang::Decl *d) {
  if (llvm::isa<clang::CXXMethodDecl>(d))
    return "method";
  if (llvm::isa<clang::FunctionDecl>(d))
    return "function";
  if (llvm::isa<clang::EnumDecl>(d))
    return "enum";
  if (const auto *td = llvm::dyn_cast<clang::TagDecl>(d)) {
    switch (td->getTagKind()) {
    case clang::TagTypeKind::Class:
      return "class";
    case clang::TagTypeKind::Struct:
      return "struct";
    case clang::TagTypeKind::Union:
      return "union";
    default:
      return "other";
    }
  }
  if (llvm::isa<clang::VarDecl>(d) || llvm::isa<clang::FieldDecl>(d))
    return "variable";
  if (llvm::isa<clang::TypedefNameDecl>(d))
    return "typedef";
  if (llvm::isa<clang::NamespaceDecl>(d) ||
      llvm::isa<clang::NamespaceAliasDecl>(d))
    return "namespace";
  return "other";
}

// Stable name for a sizeof/alignof-family operator. The numeric fallback keeps
// distinct trait kinds distinct across LLVM versions even when unnamed here.
std::string unary_trait_name(clang::UnaryExprOrTypeTrait k) {
  switch (k) {
  case clang::UETT_SizeOf:
    return "sizeof";
  case clang::UETT_DataSizeOf:
    return "__datasizeof";
  case clang::UETT_AlignOf:
    return "alignof";
  case clang::UETT_PreferredAlignOf:
    return "__alignof";
  case clang::UETT_VecStep:
    return "vec_step";
  default:
    return "typetrait#" + std::to_string(static_cast<int>(k));
  }
}

const char *access_str(clang::AccessSpecifier as) {
  switch (as) {
  case clang::AS_private:
    return "private";
  case clang::AS_protected:
    return "protected";
  default:
    return "public";
  }
}

std::string source_slice(const clang::ASTContext &ctx, clang::SourceRange r) {
  const clang::SourceManager &sm = ctx.getSourceManager();
  const clang::CharSourceRange cr = clang::CharSourceRange::getTokenRange(
      sm.getExpansionRange(r).getAsRange());
  return clang::Lexer::getSourceText(cr, sm, ctx.getLangOpts()).str();
}

std::string join(const std::vector<std::string> &parts, const char *sep) {
  std::string out;
  for (const std::string &p : parts) {
    if (!out.empty())
      out += sep;
    out += p;
  }
  return out;
}

// cv/ref qualifiers of an instance method as one canonical token stream:
// "const", "const volatile &&", ... ; "" for free/static callables.
std::string method_quals(const clang::FunctionDecl *fd) {
  const auto *m = llvm::dyn_cast<clang::CXXMethodDecl>(fd);
  if (m == nullptr || !m->isInstance())
    return {};
  const auto *fpt = fd->getType()->getAs<clang::FunctionProtoType>();
  if (fpt == nullptr)
    return {};
  std::vector<std::string> parts;
  if (fpt->getMethodQuals().hasConst())
    parts.push_back("const");
  if (fpt->getMethodQuals().hasVolatile())
    parts.push_back("volatile");
  if (fpt->getRefQualifier() == clang::RQ_LValue)
    parts.push_back("&");
  else if (fpt->getRefQualifier() == clang::RQ_RValue)
    parts.push_back("&&");
  return join(parts, " ");
}

// The *written* explicit-specifier of a constructor/conversion as a canonical
// token, for the syntax fingerprint. `isExplicit()` collapses a written
// `explicit(false)` to "not explicit", so it would compare equal to a plain
// declaration and hide a real source change; `getExplicitSpecifier()` preserves
// the written form. When a condition expression is written -- `explicit(cond)`
// -- its normalized source is kept regardless of whether Clang resolved the
// condition, so `explicit(true)`, `explicit(0)`, and a dependent
// `explicit(sizeof(T)>1)` stay distinct from a bare `explicit` and from each
// other (`getKind()` alone maps explicit(true)->explicit and
// explicit(0)->explicit(false), collapsing written forms). Whitespace is
// stripped so `explicit( true )` and `explicit(true)` fingerprint identically.
// Bare `explicit` is reserved for a specified form with no expression. Empty
// when none is written.
std::string explicit_spec_token(const clang::FunctionDecl *fd) {
  clang::ExplicitSpecifier es;
  if (const auto *ctor = llvm::dyn_cast<clang::CXXConstructorDecl>(fd))
    es = ctor->getExplicitSpecifier();
  else if (const auto *conv = llvm::dyn_cast<clang::CXXConversionDecl>(fd))
    es = conv->getExplicitSpecifier();
  else
    return {};
  if (!es.isSpecified())
    return {};
  if (const clang::Expr *e = es.getExpr()) {
    std::string cond = source_slice(fd->getASTContext(), e->getSourceRange());
    cond.erase(std::remove_if(cond.begin(), cond.end(),
                              [](char c) {
                                return c == ' ' || c == '\t' || c == '\n' ||
                                       c == '\r' || c == '\f' || c == '\v';
                              }),
               cond.end());
    return "explicit(" + cond + ")";
  }
  return "explicit";
}

// Canonical method API-state token appended to a callable's syntax label so a
// directly selected method -- whose extent excludes the surrounding access
// specifier -- still fingerprints access / final / pure / override / explicit /
// deleted / defaulted changes. This is the *syntax* fingerprint, so it tracks
// the written tokens: the `override` keyword (OverrideAttr) rather than whether
// the method happens to override (size_overridden_methods() stays non-zero when
// the keyword is removed), and the written explicit-specifier including an
// `explicit(false)` condition. A public->private move, a removed `override`, or
// an added `= delete` is a real source change, not a syntactically unchanged
// one. Empty for callables with no API state (e.g. an ordinary free function),
// so their fingerprints are unaffected.
std::string method_api_state(const clang::FunctionDecl *fd) {
  std::vector<std::string> parts;
  if (const auto *m = llvm::dyn_cast<clang::CXXMethodDecl>(fd)) {
    if (m->getAccess() != clang::AS_none)
      parts.push_back(std::string("access=") + access_str(m->getAccess()));
    if (m->hasAttr<clang::FinalAttr>())
      parts.push_back("final");
    if (m->isPureVirtual())
      parts.push_back("pure");
    if (m->hasAttr<clang::OverrideAttr>())
      parts.push_back("override");
  }
  if (const std::string ex = explicit_spec_token(fd); !ex.empty())
    parts.push_back(ex);
  if (fd->isDeleted())
    parts.push_back("deleted");
  if (fd->isDefaulted())
    parts.push_back("defaulted");
  if (parts.empty())
    return {};
  return " [" + join(parts, " ") + "]";
}

// Collects constructs outside the supported behavioral subset (docs/diff.md
// "Semantic model") with their source ranges instead of approximating them.
struct UnsupportedScan {
  const clang::ASTContext &ctx;
  std::vector<Unsupported> *out;

  void add(const char *what, clang::SourceRange r) {
    out->push_back({what, to_range(ctx, r), ""});
  }

  void walk(const clang::Stmt *s) {
    if (s == nullptr)
      return;
    if (llvm::isa<clang::AsmStmt>(s)) {
      add("inline assembly", s->getSourceRange());
    } else if (llvm::isa<clang::CXXTryStmt>(s) ||
               llvm::isa<clang::SEHTryStmt>(s)) {
      add("try/catch", s->getSourceRange());
    } else if (llvm::isa<clang::CoroutineBodyStmt>(s) ||
               llvm::isa<clang::CoroutineSuspendExpr>(s) ||
               llvm::isa<clang::CoreturnStmt>(s)) {
      add("coroutine", s->getSourceRange());
    } else if (llvm::isa<clang::StmtExpr>(s)) {
      add("statement expression", s->getSourceRange());
    } else if (llvm::isa<clang::LambdaExpr>(s)) {
      add("lambda", s->getSourceRange());
      return; // the closure body is not part of this callable's subset
    } else if (llvm::isa<clang::GotoStmt>(s) ||
               llvm::isa<clang::IndirectGotoStmt>(s)) {
      add("goto", s->getSourceRange());
    } else if (llvm::isa<clang::VAArgExpr>(s)) {
      add("varargs", s->getSourceRange());
    } else if (llvm::isa<clang::AtomicExpr>(s)) {
      add("atomic access", s->getSourceRange());
    }
    if (const auto *e = llvm::dyn_cast<clang::Expr>(s);
        e != nullptr && !e->getType().isNull()) {
      if (e->isTypeDependent() || e->isValueDependent()) {
        add("dependent template code", s->getSourceRange());
        return;
      }
      const bool named_access =
          llvm::isa<clang::DeclRefExpr>(e) || llvm::isa<clang::MemberExpr>(e);
      const auto *cast = llvm::dyn_cast<clang::CastExpr>(e);
      const bool load =
          cast != nullptr && cast->getCastKind() == clang::CK_LValueToRValue &&
          cast->getSubExpr()->getType().isVolatileQualified();
      const auto *bin = llvm::dyn_cast<clang::BinaryOperator>(e);
      const bool store = bin != nullptr &&
                         (bin->isAssignmentOp() ||
                          bin->isCompoundAssignmentOp()) &&
                         bin->getLHS()->getType().isVolatileQualified();
      const auto *un = llvm::dyn_cast<clang::UnaryOperator>(e);
      const bool incdec =
          un != nullptr && un->isIncrementDecrementOp() &&
          un->getSubExpr()->getType().isVolatileQualified();
      if ((named_access && e->getType().isVolatileQualified()) || load ||
          store || incdec)
        add("volatile access", s->getSourceRange());
      if (named_access && e->getType()->isAtomicType())
        add("atomic access", s->getSourceRange());
    }
    if (const auto *ds = llvm::dyn_cast<clang::DeclStmt>(s)) {
      for (const clang::Decl *d : ds->decls()) {
        if (const auto *vd = llvm::dyn_cast<clang::VarDecl>(d)) {
          if (vd->getTLSKind() != clang::VarDecl::TLS_None)
            add("thread_local local", d->getSourceRange());
          else if (vd->isStaticLocal())
            add("static local", d->getSourceRange());
        }
      }
    }
    for (const clang::Stmt *c : s->children())
      walk(c);
  }
};

// Renders statements/expressions of one callable body into the normalized
// Behaviour IR text: parameters/locals alpha-renamed in first-appearance
// order, ParenExpr/ExprWithCleanups/no-op implicit casts erased (loads and
// value-changing casts kept), calls/ctors/dtors resolved by USR.
struct IrBuilder {
  clang::ASTContext &ctx;
  const clang::PrintingPolicy &pp;
  std::map<const clang::ValueDecl *, std::string> names;
  int locals = 0;

  std::string type_str(clang::QualType t) const {
    return scrub_spelling(t.getCanonicalType().getAsString(pp));
  }

  std::string value_name(const clang::ValueDecl *vd) {
    const auto it = names.find(vd);
    if (it != names.end())
      return it->second;
    if (const auto *var = llvm::dyn_cast<clang::VarDecl>(vd);
        var != nullptr && var->hasLocalStorage()) {
      std::string n = "%l" + std::to_string(locals++);
      names.emplace(vd, n);
      return n;
    }
    const std::string usr = ast::usr_for_decl(vd);
    return usr.empty() ? "id:" + scrub_spelling(ast::spelling(vd))
                       : scrub_usr(ctx, usr);
  }

  std::string callee_name(const clang::NamedDecl *nd) const {
    const std::string usr = ast::usr_for_decl(nd);
    return usr.empty() ? scrub_spelling(ast::spelling(nd))
                       : scrub_usr(ctx, usr);
  }

  std::string text(const clang::Stmt *s) {
    if (s == nullptr)
      return "<null>";
    if (const auto *pe = llvm::dyn_cast<clang::ParenExpr>(s))
      return text(pe->getSubExpr());
    if (const auto *fe = llvm::dyn_cast<clang::FullExpr>(s))
      return text(fe->getSubExpr()); // ExprWithCleanups / ConstantExpr framing
    if (const auto *ice = llvm::dyn_cast<clang::ImplicitCastExpr>(s)) {
      if (ice->getCastKind() == clang::CK_NoOp)
        return text(ice->getSubExpr());
      if (ice->getCastKind() == clang::CK_LValueToRValue)
        return "load(" + text(ice->getSubExpr()) + ")";
      return std::string("cast<") + ice->getCastKindName() + "," +
             type_str(ice->getType()) + ">(" + text(ice->getSubExpr()) + ")";
    }
    if (const auto *ec = llvm::dyn_cast<clang::ExplicitCastExpr>(s))
      return std::string("cast<") + ec->getCastKindName() + "," +
             type_str(ec->getTypeAsWritten()) + ">(" + text(ec->getSubExpr()) +
             ")";
    if (const auto *da = llvm::dyn_cast<clang::CXXDefaultArgExpr>(s))
      return text(da->getExpr());
    if (const auto *di = llvm::dyn_cast<clang::CXXDefaultInitExpr>(s))
      return text(di->getExpr());
    if (const auto *dre = llvm::dyn_cast<clang::DeclRefExpr>(s))
      return value_name(dre->getDecl());
    if (llvm::isa<clang::CXXThisExpr>(s))
      return "%this";
    if (const auto *il = llvm::dyn_cast<clang::IntegerLiteral>(s)) {
      llvm::SmallString<32> buf;
      il->getValue().toString(buf, 10, il->getType()->isSignedIntegerType());
      return std::string(buf);
    }
    if (const auto *fl = llvm::dyn_cast<clang::FloatingLiteral>(s)) {
      llvm::SmallString<32> buf;
      fl->getValue().toString(buf);
      return std::string(buf);
    }
    if (const auto *cl = llvm::dyn_cast<clang::CharacterLiteral>(s))
      return "'" + std::to_string(cl->getValue()) + "'";
    if (const auto *bl = llvm::dyn_cast<clang::CXXBoolLiteralExpr>(s))
      return bl->getValue() ? "true" : "false";
    if (const auto *sl = llvm::dyn_cast<clang::StringLiteral>(s))
      return "\"" + sl->getBytes().str() + "\"";
    if (llvm::isa<clang::CXXNullPtrLiteralExpr>(s))
      return "nullptr";
    if (const auto *me = llvm::dyn_cast<clang::MemberExpr>(s))
      return text(me->getBase()) + (me->isArrow() ? "->" : ".") +
             me->getMemberNameInfo().getAsString();
    if (const auto *ce = llvm::dyn_cast<clang::CallExpr>(s)) {
      std::string out = "call ";
      const clang::Decl *cd = ce->getCalleeDecl();
      const auto *nd =
          cd != nullptr ? llvm::dyn_cast<clang::NamedDecl>(cd) : nullptr;
      out += nd != nullptr ? callee_name(nd) : text(ce->getCallee());
      std::vector<std::string> parts;
      if (const auto *mc = llvm::dyn_cast<clang::CXXMemberCallExpr>(ce))
        parts.push_back(text(mc->getImplicitObjectArgument()));
      for (const clang::Expr *a : ce->arguments())
        parts.push_back(text(a));
      return out + "(" + join(parts, ", ") + ")";
    }
    if (const auto *cc = llvm::dyn_cast<clang::CXXConstructExpr>(s)) {
      std::vector<std::string> parts;
      for (const clang::Expr *a : cc->arguments())
        parts.push_back(text(a));
      return "ctor " + callee_name(cc->getConstructor()) + "(" +
             join(parts, ", ") + ")";
    }
    if (const auto *ne = llvm::dyn_cast<clang::CXXNewExpr>(s)) {
      std::string out = ne->isArray() ? "new[] " : "new ";
      out += type_str(ne->getAllocatedType());
      if (ne->isArray() && ne->getArraySize())
        out += "[" + text(*ne->getArraySize()) + "]";
      if (ne->hasInitializer())
        out += "(" + text(ne->getInitializer()) + ")";
      return out;
    }
    if (const auto *de = llvm::dyn_cast<clang::CXXDeleteExpr>(s))
      return (de->isArrayForm() ? "delete[] " : "delete ") +
             text(de->getArgument());
    if (const auto *te = llvm::dyn_cast<clang::CXXThrowExpr>(s))
      return te->getSubExpr() != nullptr ? "throw " + text(te->getSubExpr())
                                         : "throw";
    if (const auto *mte = llvm::dyn_cast<clang::MaterializeTemporaryExpr>(s))
      return "temp(" + text(mte->getSubExpr()) + ")";
    if (const auto *bt = llvm::dyn_cast<clang::CXXBindTemporaryExpr>(s))
      return "bind(" + text(bt->getSubExpr()) + ")";
    if (const auto *bo = llvm::dyn_cast<clang::BinaryOperator>(s))
      return "(" + text(bo->getLHS()) + " " + bo->getOpcodeStr().str() + " " +
             text(bo->getRHS()) + ")";
    if (const auto *ue = llvm::dyn_cast<clang::UnaryExprOrTypeTraitExpr>(s)) {
      // sizeof/alignof of a type carries the operand as a type, not a child
      // node; encode the canonical type so sizeof(int) != sizeof(long).
      if (ue->isArgumentType())
        return unary_trait_name(ue->getKind()) + "(" +
               type_str(ue->getArgumentType()) + ")";
      return unary_trait_name(ue->getKind()) + "(" +
             text(ue->getArgumentExpr()) + ")";
    }
    if (const auto *uo = llvm::dyn_cast<clang::UnaryOperator>(s))
      return "(" +
             clang::UnaryOperator::getOpcodeStr(uo->getOpcode()).str() +
             (uo->isPostfix() ? "post" : "") + " " + text(uo->getSubExpr()) +
             ")";
    if (const auto *co = llvm::dyn_cast<clang::ConditionalOperator>(s))
      return "(" + text(co->getCond()) + " ? " + text(co->getTrueExpr()) +
             " : " + text(co->getFalseExpr()) + ")";
    if (const auto *as = llvm::dyn_cast<clang::ArraySubscriptExpr>(s))
      return text(as->getBase()) + "[" + text(as->getIdx()) + "]";
    if (const auto *ile = llvm::dyn_cast<clang::InitListExpr>(s)) {
      std::vector<std::string> parts;
      for (const clang::Stmt *c : ile->children())
        parts.push_back(text(c));
      return "{" + join(parts, ", ") + "}";
    }
    if (llvm::isa<clang::ImplicitValueInitExpr>(s))
      return "zeroinit";
    if (const auto *ds = llvm::dyn_cast<clang::DeclStmt>(s)) {
      std::vector<std::string> parts;
      for (const clang::Decl *d : ds->decls()) {
        if (const auto *vd = llvm::dyn_cast<clang::VarDecl>(d)) {
          std::string p =
              "decl " + value_name(vd) + " : " + type_str(vd->getType());
          if (vd->hasInit())
            p += " = " + text(vd->getInit());
          parts.push_back(std::move(p));
        } else if (const auto *nd = llvm::dyn_cast<clang::NamedDecl>(d)) {
          parts.push_back("decl " + ast::spelling(nd));
        }
      }
      return join(parts, "; ");
    }
    if (const auto *rs = llvm::dyn_cast<clang::ReturnStmt>(s))
      return rs->getRetValue() != nullptr ? "return " + text(rs->getRetValue())
                                          : "return";
    std::string out = s->getStmtClassName();
    std::vector<std::string> parts;
    for (const clang::Stmt *c : s->children())
      parts.push_back(text(c));
    return out + "(" + join(parts, ", ") + ")";
  }
};

// A CFG element that is pure framing renders nothing: the contract's erasure
// set (ParenExpr, ExprWithCleanups, no-op implicit casts) applied to the
// element stream so a paren-only edit does not change the IR.
bool transparent_element(const clang::Stmt *s) {
  if (llvm::isa<clang::ParenExpr>(s) || llvm::isa<clang::FullExpr>(s))
    return true;
  if (const auto *ice = llvm::dyn_cast<clang::ImplicitCastExpr>(s))
    return ice->getCastKind() == clang::CK_NoOp;
  return false;
}

struct Analyzer {
  clang::ASTContext &ctx;
  const clang::PrintingPolicy &pp;
  std::string target_file;
  std::vector<BehaviourIR> *behaviours;
  std::vector<ClassProfile> *profiles;

  std::string type_str(clang::QualType t) const {
    return scrub_spelling(t.getCanonicalType().getAsString(pp));
  }

  std::string param_types(const clang::FunctionDecl *fd) const {
    std::string out;
    for (const clang::ParmVarDecl *p : fd->parameters()) {
      if (!out.empty())
        out += ", ";
      out += type_str(p->getType());
    }
    if (fd->isVariadic())
      out += out.empty() ? "..." : ", ...";
    return out;
  }

  std::string fn_quals(const clang::FunctionDecl *fd) const {
    std::string q;
    const auto *m = llvm::dyn_cast<clang::CXXMethodDecl>(fd);
    if (m == nullptr || !m->isInstance())
      return q;
    if (const auto *fpt = fd->getType()->getAs<clang::FunctionProtoType>()) {
      if (fpt->getMethodQuals().hasConst())
        q += " const";
      if (fpt->getMethodQuals().hasVolatile())
        q += " volatile";
      if (fpt->getRefQualifier() == clang::RQ_LValue)
        q += " &";
      else if (fpt->getRefQualifier() == clang::RQ_RValue)
        q += " &&";
    }
    return q;
  }

  // Transparent nodes are erased (children hoisted); purely implicit nodes
  // are dropped, so an AST rebuilt from identical tokens lowers identically.
  void add_stmt(const clang::Stmt *s, std::vector<SynNode> &out) {
    if (s == nullptr)
      return;
    if (const auto *ice = llvm::dyn_cast<clang::ImplicitCastExpr>(s)) {
      add_stmt(ice->getSubExpr(), out);
      return;
    }
    if (const auto *fe = llvm::dyn_cast<clang::FullExpr>(s)) {
      add_stmt(fe->getSubExpr(), out);
      return;
    }
    if (const auto *mte = llvm::dyn_cast<clang::MaterializeTemporaryExpr>(s)) {
      add_stmt(mte->getSubExpr(), out);
      return;
    }
    if (const auto *bt = llvm::dyn_cast<clang::CXXBindTemporaryExpr>(s)) {
      add_stmt(bt->getSubExpr(), out);
      return;
    }
    if (llvm::isa<clang::CXXDefaultArgExpr>(s) ||
        llvm::isa<clang::CXXDefaultInitExpr>(s) ||
        llvm::isa<clang::ImplicitValueInitExpr>(s))
      return;
    if (const auto *te = llvm::dyn_cast<clang::CXXThisExpr>(s);
        te != nullptr && te->isImplicit())
      return;
    out.push_back(lower_stmt(s));
  }

  void set_stmt_label(const clang::Stmt *s, SynNode &n) {
    if (const auto *bo = llvm::dyn_cast<clang::BinaryOperator>(s)) {
      n.label = bo->getOpcodeStr().str();
      n.detail = "operator " + n.label;
    } else if (const auto *uo = llvm::dyn_cast<clang::UnaryOperator>(s)) {
      n.label = clang::UnaryOperator::getOpcodeStr(uo->getOpcode()).str();
      // x++ and ++x share an opcode string; the fixity is the whole difference.
      if (uo->isPostfix())
        n.label += " (postfix)";
      n.detail = "operator " + n.label;
    } else if (const auto *ue =
                   llvm::dyn_cast<clang::UnaryExprOrTypeTraitExpr>(s)) {
      // A type operand (sizeof(int)) is not a child node, so it must ride in
      // the label or the syntax fingerprint cannot tell the operands apart.
      n.label = unary_trait_name(ue->getKind());
      if (ue->isArgumentType())
        n.label += "(" + type_str(ue->getArgumentType()) + ")";
      n.detail = "trait " + n.label;
    } else if (const auto *dre = llvm::dyn_cast<clang::DeclRefExpr>(s)) {
      n.label = dre->getNameInfo().getAsString();
      n.detail = "ref " + n.label;
    } else if (const auto *me = llvm::dyn_cast<clang::MemberExpr>(s)) {
      n.label = me->getMemberNameInfo().getAsString();
      n.detail = "member " + n.label;
    } else if (const auto *il = llvm::dyn_cast<clang::IntegerLiteral>(s)) {
      llvm::SmallString<32> buf;
      il->getValue().toString(buf, 10, il->getType()->isSignedIntegerType());
      n.label = std::string(buf);
      n.detail = "literal " + n.label;
    } else if (const auto *fl = llvm::dyn_cast<clang::FloatingLiteral>(s)) {
      llvm::SmallString<32> buf;
      fl->getValue().toString(buf);
      n.label = std::string(buf);
      n.detail = "literal " + n.label;
    } else if (const auto *cl = llvm::dyn_cast<clang::CharacterLiteral>(s)) {
      n.label = std::to_string(cl->getValue());
      n.detail = "literal " + n.label;
    } else if (const auto *bl = llvm::dyn_cast<clang::CXXBoolLiteralExpr>(s)) {
      n.label = bl->getValue() ? "true" : "false";
      n.detail = "literal " + n.label;
    } else if (const auto *sl = llvm::dyn_cast<clang::StringLiteral>(s)) {
      n.label = sl->getBytes().str();
      n.detail = "literal " + n.label;
    } else if (llvm::isa<clang::CXXNullPtrLiteralExpr>(s)) {
      n.label = "nullptr";
      n.detail = "literal nullptr";
    } else if (const auto *cc = llvm::dyn_cast<clang::CXXConstructExpr>(s)) {
      const std::string name =
          scrub_spelling(ast::spelling(cc->getConstructor()));
      n.label =
          name + "|" + scrub_usr(ctx, ast::usr_for_decl(cc->getConstructor()));
      n.detail = "ctor " + name;
    } else if (const auto *ec = llvm::dyn_cast<clang::ExplicitCastExpr>(s)) {
      const std::string ty = type_str(ec->getTypeAsWritten());
      n.label = std::string(ec->getCastKindName()) + " " + ty;
      n.detail = "cast " + ty;
    } else if (const auto *ls = llvm::dyn_cast<clang::LabelStmt>(s)) {
      n.label = ls->getName();
      n.detail = "label " + n.label;
    } else if (const auto *gs = llvm::dyn_cast<clang::GotoStmt>(s)) {
      n.label = gs->getLabel()->getName().str();
      n.detail = "label " + n.label;
    }
  }

  SynNode lower_stmt(const clang::Stmt *s) {
    SynNode n;
    n.kind = s->getStmtClassName();
    n.range = to_range(ctx, s->getSourceRange());
    if (const auto *ds = llvm::dyn_cast<clang::DeclStmt>(s)) {
      for (const clang::Decl *d : ds->decls())
        add_decl(d, n.children);
    } else if (const auto *ce = llvm::dyn_cast<clang::CallExpr>(s)) {
      // The resolved callee lives in the label (USR + spelling); the callee
      // subexpression is lowered only when unresolved, so a callee change is
      // exactly one op.
      const clang::Decl *cd = ce->getCalleeDecl();
      const auto *nd = cd != nullptr ? llvm::dyn_cast<clang::NamedDecl>(cd)
                                     : nullptr;
      if (nd != nullptr) {
        const std::string name = scrub_spelling(ast::spelling(nd));
        n.label = name + "|" + scrub_usr(ctx, ast::usr_for_decl(nd));
        n.detail = "callee " + name;
      } else {
        add_stmt(ce->getCallee(), n.children);
      }
      if (const auto *mc = llvm::dyn_cast<clang::CXXMemberCallExpr>(ce))
        add_stmt(mc->getImplicitObjectArgument(), n.children);
      for (const clang::Expr *a : ce->arguments())
        add_stmt(a, n.children);
    } else {
      set_stmt_label(s, n);
      for (const clang::Stmt *c : s->children())
        add_stmt(c, n.children);
    }
    seal(n);
    return n;
  }

  void add_decl(const clang::Decl *d, std::vector<SynNode> &out) {
    if (d == nullptr || d->isImplicit() || llvm::isa<clang::EmptyDecl>(d))
      return;
    out.push_back(lower_decl(d));
  }

  void lower_function(const clang::FunctionDecl *fd, SynNode &n) {
    const std::string name = ast::spelling(fd);
    n.label =
        name + "(" + param_types(fd) + ")" + fn_quals(fd) + method_api_state(fd);
    n.detail = "name " + name;
    for (const clang::ParmVarDecl *p : fd->parameters())
      add_decl(p, n.children);
    if (const auto *ctor = llvm::dyn_cast<clang::CXXConstructorDecl>(fd)) {
      for (const clang::CXXCtorInitializer *ci : ctor->inits()) {
        if (!ci->isWritten())
          continue;
        SynNode c;
        c.kind = "CXXCtorInitializer";
        if (ci->isMemberInitializer())
          c.label = ci->getMember()->getNameAsString();
        else if (ci->isBaseInitializer())
          c.label = type_str(clang::QualType(ci->getBaseClass(), 0));
        if (!c.label.empty())
          c.detail = "init " + c.label;
        c.range = to_range(ctx, ci->getSourceRange());
        add_stmt(ci->getInit(), c.children);
        seal(c);
        n.children.push_back(std::move(c));
      }
    }
    if (fd->doesThisDeclarationHaveABody())
      add_stmt(fd->getBody(), n.children);
  }

  void lower_record(const clang::RecordDecl *rd, SynNode &n) {
    const std::string name = ast::spelling(rd);
    n.label = name;
    if (!name.empty())
      n.detail = "name " + name;
    if (!rd->isThisDeclarationADefinition())
      return;
    if (const auto *cxx = llvm::dyn_cast<clang::CXXRecordDecl>(rd)) {
      for (const clang::CXXBaseSpecifier &b : cxx->bases()) {
        SynNode c;
        c.kind = "CXXBaseSpecifier";
        const std::string ty = type_str(b.getType());
        c.label = std::string(access_str(b.getAccessSpecifier())) +
                  (b.isVirtual() ? " virtual " : " ") + ty;
        c.detail = "base " + ty;
        c.range = to_range(ctx, b.getSourceRange());
        seal(c);
        n.children.push_back(std::move(c));
      }
    }
    for (const clang::Decl *m : rd->decls())
      add_decl(m, n.children);
  }

  SynNode lower_decl(const clang::Decl *d) {
    SynNode n;
    n.kind = d->getDeclKindName();
    n.range = to_range(ctx, d->getSourceRange());
    if (const auto *td = llvm::dyn_cast<clang::TemplateDecl>(d);
        td != nullptr && td->getTemplatedDecl() != nullptr) {
      const std::string name = ast::spelling(td);
      n.label = name;
      n.detail = "name " + name;
      n.children.push_back(lower_decl(td->getTemplatedDecl()));
    } else if (const auto *fd = llvm::dyn_cast<clang::FunctionDecl>(d)) {
      lower_function(fd, n);
    } else if (const auto *vd = llvm::dyn_cast<clang::VarDecl>(d)) {
      const std::string name = ast::spelling(vd);
      n.label = name + " : " + type_str(vd->getType());
      n.detail = "name " + name;
      if (const auto *pv = llvm::dyn_cast<clang::ParmVarDecl>(vd)) {
        if (pv->hasDefaultArg() && !pv->hasUnparsedDefaultArg() &&
            !pv->hasUninstantiatedDefaultArg())
          add_stmt(pv->getDefaultArg(), n.children);
      } else if (vd->hasInit()) {
        add_stmt(vd->getInit(), n.children);
      }
    } else if (const auto *fld = llvm::dyn_cast<clang::FieldDecl>(d)) {
      const std::string name = ast::spelling(fld);
      n.label = name + " : " + type_str(fld->getType());
      n.detail = "name " + name;
      if (fld->isBitField())
        add_stmt(fld->getBitWidth(), n.children);
      if (fld->hasInClassInitializer())
        add_stmt(fld->getInClassInitializer(), n.children);
    } else if (const auto *ecd = llvm::dyn_cast<clang::EnumConstantDecl>(d)) {
      const std::string name = ast::spelling(ecd);
      // The computed value rides in the label so an implicit-value shift
      // (an earlier enumerator added/renumbered) is a visible change.
      n.label = name;
      if (!ecd->getType()->isDependentType())
        n.label += "|" + ast::compat::integral_to_string(ecd->getInitVal());
      n.detail = "name " + name;
      add_stmt(ecd->getInitExpr(), n.children);
    } else if (const auto *ed = llvm::dyn_cast<clang::EnumDecl>(d)) {
      const std::string name = ast::spelling(ed);
      n.label = name;
      n.label += ed->isScoped() ? "|enum class" : "|enum";
      if (!ed->getIntegerType().isNull() &&
          !ed->getIntegerType()->isDependentType())
        n.label += "|" + type_str(ed->getIntegerType());
      if (!name.empty())
        n.detail = "name " + name;
      for (const clang::EnumConstantDecl *ec : ed->enumerators())
        add_decl(ec, n.children);
    } else if (const auto *fr = llvm::dyn_cast<clang::FriendDecl>(d)) {
      if (const clang::NamedDecl *fnd = fr->getFriendDecl()) {
        n.children.push_back(lower_decl(fnd));
      } else if (const clang::TypeSourceInfo *ti = fr->getFriendType()) {
        n.label = type_str(ti->getType());
        n.detail = "friend " + n.label;
      }
    } else if (const auto *rd = llvm::dyn_cast<clang::RecordDecl>(d)) {
      lower_record(rd, n);
    } else if (const auto *tn = llvm::dyn_cast<clang::TypedefNameDecl>(d)) {
      const std::string name = ast::spelling(tn);
      n.label = name + " = " + type_str(tn->getUnderlyingType());
      n.detail = "name " + name;
    } else if (const auto *as = llvm::dyn_cast<clang::AccessSpecDecl>(d)) {
      n.label = access_str(as->getAccess());
      n.detail = "access " + n.label;
    } else if (const auto *sa = llvm::dyn_cast<clang::StaticAssertDecl>(d)) {
      add_stmt(sa->getAssertExpr(), n.children);
    } else if (const auto *ud = llvm::dyn_cast<clang::UsingDirectiveDecl>(d)) {
      n.label = ast::qualified_name(ctx, ud->getNominatedNamespace());
      n.detail = "namespace " + n.label;
    } else if (const auto *nd = llvm::dyn_cast<clang::NamedDecl>(d)) {
      n.label = ast::spelling(nd);
      if (!n.label.empty())
        n.detail = "name " + n.label;
    }
    seal(n);
    return n;
  }

  std::string default_arg_text(const clang::ParmVarDecl *p) const {
    if (!p->hasDefaultArg())
      return {};
    // Dependent/uninstantiable defaults keep their raw source spelling.
    if (p->hasUnparsedDefaultArg() || p->hasUninstantiatedDefaultArg())
      return "= " + source_slice(ctx, p->getDefaultArgRange());
    const clang::Expr *e = p->getDefaultArg();
    if (e == nullptr)
      return {};
    std::string text;
    llvm::raw_string_ostream os(text);
    e->printPretty(os, nullptr, pp);
    return "= " + scrub_spelling(text);
  }

  void fill_signature(const clang::FunctionDecl *fd, BehaviourIR &ir) const {
    ir.return_type = type_str(fd->getReturnType());
    for (const clang::ParmVarDecl *p : fd->parameters()) {
      ir.param_types.push_back(type_str(p->getType()));
      ir.param_defaults.push_back(default_arg_text(p));
    }
    if (fd->isVariadic())
      ir.param_types.push_back("...");
    ir.quals = method_quals(fd);
    if (const auto *fpt = fd->getType()->getAs<clang::FunctionProtoType>())
      ir.is_noexcept = fpt->isNothrow();
    if (const auto *m = llvm::dyn_cast<clang::CXXMethodDecl>(fd)) {
      ir.is_virtual = m->isVirtual();
      ir.is_static_method = m->isStatic();
      // Method-level API state: access and the declaration flags that a direct
      // method comparison would otherwise miss (they live outside the selected
      // method extent). A public->private move, an added `= delete`, or a lost
      // `override` is a real, observable contract change, not equivalence.
      if (m->getAccess() != clang::AS_none)
        ir.access = access_str(m->getAccess());
      ir.is_final = m->hasAttr<clang::FinalAttr>();
      ir.is_pure = m->isPureVirtual();
      ir.is_override = m->size_overridden_methods() > 0;
    }
    if (const auto *ctor = llvm::dyn_cast<clang::CXXConstructorDecl>(fd))
      ir.is_explicit = ctor->isExplicit();
    else if (const auto *conv = llvm::dyn_cast<clang::CXXConversionDecl>(fd))
      ir.is_explicit = conv->isExplicit();
    ir.is_deleted = fd->isDeleted();
    ir.is_defaulted = fd->isDefaulted();
    if (fd->getStorageClass() == clang::SC_Static)
      ir.storage = "static";
    else if (fd->getStorageClass() == clang::SC_Extern)
      ir.storage = "extern";
    ir.is_inline = fd->isInlineSpecified();
    switch (fd->getConstexprKind()) {
    case clang::ConstexprSpecKind::Constexpr:
      ir.constexpr_kind = "constexpr";
      break;
    case clang::ConstexprSpecKind::Consteval:
      ir.constexpr_kind = "consteval";
      break;
    case clang::ConstexprSpecKind::Constinit:
      ir.constexpr_kind = "constinit";
      break;
    default:
      break;
    }
    ir.is_noreturn = fd->isNoReturn();
    ir.signature = ir.return_type + "(" + join(ir.param_types, ", ") + ")";
    if (!ir.quals.empty())
      ir.signature += " " + ir.quals;
    if (ir.is_noexcept)
      ir.signature += " noexcept";
    if (ir.is_virtual)
      ir.signature += " virtual";
  }

  void lower_cfg(const clang::CFG &cfg, const clang::FunctionDecl *fd,
                 BehaviourIR &ir) {
    IrBuilder b{ctx, pp, {}, 0};
    int pi = 0;
    for (const clang::ParmVarDecl *p : fd->parameters())
      b.names.emplace(p, "%p" + std::to_string(pi++));
    for (const clang::CFGBlock *blk : cfg) {
      std::string head = "B" + std::to_string(blk->getBlockID()) + ":";
      if (blk == &cfg.getEntry())
        head += " (entry)";
      if (blk == &cfg.getExit())
        head += " (exit)";
      ir.blocks.push_back(std::move(head));
      for (const clang::CFGElement &el : *blk) {
        if (const auto cs = el.getAs<clang::CFGStmt>()) {
          const clang::Stmt *s = cs->getStmt();
          if (transparent_element(s))
            continue;
          std::string line = "  " + b.text(s);
          if (const auto *e = llvm::dyn_cast<clang::Expr>(s);
              e != nullptr && !e->getType().isNull())
            line += " : " + type_str(e->getType());
          ir.blocks.push_back(std::move(line));
        } else if (const auto ci = el.getAs<clang::CFGInitializer>()) {
          const clang::CXXCtorInitializer *init = ci->getInitializer();
          std::string label;
          if (init->isMemberInitializer())
            label = init->getMember()->getNameAsString();
          else if (init->isBaseInitializer())
            label = type_str(clang::QualType(init->getBaseClass(), 0));
          else
            label = "delegate";
          ir.blocks.push_back("  init " + label + "(" + b.text(init->getInit()) +
                              ")");
        } else if (const auto ad = el.getAs<clang::CFGAutomaticObjDtor>()) {
          const clang::CXXDestructorDecl *dd = ad->getDestructorDecl(ctx);
          ir.blocks.push_back(
              "  dtor " + b.value_name(ad->getVarDecl()) + " " +
              (dd != nullptr ? scrub_usr(ctx, ast::usr_for_decl(dd))
                             : std::string("-")));
        } else if (const auto id = el.getAs<clang::CFGImplicitDtor>()) {
          const clang::CXXDestructorDecl *dd = id->getDestructorDecl(ctx);
          ir.blocks.push_back(
              "  dtor " + (dd != nullptr
                               ? scrub_usr(ctx, ast::usr_for_decl(dd))
                               : std::string("-")));
        } else {
          ir.blocks.push_back("  <element>");
        }
      }
      if (const clang::Stmt *t = blk->getTerminatorStmt())
        ir.blocks.push_back(std::string("  T: ") + t->getStmtClassName());
      std::string succ = "  succ:";
      for (auto it = blk->succ_begin(); it != blk->succ_end(); ++it) {
        succ += " ";
        succ += *it != nullptr ? "B" + std::to_string((*it)->getBlockID())
                               : std::string("-");
      }
      ir.blocks.push_back(std::move(succ));
    }
  }

  int build_behaviour(const clang::FunctionDecl *fd, bool templated) {
    BehaviourIR ir;
    fill_signature(fd, ir);
    const bool dependent = templated || fd->isTemplated();
    if (dependent)
      ir.unsupported.push_back({"dependent template code",
                                to_range(ctx, fd->getSourceRange()), ""});
    if (fd->isVariadic())
      ir.unsupported.push_back(
          {"varargs", to_range(ctx, fd->getSourceRange()), ""});
    if (fd->doesThisDeclarationHaveABody()) {
      ir.has_body = true;
      if (!dependent) {
        clang::Stmt *body = fd->getBody();
        UnsupportedScan scan{ctx, &ir.unsupported};
        if (const auto *ctor = llvm::dyn_cast<clang::CXXConstructorDecl>(fd))
          for (const clang::CXXCtorInitializer *ci : ctor->inits())
            scan.walk(ci->getInit());
        scan.walk(body);
        clang::CFG::BuildOptions bo;
        bo.AddInitializers = true;
        bo.AddImplicitDtors = true;
        bo.AddTemporaryDtors = true;
        const std::unique_ptr<clang::CFG> cfg =
            clang::CFG::buildCFG(fd, body, &ctx, bo);
        if (cfg != nullptr)
          lower_cfg(*cfg, fd, ir);
        else if (ir.unsupported.empty())
          ir.unsupported.push_back(
              {"CFG construction failed", to_range(ctx, fd->getSourceRange()),
               ""});
      }
    }
    behaviours->push_back(std::move(ir));
    return static_cast<int>(behaviours->size()) - 1;
  }

  std::string method_row(const clang::CXXMethodDecl *md,
                         const std::string &prefix) const {
    std::string row = prefix;
    row += access_str(md->getAccess());
    row += std::string(" ") + type_str(md->getReturnType()) + " " +
           ast::spelling(md) + "(" + param_types(md) + ")";
    if (const std::string q = method_quals(md); !q.empty())
      row += " " + q;
    if (const auto *fpt = md->getType()->getAs<clang::FunctionProtoType>();
        fpt != nullptr && fpt->isNothrow())
      row += " noexcept";
    if (md->isVirtual())
      row += " virtual";
    if (md->size_overridden_methods() > 0)
      row += " override";
    if (md->hasAttr<clang::FinalAttr>())
      row += " final";
    if (md->isPureVirtual())
      row += " pure";
    if (md->isStatic())
      row += " static";
    if (md->isDeleted())
      row += " deleted";
    if (md->isDefaulted())
      row += " defaulted";
    return row;
  }

  std::string using_row(const clang::UsingDecl *ud) const {
    return "using " + ast::compat::nns_spelling(ud->getQualifier(), pp) +
           ud->getNameInfo().getAsString();
  }

  std::string friend_row(const clang::FriendDecl *fr) const {
    const clang::NamedDecl *fnd = fr->getFriendDecl();
    if (fnd == nullptr) {
      if (const clang::TypeSourceInfo *ti = fr->getFriendType())
        return "friend " + type_str(ti->getType());
      return "friend <unknown>";
    }
    const clang::Decl *inner = fnd;
    std::string prefix = "friend ";
    if (const auto *ft = llvm::dyn_cast<clang::FunctionTemplateDecl>(fnd);
        ft != nullptr && ft->getTemplatedDecl() != nullptr) {
      inner = ft->getTemplatedDecl();
      prefix = "friend template ";
    }
    if (const auto *ffn = llvm::dyn_cast<clang::FunctionDecl>(inner))
      return prefix + type_str(ffn->getReturnType()) + " " +
             ast::spelling(ffn) + "(" + param_types(ffn) + ")" +
             fn_quals(ffn);
    return prefix + ast::spelling(llvm::cast<clang::NamedDecl>(inner));
  }

  int build_profile(const clang::RecordDecl *rd) {
    ClassProfile p;
    const auto *cxx = llvm::dyn_cast<clang::CXXRecordDecl>(rd);
    if (cxx != nullptr) {
      for (const clang::CXXBaseSpecifier &b : cxx->bases())
        p.bases.push_back(std::string(access_str(b.getAccessSpecifier())) +
                          (b.isVirtual() ? " virtual " : " ") +
                          type_str(b.getType()));
      p.polymorphic = cxx->isPolymorphic();
      p.abstract = cxx->isAbstract();
      if (const clang::ClassTemplateDecl *ct = cxx->getDescribedClassTemplate())
        for (const clang::NamedDecl *tp : *ct->getTemplateParameters()) {
          if (llvm::isa<clang::TemplateTypeParmDecl>(tp))
            p.template_params.push_back("typename " + ast::spelling(tp));
          else if (const auto *nt =
                       llvm::dyn_cast<clang::NonTypeTemplateParmDecl>(tp))
            p.template_params.push_back(type_str(nt->getType()) + " " +
                                        ast::spelling(nt));
          else
            p.template_params.push_back("template " + ast::spelling(tp));
        }
    }
    for (const clang::FieldDecl *f : rd->fields()) {
      std::string row = access_str(f->getAccess());
      if (f->isMutable())
        row += " mutable";
      row += " " + ast::spelling(f) + " : " + type_str(f->getType());
      if (f->isBitField() && !f->getBitWidth()->isValueDependent())
        row += " : " + std::to_string(f->getBitWidthValue());
      p.fields.push_back(std::move(row));
    }
    for (const clang::Decl *m : rd->decls()) {
      if (m->isImplicit())
        continue;
      const clang::Decl *inner = m;
      std::string prefix;
      if (const auto *ft = llvm::dyn_cast<clang::FunctionTemplateDecl>(m);
          ft != nullptr && ft->getTemplatedDecl() != nullptr) {
        inner = ft->getTemplatedDecl();
        prefix = "template ";
      }
      if (const auto *md = llvm::dyn_cast<clang::CXXMethodDecl>(inner)) {
        p.methods.push_back(method_row(md, prefix));
        continue;
      }
      if (const auto *nr = llvm::dyn_cast<clang::RecordDecl>(m)) {
        if (nr->isThisDeclarationADefinition())
          p.nested.push_back(ast::spelling(nr));
        continue;
      }
      // Fields have their own rows; tag/access decls are covered by member
      // entities / method access; shadow decls ride their UsingDecl.
      if (llvm::isa<clang::FieldDecl>(m) || llvm::isa<clang::TagDecl>(m) ||
          llvm::isa<clang::AccessSpecDecl>(m) ||
          llvm::isa<clang::UsingShadowDecl>(m))
        continue;
      if (const auto *fr = llvm::dyn_cast<clang::FriendDecl>(m)) {
        p.extras.push_back(friend_row(fr));
        continue;
      }
      if (const auto *ud = llvm::dyn_cast<clang::UsingDecl>(m)) {
        p.extras.push_back(using_row(ud));
        continue;
      }
      if (const auto *tn = llvm::dyn_cast<clang::TypedefNameDecl>(m)) {
        p.extras.push_back("alias " + ast::spelling(tn) + " = " +
                           type_str(tn->getUnderlyingType()));
        continue;
      }
      if (const auto *vd = llvm::dyn_cast<clang::VarDecl>(m)) {
        p.extras.push_back("static " + ast::spelling(vd) + " : " +
                           type_str(vd->getType()));
        continue;
      }
      if (const auto *sa = llvm::dyn_cast<clang::StaticAssertDecl>(m)) {
        p.extras.push_back("static_assert " + lower_decl(sa).fingerprint);
        continue;
      }
      // Anything the profile cannot express degrades the class verdict to
      // unknown instead of a silent equivalent.
      p.unsupported.push_back(
          {std::string("unhandled member declaration kind ") +
               m->getDeclKindName(),
           to_range(ctx, m->getSourceRange()), ""});
    }
    if (cxx != nullptr && cxx->hasAttr<clang::FinalAttr>())
      p.extras.push_back("final");
    if (rd->isCompleteDefinition() && !rd->isInvalidDecl() &&
        !rd->isDependentType()) {
      const clang::ASTRecordLayout &rl = ctx.getASTRecordLayout(rd);
      p.extras.push_back(
          "layout size:" + std::to_string(rl.getSize().getQuantity()) +
          " align:" + std::to_string(rl.getAlignment().getQuantity()));
    }
    std::sort(p.methods.begin(), p.methods.end());
    std::sort(p.extras.begin(), p.extras.end());
    std::string acc;
    const auto fold = [&acc](const std::vector<std::string> &rows) {
      for (const std::string &r : rows) {
        acc += r;
        acc += '\x1e';
      }
      acc += '\x1f';
    };
    fold(p.bases);
    fold(p.fields);
    fold(p.methods);
    fold(p.template_params);
    fold(p.nested);
    fold(p.extras);
    acc += p.polymorphic ? "P" : "p";
    acc += p.abstract ? "A" : "a";
    p.fingerprint = sha1_hex(acc);
    profiles->push_back(std::move(p));
    return static_cast<int>(profiles->size()) - 1;
  }

  bool member_entity(const clang::Decl *m) const {
    if (const auto *fr = llvm::dyn_cast<clang::FriendDecl>(m)) {
      // A hidden friend with a body is a callable member entity; a pure
      // friend declaration is only a profile row.
      const clang::NamedDecl *fnd = fr->getFriendDecl();
      const auto *ffn =
          fnd != nullptr ? llvm::dyn_cast<clang::FunctionDecl>(fnd) : nullptr;
      return ffn != nullptr && ffn->doesThisDeclarationHaveABody();
    }
    return llvm::isa<clang::CXXMethodDecl>(m) ||
           llvm::isa<clang::FieldDecl>(m) || llvm::isa<clang::VarDecl>(m) ||
           llvm::isa<clang::TagDecl>(m) ||
           llvm::isa<clang::TypedefNameDecl>(m) ||
           llvm::isa<clang::FunctionTemplateDecl>(m);
  }

  Entity make_entity(const clang::Decl *d) {
    if (const auto *fr = llvm::dyn_cast<clang::FriendDecl>(d);
        fr != nullptr && fr->getFriendDecl() != nullptr)
      d = fr->getFriendDecl();
    Entity e;
    const clang::Decl *inner = d;
    if (const auto *td = llvm::dyn_cast<clang::TemplateDecl>(d);
        td != nullptr && td->getTemplatedDecl() != nullptr)
      inner = td->getTemplatedDecl();
    e.kind = entity_kind(inner);
    if (const auto *nd = llvm::dyn_cast<clang::NamedDecl>(inner))
      e.name = scrub_spelling(ast::qualified_name(ctx, nd));
    if (const auto *fd = llvm::dyn_cast<clang::FunctionDecl>(inner))
      e.signature = e.name + "(" + param_types(fd) + ")" + fn_quals(fd);
    e.usr = ast::usr_for_decl(d);
    e.range = to_range(ctx, d->getSourceRange());
    e.is_definition = ast::is_definition(inner);
    e.syntax = lower_decl(d);
    e.fingerprint = structural_fingerprint(e.syntax);
    e.slice = source_slice(ctx, d->getSourceRange());
    if (const auto *fd = llvm::dyn_cast<clang::FunctionDecl>(inner)) {
      // Bind the body through the TU-wide definition so an out-of-line
      // member body defined elsewhere in the file is still compared; the
      // entity's extent and slice stay the declaration's.
      const clang::FunctionDecl *def = fd->getDefinition();
      e.behaviour = build_behaviour(def != nullptr ? def : fd, inner != d);
    }
    if (const auto *rd = llvm::dyn_cast<clang::RecordDecl>(inner);
        rd != nullptr && rd->isThisDeclarationADefinition()) {
      e.profile = build_profile(rd);
      for (const clang::Decl *m : rd->decls())
        if (!m->isImplicit() && member_entity(m))
          e.members.push_back(make_entity(m));
    }
    return e;
  }

  bool in_target(const clang::Decl *d) const {
    return ast::expansion_loc(ctx, d->getLocation()).file == target_file;
  }

  void collect(const clang::DeclContext *dc, std::vector<Entity> &out) {
    for (const clang::Decl *d : dc->decls()) {
      if (d->isImplicit() || llvm::isa<clang::EmptyDecl>(d))
        continue;
      if (const auto *ns = llvm::dyn_cast<clang::NamespaceDecl>(d)) {
        collect(ns, out);
        continue;
      }
      if (const auto *ls = llvm::dyn_cast<clang::LinkageSpecDecl>(d)) {
        collect(ls, out);
        continue;
      }
      if (!in_target(d) || ast::is_template_instantiation(d))
        continue;
      out.push_back(make_entity(d));
    }
  }
};

// One row per USR: a declaration and its definition in the same file collapse
// to the definition.
std::vector<Entity> dedupe_entities(std::vector<Entity> in) {
  std::vector<Entity> out;
  std::map<std::string, std::size_t> by_usr;
  for (Entity &e : in) {
    if (e.usr.empty()) {
      out.push_back(std::move(e));
      continue;
    }
    const auto [it, fresh] = by_usr.emplace(e.usr, out.size());
    if (fresh)
      out.push_back(std::move(e));
    else if (e.is_definition && !out[it->second].is_definition)
      out[it->second] = std::move(e);
  }
  return out;
}

void flatten(const std::vector<Entity> &es, std::vector<const Entity *> &out) {
  for (const Entity &e : es) {
    out.push_back(&e);
    flatten(e.members, out);
  }
}

std::string strip_ws(const std::string &s) {
  std::string out;
  for (const char c : s)
    if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
      out += c;
  return out;
}

std::optional<int> line_selector(const std::string &raw) {
  if (raw.rfind("line:", 0) != 0)
    return std::nullopt;
  const std::string num = raw.substr(5);
  if (num.empty() || num.size() > 9 ||
      num.find_first_not_of("0123456789") != std::string::npos)
    return std::nullopt;
  return std::stoi(num);
}

// Contract tiers: exact USR, exact qualified signature (whitespace-
// insensitive), exact qualified name (+ --kind), line:N innermost. The first
// tier with >= 1 candidate wins; > 1 candidate in that tier is an error
// listing every candidate.
Entity select_entity(const std::vector<Entity> &top, const Selector &sel,
                     const ParseConfig &cfg) {
  std::vector<const Entity *> flat;
  flatten(top, flat);
  std::vector<const Entity *> cands;
  std::map<std::string, std::size_t> by_usr;
  for (const Entity *e : flat) {
    if (e->usr.empty()) {
      cands.push_back(e);
      continue;
    }
    const auto [it, fresh] = by_usr.emplace(e->usr, cands.size());
    if (fresh)
      cands.push_back(e);
    else if (e->is_definition && !cands[it->second]->is_definition)
      cands[it->second] = e;
  }

  std::vector<const Entity *> hits;
  for (const Entity *e : cands)
    if (!e->usr.empty() && e->usr == sel.raw)
      hits.push_back(e);
  if (hits.empty()) {
    const std::string want = strip_ws(sel.raw);
    for (const Entity *e : cands)
      if (!e->signature.empty() && strip_ws(e->signature) == want)
        hits.push_back(e);
  }
  if (hits.empty()) {
    for (const Entity *e : cands)
      if (!e->name.empty() && e->name == sel.raw &&
          (!sel.kind || e->kind == *sel.kind))
        hits.push_back(e);
  }
  if (hits.empty()) {
    if (const std::optional<int> line = line_selector(sel.raw)) {
      int best = -1;
      for (const Entity *e : cands) {
        if (e->range.line > *line || e->range.end_line < *line)
          continue;
        const int span = e->range.end_line - e->range.line;
        if (best < 0 || span < best) {
          best = span;
          hits.clear();
        }
        if (span == best)
          hits.push_back(e);
      }
    }
  }
  if (hits.empty())
    throw CidxError("selector '" + sel.raw + "' matches nothing in " +
                    cfg.file);
  if (hits.size() > 1) {
    std::string msg =
        "selector '" + sel.raw + "' is ambiguous in " + cfg.file +
        "; candidates:";
    for (const Entity *e : hits)
      msg += "\n  " + e->kind + " " +
             (e->signature.empty() ? e->name : e->signature) + " " +
             (e->usr.empty() ? "-" : e->usr) + " L" + fmt_range(e->range);
    throw CidxError(msg);
  }
  return *hits[0];
}

class DiffConsumer final : public clang::ASTConsumer {
public:
  DiffConsumer(const ParseConfig *config,
               const std::optional<Selector> *selector, SideAnalysis *out,
               std::exception_ptr *err, bool *handled)
      : config_(config), selector_(selector), out_(out), err_(err),
        handled_(handled) {}

  void HandleTranslationUnit(clang::ASTContext &ctx) override {
    try {
      Analyzer a{ctx, ast::printing_policy(ctx), config_->file,
                 &out_->behaviours, &out_->profiles};
      std::vector<Entity> ents;
      a.collect(ctx.getTranslationUnitDecl(), ents);
      ents = dedupe_entities(std::move(ents));
      if (selector_->has_value())
        out_->entities = {select_entity(ents, **selector_, *config_)};
      else
        out_->entities = std::move(ents);
      *handled_ = true;
    } catch (...) {
      *err_ = std::current_exception();
    }
  }

private:
  const ParseConfig *config_;
  const std::optional<Selector> *selector_;
  SideAnalysis *out_;
  std::exception_ptr *err_;
  bool *handled_;
};

class DiffAction final : public clang::ASTFrontendAction {
public:
  DiffAction(const ParseConfig *config, const std::optional<Selector> *selector,
             SideAnalysis *out, std::exception_ptr *err, bool *handled)
      : config_(config), selector_(selector), out_(out), err_(err),
        handled_(handled) {}

  std::unique_ptr<clang::ASTConsumer>
  CreateASTConsumer(clang::CompilerInstance & /*ci*/,
                    llvm::StringRef /*file*/) override {
    return std::make_unique<DiffConsumer>(config_, selector_, out_, err_,
                                          handled_);
  }

private:
  const ParseConfig *config_;
  const std::optional<Selector> *selector_;
  SideAnalysis *out_;
  std::exception_ptr *err_;
  bool *handled_;
};

// Captures error-level diagnostics instead of printing them to stderr: a
// parse error must fail the comparison (no verdicts from recovery ASTs).
class CaptureDiagnostics final : public clang::DiagnosticConsumer {
public:
  void HandleDiagnostic(clang::DiagnosticsEngine::Level level,
                        const clang::Diagnostic &info) override {
    clang::DiagnosticConsumer::HandleDiagnostic(level, info);
    if (level < clang::DiagnosticsEngine::Error || !first_error_.empty())
      return;
    std::string loc;
    if (info.hasSourceManager() && info.getLocation().isValid()) {
      const clang::PresumedLoc pl =
          info.getSourceManager().getPresumedLoc(info.getLocation());
      if (pl.isValid())
        loc = std::string(pl.getFilename()) + ":" +
              std::to_string(pl.getLine()) + ":" +
              std::to_string(pl.getColumn()) + ": ";
    }
    llvm::SmallString<256> msg;
    info.FormatDiagnostic(msg);
    first_error_ = loc + std::string(msg);
  }

  const std::string &first_error() const { return first_error_; }

private:
  std::string first_error_;
};

class DiffActionFactory final : public clang::tooling::FrontendActionFactory {
public:
  DiffActionFactory(const ParseConfig *config,
                    const std::optional<Selector> *selector, SideAnalysis *out,
                    std::exception_ptr *err, bool *handled)
      : config_(config), selector_(selector), out_(out), err_(err),
        handled_(handled) {}

  std::unique_ptr<clang::FrontendAction> create() override {
    return std::make_unique<DiffAction>(config_, selector_, out_, err_,
                                        handled_);
  }

private:
  const ParseConfig *config_;
  const std::optional<Selector> *selector_;
  SideAnalysis *out_;
  std::exception_ptr *err_;
  bool *handled_;
};

} // namespace

SideAnalysis analyze_side(const ParseConfig &config,
                          const std::optional<Selector> &selector) {
  SideAnalysis out;
  out.config = config;
  std::exception_ptr err;
  bool handled = false;

  // Flag assembly identical to cidx-astgraph dump_tu: resolved stored args
  // plus toolchain search paths + -ferror-limit=0; -resource-dir injected
  // first.
  Toolchain toolchain;
  const bool cpp = Toolchain::is_cpp(config.parse_file, config.args);
  std::vector<std::string> full = config.args;
  for (std::string &f : toolchain.toolchain_flags(cpp, config.driver))
    full.push_back(std::move(f));
  full.push_back("-ferror-limit=0");
  // Diagnostics are captured by CaptureDiagnostics below; this also silences
  // the "N errors generated." summary CompilerInstance prints to stderr.
  full.push_back("-fno-caret-diagnostics");

  clang::tooling::FixedCompilationDatabase cdb(".", full);
  clang::tooling::ClangTool tool(cdb, {config.parse_file});
#ifdef CIDX_CLANG_RESOURCE_DIR
  tool.appendArgumentsAdjuster(clang::tooling::getInsertArgumentAdjuster(
      {"-resource-dir", CIDX_CLANG_RESOURCE_DIR},
      clang::tooling::ArgumentInsertPosition::BEGIN));
#endif
  CaptureDiagnostics diags;
  tool.setDiagnosticConsumer(&diags);
  DiffActionFactory factory(&config, &selector, &out, &err, &handled);
  tool.run(&factory);
  if (diags.getNumErrors() > 0)
    throw CidxError("cannot parse " + config.parse_file + ": " +
                    diags.first_error());
  if (err)
    std::rethrow_exception(err);
  if (!handled)
    throw CidxError("cannot parse " + config.parse_file +
                    " (no AST produced)");
  return out;
}

} // namespace diff
} // namespace cidx
