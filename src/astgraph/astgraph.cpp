// astgraph — dump one TU's Clang C++ AST into a per-TU SQLite graph DB for
// Soufflé/Datalog reasoning. Rebuilt 2026-07-12 on the Clang C++ API
// (RecursiveASTVisitor + ClangTool); the libclang cursor/type C API was
// removed.
//
// Node model: declarations and statements/expressions are interned by their
// clang AST pointer; types by their canonical Type*. node_kind ids are the
// CIDX-STABLE constants in schema.hpp (decls 1000-1999, stmts 2000-2999, types
// 3000-3999), mapped from the live clang enums so the generated Souffle rules
// (which hardcode kind literals) stay valid across LLVM majors. A structural
// RecursiveASTVisitor emits `child` edges with sibling order; a worklist then
// drains every node's semantic cross-references
// (references/definition/canonical /parents/overrides/type relations) to a
// fixpoint — referenced header entities surface as shallow nodes exactly as the
// libclang version did.
#include "astgraph/astgraph.hpp"

#include <cstdint>
#include <cstdio>
#include <deque>
#include <exception>
#include <filesystem>
#include <optional>
#include <ranges>
#include <string>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <unistd.h>

#include "clang/AST/ASTConsumer.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/Type.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Basic/Version.h"
#include "clang/Frontend/FrontendAction.h"
#include "clang/Tooling/ArgumentsAdjusters.h"
#include "clang/Tooling/CompilationDatabase.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/Config/llvm-config.h"
#include "llvm/Support/Casting.h"

#include "ast/clang_compat.hpp"
#include "ast/usr.hpp"
#include "astgraph/schema.hpp"
#include "catalogs/generated_catalog.hpp"
#include "cli/args.hpp" // kVersion (meta.generator provenance)
#include "storage/artifacts.hpp"
#include "storage/sqlite.hpp"
#include "storage/storage.hpp"
#include "toolchain/toolchain.hpp"
#include "util/errors.hpp"
#include "util/hashing.hpp"
#include "util/json_min.hpp"

namespace cidx::astgraph {
namespace {

using clang::dyn_cast;
using clang::isa;

std::string stable_option(std::string_view option,
                          const std::filesystem::path &component_root) {
  const auto make_stable = [&](std::string_view value) {
    const std::filesystem::path path(value);
    if (!path.is_absolute()) {
      return std::string(value);
    }
    std::error_code ec;
    const auto relative = std::filesystem::relative(path, component_root, ec);
    if (!ec && !relative.empty() && relative != "." &&
        !std::ranges::any_of(relative,
                             [](const auto &part) { return part == ".."; })) {
      return relative.generic_string();
    }
    return std::string("external:") +
           cidx::sha256_hex(path.lexically_normal().generic_string());
  };
  for (const std::string_view prefix :
       {"-I", "-isystem", "-iquote", "-include", "-include-pch",
        "-resource-dir", "-o", "-MF", "-MT"}) {
    if (option.starts_with(prefix) && option.size() > prefix.size()) {
      return std::string(prefix) + make_stable(option.substr(prefix.size()));
    }
  }
  return make_stable(option);
}

// The <TU>.db DDL. Every column is NOT NULL with a 0/'' sentinel — Soufflé's
// sqlite IO reads these tables verbatim and has no notion of NULL. The edge
// UNIQUE clause makes re-emission (same cross-ref reachable twice) a no-op.
constexpr const char *kSchemaSql = R"sql(
PRAGMA journal_mode = MEMORY;
PRAGMA synchronous = OFF;
CREATE TABLE meta (
  key   TEXT PRIMARY KEY,
  value TEXT NOT NULL
) WITHOUT ROWID;
CREATE TABLE file (
  id      INTEGER PRIMARY KEY,
  path    TEXT NOT NULL UNIQUE,
  is_main INTEGER NOT NULL DEFAULT 0
);
CREATE TABLE node_kind (
  id       INTEGER PRIMARY KEY,   -- CIDX-STABLE kind (schema.hpp)
  name     TEXT NOT NULL,
  category TEXT NOT NULL          -- decl|expr|stmt|type
);
CREATE TABLE relation_kind (
  id   INTEGER PRIMARY KEY,
  name TEXT NOT NULL UNIQUE
);
CREATE TABLE symbol (
  id      INTEGER PRIMARY KEY,
  usr     TEXT NOT NULL UNIQUE,   -- joins cidx index.db symbol.usr
  name    TEXT NOT NULL,
  kind_id INTEGER NOT NULL,
  linkage INTEGER NOT NULL DEFAULT 0
);
CREATE TABLE node (
  id            INTEGER PRIMARY KEY,
  kind_id       INTEGER NOT NULL REFERENCES node_kind(id),
  symbol_id     INTEGER NOT NULL DEFAULT 0,  -- 0 = no USR
  type_id       INTEGER NOT NULL DEFAULT 0,  -- the node's own type (0 = none)
  spelling      TEXT NOT NULL DEFAULT '',
  file_id       INTEGER NOT NULL DEFAULT 0,  -- 0 = no location (type nodes)
  line          INTEGER NOT NULL DEFAULT 0,
  col           INTEGER NOT NULL DEFAULT 0,
  end_line      INTEGER NOT NULL DEFAULT 0,
  end_col       INTEGER NOT NULL DEFAULT 0,
  is_definition INTEGER NOT NULL DEFAULT 0,
  access        INTEGER NOT NULL DEFAULT 0,  -- clang::AccessSpecifier
  is_const      INTEGER NOT NULL DEFAULT 0,  -- type nodes only
  is_volatile   INTEGER NOT NULL DEFAULT 0,
  is_restrict   INTEGER NOT NULL DEFAULT 0
);
CREATE TABLE edge (
  src_id INTEGER NOT NULL,
  dst_id INTEGER NOT NULL,
  rel_id INTEGER NOT NULL REFERENCES relation_kind(id),
  ord    INTEGER NOT NULL DEFAULT 0,
  UNIQUE (src_id, dst_id, rel_id, ord) ON CONFLICT IGNORE
);
CREATE INDEX idx_edge_src ON edge(src_id, rel_id);
CREATE INDEX idx_edge_dst ON edge(dst_id, rel_id);
CREATE INDEX idx_node_kind ON node(kind_id);
CREATE INDEX idx_node_symbol ON node(symbol_id);
)sql";

struct RelName {
  int id;
  const char *name;
};
constexpr RelName kRelNames[] = {
    {.id = kRelChild, .name = "child"},
    {.id = kRelReferences, .name = "references"},
    {.id = kRelDefinition, .name = "definition"},
    {.id = kRelCanonical, .name = "canonical"},
    {.id = kRelSemanticParent, .name = "semantic_parent"},
    {.id = kRelLexicalParent, .name = "lexical_parent"},
    {.id = kRelSpecializes, .name = "specializes"},
    {.id = kRelOverrides, .name = "overrides"},
    {.id = kRelTypeDecl, .name = "type_decl"},
    {.id = kRelCanonicalType, .name = "canonical_type"},
    {.id = kRelPointee, .name = "pointee"},
    {.id = kRelElementType, .name = "element_type"},
    {.id = kRelResultType, .name = "result_type"},
    {.id = kRelArgType, .name = "arg_type"},
    {.id = kRelNamedType, .name = "named_type"},
    {.id = kRelUnderlyingType, .name = "underlying_type"},
    {.id = kRelTemplateArg, .name = "template_arg"},
    {.id = kRelClassType, .name = "class_type"},
};

// --- CIDX-STABLE kind mapping (live clang enum -> schema.hpp constant)
// --------

int decl_kind_id(const clang::Decl *d) {
  switch (d->getKind()) {
  case clang::Decl::TranslationUnit:
    return kNodeTranslationUnit;
  case clang::Decl::Function:
    return kNodeFunction;
  case clang::Decl::CXXMethod:
    return kNodeCXXMethod;
  case clang::Decl::CXXConstructor:
    return kNodeCXXConstructor;
  case clang::Decl::CXXDestructor:
    return kNodeCXXDestructor;
  case clang::Decl::CXXConversion:
    return kNodeCXXMethod;
  case clang::Decl::Var:
    return kNodeVar;
  case clang::Decl::ParmVar:
    return kNodeParmVar;
  case clang::Decl::Field:
    return kNodeField;
  case clang::Decl::CXXRecord:
    return kNodeCXXRecord;
  case clang::Decl::Record:
    return kNodeRecord;
  case clang::Decl::Enum:
    return kNodeEnum;
  case clang::Decl::EnumConstant:
    return kNodeEnumConstant;
  case clang::Decl::Namespace:
    return kNodeNamespace;
  case clang::Decl::Typedef:
    return kNodeTypedef;
  case clang::Decl::TypeAlias:
    return kNodeTypeAlias;
  case clang::Decl::FunctionTemplate:
    return kNodeFunctionTemplate;
  case clang::Decl::ClassTemplate:
    return kNodeClassTemplate;
  default:
    return kNodeOtherDecl;
  }
}

int stmt_kind_id(const clang::Stmt *s) {
  if (isa<clang::CallExpr>(s)) {
    return kNodeCallExpr; // incl. member/operator
  }
  if (isa<clang::CXXConstructExpr>(s)) {
    return kNodeCXXConstructExpr;
  }
  if (isa<clang::DeclRefExpr>(s)) {
    return kNodeDeclRefExpr;
  }
  if (isa<clang::MemberExpr>(s)) {
    return kNodeMemberExpr;
  }
  if (isa<clang::CXXNewExpr>(s)) {
    return kNodeCXXNewExpr;
  }
  if (isa<clang::CXXDeleteExpr>(s)) {
    return kNodeCXXDeleteExpr;
  }
  return isa<clang::Expr>(s) ? kNodeOtherExpr : kNodeOtherStmt;
}

int type_kind_id(const clang::Type *t) {
  switch (t->getTypeClass()) {
  case clang::Type::Builtin:
    return kNodeBuiltinType;
  case clang::Type::Pointer:
    return kNodePointerType;
  case clang::Type::LValueReference:
    return kNodeLValueReferenceType;
  case clang::Type::RValueReference:
    return kNodeRValueReferenceType;
  case clang::Type::Record:
    return kNodeRecordType;
  case clang::Type::Enum:
    return kNodeEnumType;
  case clang::Type::Typedef:
    return kNodeTypedefType;
#if LLVM_VERSION_MAJOR < 22
  case clang::Type::Elaborated:
    return kNodeElaboratedType;
#endif
  case clang::Type::FunctionProto:
    return kNodeFunctionProtoType;
  case clang::Type::ConstantArray:
    return kNodeConstantArrayType;
  case clang::Type::TemplateSpecialization:
    return kNodeTemplateSpecializationType;
  case clang::Type::MemberPointer:
    return kNodeMemberPointerType;
  default:
    return kNodeOtherType;
  }
}

int64_t decl_is_definition(const clang::Decl *d) {
  if (const auto *fd = dyn_cast<clang::FunctionDecl>(d)) {
    return fd->isThisDeclarationADefinition() ? 1 : 0;
  }
  if (const auto *td = dyn_cast<clang::TagDecl>(d)) {
    return td->isThisDeclarationADefinition() ? 1 : 0;
  }
  if (const auto *vd = dyn_cast<clang::VarDecl>(d)) {
    return vd->isThisDeclarationADefinition() != clang::VarDecl::DeclarationOnly
               ? 1
               : 0;
  }
  return 0;
}

std::string decl_name(const clang::Decl *d) {
  if (const auto *nd = dyn_cast<clang::NamedDecl>(d)) {
    return nd->getNameAsString();
  }
  return "";
}

class Dumper {
public:
  Dumper(const std::string &path, clang::ASTContext &ctx, const Options &opts)
      : db_(path), ctx_(ctx), sm_(ctx.getSourceManager()),
        main_only_(opts.main_only), descriptor_hash_(opts.descriptor_hash),
        options_(opts) {
    db_.exec(kSchemaSql);
    db_.exec("BEGIN");
  }

  void write_meta(const std::string &source,
                  const std::vector<std::string> &args,
                  const std::optional<std::string> &driver) {
    put_meta("schema_version", std::to_string(kSchemaVersion));
    put_meta("catalog_version", std::to_string(catalog::kCatalogVersion));
    put_meta("catalog_hash", std::string(catalog::kCatalogHash));
    put_meta("artifact_kind", "astgraph");
    put_meta("status", "complete");
    put_meta("trust", "producer-verified");
    put_meta("evidence", "source");
    put_meta("generator", std::string("cidx-astgraph ") + cli::kVersion);
    put_meta("source", source);
    put_meta("args", json_min::encode_string_array(args));
    put_meta("driver", driver ? *driver : "");
    put_meta("clang", clang::getClangFullVersion());
    put_meta("main_only", main_only_ ? "1" : "0");
    put_meta("kind_scheme",
             "stable: decl 1000-1999, stmt/expr 2000-2999, type 3000-3999");
    put_meta("source_md5", md5_of(source).value_or(""));
    put_meta("options_sha1",
             sha1_flags_hash(AstCacheKey{
                 .abspath = source, .flags = args, .driver = driver}));
    put_meta("descriptor_hash", descriptor_hash_.value_or(""));
    put_meta("artifact_key", artifact_key(source, args, driver, options_));
  }

  void seed_relation_kinds() {
    for (const RelName &r : kRelNames) {
      run("INSERT INTO relation_kind(id, name) VALUES (?, ?)",
          {static_cast<int64_t>(r.id), std::string(r.name)});
    }
  }

  // --- interning -----------------------------------------------------------

  int64_t intern_decl(const clang::Decl *d) {
    if (d == nullptr) {
      return 0;
    }
    // Collapse a class's injected-class-name (the implicit same-named record
    // inside every class body) onto the class it names, so a class is a single
    // record node — libclang's cursor API did this implicitly.
    if (const auto *rd = dyn_cast<clang::CXXRecordDecl>(d);
        rd != nullptr && rd->isInjectedClassName()) {
      if (const clang::DeclContext *dc = rd->getDeclContext(); dc != nullptr) {
        if (const auto *outer = dyn_cast<clang::CXXRecordDecl>(
                clang::Decl::castFromDeclContext(dc))) {
          return intern_decl(outer);
        }
      }
    }
    if (const auto it = decl_ids_.find(d); it != decl_ids_.end()) {
      return it->second;
    }
    const int64_t id = next_node_++;
    decl_ids_.emplace(d, id);
    const int kind = decl_kind_id(d);
    seed_kind(kind, d->getDeclKindName(), "decl");

    bool is_main = false;
    int64_t file_id = 0;
    int64_t line = 0;
    int64_t col = 0;
    int64_t end_line = 0;
    int64_t end_col = 0;
    loc_of(d->getBeginLoc(), d->getEndLoc(), is_main, file_id, line, col,
           end_line, end_col);

    const std::string spelling = decl_name(d);
    int64_t symbol_id = 0;
    std::string usr = cidx::ast::usr_for_decl(d);
    if (!usr.empty()) {
      symbol_id = intern_symbol(std::move(usr), spelling, d, kind);
    }

    int64_t type_id = 0;
    if (const auto *vd = dyn_cast<clang::ValueDecl>(d)) {
      type_id = intern_type(vd->getType());
    }

    run("INSERT INTO node(id, kind_id, symbol_id, type_id, spelling, file_id, "
        "line, col, end_line, end_col, is_definition, access, is_const, "
        "is_volatile, is_restrict) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
        {id, static_cast<int64_t>(kind), symbol_id, type_id, spelling, file_id,
         line, col, end_line, end_col, decl_is_definition(d),
         static_cast<int64_t>(d->getAccess()), int64_t{0}, int64_t{0},
         int64_t{0}});
    ++stats_.cursor_nodes;
    decl_work_.emplace_back(d, id);
    return id;
  }

  int64_t intern_stmt(const clang::Stmt *s) {
    if (s == nullptr) {
      return 0;
    }
    if (const auto it = stmt_ids_.find(s); it != stmt_ids_.end()) {
      return it->second;
    }
    const int64_t id = next_node_++;
    stmt_ids_.emplace(s, id);
    const int kind = stmt_kind_id(s);
    seed_kind(kind, s->getStmtClassName(),
              isa<clang::Expr>(s) ? "expr" : "stmt");

    bool is_main = false;
    int64_t file_id = 0;
    int64_t line = 0;
    int64_t col = 0;
    int64_t end_line = 0;
    int64_t end_col = 0;
    loc_of(s->getBeginLoc(), s->getEndLoc(), is_main, file_id, line, col,
           end_line, end_col);

    int64_t type_id = 0;
    if (const auto *e = dyn_cast<clang::Expr>(s)) {
      type_id = intern_type(e->getType());
    }

    run("INSERT INTO node(id, kind_id, symbol_id, type_id, spelling, file_id, "
        "line, col, end_line, end_col, is_definition, access, is_const, "
        "is_volatile, is_restrict) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
        {id, static_cast<int64_t>(kind), int64_t{0}, type_id, std::string(),
         file_id, line, col, end_line, end_col, int64_t{0}, int64_t{0},
         int64_t{0}, int64_t{0}, int64_t{0}});
    ++stats_.cursor_nodes;
    stmt_work_.emplace_back(s, id);
    return id;
  }

  int64_t intern_type(clang::QualType qt) {
    if (qt.isNull()) {
      return 0;
    }
    const clang::Type *tp = qt.getTypePtrOrNull();
    if (tp == nullptr) {
      return 0;
    }
    if (const auto it = type_ids_.find(tp); it != type_ids_.end()) {
      return it->second;
    }
    const int64_t id = next_node_++;
    type_ids_.emplace(tp, id);
    const int kind = type_kind_id(tp);
    seed_kind(kind, tp->getTypeClassName(), "type");
    run("INSERT INTO node(id, kind_id, symbol_id, type_id, spelling, file_id, "
        "line, col, end_line, end_col, is_definition, access, is_const, "
        "is_volatile, is_restrict) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
        {id, static_cast<int64_t>(kind), int64_t{0}, int64_t{0},
         qt.getAsString(), int64_t{0}, int64_t{0}, int64_t{0}, int64_t{0},
         int64_t{0}, int64_t{0}, int64_t{0},
         static_cast<int64_t>(qt.isConstQualified() ? 1 : 0),
         static_cast<int64_t>(qt.isVolatileQualified() ? 1 : 0),
         static_cast<int64_t>(qt.isRestrictQualified() ? 1 : 0)});
    ++stats_.type_nodes;
    type_work_.emplace_back(tp, id);
    return id;
  }

  void add_edge(int64_t src, int64_t dst, int rel, int64_t ord) {
    if (src == 0 || dst == 0) {
      return;
    }
    run("INSERT INTO edge(src_id, dst_id, rel_id, ord) VALUES (?,?,?,?)",
        {src, dst, static_cast<int64_t>(rel), ord});
    if (db_.changes() == 1) {
      ++stats_.edges;
    }
  }

  // Drain the cross-reference worklists to fixpoint. Processing may intern new
  // decls/types, which re-feed the queues; all three are finite (bounded by the
  // TU's reachable decls/stmts/types), so this terminates.
  void drain() {
    while (!decl_work_.empty() || !stmt_work_.empty() || !type_work_.empty()) {
      if (!decl_work_.empty()) {
        const auto [d, id] = decl_work_.front();
        decl_work_.pop_front();
        process_decl(d, id);
      } else if (!stmt_work_.empty()) {
        const auto [s, id] = stmt_work_.front();
        stmt_work_.pop_front();
        process_stmt(s, id);
      } else {
        const auto [t, id] = type_work_.front();
        type_work_.pop_front();
        process_type(t, id);
      }
    }
  }

  DumpStats finish() {
    db_.exec("COMMIT");
    return stats_;
  }

private:
  void process_decl(const clang::Decl *d, int64_t id) {
    if (const auto *fd = dyn_cast<clang::FunctionDecl>(d)) {
      if (const auto *def = fd->getDefinition(); def != nullptr && def != fd) {
        add_edge(id, intern_decl(def), kRelDefinition, 0);
      }
      if (const auto *pt = fd->getPrimaryTemplate()) {
        add_edge(id, intern_decl(pt), kRelSpecializes, 0);
      }
    } else if (const auto *td = dyn_cast<clang::TagDecl>(d)) {
      if (const auto *def = td->getDefinition(); def != nullptr && def != td) {
        add_edge(id, intern_decl(def), kRelDefinition, 0);
      }
    } else if (const auto *vd = dyn_cast<clang::VarDecl>(d)) {
      if (const auto *def = vd->getDefinition(); def != nullptr && def != vd) {
        add_edge(id, intern_decl(def), kRelDefinition, 0);
      }
    }

    if (const clang::Decl *canon = d->getCanonicalDecl();
        canon != nullptr && canon != d) {
      add_edge(id, intern_decl(canon), kRelCanonical, 0);
    }

    if (const clang::DeclContext *dc = d->getDeclContext(); dc != nullptr) {
      add_edge(id, intern_decl(clang::Decl::castFromDeclContext(dc)),
               kRelSemanticParent, 0);
    }
    if (const clang::DeclContext *lc = d->getLexicalDeclContext();
        lc != nullptr) {
      add_edge(id, intern_decl(clang::Decl::castFromDeclContext(lc)),
               kRelLexicalParent, 0);
    }

    if (const auto *ctsd =
            dyn_cast<clang::ClassTemplateSpecializationDecl>(d)) {
      add_edge(id, intern_decl(ctsd->getSpecializedTemplate()), kRelSpecializes,
               0);
    }

    if (const auto *md = dyn_cast<clang::CXXMethodDecl>(d)) {
      int64_t i = 0;
      for (const clang::CXXMethodDecl *o : md->overridden_methods()) {
        add_edge(id, intern_decl(o), kRelOverrides, i++);
      }
    }

    if (const auto *tnd = dyn_cast<clang::TypedefNameDecl>(d)) {
      add_edge(id, intern_type(tnd->getUnderlyingType()), kRelUnderlyingType,
               0);
    }
  }

  void process_stmt(const clang::Stmt *s, int64_t id) {
    if (const auto *ce = dyn_cast<clang::CallExpr>(s)) {
      if (const clang::Decl *callee = ce->getCalleeDecl()) {
        add_edge(id, intern_decl(callee), kRelReferences, 0);
      }
    } else if (const auto *dre = dyn_cast<clang::DeclRefExpr>(s)) {
      add_edge(id, intern_decl(dre->getDecl()), kRelReferences, 0);
    } else if (const auto *me = dyn_cast<clang::MemberExpr>(s)) {
      add_edge(id, intern_decl(me->getMemberDecl()), kRelReferences, 0);
    } else if (const auto *cce = dyn_cast<clang::CXXConstructExpr>(s)) {
      add_edge(id, intern_decl(cce->getConstructor()), kRelReferences, 0);
    }
  }

  void process_type(const clang::Type *tp, int64_t id) {
    const clang::QualType qt(tp, 0);
    if (clang::TagDecl *tag = tp->getAsTagDecl()) {
      add_edge(id, intern_decl(tag), kRelTypeDecl, 0);
    } else if (const auto *tdt = dyn_cast<clang::TypedefType>(tp)) {
      add_edge(id, intern_decl(tdt->getDecl()), kRelTypeDecl, 0);
    }

    if (const clang::QualType canon = cidx::ast::compat::canonical_type(ctx_, qt);
        !canon.isNull() && canon.getTypePtrOrNull() != tp) {
      add_edge(id, intern_type(canon), kRelCanonicalType, 0);
    }

    if (tp->isPointerType() || tp->isReferenceType() ||
        tp->isBlockPointerType()) {
      add_edge(id, intern_type(tp->getPointeeType()), kRelPointee, 0);
    }
    // class_type (rel 19) edges for member pointers are intentionally NOT
    // emitted: the "class a member pointer belongs to" accessor churned hard
    // across LLVM 21/22 (getClass() removed; getTypeDeclType / QualType(Type*)
    // paths deleted on 22). It is an obscure, untested relation — the catalog
    // keeps rel 19 but produces no edges of that kind. The member-pointer
    // pointee edge (the useful one) is still emitted.
    if (const auto *mpt = dyn_cast<clang::MemberPointerType>(tp)) {
      add_edge(id, intern_type(mpt->getPointeeType()), kRelPointee, 0);
    }

    if (const auto *at = dyn_cast<clang::ArrayType>(tp)) {
      add_edge(id, intern_type(at->getElementType()), kRelElementType, 0);
    } else if (const auto *vt = dyn_cast<clang::VectorType>(tp)) {
      add_edge(id, intern_type(vt->getElementType()), kRelElementType, 0);
    } else if (const auto *cx = dyn_cast<clang::ComplexType>(tp)) {
      add_edge(id, intern_type(cx->getElementType()), kRelElementType, 0);
    }

    if (const auto *ft = dyn_cast<clang::FunctionType>(tp)) {
      add_edge(id, intern_type(ft->getReturnType()), kRelResultType, 0);
      if (const auto *fpt = dyn_cast<clang::FunctionProtoType>(tp)) {
        int64_t i = 0;
        for (const clang::QualType &pt : fpt->getParamTypes()) {
          add_edge(id, intern_type(pt), kRelArgType, i++);
        }
      }
    }

#if LLVM_VERSION_MAJOR < 22
    // ElaboratedType was folded away in LLVM 22; the named_type sugar edge only
    // applies on earlier LLVM (e.g. 21 on RHEL 9.8).
    if (const auto *et = dyn_cast<clang::ElaboratedType>(tp))
      add_edge(id, intern_type(et->getNamedType()), kRelNamedType, 0);
#endif

    if (const auto *tst = dyn_cast<clang::TemplateSpecializationType>(tp)) {
      int64_t i = 0;
      for (const clang::TemplateArgument &ta : tst->template_arguments()) {
        if (ta.getKind() == clang::TemplateArgument::Type) {
          add_edge(id, intern_type(ta.getAsType()), kRelTemplateArg, i);
        }
        ++i;
      }
    }
  }

  void loc_of(clang::SourceLocation begin, clang::SourceLocation end,
              bool &is_main, int64_t &file_id, int64_t &line, int64_t &col,
              int64_t &end_line, int64_t &end_col) {
    if (begin.isInvalid()) {
      return;
    }
    is_main = sm_.isInMainFile(sm_.getExpansionLoc(begin));
    const clang::PresumedLoc pb = sm_.getPresumedLoc(begin);
    if (pb.isValid()) {
      file_id = intern_file(pb.getFilename(), is_main);
      line = pb.getLine();
      col = pb.getColumn();
    }
    const clang::PresumedLoc pe =
        sm_.getPresumedLoc(end.isValid() ? end : begin);
    if (pe.isValid()) {
      end_line = pe.getLine();
      end_col = pe.getColumn();
    }
  }

  int64_t intern_file(const std::string &path, bool is_main) {
    if (path.empty()) {
      return 0;
    }
    const auto it = file_ids_.find(path);
    int64_t id = 0;
    if (it != file_ids_.end()) {
      id = it->second;
    } else {
      id = next_file_++;
      file_ids_.emplace(path, id);
      run("INSERT INTO file(id, path, is_main) VALUES (?,?,?)",
          {id, path, int64_t{0}});
      ++stats_.files;
    }
    if (is_main && main_files_.insert(id).second) {
      run("UPDATE file SET is_main = 1 WHERE id = ?", {id});
    }
    return id;
  }

  int64_t intern_symbol(std::string usr, const std::string &name,
                        const clang::Decl *d, int kind) {
    if (const auto it = symbol_ids_.find(usr); it != symbol_ids_.end()) {
      return it->second;
    }
    const int64_t id = next_symbol_++;
    int64_t linkage = 0;
    if (const auto *nd = dyn_cast<clang::NamedDecl>(d)) {
      linkage = static_cast<int64_t>(nd->getFormalLinkage());
    }
    run("INSERT INTO symbol(id, usr, name, kind_id, linkage) VALUES "
        "(?,?,?,?,?)",
        {id, usr, name, static_cast<int64_t>(kind), linkage});
    symbol_ids_.emplace(std::move(usr), id);
    ++stats_.symbols;
    return id;
  }

  void seed_kind(int id, const char *name, const char *category) {
    if (!seeded_kinds_.insert(id).second) {
      return;
    }
    run("INSERT OR IGNORE INTO node_kind(id, name, category) VALUES (?,?,?)",
        {static_cast<int64_t>(id), std::string(name), std::string(category)});
  }

  void put_meta(const std::string &key, const std::string &value) {
    run("INSERT INTO meta(key, value) VALUES (?,?)", {key, value});
  }

  void run(const char *sql, std::initializer_list<SqlValue> vals) {
    SqliteStmt stmt = db_.prepare(sql);
    int idx = 1;
    for (const SqlValue &v : vals) {
      stmt.bind(idx++, v);
    }
    stmt.step_done();
  }

  SqliteDb db_;
  clang::ASTContext &ctx_;
  const clang::SourceManager &sm_;
  bool main_only_ = false;
  std::optional<std::string> descriptor_hash_;
  Options options_;
  DumpStats stats_;

  int64_t next_node_ = 1;
  int64_t next_file_ = 1;
  int64_t next_symbol_ = 1;

  std::unordered_map<const clang::Decl *, int64_t> decl_ids_;
  std::unordered_map<const clang::Stmt *, int64_t> stmt_ids_;
  std::unordered_map<const clang::Type *, int64_t> type_ids_;
  std::unordered_map<std::string, int64_t> file_ids_;
  std::unordered_map<std::string, int64_t> symbol_ids_;
  std::unordered_set<int> seeded_kinds_;
  std::unordered_set<int64_t> main_files_;
  std::deque<std::pair<const clang::Decl *, int64_t>> decl_work_;
  std::deque<std::pair<const clang::Stmt *, int64_t>> stmt_work_;
  std::deque<std::pair<const clang::Type *, int64_t>> type_work_;
};

// Structural walk: intern every decl/stmt and emit `child` edges with sibling
// order. Data recursion is disabled so TraverseStmt fires for every node, which
// is what lets the parent/ord stack track containment precisely.
class StructVisitor : public clang::RecursiveASTVisitor<StructVisitor> {
public:
  StructVisitor(Dumper &d, const clang::SourceManager &sm, bool main_only)
      : d_(d), sm_(sm), main_only_(main_only) {}

  [[nodiscard]] static bool shouldVisitTemplateInstantiations() {
    return false;
  }
  [[nodiscard]] static bool shouldVisitImplicitCode() { return false; }
  static bool shouldUseDataRecursionFor(clang::Stmt * /*s*/) { return false; }

  bool TraverseDecl(clang::Decl *decl) {
    if (decl == nullptr) {
      return true;
    }
    // main_only: prune header-declared subtrees (referenced header decls still
    // surface as shallow nodes via cross-ref edges). The TU has no location.
    if (main_only_ && !isa<clang::TranslationUnitDecl>(decl) &&
        !in_main(decl->getLocation())) {
      return true;
    }
    const int64_t id = d_.intern_decl(decl);
    return with_child(id, [&] {
      return clang::RecursiveASTVisitor<StructVisitor>::TraverseDecl(decl);
    });
  }

  bool TraverseStmt(clang::Stmt *stmt, DataRecursionQueue *queue = nullptr) {
    if (stmt == nullptr) {
      return true;
    }
    const int64_t id = d_.intern_stmt(stmt);
    return with_child(id, [&] {
      return clang::RecursiveASTVisitor<StructVisitor>::TraverseStmt(stmt,
                                                                     queue);
    });
  }

private:
  [[nodiscard]] bool in_main(clang::SourceLocation loc) const {
    return loc.isValid() && sm_.isInMainFile(sm_.getExpansionLoc(loc));
  }

  template <class Recurse> bool with_child(int64_t id, Recurse &&recurse) {
    if (id != 0 && !stack_.empty()) {
      d_.add_edge(stack_.back().first, id, kRelChild, stack_.back().second++);
    }
    stack_.emplace_back(id, 0);
    const bool ok = recurse();
    stack_.pop_back();
    return ok;
  }

  Dumper &d_;
  const clang::SourceManager &sm_;
  bool main_only_;
  std::vector<std::pair<int64_t, int64_t>> stack_;
};

// ASTConsumer / FrontendAction / factory: run the Dumper over the built AST.
class AstGraphConsumer : public clang::ASTConsumer {
public:
  AstGraphConsumer(std::string temp_path, Options opts, std::string source,
                   std::vector<std::string> args,
                   std::optional<std::string> driver, DumpStats *out,
                   std::exception_ptr *err, bool *handled)
      : temp_path_(std::move(temp_path)), opts_(opts),
        source_(std::move(source)), args_(std::move(args)),
        driver_(std::move(driver)), out_(out), err_(err), handled_(handled) {}

  void HandleTranslationUnit(clang::ASTContext &ctx) override {
    *handled_ = true;
    try {
      Dumper dumper(temp_path_, ctx, opts_);
      dumper.write_meta(source_, args_, driver_);
      dumper.seed_relation_kinds();
      StructVisitor visitor(dumper, ctx.getSourceManager(), opts_.main_only);
      visitor.TraverseDecl(ctx.getTranslationUnitDecl());
      dumper.drain();
      *out_ = dumper.finish();
    } catch (...) {
      *err_ = std::current_exception();
    }
  }

private:
  std::string temp_path_;
  Options opts_;
  std::string source_;
  std::vector<std::string> args_;
  std::optional<std::string> driver_;
  DumpStats *out_;
  std::exception_ptr *err_;
  bool *handled_;
};

class AstGraphAction : public clang::ASTFrontendAction {
public:
  AstGraphAction(std::string temp_path, Options opts, std::string source,
                 std::vector<std::string> args,
                 std::optional<std::string> driver, DumpStats *out,
                 std::exception_ptr *err, bool *handled)
      : temp_path_(std::move(temp_path)), opts_(opts),
        source_(std::move(source)), args_(std::move(args)),
        driver_(std::move(driver)), out_(out), err_(err), handled_(handled) {}

  std::unique_ptr<clang::ASTConsumer>
  CreateASTConsumer(clang::CompilerInstance & /*ci*/,
                    llvm::StringRef /*file*/) override {
    return std::make_unique<AstGraphConsumer>(temp_path_, opts_, source_, args_,
                                              driver_, out_, err_, handled_);
  }

private:
  std::string temp_path_;
  Options opts_;
  std::string source_;
  std::vector<std::string> args_;
  std::optional<std::string> driver_;
  DumpStats *out_;
  std::exception_ptr *err_;
  bool *handled_;
};

class AstGraphActionFactory : public clang::tooling::FrontendActionFactory {
public:
  AstGraphActionFactory(std::string temp_path, Options opts, std::string source,
                        std::vector<std::string> args,
                        std::optional<std::string> driver, DumpStats *out,
                        std::exception_ptr *err, bool *handled)
      : temp_path_(std::move(temp_path)), opts_(opts),
        source_(std::move(source)), args_(std::move(args)),
        driver_(std::move(driver)), out_(out), err_(err), handled_(handled) {}

  std::unique_ptr<clang::FrontendAction> create() override {
    return std::make_unique<AstGraphAction>(temp_path_, opts_, source_, args_,
                                            driver_, out_, err_, handled_);
  }

private:
  std::string temp_path_;
  Options opts_;
  std::string source_;
  std::vector<std::string> args_;
  std::optional<std::string> driver_;
  DumpStats *out_;
  std::exception_ptr *err_;
  bool *handled_;
};

} // namespace

std::string artifact_key(const std::string &source_path,
                         const std::vector<std::string> &args,
                         const std::optional<std::string> &driver,
                         const Options &opts) {
  const AstCacheKey key{
      .abspath = source_path, .flags = args, .driver = driver};
  return sha1_hex(sha1_cache_key(key) + std::string("\0main_only=", 11) +
                  (opts.main_only ? "1" : "0"));
}

DumpStats dump_tu(const std::string &source_path,
                  const std::vector<std::string> &args,
                  const std::optional<std::string> &driver,
                  const std::string &out_db_path, const Options &opts) {
  // Publish atomically: write beside the destination and rename only after
  // SQLite commits, so a failed dump preserves any previous complete artifact.
  const std::string temp_path =
      out_db_path + ".tmp-" +
      std::to_string(static_cast<long long>(::getpid()));
  std::remove(temp_path.c_str());
  std::remove((temp_path + "-journal").c_str());

  DumpStats stats;
  std::exception_ptr err;
  bool handled = false;
  try {
    // Flag assembly identical to the LT index engine: caller-resolved args plus
    // toolchain search paths + -ferror-limit=0.
    Toolchain toolchain;
    const bool cpp = Toolchain::is_cpp(source_path, args);
    std::vector<std::string> full = args;
    for (std::string &f : toolchain.toolchain_flags(cpp, driver)) {
      full.push_back(std::move(f));
    }
    full.emplace_back("-ferror-limit=0");

    clang::tooling::FixedCompilationDatabase cdb(".", full);
    clang::tooling::ClangTool tool(cdb, {source_path});
    const std::string resource_dir = opts.resource_dir.value_or(
#ifdef CIDX_CLANG_RESOURCE_DIR
        std::string(CIDX_CLANG_RESOURCE_DIR)
#else
        std::string{}
#endif
    );
    if (!resource_dir.empty()) {
      tool.appendArgumentsAdjuster(clang::tooling::getInsertArgumentAdjuster(
          {"-resource-dir", resource_dir},
          clang::tooling::ArgumentInsertPosition::BEGIN));
    }
    AstGraphActionFactory factory(temp_path, opts, source_path, args, driver,
                                  &stats, &err, &handled);
    tool.run(&factory);
    if (err) {
      std::rethrow_exception(err);
    }
    if (!handled) {
      throw CidxError("cannot parse " + source_path + " (no AST produced)");
    }
    if (std::rename(temp_path.c_str(), out_db_path.c_str()) != 0) {
      throw CidxError("cannot publish AST database at " + out_db_path);
    }
  } catch (...) {
    std::remove(temp_path.c_str());
    std::remove((temp_path + "-journal").c_str());
    throw;
  }
  return stats;
}

ArtifactRecord publish_tu_artifact(Storage &storage,
                                   const std::string &index_path,
                                   const std::string &source_path,
                                   const std::string &out_db_path,
                                   const std::vector<std::string> &args,
                                   const std::optional<std::string> &driver,
                                   const Options &opts) {
  const auto component = storage.component_for_path(source_path);
  const std::string workspace_identity = storage.index_identity().workspace;
  const std::filesystem::path component_root =
      component ? std::filesystem::path(storage.component_abs_base(*component))
                : std::filesystem::path(source_path).parent_path();
  const std::string relative_tu =
      std::filesystem::relative(source_path, component_root).generic_string();

  std::string configuration_material;
  for (const auto &option : args) {
    const auto stable = stable_option(option, component_root);
    configuration_material += std::to_string(stable.size()) + ":" + stable;
  }
  const auto stable_driver =
      driver ? stable_option(*driver, component_root) : std::string{};
  configuration_material +=
      "driver:" + stable_driver + ":main-only:" + (opts.main_only ? "1" : "0");
  const std::string separator(1, '\0');
  const std::string configuration_identity =
      sha256_hex(workspace_identity + separator + configuration_material);
  const std::string tu_identity =
      sha256_hex(workspace_identity + separator + relative_tu + separator +
                 configuration_identity);

  ArtifactSpec artifact;
  artifact.logical_id = "astgraph:" + workspace_identity + ":" + relative_tu +
                        ":" + configuration_identity;
  artifact.kind = "astgraph";
  artifact.artifact_schema = "cidx-astgraph/v" + std::to_string(kSchemaVersion);
  artifact.catalog_version = catalog::kCatalogVersion;
  artifact.catalog_hash = std::string(catalog::kCatalogHash);
  artifact.producer_version = std::string("cidx-astgraph ") + cli::kVersion;
  artifact.engine_version = std::string("cidx ") + cli::kVersion;
  artifact.workspace_identity = workspace_identity;
  artifact.tu_identity = tu_identity;
  artifact.configuration_identity = configuration_identity;
  artifact.input_fact_set_identity =
      sha256_hex("facts:" + workspace_identity + ":" + tu_identity + ":" +
                 std::string(catalog::kCatalogHash));
  artifact.completeness = ArtifactCompleteness::complete;
  artifact.truncation = ArtifactTruncation::none;
  artifact.trust = ArtifactTrust::producer_verified;
  artifact.evidence = "source";
  const std::size_t hash_prefix = tu_identity.starts_with("sha256:") ? 7 : 0;
  artifact.attachment_name = "astgraph_" + tu_identity.substr(hash_prefix, 16);
  artifact.exposed_relations = {"node", "edge", "symbol", "meta"};

  ArtifactStore artifacts(storage,
                          std::filesystem::path(index_path).parent_path());
  return artifacts.publish_existing(
      artifact, out_db_path, [&](const ArtifactRecord &record) {
        const auto published_path =
            std::filesystem::path(index_path).parent_path() /
            record.relative_path;
        SqliteDb sidecar(published_path.string(), true,
                         SqliteProfile::read_only_replay);
        auto sidecar_symbols = sidecar.prepare(
            "SELECT usr FROM symbol WHERE usr <> '' ORDER BY usr");
        while (sidecar_symbols.step()) {
          const std::string usr = sidecar_symbols.col_text(0);
          const auto core_symbol = storage.lookup_symbol(usr);
          artifacts.record_identity_mapping(
              record.spec.logical_id, usr, "usr", usr,
              core_symbol ? "resolved" : "unresolved",
              core_symbol ? std::optional<std::int64_t>(core_symbol->id)
                          : std::nullopt,
              core_symbol ? "" : "core symbol is not present in index");
        }
      });
}

} // namespace cidx::astgraph
