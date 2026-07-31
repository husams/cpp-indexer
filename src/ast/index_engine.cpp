#include "ast/index_engine.hpp"

#include "ast/declaration_edge_visitor.hpp"
#include "ast/display_name_rewrite.hpp"
#include "ast/function_definition_visitor.hpp"
#include "ast/include_facts.hpp"
#include "ast/location.hpp"
#include "ast/namespace_use_visitor.hpp"
#include "ast/pass_registry.hpp"
#include "ast/routed_root_events.hpp"
#include "ast/storage_edge_sink.hpp"
#include "ast/storage_symbol_sink.hpp"
#include "ast/symbol_visitor.hpp"

#include "catalogs/generated_catalog.hpp"
#include "compiledb/compiledb.hpp"
#include "profile/index_profile.hpp"
#include "storage/ports.hpp"
#include "storage/sqlite_adapters.hpp"
#include "storage/storage.hpp"
#include "toolchain/toolchain.hpp"
#include "util/env.hpp"
#include "util/files.hpp"
#include "util/hashing.hpp"
#include "util/pathutil.hpp"
#include "workspace/context.hpp"

#include "clang/AST/ASTConsumer.h"
#include "clang/AST/ASTContext.h"
#include "clang/Analysis/CFG.h"
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
#include <chrono>
#include <ctime>
#include <functional>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

namespace cidx::ast {

namespace {

using ProfileClock = std::chrono::steady_clock;

auto elapsed_seconds(ProfileClock::time_point started) -> double {
  return std::chrono::duration<double>(ProfileClock::now() - started).count();
}

auto file_bytes(const std::string &path) -> std::uint64_t {
  struct stat status{};
  if (::stat(path.c_str(), &status) != 0 || status.st_size < 0) {
    return 0;
  }
  return static_cast<std::uint64_t>(status.st_size);
}

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

class ComponentOwnershipIndex {
public:
  void rebuild(cidx::Storage &db, const WorkspaceSnapshot &snapshot,
               IndexSessionMetrics &metrics) {
    metrics.cache_evictions += cache_.size();
    cache_.clear();
    roots_.clear();
    roots_.reserve(snapshot.components.size());
    for (const Component &component : snapshot.components) {
      std::string root = pathutil::normpath(db.component_abs_base(component));
      while (root.size() > 1U && root.ends_with('/')) {
        root.pop_back();
      }
      const auto position =
          std::ranges::find_if(roots_, [&root](const std::string &existing) {
            return root.size() > existing.size() ||
                   (root.size() == existing.size() && root > existing);
          });
      roots_.insert(position, std::move(root));
    }
    ++metrics.component_scans;
  }

  [[nodiscard]] bool owns(const std::string &path,
                          IndexSessionMetrics &metrics) {
    const std::string absolute = pathutil::normpath(pathutil::abspath(path));
    if (const auto found = cache_.find(absolute); found != cache_.end()) {
      ++metrics.ownership_hits;
      return found->second;
    }
    ++metrics.ownership_misses;
    const bool owned =
        std::ranges::any_of(roots_, [&absolute](const auto &root) {
          return absolute == root || absolute.starts_with(root + "/");
        });
    cache_.emplace(absolute, owned);
    return owned;
  }

private:
  std::vector<std::string> roots_;
  std::unordered_map<std::string, bool> cache_;
};

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
  std::vector<PresentationIntent> presentation_intents;
  const cidx::IncludeConfig *config = nullptr; // v31: this TU's normalized args
  int64_t normalized_config_id = -1;
  ComponentOwnershipIndex *ownership = nullptr;
  IndexSessionMetrics *session_metrics = nullptr;
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
        symbols_(*state.ports),
        edges_(*state.ports, &state.out->evidence, &state.presentation_intents),
        tu_(context.getTranslationUnitDecl()) {}

  void run() {
    db_.delete_symbols_for_file(state_.rec->id);
    ExtractionPassRegistry registry;
    const auto descriptor =
        [](std::string id, std::vector<FrontendCapability> capabilities,
           std::vector<std::string> consumed, std::vector<std::string> produced,
           std::vector<std::string> dependencies, PassScope scope,
           TraversalMode traversal,
           FactCompleteness completeness = FactCompleteness::complete,
           FactTrust trust = FactTrust::trusted,
           std::size_t max_whole_tu_traversals =
               0) -> ExtractionPassDescriptor {
      return ExtractionPassDescriptor{
          .id = std::move(id),
          .version = 1,
          .required_capabilities = std::move(capabilities),
          .consumed_fact_families = std::move(consumed),
          .produced_fact_families = std::move(produced),
          .catalog_versions = {catalog::kCatalogVersion},
          .dependencies = std::move(dependencies),
          .scope = scope,
          .traversal = traversal,
          .completeness = completeness,
          .trust = trust,
          .budget = {.max_visited_constructs = 10'000'000,
                     .max_emitted_facts = 20'000'000,
                     .max_diagnostics = 1'024,
                     .max_whole_tu_traversals = max_whole_tu_traversals,
                     .declared = true}};
    };
    registry.register_pass(
        descriptor("symbols.main", {FrontendCapability::ast}, {}, {"symbols"},
                   {}, PassScope::main_file, TraversalMode::declaration),
        [this](PassExecutionContext &execution) -> void {
          for (const cidx::Diagnostic &diagnostic : state_.out->diagnostics) {
            execution.metrics.note_diagnostic(diagnostic.spelling);
          }
          prepare_routed_files();
        });
    registry.register_pass(
        descriptor("symbols.headers",
                   {FrontendCapability::ast, FrontendCapability::preprocessor},
                   {"includes"}, {"symbols"}, {"symbols.main"},
                   PassScope::owned_header, TraversalMode::declaration,
                   FactCompleteness::complete, FactTrust::trusted, 1),
        [this](PassExecutionContext &execution) -> void {
          run_routed_symbol_pass(execution);
          state_.out->stored = symbols_.stored_count(state_.rec->id);
          main_symbol_ids_ = symbols_.symbol_ids(state_.rec->id);
          for (PendingHeader &header : pending_headers_) {
            header.stored = symbols_.stored_count(header.file_id);
            header.symbol_ids = symbols_.symbol_ids(header.file_id);
          }
        });
    registry.register_pass(
        descriptor("lifecycle.headers", {}, {"symbols"}, {"fact_lifecycle"},
                   {"symbols.headers"}, PassScope::owned_header,
                   TraversalMode::lifecycle),
        [this](PassExecutionContext &execution) -> void {
          if (state_.failure_injector != nullptr) {
            state_.failure_injector->inject(
                cidx::storage::FailurePoint::partial_transform);
          }
          for (const PendingHeader &header : pending_headers_) {
            configure_fact_file(header.file_id, true);
            execution.metrics.note_visited();
          }
        });
    registry.register_pass(
        descriptor("graph.headers", {FrontendCapability::ast},
                   {"symbols", "fact_lifecycle"}, {"root_events"},
                   {"symbols.headers", "lifecycle.headers", "lifecycle.main"},
                   PassScope::owned_header, TraversalMode::declaration,
                   FactCompleteness::complete, FactTrust::trusted, 1),
        [this](PassExecutionContext &execution) -> void {
          run_routed_graph_stage(execution);
        });
    registry.register_pass(
        descriptor(
            "declarations.headers", {FrontendCapability::ast},
            {"symbols", "fact_lifecycle", "root_events"},
            {"relations", "types", "definitions", "presentation_intents"},
            {"graph.headers"}, PassScope::owned_header,
            TraversalMode::declaration),
        [this](PassExecutionContext &execution) -> void {
          run_routed_declaration_stage(execution);
          for (PendingHeader &header : pending_headers_) {
            header.edge_ids = edges_.edge_ids(header.file_id);
            header.definition_ids = edges_.definition_ids(header.file_id);
          }
          main_edge_ids_ = edges_.edge_ids(state_.rec->id);
          main_definition_ids_ = edges_.definition_ids(state_.rec->id);
        });
    registry.register_pass(
        descriptor("definitions.headers", {FrontendCapability::ast},
                   {"symbols"}, {"definitions"}, {"declarations.headers"},
                   PassScope::owned_header, TraversalMode::declaration),
        [this](PassExecutionContext &execution) -> void {
          routed_definition_visitor_ = collect_routed_definitions(execution);
          for (PendingHeader &header : pending_headers_) {
            header.edge_ids = edges_.edge_ids(header.file_id);
            header.definition_ids = edges_.definition_ids(header.file_id);
          }
          main_edge_ids_ = edges_.edge_ids(state_.rec->id);
          main_definition_ids_ = edges_.definition_ids(state_.rec->id);
        });
    registry.register_pass(
        descriptor("statements.headers",
                   {FrontendCapability::ast, FrontendCapability::templates},
                   {"definitions", "relations", "types"},
                   {"relations", "types", "evidence", "definitions", "symbols"},
                   {"definitions.headers"}, PassScope::owned_header,
                   TraversalMode::body, FactCompleteness::partial,
                   FactTrust::inferred),
        [](PassExecutionContext & /*execution*/) -> void {});
    registry.register_pass(
        descriptor("namespaces.headers", {FrontendCapability::ast},
                   {"symbols", "relations"}, {"relations"},
                   {"statements.headers"}, PassScope::owned_header,
                   TraversalMode::declaration),
        [this](PassExecutionContext &execution) -> void {
          run_routed_namespace_stage(execution);
          for (PendingHeader &header : pending_headers_) {
            header.edge_ids = edges_.edge_ids(header.file_id);
            header.definition_ids = edges_.definition_ids(header.file_id);
          }
          main_edge_ids_ = edges_.edge_ids(state_.rec->id);
          main_definition_ids_ = edges_.definition_ids(state_.rec->id);
        });
    auto header_association = descriptor(
        "headers.associate", {}, {"symbols", "relations", "definitions"},
        {"file_associations"},
        {"symbols.headers", "statements.headers", "statements.main",
         "namespaces.headers"},
        PassScope::owned_header, TraversalMode::lifecycle);
    registry.register_pass(
        std::move(header_association),
        [this](PassExecutionContext &execution) -> void {
          for (PendingHeader &header : pending_headers_) {
            if (!SourceSnapshot{.md5 = header.md5}.matches(header.path)) {
              state_.out->source_changed = true;
              db_.set_file_indexed(header.file_id, false);
              continue;
            }
            const bool profiling = profile::active();
            const auto metrics_started =
                profiling ? ProfileClock::now() : ProfileClock::time_point{};
            const auto fact_count = db_.association_fact_count(
                header.file_id, header.symbol_ids, header.edge_ids,
                header.definition_ids);
            if (profiling) {
              profile::add_timing("metrics_only_sql",
                                  elapsed_seconds(metrics_started));
              profile::add_counter("association_fact_count", fact_count);
            }
            execution.metrics.note_emitted(fact_count);
            db_.associate_facts_for_file(
                header.file_id, state_.normalized_config_id, header.symbol_ids,
                header.edge_ids, header.definition_ids);
            execution.metrics.note_fact_family("file_associations", fact_count,
                                               fact_count);
            db_.mark_file_indexed(header.file_id, header.mtime, header.md5);
            ++state_.out->headers.indexed;
            state_.out->headers.symbols += header.stored;
          }
        });
    registry.register_pass(descriptor("lifecycle.main", {}, {},
                                      {"fact_lifecycle"}, {"lifecycle.headers"},
                                      PassScope::main_file,
                                      TraversalMode::lifecycle),
                           [this](PassExecutionContext &execution) -> void {
                             configure_fact_file(state_.rec->id, true);
                             execution.metrics.note_visited();
                           });
    registry.register_pass(
        descriptor(
            "declarations.main", {FrontendCapability::ast},
            {"symbols", "fact_lifecycle"},
            {"relations", "types", "definitions", "presentation_intents"},
            {"declarations.headers", "lifecycle.main"}, PassScope::main_file,
            TraversalMode::declaration),
        [](PassExecutionContext & /*execution*/) -> void {});
    registry.register_pass(
        descriptor("definitions.main", {FrontendCapability::ast}, {"symbols"},
                   {"definitions"}, {"declarations.main"}, PassScope::main_file,
                   TraversalMode::declaration),
        [](PassExecutionContext & /*execution*/) -> void {});
    registry.register_pass(
        descriptor("statements.main",
                   {FrontendCapability::ast, FrontendCapability::templates},
                   {"definitions", "relations", "types"},
                   {"relations", "types", "evidence", "definitions", "symbols"},
                   {"definitions.main"}, PassScope::main_file,
                   TraversalMode::body, FactCompleteness::partial,
                   FactTrust::inferred),
        [this](PassExecutionContext &execution) -> void {
          if (routed_definition_visitor_) {
            run_statement_stage(*routed_definition_visitor_, execution, nullptr,
                                nullptr);
          }
          for (PendingHeader &header : pending_headers_) {
            header.edge_ids = edges_.edge_ids(header.file_id);
            header.definition_ids = edges_.definition_ids(header.file_id);
          }
          main_edge_ids_ = edges_.edge_ids(state_.rec->id);
          main_definition_ids_ = edges_.definition_ids(state_.rec->id);
        });
    registry.register_pass(
        descriptor("namespaces.main", {FrontendCapability::ast},
                   {"symbols", "relations"}, {"relations"}, {"statements.main"},
                   PassScope::main_file, TraversalMode::declaration),
        [](PassExecutionContext & /*execution*/) -> void {});
    registry.register_pass(
        descriptor("presentation.persist", {}, {"presentation_intents"},
                   {"display_names"},
                   {"declarations.headers", "declarations.main"},
                   PassScope::translation_unit, TraversalMode::lifecycle),
        [this](PassExecutionContext &execution) -> void {
          std::ranges::sort(state_.presentation_intents, {},
                            [](const PresentationIntent &intent) -> auto {
                              return std::tie(intent.symbol_id,
                                              intent.display_args);
                            });
          const std::size_t before = state_.presentation_intents.size();
          const auto unique_end = std::ranges::unique(
              state_.presentation_intents,
              [](const PresentationIntent &left,
                 const PresentationIntent &right) -> bool {
                return left.symbol_id == right.symbol_id &&
                       left.display_args == right.display_args;
              });
          state_.presentation_intents.erase(unique_end.begin(),
                                            state_.presentation_intents.end());
          execution.metrics.note_duplicate(before -
                                           state_.presentation_intents.size());
          std::vector<std::pair<std::int64_t, std::string>> updates;
          updates.reserve(state_.presentation_intents.size());
          for (const PresentationIntent &intent : state_.presentation_intents) {
            const auto display = edges_.lookup_display_name(intent.symbol_id);
            if (display) {
              if (const auto rewritten = rewrite_template_display_name(
                      *display, intent.display_args)) {
                updates.emplace_back(intent.symbol_id, *rewritten);
              }
            }
          }
          execution.metrics.note_emitted(updates.size());
          for (const auto &[symbol_id, display] : updates) {
            edges_.update_display_name(symbol_id, display);
          }
          execution.metrics.note_fact_family(
              "display_names", before, updates.size(),
              before - state_.presentation_intents.size());
        });
    auto main_association = descriptor(
        "main.associate", {}, {"symbols", "relations", "definitions"},
        {"file_associations"},
        {"symbols.main", "lifecycle.main", "headers.associate",
         "statements.main", "namespaces.main"},
        PassScope::main_file, TraversalMode::lifecycle);
    registry.register_pass(
        std::move(main_association),
        [this](PassExecutionContext &execution) -> void {
          const bool profiling = profile::active();
          const auto metrics_started =
              profiling ? ProfileClock::now() : ProfileClock::time_point{};
          const auto fact_count =
              db_.association_fact_count(state_.rec->id, main_symbol_ids_,
                                         main_edge_ids_, main_definition_ids_);
          if (profiling) {
            profile::add_timing("metrics_only_sql",
                                elapsed_seconds(metrics_started));
            profile::add_counter("association_fact_count", fact_count);
          }
          execution.metrics.note_emitted(fact_count);
          db_.associate_facts_for_file(
              state_.rec->id, state_.normalized_config_id, main_symbol_ids_,
              main_edge_ids_, main_definition_ids_);
          execution.metrics.note_fact_family("file_associations", fact_count,
                                             fact_count);
        });
    registry.register_pass(
        descriptor("includes.persist", {FrontendCapability::preprocessor},
                   {"preprocessor_facts"}, {"include_facts"},
                   {"main.associate"}, PassScope::translation_unit,
                   TraversalMode::preprocessing),
        [this](PassExecutionContext &execution) -> void {
          if (state_.pp != nullptr) {
            resolve_include_guards(*state_.pp, state_.includes);
          }
          execution.metrics.note_visited(state_.includes.includes.size());
          const bool profiling = profile::active();
          const auto metrics_started =
              profiling ? ProfileClock::now() : ProfileClock::time_point{};
          const IncludeFactCounts counts =
              include_fact_count(db_, state_.includes);
          if (profiling) {
            profile::add_timing("metrics_only_sql",
                                elapsed_seconds(metrics_started));
            profile::add_counter("include_fact_count", counts.emitted_facts);
          }
          execution.metrics.note_duplicate(counts.duplicates);
          execution.metrics.note_emitted(counts.emitted_facts);
          persist_include_facts(db_, state_.includes, *state_.config);
          execution.metrics.note_fact_family(
              "include_facts", counts.emitted_facts + counts.duplicates,
              counts.emitted_facts, counts.duplicates);
        });
    registry.register_pass(
        descriptor(
            "evidence.persist", {}, {"evidence"}, {"evidence_artifact"},
            {"statements.headers", "statements.main", "includes.persist"},
            PassScope::translation_unit, TraversalMode::lifecycle,
            FactCompleteness::partial, FactTrust::inferred),
        [this](PassExecutionContext &execution) -> void {
          std::ranges::sort(state_.out->evidence, {},
                            [](const EvidenceRecord &record) -> auto {
                              return std::tie(record.producer, record.construct,
                                              record.file, record.line,
                                              record.col, record.completeness,
                                              record.trust, record.detail);
                            });
          const std::size_t before = state_.out->evidence.size();
          const auto unique_end = std::ranges::unique(
              state_.out->evidence,
              [](const EvidenceRecord &left,
                 const EvidenceRecord &right) -> bool {
                return std::tie(left.producer, left.construct, left.file,
                                left.line, left.col, left.completeness,
                                left.trust, left.detail) ==
                       std::tie(right.producer, right.construct, right.file,
                                right.line, right.col, right.completeness,
                                right.trust, right.detail);
              });
          state_.out->evidence.erase(unique_end.begin(),
                                     state_.out->evidence.end());
          execution.metrics.note_duplicate(before -
                                           state_.out->evidence.size());
          execution.metrics.note_emitted(state_.out->evidence.size());
          execution.metrics.note_fact_family(
              "evidence_artifact", before, state_.out->evidence.size(),
              before - state_.out->evidence.size());
        });

    IndexingPlan plan;
    plan.add("symbols.main");
    plan.add("symbols.headers");
    plan.add("lifecycle.headers");
    plan.add("lifecycle.main");
    plan.add("graph.headers");
    plan.add("declarations.headers");
    plan.add("definitions.headers");
    plan.add("statements.headers");
    plan.add("namespaces.headers");
    plan.add("declarations.main");
    plan.add("definitions.main");
    plan.add("statements.main");
    plan.add("namespaces.main");
    plan.add("headers.associate");
    plan.add("main.associate");
    plan.add("presentation.persist");
    plan.add("includes.persist");
    plan.add("evidence.persist");
    FrontendSession session{
        .ast_context = &context_,
        .preprocessor = state_.pp,
        .declaration_ports = &static_cast<DeclarationPassPorts &>(edges_),
        .statement_ports = &static_cast<StatementFactPorts &>(edges_),
        .namespace_ports = &static_cast<NamespacePassPorts &>(edges_),
        .definition_ports = &static_cast<DefinitionScopeEmitter &>(edges_),
        .evidence = &static_cast<EvidenceEmitter &>(edges_),
        .presentation_intents =
            &static_cast<PresentationIntentEmitter &>(edges_),
        .lifecycle = &static_cast<IndexingLifecycle &>(edges_),
        .cfg_builder = [this](const clang::FunctionDecl *function)
            -> std::unique_ptr<clang::CFG> {
          if (function == nullptr || function->getBody() == nullptr) {
            return nullptr;
          }
          clang::CFG::BuildOptions options;
          return clang::CFG::buildCFG(function, function->getBody(), &context_,
                                      options);
        },
        .template_arguments = [](const clang::FunctionDecl *function)
            -> const clang::TemplateArgumentList * {
          return function == nullptr
                     ? nullptr
                     : function->getTemplateSpecializationArgs();
        }};
    for (const FrontendPassProvider &provider : frontend_pass_providers()) {
      provider(session, registry, plan);
    }
    const PassExecutionReport report = registry.run(plan, &session);
    state_.out->pass_metrics.clear();
    state_.out->pass_metrics.reserve(report.passes.size());
    state_.out->registered_whole_tu_traversal_budget = 0;
    state_.out->observed_whole_tu_traversals = 0;
    for (const PassExecutionRecord &pass : report.passes) {
      state_.out->registered_whole_tu_traversal_budget +=
          pass.descriptor.budget.max_whole_tu_traversals;
      state_.out->observed_whole_tu_traversals +=
          pass.metrics.whole_tu_traversals;
      state_.out->pass_metrics.push_back(
          {.id = pass.descriptor.id,
           .required_capabilities = pass.descriptor.required_capabilities,
           .dependencies = pass.descriptor.dependencies,
           .consumed_fact_families = pass.descriptor.consumed_fact_families,
           .produced_fact_families = pass.descriptor.produced_fact_families,
           .completeness = pass.descriptor.completeness,
           .trust = pass.descriptor.trust,
           .visited_constructs = pass.metrics.visited_constructs,
           .emitted_facts = pass.metrics.emitted_facts,
           .unknown_constructs = pass.metrics.unknown_constructs,
           .duplicates = pass.metrics.duplicates,
           .diagnostics = pass.metrics.diagnostics,
           .registered_whole_tu_traversal_budget =
               pass.descriptor.budget.max_whole_tu_traversals,
           .whole_tu_traversals = pass.metrics.whole_tu_traversals,
           .fact_families = pass.metrics.fact_families,
           .elapsed_microseconds = pass.metrics.elapsed.count(),
           .budget_exhausted = pass.metrics.budget_exhausted});
    }
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
    std::vector<int64_t> edge_ids;
    std::vector<int64_t> definition_ids;
  };

  void run_routed_graph_stage(PassExecutionContext &execution) {
    if (!state_.graph_enabled) {
      return;
    }
    routed_root_events_.emplace(10'000'000, &execution.metrics);
    execution.metrics.note_whole_tu_traversal();
    profile::add_counter("root_traverse_decl_calls");
    routed_root_events_->collect(tu_);
  }

  void run_routed_symbol_pass(PassExecutionContext &execution) {
    routed_symbol_file_id_ = -1;
    symbols_.set_identity_translation_unit_config_id(
        state_.normalized_config_id, state_.rec->id);
    symbols_.reset_all_counters();
    symbols_.set_metrics(&execution.metrics);
    SymbolVisitor visitor(
        context_, symbols_, {}, &execution.metrics,
        [this](const std::string &path) { return route_symbol_file(path); });
    execution.metrics.note_whole_tu_traversal();
    profile::add_counter("root_traverse_decl_calls");
    visitor.TraverseDecl(tu_);
  }

  void prepare_routed_files() {
    pending_headers_ = plan_owned_headers();
    routed_file_ids_.clear();
    routed_file_ids_.emplace(state_.path, state_.rec->id);
    for (const PendingHeader &header : pending_headers_) {
      routed_file_ids_.emplace(header.path, header.file_id);
      if (header.covered_by_current_config) {
        db_.delete_symbols_for_file(header.file_id);
      }
    }
  }

  bool route_symbol_file(const std::string &path) {
    const auto file = routed_file_ids_.find(path);
    if (file == routed_file_ids_.end()) {
      return false;
    }
    if (routed_symbol_file_id_ != file->second) {
      symbols_.set_current_file_id(file->second);
      routed_symbol_file_id_ = file->second;
    }
    return true;
  }

  auto route_fact_file(const std::string &path) -> std::optional<int64_t> {
    const auto file = routed_file_ids_.find(path);
    if (file == routed_file_ids_.end()) {
      return std::nullopt;
    }
    if (routed_fact_file_id_ != file->second) {
      edges_.set_current_file_id(file->second);
      routed_fact_file_id_ = file->second;
    }
    return file->second;
  }

  void configure_fact_file(int64_t file_id, bool reset) {
    edges_.set_current_file_id(file_id);
    edges_.set_identity_translation_unit_config_id(state_.normalized_config_id,
                                                   state_.rec->id);
    if (reset) {
      edges_.delete_edges_for_file(file_id);
      edges_.delete_definitions_for_file(file_id);
      edges_.reset_fact_ids();
    }
  }

  static auto append_fact_ids(std::vector<int64_t> &destination,
                              const std::vector<int64_t> &source) -> void {
    for (const int64_t id : source) {
      if (std::ranges::find(destination, id) == destination.end()) {
        destination.push_back(id);
      }
    }
  }

  void run_routed_declaration_stage(PassExecutionContext &execution) {
    if (!state_.graph_enabled) {
      return;
    }
    edges_.reset_all_fact_ids();
    routed_fact_file_id_ = -1;
    BudgetedDeclarationPassPorts ports(
        static_cast<DeclarationPassPorts &>(edges_), execution.metrics);
    BudgetedPresentationIntentEmitter presentation_intents(
        static_cast<PresentationIntentEmitter &>(edges_), execution.metrics);
    BudgetedDefinitionScopeEmitter definitions(
        static_cast<DefinitionScopeEmitter &>(edges_), execution.metrics);
    DeclarationEdgeVisitor decls(
        context_, ports, {}, -1, &definitions, &execution.metrics,
        &presentation_intents,
        [this](const std::string &path) { return route_fact_file(path); });
    if (routed_root_events_) {
      routed_root_events_->replay_declarations(decls);
    }
  }

  auto collect_routed_definitions(PassExecutionContext &execution)
      -> std::unique_ptr<FunctionDefinitionVisitor> {
    if (!state_.graph_enabled) {
      return nullptr;
    }
    edges_.reset_fact_ids();
    routed_fact_file_id_ = -1;
    auto visitor = std::make_unique<FunctionDefinitionVisitor>(
        context_, static_cast<DeclarationIdentityResolver &>(edges_),
        static_cast<DefinitionScopeEmitter &>(edges_), std::string{}, -1,
        &execution.metrics,
        [this](const std::string &path) { return route_fact_file(path); });
    if (routed_root_events_) {
      routed_root_events_->replay_definitions(*visitor);
    }
    return visitor;
  }

  void run_statement_stage(FunctionDefinitionVisitor &visitor,
                           PassExecutionContext &execution,
                           std::vector<int64_t> *edge_ids,
                           std::vector<int64_t> *definition_ids) {
    if (!state_.graph_enabled) {
      return;
    }
    configure_fact_file(visitor.file_id(), false);
    edges_.reset_fact_ids();
    routed_fact_file_id_ = -1;
    BudgetedStatementFactPorts ports(static_cast<StatementFactPorts &>(edges_),
                                     execution.metrics);
    BudgetedDefinitionScopeEmitter definitions(
        static_cast<DefinitionScopeEmitter &>(edges_), execution.metrics);
    visitor.run_statement_pass(ports, &execution.metrics, &definitions);
    if (edge_ids != nullptr) {
      append_fact_ids(*edge_ids, edges_.edge_ids());
    }
    if (definition_ids != nullptr) {
      append_fact_ids(*definition_ids, edges_.definition_ids());
    }
  }

  void run_routed_namespace_stage(PassExecutionContext &execution) {
    if (!state_.graph_enabled) {
      return;
    }
    edges_.reset_fact_ids();
    BudgetedNamespacePassPorts ports(static_cast<NamespacePassPorts &>(edges_),
                                     execution.metrics);
    NamespaceUseVisitor ns(
        context_, ports, {}, -1, &execution.metrics,
        [this](const std::string &path) { return route_fact_file(path); });
    if (routed_root_events_) {
      routed_root_events_->replay_namespaces(ns);
    }
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
      if (state_.ownership == nullptr || state_.session_metrics == nullptr ||
          !state_.ownership->owns(abs, *state_.session_metrics)) {
        ++counts.unowned;
        continue;
      }
      ++state_.session_metrics->file_hash_reads;
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
      ++state_.session_metrics->file_stat_reads;
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
  std::vector<PendingHeader> pending_headers_;
  std::unordered_map<std::string, int64_t> routed_file_ids_;
  int64_t routed_symbol_file_id_ = -1;
  int64_t routed_fact_file_id_ = -1;
  std::vector<int64_t> main_symbol_ids_;
  std::vector<int64_t> main_edge_ids_;
  std::vector<int64_t> main_definition_ids_;
  std::optional<RoutedRootEventBuffer> routed_root_events_;
  std::unique_ptr<FunctionDefinitionVisitor> routed_definition_visitor_;
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
  return {.mtime = file_mtime(path), .md5 = cidx::md5_of(path)};
}

bool SourceSnapshot::matches(const std::string &path) const {
  const std::optional<double> current_mtime = file_mtime(path);
  const std::optional<std::string> current = cidx::md5_of(path);
  const bool mtime_matches = !mtime.has_value() || (current_mtime.has_value() &&
                                                    mtime == current_mtime);
  return mtime_matches && md5.has_value() && current.has_value() &&
         md5 == current;
}

class PipelineFailureInjector final : public cidx::storage::FailureInjector {
public:
  explicit PipelineFailureInjector(IndexFailurePoint target)
      : target_(target) {}

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
      throw std::runtime_error(std::string("injected ") +
                               failure_name(current) + " failure");
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

std::int64_t database_data_version(cidx::Storage &db) {
  return db.data_version();
}

std::string repository_fingerprint(const WorkspaceSnapshot &snapshot) {
  std::string data;
  for (const Repository &repository : snapshot.repositories) {
    data += repository.name + '\0' + repository.kind + '\0' +
            repository.remote_url.value_or("") + '\0';
  }
  return sha256_hex(data);
}

std::string clone_fingerprint(const WorkspaceSnapshot &snapshot) {
  std::string data;
  for (const Clone &clone : snapshot.active_clones) {
    data += std::to_string(clone.repository_id) + '\0' + clone.path + '\0' +
            clone.label.value_or("") + '\0';
  }
  return sha256_hex(data);
}

std::string component_fingerprint(const WorkspaceSnapshot &snapshot) {
  std::string data;
  for (const Component &component : snapshot.components) {
    data += component.name + '\0' + component.path + '\0' + component.kind +
            '\0' + component.version.value_or("") + '\0' +
            std::to_string(component.repository_id.value_or(-1)) + '\0';
  }
  return sha256_hex(data);
}

std::string file_configuration_fingerprint(cidx::Storage &db,
                                           std::int64_t file_id) {
  const auto file = db.get_file_by_id(file_id);
  if (!file) {
    return "<unregistered>";
  }
  std::string data = file->driver.value_or("") + '\0';
  for (const std::string &option :
       file->compile_options.value_or(std::vector<std::string>{})) {
    data += option + '\0';
  }
  return sha256_hex(data);
}

TranslationUnitConfig
configuration_without_source(const TranslationUnitDescriptor &descriptor) {
  const TranslationUnitConfig &input = descriptor.configuration;
  TranslationUnitConfig configuration = resolve_translation_unit_config(
      input.driver, input.working_dir, input.arguments, input.language,
      input.resource_dir, input.diagnostics_policy);
  configuration.standard = input.standard;
  configuration.target = input.target;
  configuration.abi_options = input.abi_options;
  configuration.sysroot = input.sysroot;
  configuration.include_paths = input.include_paths;
  configuration.macro_state = input.macro_state;
  configuration.relevant_environment = input.relevant_environment;
  configuration.generated_inputs = input.generated_inputs;
  configuration.state = input.state;
  const std::string source =
      pathutil::normpath(pathutil::abspath(descriptor.source_identity));
  std::erase_if(
      configuration.arguments, [&source](const std::string &argument) {
        return pathutil::normpath(pathutil::abspath(argument)) == source;
      });
  return configuration;
}

class IndexSession::Impl {
public:
  Impl(cidx::Storage &db, cidx::Logger &log)
      : db_(db), workspace_data_(db), toolchain_(log) {
    rebuild(IndexInvalidationReason::manual, false);
  }

  void rebuild(IndexInvalidationReason reason, bool count_invalidation = true) {
    const std::size_t evictions =
        descriptor_cache_.size() + configuration_id_cache_.size() +
        source_hashes_.size() + generated_input_hashes_.size() +
        file_configuration_hashes_.size();
    metrics_.cache_evictions += evictions;
    descriptor_cache_.clear();
    configuration_id_cache_.clear();
    source_hashes_.clear();
    generated_input_hashes_.clear();
    file_configuration_hashes_.clear();
    context_ = std::make_unique<WorkspaceContext>(WorkspaceContext::borrow(
        workspace_data_, WorkspaceReadWriteMode::read_write));
    resolver_ = std::make_unique<TranslationUnitConfigurationService>(
        *context_, toolchain_);
    ownership_.rebuild(db_, context_->snapshot(), metrics_);
    data_version_ = database_data_version(db_);
    ++metrics_.generation;
    ++metrics_.snapshot_rebuilds;
    if (count_invalidation) {
      note_invalidation(reason);
    }
  }

  void refresh_external_state(const std::string &path, std::int64_t file_id) {
    const std::int64_t current_version = database_data_version(db_);
    if (current_version == data_version_) {
      return;
    }
    const std::string key = pathutil::normpath(pathutil::abspath(path));
    const std::string current_configuration =
        file_configuration_fingerprint(db_, file_id);
    const bool configuration_changed =
        file_configuration_hashes_.contains(key) &&
        file_configuration_hashes_.at(key) != current_configuration;
    const WorkspaceSnapshot before = context_->snapshot();
    const std::string repositories = repository_fingerprint(before);
    const std::string clones = clone_fingerprint(before);
    const std::string components = component_fingerprint(before);
    rebuild(IndexInvalidationReason::compile_option_update);
    const WorkspaceSnapshot &after = context_->snapshot();
    if (configuration_changed) {
      file_configuration_hashes_.insert_or_assign(key, current_configuration);
    } else if (repository_fingerprint(after) != repositories) {
      replace_last_invalidation(IndexInvalidationReason::repository_switch);
    } else if (clone_fingerprint(after) != clones) {
      replace_last_invalidation(IndexInvalidationReason::clone_change);
    } else if (component_fingerprint(after) != components) {
      replace_last_invalidation(IndexInvalidationReason::component_update);
    }
  }

  void observe_source(const std::string &path,
                      const std::optional<std::string> &hash) {
    const std::string key = pathutil::normpath(pathutil::abspath(path));
    if (const auto found = source_hashes_.find(key);
        found != source_hashes_.end() && found->second != hash) {
      rebuild(IndexInvalidationReason::source_mutation);
    }
    source_hashes_.insert_or_assign(key, hash);
  }

  const TranslationUnitDescriptor &
  prepared_descriptor(const std::string &path, std::int64_t file_id,
                      const std::optional<std::string> &source_hash) {
    refresh_external_state(path, file_id);
    observe_source(path, source_hash);
    const TranslationUnitDescriptor *resolved = &descriptor(path);
    const std::string key = pathutil::normpath(pathutil::abspath(path));
    file_configuration_hashes_.insert_or_assign(
        key, file_configuration_fingerprint(db_, file_id));
    const std::string input_hash = generated_input_fingerprint(*resolved);
    if (const auto found =
            generated_input_hashes_.find(resolved->semantic_hash);
        found != generated_input_hashes_.end() && found->second != input_hash) {
      rebuild(IndexInvalidationReason::generated_input_change);
      observe_source(path, source_hash);
      resolved = &descriptor(path);
    }
    generated_input_hashes_.insert_or_assign(resolved->semantic_hash,
                                             input_hash);
    file_configuration_hashes_.insert_or_assign(
        key, file_configuration_fingerprint(db_, file_id));
    return *resolved;
  }

  const TranslationUnitDescriptor &descriptor(const std::string &path) {
    const std::string key = pathutil::normpath(pathutil::abspath(path));
    if (const auto found = descriptor_cache_.find(key);
        found != descriptor_cache_.end()) {
      ++metrics_.descriptor_hits;
      return found->second;
    }
    ++metrics_.descriptor_misses;
    return descriptor_cache_.emplace(key, resolver_->resolve(key))
        .first->second;
  }

  int64_t configuration_id(const TranslationUnitDescriptor &descriptor) {
    const TranslationUnitConfig configuration =
        configuration_without_source(descriptor);
    const std::string configuration_hash =
        translation_unit_config_hash(configuration);
    if (const auto found = configuration_id_cache_.find(configuration_hash);
        found != configuration_id_cache_.end()) {
      ++metrics_.configuration_id_hits;
      return found->second;
    }
    ++metrics_.configuration_id_misses;
    const int64_t id = db_.add_translation_unit_config(configuration);
    configuration_id_cache_.emplace(configuration_hash, id);
    return id;
  }

  [[nodiscard]] ComponentOwnershipIndex &ownership() noexcept {
    return ownership_;
  }

  [[nodiscard]] IndexSessionMetrics metrics() const {
    IndexSessionMetrics result = metrics_;
    const ToolchainMetrics &toolchain = toolchain_.metrics();
    result.configuration_hits = toolchain.configuration_hits;
    result.configuration_misses = toolchain.configuration_misses;
    result.driver_subprocesses = toolchain.driver_subprocesses;
    return result;
  }

  cidx::Storage &db_;
  cidx::StorageWorkspaceAdapter workspace_data_;
  Toolchain toolchain_;
  std::unique_ptr<WorkspaceContext> context_;
  std::unique_ptr<TranslationUnitConfigurationService> resolver_;
  std::unordered_map<std::string, TranslationUnitDescriptor> descriptor_cache_;
  std::unordered_map<std::string, int64_t> configuration_id_cache_;
  std::unordered_map<std::string, std::optional<std::string>> source_hashes_;
  std::unordered_map<std::string, std::string> generated_input_hashes_;
  std::unordered_map<std::string, std::string> file_configuration_hashes_;
  ComponentOwnershipIndex ownership_;
  std::int64_t data_version_ = 0;
  IndexSessionMetrics metrics_;
  IndexSessionMetrics profiled_metrics_;

private:
  std::string
  generated_input_fingerprint(const TranslationUnitDescriptor &descriptor) {
    std::string data;
    for (const std::string &input : descriptor.configuration.generated_inputs) {
      const std::string path = pathutil::abspath(pathutil::join(
          descriptor.configuration.working_dir.value_or("."), input));
      ++metrics_.file_stat_reads;
      const bool exists = files::is_regular_file(path);
      ++metrics_.file_hash_reads;
      data += path + '\0' + (exists ? md5_of(path).value_or("") : "<missing>") +
              '\0';
    }
    return sha256_hex(data);
  }

  void note_invalidation(IndexInvalidationReason reason) {
    switch (reason) {
    case IndexInvalidationReason::manual:
      return;
    case IndexInvalidationReason::repository_switch:
      ++metrics_.repository_invalidations;
      return;
    case IndexInvalidationReason::clone_change:
      ++metrics_.clone_invalidations;
      return;
    case IndexInvalidationReason::component_update:
      ++metrics_.component_invalidations;
      return;
    case IndexInvalidationReason::compile_option_update:
      ++metrics_.configuration_invalidations;
      return;
    case IndexInvalidationReason::generated_input_change:
      ++metrics_.generated_input_invalidations;
      return;
    case IndexInvalidationReason::source_mutation:
      ++metrics_.source_invalidations;
      return;
    }
  }

  void replace_last_invalidation(IndexInvalidationReason reason) {
    if (metrics_.configuration_invalidations > 0) {
      --metrics_.configuration_invalidations;
    }
    note_invalidation(reason);
  }
};

IndexSession::IndexSession(cidx::Storage &db, cidx::Logger &log)
    : impl_(std::make_unique<Impl>(db, log)) {}
IndexSession::~IndexSession() = default;
IndexSession::IndexSession(IndexSession &&) noexcept = default;
IndexSession &IndexSession::operator=(IndexSession &&) noexcept = default;

void IndexSession::invalidate(IndexInvalidationReason reason) {
  impl_->rebuild(reason);
}

IndexSessionMetrics IndexSession::metrics() const { return impl_->metrics(); }

IndexOneOutcome run_index_one(cidx::Storage &db, const cidx::File &rec,
                              const std::string &path, bool graph_enabled,
                              IndexFailurePoint failure) {
  IndexSession session(db);
  return run_index_one(db, session, rec, path, graph_enabled, failure);
}

IndexOneOutcome run_index_one(cidx::Storage &db, IndexSession &session,
                              const cidx::File &rec, const std::string &path,
                              bool graph_enabled, IndexFailurePoint failure) {
  const IndexSessionMetrics session_before = session.impl_->profiled_metrics_;
  const bool profiling = profile::active();
  const auto wall_started =
      profiling ? ProfileClock::now() : ProfileClock::time_point{};
  const std::clock_t cpu_started = profiling ? std::clock() : 0;
  const std::uint64_t start_position =
      profiling ? profile::next_translation_unit_position() : 0;
  const double child_wall_before =
      profiling ? profile::driver_subprocess_wall_seconds() : 0.0;
  const auto source_started =
      profiling ? ProfileClock::now() : ProfileClock::time_point{};
  IndexOneOutcome out;
  const SourceSnapshot source = SourceSnapshot::capture(path);
  ++session.impl_->metrics_.file_stat_reads;
  ++session.impl_->metrics_.file_hash_reads;
  if (profiling) {
    profile::add_timing("source_validation_hashing",
                        elapsed_seconds(source_started));
  }
  std::pair<std::int64_t, std::int64_t> cardinality_before{};
  if (profiling) {
    const auto metrics_started = ProfileClock::now();
    cardinality_before = db.indexing_cardinality();
    profile::add_timing("metrics_only_sql", elapsed_seconds(metrics_started));
  }
  const auto workspace_started =
      profiling ? ProfileClock::now() : ProfileClock::time_point{};
  out.source_mtime = source.mtime;
  out.source_md5 = source.md5;
  const TranslationUnitDescriptor &descriptor =
      session.impl_->prepared_descriptor(path, rec.id, source.md5);
  const TranslationUnitConfig resolved =
      configuration_without_source(descriptor);
  const std::vector<std::string> &args = resolved.arguments;
  if (profiling) {
    profile::add_timing("workspace_snapshot_configuration",
                        elapsed_seconds(workspace_started));
  }
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
  state.ownership = &session.impl_->ownership();
  state.session_metrics = &session.impl_->metrics_;
  state.unit = ports.unit_of_work().begin();
  if (profiling) {
    profile::note_transaction_begin();
  }
  state.normalized_config_id = session.impl_->configuration_id(descriptor);

  IndexFrontendActionFactory factory(state);
  const auto clang_started =
      profiling ? ProfileClock::now() : ProfileClock::time_point{};
  try {
    (void)setup.tool.run(&factory);
  } catch (const PassBudgetExceeded &error) {
    out.parse_failed = true;
    out.error = error.what();
  }
  if (profiling) {
    const double clang_tool_inclusive = elapsed_seconds(clang_started);
    const double pass_seconds = std::transform_reduce(
        out.pass_metrics.begin(), out.pass_metrics.end(), 0.0, std::plus<>(),
        [](const IndexPassMetrics &pass) {
          return static_cast<double>(pass.elapsed_microseconds) / 1'000'000.0;
        });
    profile::add_timing("clang_tool_inclusive", clang_tool_inclusive);
    // LibTooling runs the registered visitors and persistence passes inside
    // tool.run(). Subtract their disjoint registry timings so this attribution
    // remains exclusive instead of double-counting those categories.
    profile::add_timing("clang_front_end",
                        std::max(0.0, clang_tool_inclusive - pass_seconds));
  }
  const auto record_profile = [&] {
    if (!profiling) {
      return;
    }
    profile::add_counter("registered_root_traversal_budget",
                         out.registered_whole_tu_traversal_budget);
    profile::add_counter("observed_root_traversals",
                         out.observed_whole_tu_traversals);
    for (const IndexPassMetrics &pass : out.pass_metrics) {
      const double elapsed =
          static_cast<double>(pass.elapsed_microseconds) / 1'000'000.0;
      profile::add_timing("pass." + pass.id, elapsed);
      if (pass.id.starts_with("symbols.")) {
        profile::add_timing("root_symbols", elapsed);
      } else if (pass.id.starts_with("declarations.")) {
        profile::add_timing("root_declarations", elapsed);
      } else if (pass.id.starts_with("definitions.")) {
        profile::add_timing("root_definitions", elapsed);
      } else if (pass.id.starts_with("namespaces.")) {
        profile::add_timing("root_namespaces", elapsed);
      }
      if (pass.id.starts_with("statements.")) {
        profile::add_timing("body_extraction", elapsed);
      }
      if (pass.id == "includes.persist") {
        profile::add_timing("include_persistence", elapsed);
      }
      if (pass.id.ends_with(".associate")) {
        profile::add_timing("applicability_association", elapsed);
      }
      if (pass.id.ends_with(".persist") || pass.id.ends_with(".associate")) {
        profile::add_timing("fact_persistence", elapsed);
      }
      for (const auto &[family, counts] : pass.fact_families) {
        profile::add_fact_family(family, counts.attempted, counts.persisted,
                                 counts.duplicates);
      }
    }
    const IndexSessionMetrics current = session.metrics();
    const auto add_delta = [&current, &session_before](
                               std::string_view name,
                               std::size_t IndexSessionMetrics::*member) {
      profile::add_counter(name, static_cast<std::uint64_t>(
                                     current.*member - session_before.*member));
    };
    add_delta("index_session.snapshot_rebuilds",
              &IndexSessionMetrics::snapshot_rebuilds);
    add_delta("index_session.descriptor_hits",
              &IndexSessionMetrics::descriptor_hits);
    add_delta("index_session.descriptor_misses",
              &IndexSessionMetrics::descriptor_misses);
    add_delta("index_session.configuration_id_hits",
              &IndexSessionMetrics::configuration_id_hits);
    add_delta("index_session.configuration_id_misses",
              &IndexSessionMetrics::configuration_id_misses);
    add_delta("index_session.cache_evictions",
              &IndexSessionMetrics::cache_evictions);
    add_delta("index_session.file_stat_reads",
              &IndexSessionMetrics::file_stat_reads);
    add_delta("index_session.file_hash_reads",
              &IndexSessionMetrics::file_hash_reads);
    add_delta("index_session.component_scans",
              &IndexSessionMetrics::component_scans);
    add_delta("index_session.ownership_hits",
              &IndexSessionMetrics::ownership_hits);
    add_delta("index_session.ownership_misses",
              &IndexSessionMetrics::ownership_misses);
    add_delta("index_session.repository_invalidations",
              &IndexSessionMetrics::repository_invalidations);
    add_delta("index_session.clone_invalidations",
              &IndexSessionMetrics::clone_invalidations);
    add_delta("index_session.component_invalidations",
              &IndexSessionMetrics::component_invalidations);
    add_delta("index_session.configuration_invalidations",
              &IndexSessionMetrics::configuration_invalidations);
    add_delta("index_session.generated_input_invalidations",
              &IndexSessionMetrics::generated_input_invalidations);
    add_delta("index_session.source_invalidations",
              &IndexSessionMetrics::source_invalidations);
    session.impl_->profiled_metrics_ = current;
    std::uint64_t preprocessed_bytes = file_bytes(path);
    for (const IncludeFact &include : state.includes.includes) {
      if (include.resolved) {
        preprocessed_bytes += file_bytes(include.dst_path);
      }
    }
    profile::record_translation_unit(
        {.path = path,
         .start_position = start_position,
         .database_cardinality_before = cardinality_before.first,
         .fact_cardinality_before = cardinality_before.second,
         .source_bytes = file_bytes(path),
         .preprocessed_bytes = preprocessed_bytes,
         .include_count = state.includes.includes.size(),
         .new_headers = static_cast<std::uint64_t>(out.headers.indexed),
         .already_indexed_headers =
             static_cast<std::uint64_t>(out.headers.already),
         .configuration_state = std::to_string(state.normalized_config_id),
         .wall_seconds = elapsed_seconds(wall_started),
         .in_process_cpu_seconds =
             static_cast<double>(std::clock() - cpu_started) /
             static_cast<double>(CLOCKS_PER_SEC),
         .child_process_wall_seconds =
             profile::driver_subprocess_wall_seconds() - child_wall_before,
         .peak_rss_bytes = profile::process_peak_rss_bytes()});
  };
  if (!state.tu_handled) {
    out.parse_failed = true;
    out.error = "cannot parse " + path;
    out.session_metrics = session.metrics();
    if (profiling) {
      profile::note_transaction_rollback();
    }
    record_profile();
    return out;
  }
  const auto verification_started =
      profiling ? ProfileClock::now() : ProfileClock::time_point{};
  apply_diagnostic_policy(path, state.strict, args, out);
  ++session.impl_->metrics_.source_change_checks;
  ++session.impl_->metrics_.file_stat_reads;
  ++session.impl_->metrics_.file_hash_reads;
  if (!source.matches(path)) {
    session.impl_->rebuild(IndexInvalidationReason::source_mutation);
    out.source_changed = true;
    out.error = path + ": source changed during indexing; retry required";
  } else if (out.source_changed && out.error.empty()) {
    out.error = path + ": source changed during indexing; retry required";
  }
  if (profiling) {
    profile::add_timing("verification", elapsed_seconds(verification_started));
  }
  if (!out.parse_failed && !out.source_changed) {
    const auto commit_started =
        profiling ? ProfileClock::now() : ProfileClock::time_point{};
    state.unit->commit();
    if (profiling) {
      profile::note_transaction_commit();
      profile::add_timing("commit", elapsed_seconds(commit_started));
    }
  } else if (profiling) {
    profile::note_transaction_rollback();
  }
  record_profile();
  out.session_metrics = session.metrics();
  return out;
}

} // namespace cidx::ast
