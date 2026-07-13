#include "ast/lt_engine.hpp"

#include "ast/body_pass_visitor.hpp"
#include "ast/edge_visitor.hpp"
#include "ast/location.hpp"
#include "ast/ns_uses_visitor.hpp"
#include "ast/storage_edge_sink.hpp"
#include "ast/storage_symbol_sink.hpp"
#include "ast/symbol_visitor.hpp"

#include "clangx/toolchain.hpp"
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

namespace cidx::lt {

namespace {

std::optional<double> file_mtime(const std::string &path) {
  struct stat st{};
  if (::stat(path.c_str(), &st) != 0)
    return std::nullopt;
#ifdef __APPLE__
  return static_cast<double>(st.st_mtimespec.tv_sec) +
         static_cast<double>(st.st_mtimespec.tv_nsec) * 1e-9;
#else
  return static_cast<double>(st.st_mtim.tv_sec) +
         static_cast<double>(st.st_mtim.tv_nsec) * 1e-9;
#endif
}

// One transitive inclusion (clang_getInclusions parity): recorded in
// include-directive order, depth > 0 only.
struct EngineState {
  cidx::Storage *db = nullptr;
  const cidx::File *rec = nullptr;
  std::string path; // main file (canonical absolute)
  bool graph_enabled = true;
  bool strict = false; // CIDX_STRICT: abort on Error, not just Fatal
  IndexOneOutcome *out = nullptr;
  std::vector<std::string> inclusions; // canonical absolute header paths
  bool tu_handled = false;
};

class IncludeRecorder : public clang::PPCallbacks {
public:
  IncludeRecorder(EngineState &state, clang::SourceManager &sm)
      : state_(state), sm_(sm) {}

  void InclusionDirective(clang::SourceLocation, const clang::Token &,
                          llvm::StringRef, bool, clang::CharSourceRange,
                          clang::OptionalFileEntryRef file, llvm::StringRef,
                          llvm::StringRef, const clang::Module *, bool,
                          clang::SrcMgr::CharacteristicKind) override {
    if (!file)
      return;
    // clang_getFileName parity: the path AS OPENED (search dir + spelling),
    // never symlink-resolved (/var vs /private/var matters for ownership).
    const std::string p = file->getName().str();
    if (!p.empty())
      state_.inclusions.push_back(p);
  }

private:
  EngineState &state_;
  clang::SourceManager &sm_;
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
    if (level < clang::DiagnosticsEngine::Warning)
      return; // cidx stores warnings and above (collect_diagnostics)
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

class EngineConsumer : public clang::ASTConsumer {
public:
  explicit EngineConsumer(EngineState &state) : state_(state) {}

  void HandleTranslationUnit(clang::ASTContext &context) override {
    state_.tu_handled = true;
    // Parity with classic parse(): a diagnostic at/above the abort level
    // makes the file fail with NO rows written (the classic path throws
    // ClangParseError before index_symbols). Gate before touching the DB.
    const clang::DiagnosticsEngine &de = context.getDiagnostics();
    if (de.hasFatalErrorOccurred() ||
        (state_.strict && de.getClient()->getNumErrors() > 0)) {
      return;
    }
    cidx::Storage &db = *state_.db;
    StorageSymbolSink symbols(db);
    StorageEdgeSink edges(db);
    clang::Decl *tu = context.getTranslationUnitDecl();

    const auto run_symbols = [&](const std::string &file, int64_t file_id) {
      symbols.set_current_file_id(file_id);
      symbols.reset_counters();
      auto txn = db.transaction();
      SymbolVisitor visitor(context, symbols, file);
      visitor.TraverseDecl(tu);
      txn.commit();
      return symbols.stored_count();
    };
    const auto run_edges = [&](const std::string &file, int64_t file_id) {
      if (!state_.graph_enabled)
        return;
      edges.delete_edges_for_file(file_id);
      edges.delete_definitions_for_file(file_id);
      auto txn = db.transaction();
      EdgeVisitor decls(context, edges, file, file_id);
      decls.TraverseDecl(tu);
      BodyPassVisitor bodies(context, edges, file, file_id);
      bodies.TraverseDecl(tu);
      NsUsesVisitor ns(context, edges, file, file_id);
      ns.TraverseDecl(tu);
      txn.commit();
    };

    // 1. symbols(main).
    state_.out->stored = run_symbols(state_.path, state_.rec->id);

    // 2. header two-pass (index_headers): pass 1 mints symbols for every
    //    not-yet-indexed OWNED non-system header, pass 2 extracts edges.
    const clang::SourceManager &sm = context.getSourceManager();
    struct Pending {
      std::string path;
      int64_t file_id;
      std::optional<double> mtime;
      int stored;
    };
    std::vector<Pending> pending;
    std::unordered_set<std::string> seen;
    cidx::HeaderStats &counts = state_.out->headers;
    for (const std::string &inc : state_.inclusions) {
      const std::string abs = cidx::pathutil::abspath(inc);
      if (!seen.insert(abs).second)
        continue;
      // System check: characteristic of the header's own content (parity
      // with clang_Location_isInSystemHeader at (file,1,1)).
      bool is_system = false;
      if (auto fe = sm.getFileManager().getFileRef(inc)) {
        const clang::FileID fid = sm.translateFile(*fe);
        if (fid.isValid())
          is_system = sm.getFileCharacteristic(
                          sm.getLocForStartOfFile(fid)) !=
                      clang::SrcMgr::C_User;
      }
      if (is_system) {
        ++counts.system;
        continue;
      }
      if (!db.component_for_path(abs)) {
        ++counts.unowned;
        continue;
      }
      const std::optional<std::string> md5 = cidx::md5_of(abs);
      if (db.is_file_indexed(abs, std::nullopt, md5)) {
        ++counts.already;
        continue;
      }
      const std::optional<double> mtime = file_mtime(abs);
      const int64_t hid = db.add_file_path(abs, mtime, md5,
                                           state_.rec->compile_options,
                                           state_.rec->driver);
      const int stored = run_symbols(abs, hid);
      pending.push_back({abs, hid, mtime, stored});
    }
    for (const Pending &ph : pending) {
      run_edges(ph.path, ph.file_id);
      db.mark_file_indexed(ph.file_id, ph.mtime);
      ++counts.indexed;
      counts.symbols += ph.stored;
    }

    // 3. edges(main) LAST (commands.cpp ordering).
    run_edges(state_.path, state_.rec->id);
  }

private:
  EngineState &state_;
};

class EngineAction : public clang::ASTFrontendAction {
public:
  explicit EngineAction(EngineState &state) : state_(state) {}

  bool BeginSourceFileAction(clang::CompilerInstance &ci) override {
    ci.getPreprocessor().addPPCallbacks(
        std::make_unique<IncludeRecorder>(state_, ci.getSourceManager()));
    return true;
  }

  std::unique_ptr<clang::ASTConsumer>
  CreateASTConsumer(clang::CompilerInstance & /*ci*/,
                    llvm::StringRef /*file*/) override {
    return std::make_unique<EngineConsumer>(state_);
  }

private:
  EngineState &state_;
};

class EngineActionFactory : public clang::tooling::FrontendActionFactory {
public:
  explicit EngineActionFactory(EngineState &state) : state_(state) {}
  std::unique_ptr<clang::FrontendAction> create() override {
    return std::make_unique<EngineAction>(state_);
  }

private:
  EngineState &state_;
};

} // namespace

IndexOneOutcome run_index_one(cidx::Storage &db, const cidx::File &rec,
                              const std::string &path, bool graph_enabled) {
  IndexOneOutcome out;

  // Flag assembly identical to Parser::parse: re-sanitized stored options,
  // <label>/$VAR resolution, toolchain search paths, -ferror-limit=0.
  const std::vector<std::string> opts = cidx::CompileDb::resolve_options(
      cidx::CompileDb::sanitize(rec.compile_options
                                    ? *rec.compile_options
                                    : std::vector<std::string>{}),
      [&db](const std::string &n) { return db.get_alias(n); });
  cidx::Toolchain toolchain;
  const bool cpp = cidx::Toolchain::is_cpp(path, opts);
  std::vector<std::string> args = opts;
  for (std::string &f : toolchain.toolchain_flags(cpp, rec.driver))
    args.push_back(std::move(f));
  args.push_back("-ferror-limit=0");

  clang::tooling::FixedCompilationDatabase cdb(".", args);
  clang::tooling::ClangTool tool(cdb, {path});
#ifdef CIDX_LT_RESOURCE_DIR
  tool.appendArgumentsAdjuster(clang::tooling::getInsertArgumentAdjuster(
      {"-resource-dir", CIDX_LT_RESOURCE_DIR},
      clang::tooling::ArgumentInsertPosition::BEGIN));
#endif
  DiagCollector collector(out.diagnostics);
  tool.setDiagnosticConsumer(&collector);

  const std::string strict_raw = [] {
    auto v = cidx::get_env("CIDX_STRICT").value_or("");
    for (char &c : v) c = static_cast<char>(std::tolower(c));
    return v;
  }();
  const bool strict = !(strict_raw.empty() || strict_raw == "0" ||
                        strict_raw == "off" || strict_raw == "none" ||
                        strict_raw == "false");

  EngineState state;
  state.db = &db;
  state.rec = &rec;
  state.path = path;
  state.graph_enabled = graph_enabled;
  state.strict = strict;
  state.out = &out;

  EngineActionFactory factory(state);
  const int rc = tool.run(&factory);
  if (!state.tu_handled) {
    out.parse_failed = true;
    out.error = "cannot parse " + path;
    return out;
  }
  // apply_diagnostic_policy parity: diagnostics at/above the abort level
  // (CIDX_STRICT: default Fatal, strict Error) fail the TU with the
  // "<path>: N fatal diagnostic(s): file:line: msg[; ...]" summary (first 3).
  {
    const int64_t level = strict ? 3 : 4;
    std::size_t fatal_count = 0;
    std::vector<std::string> summary;
    for (const cidx::Diagnostic &d : out.diagnostics) {
      if (d.severity >= level) {
        ++fatal_count;
        if (summary.size() < 3)
          summary.push_back(d.file_path.value_or("") + ":" +
                            std::to_string(d.line.value_or(0)) + ": " +
                            d.spelling);
      }
    }
    if (fatal_count > 0) {
      out.parse_failed = true;
      std::string joined;
      for (std::size_t i = 0; i < summary.size(); ++i) {
        if (i != 0)
          joined += "; ";
        joined += summary[i];
      }
      out.error = path + ": " + std::to_string(fatal_count) +
                  " fatal diagnostic(s): " + joined;
      out.failed_flags = args;
    }
  }
  (void)rc;
  return out;
}

} // namespace cidx::lt
