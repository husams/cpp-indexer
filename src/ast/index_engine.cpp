#include "ast/index_engine.hpp"

#include "ast/declaration_edge_visitor.hpp"
#include "ast/function_definition_visitor.hpp"
#include "ast/include_facts.hpp"
#include "ast/location.hpp"
#include "ast/namespace_use_visitor.hpp"
#include "ast/storage_edge_sink.hpp"
#include "ast/storage_symbol_sink.hpp"
#include "ast/symbol_visitor.hpp"

#include "compiledb/compiledb.hpp"
#include "storage/ports.hpp"
#include "storage/sqlite_adapters.hpp"
#include "storage/storage.hpp"
#include "toolchain/toolchain.hpp"
#include "util/env.hpp"
#include "util/hashing.hpp"
#include "util/pathutil.hpp"
#include "workspace/context.hpp"

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

#include <algorithm>
#include <cctype>
#include <memory>
#include <stdexcept>
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

std::optional<std::string> parsed_file_md5(clang::SourceManager &source_manager,
                                           const std::string &path) {
  auto file = source_manager.getFileManager().getFileRef(path);
  if (!file) {
    return std::nullopt;
  }
  const auto buffer = source_manager.getMemoryBufferForFileOrNone(*file);
  if (!buffer) {
    return std::nullopt;
  }
  return cidx::md5_hex(buffer->getBufferStart(), buffer->getBufferSize());
}

struct EngineState {
  cidx::Storage *db = nullptr;
  cidx::storage::AstStoragePorts *ports = nullptr;
  std::unique_ptr<cidx::storage::UnitOfWork> unit;
  const cidx::File *rec = nullptr;
  std::string path; // main file (canonical absolute)
  bool graph_enabled = true;
  bool strict = false; // CIDX_STRICT: abort on Error, not just Fatal
  cidx::storage::FailureInjector *failure_injector = nullptr;
  IndexOneOutcome *out = nullptr;
  // v31: the full preprocessing record (ast/include_facts.hpp). The header
  // two-pass consumes its resolved targets in directive order
  // (clang_getInclusions parity, depth > 0 only); `cidx include` consumes the
  // rest.
  IncludeFacts includes;
  const cidx::IncludeConfig *config = nullptr; // v31: this TU's normalized args
  int64_t normalized_config_id = -1;
  clang::Preprocessor *pp = nullptr; // v31: for include-guard status
  bool tu_handled = false;
};

// Severity map mirroring CXDiagnosticSeverity (collect_diagnostics parity).
int64_t cx_severity(clang::DiagnosticsEngine::Level level) {
  switch (level) {
  case clang::DiagnosticsEngine::Ignored:
    return 0;
  case clang::DiagnosticsEngine::Note:
    return 1;
  case clang::DiagnosticsEngine::Remark:
    return 2; // CIndex maps Remark->Warning
  case clang::DiagnosticsEngine::Warning:
    return 2;
  case clang::DiagnosticsEngine::Error:
    return 3;
  case clang::DiagnosticsEngine::Fatal:
    return 4;
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
      : context_(context), state_(state), db_(*state.db),
        symbols_(*state.ports), edges_(*state.ports),
        tu_(context.getTranslationUnitDecl()) {}

  void run() {
    db_.delete_symbols_for_file(state_.rec->id);
    state_.out->stored = run_symbol_pass(state_.path, state_.rec->id);
    const std::vector<int64_t> main_symbol_ids = symbols_.symbol_ids();
    const std::vector<PendingHeader> plan = plan_owned_headers();
    run_header_passes(plan);
    // edges(main) LAST (commands.cpp ordering).
    run_edge_pass(state_.path, state_.rec->id);
    db_.associate_facts_for_file(state_.rec->id, state_.normalized_config_id,
                                 main_symbol_ids, edges_.edge_ids(),
                                 edges_.definition_ids());
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
    std::optional<std::string> md5;
    bool covered_by_current_config = false;
    int stored = 0;
    std::vector<int64_t> symbol_ids;
  };

  int run_symbol_pass(const std::string &file, int64_t file_id) {
    symbols_.set_current_file_id(file_id);
    symbols_.set_identity_translation_unit_config_id(
        state_.normalized_config_id, state_.rec->id);
    symbols_.reset_counters();
    SymbolVisitor visitor(context_, symbols_, file);
    visitor.TraverseDecl(tu_);
    return symbols_.stored_count();
  }

  void run_edge_pass(const std::string &file, int64_t file_id) {
    if (!state_.graph_enabled) {
      return;
    }
    edges_.set_current_file_id(file_id);
    edges_.set_identity_translation_unit_config_id(state_.normalized_config_id,
                                                   state_.rec->id);
    edges_.delete_edges_for_file(file_id);
    edges_.delete_definitions_for_file(file_id);
    edges_.reset_fact_ids();
    DeclarationEdgeVisitor decls(context_, edges_, file, file_id);
    decls.TraverseDecl(tu_);
    FunctionDefinitionVisitor bodies(context_, edges_, file, file_id);
    bodies.TraverseDecl(tu_);
    NamespaceUseVisitor ns(context_, edges_, file, file_id);
    ns.TraverseDecl(tu_);
  }

  [[nodiscard]] bool header_covered_by_current_config(
      const std::optional<cidx::File> &file) const {
    if (!file) {
      return false;
    }
    const auto applicability = db_.file_configs_for(file->id);
    return std::ranges::any_of(
        applicability, [this](const cidx::FileConfigApplicability &a) {
          return a.config_id == state_.normalized_config_id &&
                 a.role == "header" &&
                 a.state == cidx::TranslationUnitConfigState::registered;
        });
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
      const std::optional<std::string> current_md5 = cidx::md5_of(abs);
      const auto existing = db_.get_file(abs);
      const bool covered_by_current_config =
          header_covered_by_current_config(existing);
      const std::optional<std::string> parsed_md5 =
          parsed_file_md5(context_.getSourceManager(), abs);
      if (current_md5 && parsed_md5 && current_md5 == parsed_md5 &&
          db_.is_file_indexed(abs, std::nullopt, current_md5) &&
          covered_by_current_config) {
        ++counts.already;
        continue;
      }
      const std::optional<double> mtime = file_mtime(abs);
      const int64_t hid =
          db_.add_file_path(abs, mtime, parsed_md5, state_.rec->compile_options,
                            state_.rec->driver);
      plan.push_back({.path = abs,
                      .file_id = hid,
                      .mtime = mtime,
                      .md5 = parsed_md5,
                      .covered_by_current_config = covered_by_current_config,
                      .stored = 0});
    }
    return plan;
  }

  // Header two-pass (index_headers): pass 1 mints symbols for every planned
  // header, pass 2 extracts its edges and marks it indexed.
  void run_header_passes(std::vector<PendingHeader> plan) {
    cidx::HeaderStats &counts = state_.out->headers;
    for (PendingHeader &ph : plan) {
      if (ph.covered_by_current_config) {
        db_.delete_symbols_for_file(ph.file_id);
      }
      ph.stored = run_symbol_pass(ph.path, ph.file_id);
      ph.symbol_ids = symbols_.symbol_ids();
    }
    if (state_.failure_injector != nullptr) {
      state_.failure_injector->inject(
          cidx::storage::FailurePoint::partial_transform);
    }
    for (const PendingHeader &ph : plan) {
      run_edge_pass(ph.path, ph.file_id);
      if (!SourceSnapshot{.md5 = ph.md5}.matches(ph.path)) {
        state_.out->source_changed = true;
        db_.set_file_indexed(ph.file_id, false);
        continue;
      }
      db_.associate_facts_for_file(ph.file_id, state_.normalized_config_id,
                                   ph.symbol_ids, edges_.edge_ids(),
                                   edges_.definition_ids());
      db_.mark_file_indexed(ph.file_id, ph.mtime, ph.md5);
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

class IndexFrontendActionFactory
    : public clang::tooling::FrontendActionFactory {
public:
  explicit IndexFrontendActionFactory(EngineState &state) : state_(state) {}
  std::unique_ptr<clang::FrontendAction> create() override {
    return std::make_unique<IndexFrontendAction>(state_);
  }

private:
  EngineState &state_;
};

} // namespace

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
                        std::to_string(d.line.value_or(0)) + ": " + d.spelling);
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

SourceSnapshot SourceSnapshot::capture(const std::string &path) {
  return {.md5 = cidx::md5_of(path)};
}

bool SourceSnapshot::matches(const std::string &path) const {
  const std::optional<std::string> current = cidx::md5_of(path);
  return md5.has_value() && current.has_value() && md5 == current;
}

class PipelineFailureInjector final : public cidx::storage::FailureInjector {
public:
  explicit PipelineFailureInjector(IndexFailurePoint target) : target_(target) {}

  void inject(cidx::storage::FailurePoint point) override {
    const IndexFailurePoint current = [&] {
      switch (point) {
      case cidx::storage::FailurePoint::begin:
        return IndexFailurePoint::begin;
      case cidx::storage::FailurePoint::adapter:
        return IndexFailurePoint::adapter;
      case cidx::storage::FailurePoint::partial_transform:
        return IndexFailurePoint::partial_transform;
      case cidx::storage::FailurePoint::commit:
        return IndexFailurePoint::commit;
      }
      return IndexFailurePoint::none;
    }();
    if (current == target_) {
      throw std::runtime_error(std::string("injected ") + failure_name(current) +
                               " failure");
    }
  }

private:
  static const char *failure_name(IndexFailurePoint point) {
    switch (point) {
    case IndexFailurePoint::begin:
      return "unit-of-work begin";
    case IndexFailurePoint::adapter:
      return "storage adapter";
    case IndexFailurePoint::partial_transform:
      return "partial transform";
    case IndexFailurePoint::commit:
      return "unit-of-work commit";
    case IndexFailurePoint::none:
      return "unexpected";
    }
    return "unexpected";
  }

  IndexFailurePoint target_;
};

IndexOneOutcome run_index_one(cidx::Storage &db, const cidx::File &rec,
                              const std::string &path, bool graph_enabled,
                              IndexFailurePoint failure) {
  IndexOneOutcome out;
  const SourceSnapshot source = SourceSnapshot::capture(path);
  out.source_md5 = source.md5;
  cidx::StorageWorkspaceAdapter workspace_data(db);
  WorkspaceContext context = WorkspaceContext::borrow(
      workspace_data, WorkspaceReadWriteMode::read_write);
  Toolchain toolchain;
  TranslationUnitConfigurationService resolver(context, toolchain);
  const TranslationUnitDescriptor descriptor = resolver.resolve(path);
  const TranslationUnitConfig &resolved = descriptor.configuration;
  const std::vector<std::string> args =
      TranslationUnitConfigurationService::invocation_arguments(path,
                                                                descriptor);
  CompilationSetup setup(args, path);
  DiagCollector collector(out.diagnostics);
  setup.tool.setDiagnosticConsumer(&collector);
  PipelineFailureInjector injector(failure);
  cidx::storage::SqliteStoragePorts ports(db, &injector);

  // v31: the configuration the include tier records against is the one this
  // parse actually used -- the same resolved args, driver, and resource dir --
  // so a cleanup plan can revalidate a removal under exactly this TU later.
  // Working dir is "." to match CompilationSetup's FixedCompilationDatabase.
  cidx::IncludeConfig config;
  config.tu_file_id = rec.id;
  config.driver = resolved.driver;
  config.working_dir = resolved.working_dir;
  config.arguments = args;
  config.lang_mode = resolved.language;
  config.resource_dir = resolved.resource_dir;

  EngineState state;
  cidx::storage::AstStoragePorts ast_ports{
      ports.workspace_catalog_read(), ports.source_read(), ports.symbol_read(),
      ports.symbol_write(),           ports.type_write(),  ports.fact_write(),
      ports.definition_write(),       ports.unit_of_work()};
  state.db = &db;
  state.ports = &ast_ports;
  state.rec = &rec;
  state.path = path;
  state.graph_enabled = graph_enabled;
  state.strict = read_strict_mode();
  state.failure_injector = &injector;
  state.out = &out;
  state.config = &config;
  state.unit = ports.unit_of_work().begin();
  state.normalized_config_id = db.add_translation_unit_config(resolved);

  IndexFrontendActionFactory factory(state);
  (void)setup.tool.run(&factory);
  if (!state.tu_handled) {
    out.parse_failed = true;
    out.error = "cannot parse " + path;
    return out;
  }
  apply_diagnostic_policy(path, state.strict, args, out);
  if (!source.matches(path)) {
    out.source_changed = true;
    out.error = path + ": source changed during indexing; retry required";
  } else if (out.source_changed && out.error.empty()) {
    out.error = path + ": source changed during indexing; retry required";
  }
  if (!out.parse_failed && !out.source_changed) {
    state.unit->commit();
  }
  return out;
}

} // namespace cidx::ast
