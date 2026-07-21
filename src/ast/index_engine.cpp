#include "ast/index_engine.hpp"

#include "ast/function_definition_visitor.hpp"
#include "ast/declaration_edge_visitor.hpp"
#include "ast/include_facts.hpp"
#include "ast/location.hpp"
#include "ast/namespace_use_visitor.hpp"
#include "ast/storage_edge_sink.hpp"
#include "ast/storage_symbol_sink.hpp"
#include "ast/symbol_visitor.hpp"

#include "toolchain/toolchain.hpp"
#include "compiledb/compiledb.hpp"
#include "storage/storage.hpp"
#include "util/env.hpp"
#include "util/hashing.hpp"
#include "util/pathutil.hpp"

#include "clang/AST/ASTConsumer.h"
#include "clang/AST/ASTContext.h"
#include "clang/Basic/Diagnostic.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendAction.h"
#include "clang/Lex/PPCallbacks.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Tooling/ArgumentsAdjusters.h"
#include "clang/Tooling/CompilationDatabase.h"
#include "clang/Tooling/Tooling.h"

#include <sys/stat.h>

#include <cctype>
#include <memory>
#include <unordered_set>

namespace cidx::ast {

namespace {

std::optional<double> file_mtime(const std::string &path) {
  struct stat st{};
  if (::stat(path.c_str(), &st) != 0) {
    return std::nullopt;
  }
#ifdef __APPLE__
  return static_cast<double>(st.st_mtimespec.tv_sec) +
         (static_cast<double>(st.st_mtimespec.tv_nsec) * 1e-9);
#else
  return static_cast<double>(st.st_mtim.tv_sec) +
         static_cast<double>(st.st_mtim.tv_nsec) * 1e-9;
#endif
}

struct EngineState {
  cidx::Storage *db = nullptr;
  const cidx::File *rec = nullptr;
  std::string path; // main file (canonical absolute)
  bool graph_enabled = true;
  bool strict = false; // CIDX_STRICT: abort on Error, not just Fatal
  IndexOneOutcome *out = nullptr;
  // v31: the full preprocessing record (ast/include_facts.hpp). The header
  // two-pass consumes its resolved targets in directive order
  // (clang_getInclusions parity, depth > 0 only); `cidx include` consumes the
  // rest.
  IncludeFacts includes;
  const cidx::IncludeConfig *config = nullptr; // v31: this TU's normalized args
  clang::Preprocessor *pp = nullptr;           // v31: for include-guard status
  bool tu_handled = false;
};

// Severity map mirroring CXDiagnosticSeverity (collect_diagnostics parity).
int64_t cx_severity(clang::DiagnosticsEngine::Level level) {
  switch (level) {
  case clang::DiagnosticsEngine::Ignored: return 0;
  case clang::DiagnosticsEngine::Note:    return 1;
  case clang::DiagnosticsEngine::Remark:  return 2; // CIndex maps Remark->Warning
  case clang::DiagnosticsEngine::Warning: return 2;
  case clang::DiagnosticsEngine::Error:   return 3;
  case clang::DiagnosticsEngine::Fatal:   return 4;
  }
  return 2;
}

class DiagCollector : public clang::DiagnosticConsumer {
public:
  explicit DiagCollector(std::vector<cidx::Diagnostic> &out) : out_(out) {}

  void HandleDiagnostic(clang::DiagnosticsEngine::Level level,
                        const clang::Diagnostic &info) override {
    clang::DiagnosticConsumer::HandleDiagnostic(level, info);
    if (level < clang::DiagnosticsEngine::Warning) {
      return; // cidx stores warnings and above (collect_diagnostics)
    }
    cidx::Diagnostic d;
    d.severity = cx_severity(level);
    llvm::SmallString<256> msg;
    info.FormatDiagnostic(msg);
    d.spelling = std::string(msg);
    if (info.hasSourceManager() && info.getLocation().isValid()) {
      const clang::SourceManager &sm = info.getSourceManager();
      const clang::SourceLocation loc = sm.getExpansionLoc(info.getLocation());
      d.file_path = sm.getFilename(loc).str();
      d.line = sm.getExpansionLineNumber(loc);
      d.col = sm.getExpansionColumnNumber(loc);
    }
    out_.push_back(std::move(d));
  }

private:
  std::vector<cidx::Diagnostic> &out_;
};

// Per-TU indexing stages: symbols(main) -> owned-header two-pass ->
// edges(main). One instance per HandleTranslationUnit call.
class TranslationUnitIndexer {
public:
  TranslationUnitIndexer(clang::ASTContext &context, EngineState &state)
      : context_(context), state_(state), db_(*state.db), symbols_(db_),
        edges_(db_), tu_(context.getTranslationUnitDecl()) {}

  void run() {
    state_.out->stored = run_symbol_pass(state_.path, state_.rec->id);
    const std::vector<PendingHeader> plan = plan_owned_headers();
    run_header_passes(plan);
    // edges(main) LAST (commands.cpp ordering).
    run_edge_pass(state_.path, state_.rec->id);
    // v31 include tier LAST of all: an include_edge references file(id), so
    // every owned header must already have its row from the header two-pass.
    if (state_.pp != nullptr) {
      resolve_include_guards(*state_.pp, state_.includes);
    }
    persist_include_facts(db_, state_.includes, *state_.config);
  }

private:
  // A not-yet-indexed OWNED non-system header discovered by the include
  // recorder, with its freshly minted file row.
  struct PendingHeader {
    std::string path;
    int64_t file_id;
    std::optional<double> mtime;
    int stored = 0;
  };

  int run_symbol_pass(const std::string &file, int64_t file_id) {
    symbols_.set_current_file_id(file_id);
    symbols_.reset_counters();
    auto txn = db_.transaction();
    SymbolVisitor visitor(context_, symbols_, file);
    visitor.TraverseDecl(tu_);
    txn.commit();
    return symbols_.stored_count();
  }

  void run_edge_pass(const std::string &file, int64_t file_id) {
    if (!state_.graph_enabled) {
      return;
    }
    edges_.delete_edges_for_file(file_id);
    edges_.delete_definitions_for_file(file_id);
    auto txn = db_.transaction();
    DeclarationEdgeVisitor decls(context_, edges_, file, file_id);
    decls.TraverseDecl(tu_);
    FunctionDefinitionVisitor bodies(context_, edges_, file, file_id);
    bodies.TraverseDecl(tu_);
    NamespaceUseVisitor ns(context_, edges_, file, file_id);
    ns.TraverseDecl(tu_);
    txn.commit();
  }

  // Classify every recorded inclusion (system / unowned / already indexed)
  // and mint file rows for the headers this TU must index.
  std::vector<PendingHeader> plan_owned_headers() {
    std::vector<PendingHeader> plan;
    std::unordered_set<std::string> seen;
    cidx::HeaderStats &counts = state_.out->headers;
    for (const IncludeFact &f : state_.includes.includes) {
      if (!f.resolved) {
        continue; // no file was opened: nothing to index
      }
      const std::string &inc = f.dst_path;
      const std::string abs = cidx::pathutil::abspath(inc);
      if (!seen.insert(abs).second) {
        continue;
      }
      if (is_system_header(inc)) {
        ++counts.system;
        continue;
      }
      if (!db_.component_for_path(abs)) {
        ++counts.unowned;
        continue;
      }
      const std::optional<std::string> md5 = cidx::md5_of(abs);
      if (db_.is_file_indexed(abs, std::nullopt, md5)) {
        ++counts.already;
        continue;
      }
      const std::optional<double> mtime = file_mtime(abs);
      const int64_t hid =
          db_.add_file_path(abs, mtime, md5, state_.rec->compile_options,
                            state_.rec->driver);
      plan.push_back(
          {.path = abs, .file_id = hid, .mtime = mtime, .stored = 0});
    }
    return plan;
  }

  // Header two-pass (index_headers): pass 1 mints symbols for every planned
  // header, pass 2 extracts its edges and marks it indexed.
  void run_header_passes(std::vector<PendingHeader> plan) {
    cidx::HeaderStats &counts = state_.out->headers;
    for (PendingHeader &ph : plan) {
      ph.stored = run_symbol_pass(ph.path, ph.file_id);
    }
    for (const PendingHeader &ph : plan) {
      run_edge_pass(ph.path, ph.file_id);
      db_.mark_file_indexed(ph.file_id, ph.mtime);
      ++counts.indexed;
      counts.symbols += ph.stored;
    }
  }

  // System check: characteristic of the header's own content (parity with
  // clang_Location_isInSystemHeader at (file,1,1)).
  [[nodiscard]] bool is_system_header(const std::string &inc) const {
    const clang::SourceManager &sm = context_.getSourceManager();
    auto fe = sm.getFileManager().getFileRef(inc);
    if (!fe) {
      return false;
    }
    const clang::FileID fid = sm.translateFile(*fe);
    if (!fid.isValid()) {
      return false;
    }
    return sm.getFileCharacteristic(sm.getLocForStartOfFile(fid)) !=
           clang::SrcMgr::C_User;
  }

  clang::ASTContext &context_;
  EngineState &state_;
  cidx::Storage &db_;
  StorageSymbolSink symbols_;
  StorageEdgeSink edges_;
  clang::Decl *tu_;
};

class IndexASTConsumer : public clang::ASTConsumer {
public:
  explicit IndexASTConsumer(EngineState &state) : state_(state) {}

  void HandleTranslationUnit(clang::ASTContext &context) override {
    state_.tu_handled = true;
    if (!diagnostics_allow_indexing(context)) {
      return;
    }
    TranslationUnitIndexer(context, state_).run();
  }

private:
  // Parity with classic parse(): a diagnostic at/above the abort level makes
  // the file fail with NO rows written (the classic path threw before
  // index_symbols). Gate before touching the DB.
  [[nodiscard]] bool
  diagnostics_allow_indexing(const clang::ASTContext &context) const {
    const clang::DiagnosticsEngine &de = context.getDiagnostics();
    return !de.hasFatalErrorOccurred() &&
           (!state_.strict || de.getClient()->getNumErrors() <= 0);
  }

  EngineState &state_;
};

class IndexFrontendAction : public clang::ASTFrontendAction {
public:
  explicit IndexFrontendAction(EngineState &state) : state_(state) {}

  bool BeginSourceFileAction(clang::CompilerInstance &ci) override {
    register_include_callbacks(ci, state_.includes);
    state_.pp = &ci.getPreprocessor();
    return true;
  }

  std::unique_ptr<clang::ASTConsumer>
  CreateASTConsumer(clang::CompilerInstance & /*ci*/,
                    llvm::StringRef /*file*/) override {
    return std::make_unique<IndexASTConsumer>(state_);
  }

private:
  EngineState &state_;
};

class IndexFrontendActionFactory : public clang::tooling::FrontendActionFactory {
public:
  explicit IndexFrontendActionFactory(EngineState &state) : state_(state) {}
  std::unique_ptr<clang::FrontendAction> create() override {
    return std::make_unique<IndexFrontendAction>(state_);
  }

private:
  EngineState &state_;
};

} // namespace

// Flag assembly identical to Parser::parse: re-sanitized stored options,
// <label>/$VAR resolution, toolchain search paths, -ferror-limit=0.
static std::vector<std::string> build_clang_arguments(cidx::Storage &db,
                                                      const cidx::File &rec,
                                                      const std::string &path) {
  const std::vector<std::string> opts = cidx::CompileDb::resolve_options(
      cidx::CompileDb::sanitize(rec.compile_options
                                    ? *rec.compile_options
                                    : std::vector<std::string>{}),
      [&db](const std::string &n) { return db.get_alias(n); });
  cidx::Toolchain toolchain;
  const bool cpp = cidx::Toolchain::is_cpp(path, opts);
  std::vector<std::string> args = opts;
  for (std::string &f : toolchain.toolchain_flags(cpp, rec.driver)) {
    args.push_back(std::move(f));
  }
  args.emplace_back("-ferror-limit=0");
  return args;
}

// create_compilation_database + tool: the fixed database must outlive the
// tool, so both live in one holder.
struct CompilationSetup {
  CompilationSetup(const std::vector<std::string> &args,
                   const std::string &path)
      : cdb(".", args), tool(cdb, {path}) {
#ifdef CIDX_CLANG_RESOURCE_DIR
    tool.appendArgumentsAdjuster(clang::tooling::getInsertArgumentAdjuster(
        {"-resource-dir", CIDX_CLANG_RESOURCE_DIR},
        clang::tooling::ArgumentInsertPosition::BEGIN));
#endif
  }
  clang::tooling::FixedCompilationDatabase cdb;
  clang::tooling::ClangTool tool;
};

// CIDX_STRICT: abort on Error (not just Fatal) when set truthy.
static bool read_strict_mode() {
  std::string v = cidx::get_env("CIDX_STRICT").value_or("");
  for (char &c : v) {
    c = static_cast<char>(std::tolower(c));
  }
  return !(v.empty() || v == "0" || v == "off" || v == "none" || v == "false");
}

// Diagnostics at/above the abort level (CIDX_STRICT: default Fatal, strict
// Error) fail the TU with the "<path>: N fatal diagnostic(s): file:line:
// msg[; ...]" summary (first 3).
static void apply_diagnostic_policy(const std::string &path, bool strict,
                                    const std::vector<std::string> &args,
                                    IndexOneOutcome &out) {
  const int64_t level = strict ? 3 : 4;
  std::size_t fatal_count = 0;
  std::vector<std::string> summary;
  for (const cidx::Diagnostic &d : out.diagnostics) {
    if (d.severity < level) {
      continue;
    }
    ++fatal_count;
    if (summary.size() < 3) {
      summary.push_back(d.file_path.value_or("") + ":" +
                        std::to_string(d.line.value_or(0)) + ": " +
                        d.spelling);
    }
  }
  if (fatal_count == 0) {
    return;
  }
  out.parse_failed = true;
  std::string joined;
  for (std::size_t i = 0; i < summary.size(); ++i) {
    if (i != 0) {
      joined += "; ";
    }
    joined += summary[i];
  }
  out.error = path + ": " + std::to_string(fatal_count) +
              " fatal diagnostic(s): " + joined;
  out.failed_flags = args;
}

IndexOneOutcome run_index_one(cidx::Storage &db, const cidx::File &rec,
                              const std::string &path, bool graph_enabled) {
  IndexOneOutcome out;
  const std::vector<std::string> args = build_clang_arguments(db, rec, path);
  CompilationSetup setup(args, path);
  DiagCollector collector(out.diagnostics);
  setup.tool.setDiagnosticConsumer(&collector);

  // v31: the configuration the include tier records against is the one this
  // parse actually used -- the same resolved args, driver, and resource dir --
  // so a cleanup plan can revalidate a removal under exactly this TU later.
  // Working dir is "." to match CompilationSetup's FixedCompilationDatabase.
  cidx::IncludeConfig config;
  config.tu_file_id = rec.id;
  config.driver = rec.driver;
  config.working_dir = std::string(".");
  config.arguments = args;
  config.lang_mode = cidx::Toolchain::is_cpp(path, args) ? "c++" : "c";
#ifdef CIDX_CLANG_RESOURCE_DIR
  config.resource_dir = std::string(CIDX_CLANG_RESOURCE_DIR);
#endif

  EngineState state;
  state.db = &db;
  state.rec = &rec;
  state.path = path;
  state.graph_enabled = graph_enabled;
  state.strict = read_strict_mode();
  state.out = &out;
  state.config = &config;

  IndexFrontendActionFactory factory(state);
  (void)setup.tool.run(&factory);
  if (!state.tu_handled) {
    out.parse_failed = true;
    out.error = "cannot parse " + path;
    return out;
  }
  apply_diagnostic_policy(path, state.strict, args, out);
  return out;
}

} // namespace cidx::ast
