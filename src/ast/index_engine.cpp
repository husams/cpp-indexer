#include "ast/index_engine.hpp"
#include "ast/front_end_reuse.hpp"

#include "ast/clang_version.hpp"
#include "ast/declaration_edge_visitor.hpp"
#include "ast/display_name_rewrite.hpp"
#include "ast/fact_batch.hpp"
#include "ast/function_definition_visitor.hpp"
#include "ast/include_facts.hpp"
#include "ast/kind_map.hpp"
#include "ast/location.hpp"
#include "ast/namespace_use_visitor.hpp"
#include "ast/owned_header_plan.hpp"
#include "ast/pass_registry.hpp"
#include "ast/routed_root_events.hpp"
#include "ast/symbol_visitor.hpp"

#include "catalogs/generated_catalog.hpp"
#include "compiledb/compiledb.hpp"
#include "profile/index_profile.hpp"
#include "storage/fact_batch_writer.hpp"
#include "storage/ports.hpp"
#include "storage/sqlite_adapters.hpp"
#include "storage/storage.hpp"
#include "toolchain/toolchain.hpp"
#include "util/env.hpp"
#include "util/files.hpp"
#include "util/front_end_reuse.hpp"
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
#include <set>
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
  const cidx::TranslationUnitConfig *publication_config = nullptr;
  int64_t normalized_config_id = -1;
  ComponentOwnershipIndex *ownership = nullptr;
  IndexSessionMetrics *session_metrics = nullptr;
  clang::Preprocessor *pp = nullptr; // v31: for include-guard status
  bool tu_handled = false;
  IndexFailurePoint requested_failure = IndexFailurePoint::none;
  std::optional<FactBatch> batch;
  OwnedHeaderRoutePlan route_plan;
  // S-074: scheduler-owned owned-header assignment. Null keeps the serial
  // contract of asking the committed database directly.
  cidx::index::HeaderClaimOracle *claims = nullptr;
  std::size_t rank = 0;
  // Set once the oracle has answered, so a translation unit that fails after
  // discovery does not release the ordered gate twice.
  bool claimed = false;
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
        recorder_("production-index"), symbol_emitter_(recorder_),
        tu_(context.getTranslationUnitDecl()) {
    // Cross-translation-unit symbol identity is NOT resolved here. It used to
    // be: a batch-local miss asked the authoritative database for the symbol
    // mid-parse. That made identity depend on what other translation units had
    // already published at that instant, and it held a SQLite read lock inside
    // extraction, which under rollback journaling cannot coexist with the
    // controlled writer's exclusive lock.
    //
    // The recorder now records the question against a batch handle and the
    // controlled writer answers it at publication, in the legacy apply order.
    // Extraction performs no database read of symbol identity at all, which is
    // what makes bounded parallel extraction both possible and deterministic.
    recorder_.set_deferred_external_identity(
        {.enabled = true, .translation_unit_path = state_.path});
  }

  void run() {
    if (!prepare_routed_files()) {
      return;
    }
    ExtractionPassRegistry registry;
    register_header_passes(registry);
    register_main_passes(registry);
    register_persistence_passes(registry);
    IndexingPlan plan = build_plan();
    FrontendSession session = make_frontend_session();
    for (const FrontendPassProvider &provider : frontend_pass_providers()) {
      provider(session, registry, plan);
    }
    record_pass_metrics(registry.run(plan, &session));
    state_.batch = recorder_.canonical_batch();
    update_output_stats(*state_.batch);
    state_.out->evidence = state_.batch->records().evidence;
  }

private:
  static auto partition_fact_count(const FactBatch &batch,
                                   const FactPartitionKey &partition,
                                   std::initializer_list<FactFamily> families)
      -> std::size_t {
    const auto found = std::ranges::find_if(
        batch.partitions(), [&partition](const FileFactPartition &candidate) {
          return candidate.key.file == partition.file;
        });
    if (found == batch.partitions().end()) {
      return 0;
    }
    return std::transform_reduce(
        families.begin(), families.end(), std::size_t{0}, std::plus<>(),
        [&found](FactFamily family) {
          const auto members = found->members.find(family);
          return members == found->members.end() ? std::size_t{0}
                                                 : members->second.size();
        });
  }

  void update_output_stats(const FactBatch &batch) const {
    state_.out->stored = 0;
    state_.out->headers.indexed = static_cast<int>(pending_headers_.size());
    state_.out->headers.symbols = 0;
    for (const SymbolRecord &symbol : batch.records().symbols) {
      if (symbol.line <= 0) {
        continue;
      }
      const std::string symbol_path =
          pathutil::normpath(pathutil::abspath(symbol.file));
      if (symbol_path == state_.path) {
        ++state_.out->stored;
        continue;
      }
      if (std::ranges::any_of(pending_headers_,
                              [&symbol_path](const PendingHeader &header) {
                                return header.path == symbol_path;
                              })) {
        ++state_.out->headers.symbols;
      }
    }
  }

  // The rooted graph collector's event bound, matching the emitted-fact budget
  // every extraction pass declares. Overflow raises the named
  // PassBudgetExceeded diagnostic instead of letting the buffer grow without
  // limit.
  static constexpr std::size_t kRoutedGraphEventBudget = 20'000'000;

  // Raise the configured fault at a pipeline phase boundary. Without an
  // injector this is a null check, so normal behaviour is unchanged.
  void inject(cidx::storage::FailurePoint point) const {
    if (state_.failure_injector != nullptr) {
      state_.failure_injector->inject(point);
    }
  }

  static auto descriptor(
      std::string id, std::vector<FrontendCapability> capabilities,
      std::vector<std::string> consumed, std::vector<std::string> produced,
      std::vector<std::string> dependencies, PassScope scope,
      TraversalMode traversal,
      FactCompleteness completeness = FactCompleteness::complete,
      FactTrust trust = FactTrust::trusted,
      std::size_t max_whole_tu_traversals = 0) -> ExtractionPassDescriptor {
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
  }

  void register_header_passes(ExtractionPassRegistry &registry) {
    register_header_symbols(registry);
    register_header_lifecycle(registry);
    register_header_association(registry);
  }

  void register_header_symbols(ExtractionPassRegistry &registry) {
    registry.register_pass(
        descriptor("symbols.main", {FrontendCapability::ast}, {}, {"symbols"},
                   {}, PassScope::main_file, TraversalMode::declaration),
        [this](PassExecutionContext &execution) -> void {
          const auto main_route = std::ranges::find_if(
              state_.route_plan.routes(), [](const PlannedFileRoute &route) {
                return route.role == PlannedFileRole::translation_unit;
              });
          if (main_route == state_.route_plan.routes().end()) {
            throw std::logic_error(
                "diagnostic capture has no translation-unit route");
          }
          for (const cidx::Diagnostic &diagnostic : state_.out->diagnostics) {
            execution.metrics.note_diagnostic(diagnostic.spelling);
            recorder_.emit(DiagnosticFactRecord{
                .partition = main_route->extraction.partition,
                .severity = static_cast<DiagnosticSeverity>(
                    std::clamp(diagnostic.severity - 1, 0, 3)),
                .spelling = diagnostic.spelling,
                .location_file = diagnostic.file_path
                                     ? std::optional{portable_file_identity(
                                           *diagnostic.file_path)}
                                     : std::nullopt,
                .line = diagnostic.line,
                .col = diagnostic.col});
          }
        });
    registry.register_pass(
        descriptor("symbols.headers",
                   {FrontendCapability::ast, FrontendCapability::preprocessor},
                   {"includes"}, {"symbols"}, {"symbols.main"},
                   PassScope::owned_header, TraversalMode::declaration,
                   FactCompleteness::complete, FactTrust::trusted, 1),
        [this](PassExecutionContext &execution) -> void {
          run_routed_symbol_pass(execution);
          inject(cidx::storage::FailurePoint::symbol_capture_complete);
        });
  }

  void register_header_lifecycle(ExtractionPassRegistry &registry) {
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
        descriptor(
            "declarations.headers", {FrontendCapability::ast},
            {"symbols", "fact_lifecycle"},
            {"relations", "types", "definitions", "presentation_intents"},
            {"symbols.headers", "lifecycle.headers", "lifecycle.main"},
            PassScope::owned_header, TraversalMode::declaration,
            FactCompleteness::complete, FactTrust::trusted, 1),
        [this](PassExecutionContext &execution) -> void {
          collect_routed_graph_events(execution);
          run_routed_declaration_stage(execution);
          inject(cidx::storage::FailurePoint::declaration_replay);
        });
    registry.register_pass(
        // Replays the graph collector's recorded stream: a logical pass with
        // its own provider id, diagnostics and fact families, but no rooted
        // traversal of its own.
        descriptor("definitions.headers", {FrontendCapability::ast},
                   {"symbols"}, {"definitions"}, {"declarations.headers"},
                   PassScope::owned_header, TraversalMode::declaration,
                   FactCompleteness::complete, FactTrust::trusted, 0),
        [this](PassExecutionContext &execution) -> void {
          routed_definition_visitor_ = collect_routed_definitions(execution);
          inject(cidx::storage::FailurePoint::definition_replay);
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
  }

  void register_header_association(ExtractionPassRegistry &registry) {
    registry.register_pass(
        // Also a replay of the graph collector's stream, and the last consumer
        // of it: no rooted traversal of its own.
        descriptor("namespaces.headers", {FrontendCapability::ast},
                   {"symbols", "relations"}, {"relations"},
                   {"statements.headers"}, PassScope::owned_header,
                   TraversalMode::declaration, FactCompleteness::complete,
                   FactTrust::trusted, 0),
        [this](PassExecutionContext &execution) -> void {
          run_routed_namespace_stage(execution);
          inject(cidx::storage::FailurePoint::namespace_replay);
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
              continue;
            }
            const auto route =
                std::ranges::find(state_.route_plan.routes(), header.path,
                                  &PlannedFileRoute::path);
            if (route == state_.route_plan.routes().end()) {
              throw std::logic_error(
                  "header route disappeared before publication");
            }
            recorder_.emit(ApplicabilityOwnershipRecord{
                .partition = route->extraction.partition,
                .file = route->extraction.partition.file,
                .role = ApplicabilityRole::header,
                .state = ApplicabilityState::registered,
                .generation = {.token = state_.route_plan.token()}});
            const FactBatch snapshot = recorder_.snapshot();
            execution.metrics.note_emitted(partition_fact_count(
                snapshot, route->extraction.partition,
                {FactFamily::symbols, FactFamily::relations,
                 FactFamily::definitions}));
          }
          inject(cidx::storage::FailurePoint::header_association);
        });
  }

  void register_main_passes(ExtractionPassRegistry &registry) {
    registry.register_pass(descriptor("lifecycle.main", {}, {},
                                      {"fact_lifecycle"}, {"lifecycle.headers"},
                                      PassScope::main_file,
                                      TraversalMode::lifecycle),
                           [this](PassExecutionContext &execution) -> void {
                             configure_fact_file(main_file_handle_, true);
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
          inject(cidx::storage::FailurePoint::statement_body_replay);
        });
    registry.register_pass(
        descriptor("namespaces.main", {FrontendCapability::ast},
                   {"symbols", "relations"}, {"relations"}, {"statements.main"},
                   PassScope::main_file, TraversalMode::declaration),
        [](PassExecutionContext & /*execution*/) -> void {});
  }

  void register_persistence_passes(ExtractionPassRegistry &registry) {
    register_presentation_pass(registry);
    register_association_and_include_passes(registry);
  }

  void register_presentation_pass(ExtractionPassRegistry &registry) {
    registry.register_pass(
        descriptor("presentation.persist", {}, {"presentation_intents"},
                   {"display_names"},
                   {"declarations.headers", "declarations.main"},
                   PassScope::translation_unit, TraversalMode::lifecycle),
        [this](PassExecutionContext &execution) -> void {
          persist_presentation(execution);
        });
  }

  void persist_presentation(PassExecutionContext &execution) {
    state_.presentation_intents = recorder_.pending_presentation_intents();
    std::ranges::sort(state_.presentation_intents, {},
                      [](const PresentationIntent &intent) -> auto {
                        return std::tie(intent.symbol_id, intent.display_args);
                      });
    const std::size_t before = state_.presentation_intents.size();
    const auto unique_end =
        std::ranges::unique(state_.presentation_intents,
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
      const auto display = recorder_.lookup_display_name(intent.symbol_id);
      if (!display) {
        continue;
      }
      const auto rewritten =
          rewrite_template_display_name(*display, intent.display_args);
      if (rewritten) {
        updates.emplace_back(intent.symbol_id, *rewritten);
      }
    }
    execution.metrics.note_emitted(updates.size());
    for (const auto &[symbol_id, display] : updates) {
      recorder_.update_display_name(symbol_id, display);
    }
    execution.metrics.note_fact_family("display_names", before, updates.size(),
                                       before -
                                           state_.presentation_intents.size());
  }

  void
  register_association_and_include_passes(ExtractionPassRegistry &registry) {
    auto main_association = descriptor(
        "main.associate", {}, {"symbols", "relations", "definitions"},
        {"file_associations"},
        {"symbols.main", "lifecycle.main", "headers.associate",
         "statements.main", "namespaces.main"},
        PassScope::main_file, TraversalMode::lifecycle);
    registry.register_pass(
        std::move(main_association),
        [this](PassExecutionContext &execution) -> void {
          const auto route = std::ranges::find_if(
              state_.route_plan.routes(),
              [](const PlannedFileRoute &candidate) {
                return candidate.role == PlannedFileRole::translation_unit;
              });
          if (route == state_.route_plan.routes().end()) {
            throw std::logic_error("main route disappeared before publication");
          }
          recorder_.emit(ApplicabilityOwnershipRecord{
              .partition = route->extraction.partition,
              .file = route->extraction.partition.file,
              .role = ApplicabilityRole::translation_unit,
              .state = ApplicabilityState::registered,
              .generation = {.token = state_.route_plan.token()}});
          const FactBatch snapshot = recorder_.snapshot();
          execution.metrics.note_emitted(
              partition_fact_count(snapshot, route->extraction.partition,
                                   {FactFamily::symbols, FactFamily::relations,
                                    FactFamily::definitions}));
          inject(cidx::storage::FailurePoint::main_association);
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
          std::uint64_t recorded = 0;
          for (const IncludeFact &fact : state_.includes.includes) {
            const auto source = routed_file_ids_.find(fact.src_path);
            // The ordered claim gate places only this TU's main file and the
            // headers it owns in routed_file_ids_. A later TU still observes
            // shared-header preprocessing callbacks, but the first claimant's
            // facts are already the durable configuration-scoped copy. Skip
            // before path partitioning or database lookup so claim-once also
            // removes the persistence-pass cost, not merely the final INSERT.
            if (source == routed_file_ids_.end()) {
              continue;
            }
            const FactPartitionKey owner = fact_partition(fact.src_path);
            recorder_.set_current_file_id(source->second);
            recorder_.emit(IncludeDirectiveRecord{
                .partition = owner,
                .source = owner.file,
                .destination =
                    fact.resolved
                        ? std::optional(portable_file_identity(fact.dst_path))
                        : std::nullopt,
                .destination_path = fact.dst_path,
                .spelling = fact.spelling,
                .directive = static_cast<IncludeDirectiveKind>(fact.directive),
                .line = fact.line,
                .col = fact.col,
                .begin_offset = fact.begin_offset,
                .end_offset = fact.end_offset,
                .conditional_fingerprint = fact.cond_fingerprint,
                .is_angled = fact.is_angled,
                .resolved = fact.resolved,
                .is_system = fact.is_system,
                .guarded = fact.guarded});
            ++recorded;
          }
          for (const MacroUseFact &fact : state_.includes.macro_uses) {
            const auto source = routed_file_ids_.find(fact.src_path);
            if (source == routed_file_ids_.end()) {
              continue;
            }
            const FactPartitionKey owner = fact_partition(fact.src_path);
            recorder_.set_current_file_id(source->second);
            recorder_.emit(MacroUseRecord{
                .partition = owner,
                .source = owner.file,
                .definition = portable_file_identity(fact.def_path),
                .definition_path = fact.def_path,
                .name = fact.name});
            ++recorded;
          }
          std::set<std::pair<std::string, std::string>> unique_edges;
          for (const IncludeFact &fact : state_.includes.includes) {
            unique_edges.emplace(fact.src_path, fact.dst_path);
          }
          const std::size_t collapsed =
              state_.includes.includes.size() - unique_edges.size();
          const std::size_t attempted = state_.includes.includes.size() +
                                        state_.includes.macro_uses.size();
          const std::size_t claim_duplicates = attempted - recorded;
          // Preserve the historical include-pass accounting contract: one
          // duplicate represents the TU include collector itself and each
          // collapsed edge represents its raw and normalized observations.
          // Facts observed from a header owned by another TU are additionally
          // reported as claim duplicates rather than disappearing from
          // telemetry.
          execution.metrics.note_duplicate(1 + (2 * collapsed) +
                                           claim_duplicates);
          // A non-empty include set contributes the legacy collector envelope
          // in addition to the concrete include and macro records.
          execution.metrics.note_emitted(
              recorded + (state_.includes.includes.empty() ? 0 : 1));
          execution.metrics.note_fact_family("include_facts", attempted,
                                             recorded, claim_duplicates);
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
  }

  [[nodiscard]] static auto build_plan() -> IndexingPlan {
    IndexingPlan plan;
    plan.add("symbols.main");
    plan.add("symbols.headers");
    plan.add("lifecycle.headers");
    plan.add("lifecycle.main");
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

    return plan;
  }

  auto make_frontend_session() -> FrontendSession {
    return FrontendSession{
        .ast_context = &context_,
        .preprocessor = state_.pp,
        .declaration_ports = &static_cast<DeclarationPassPorts &>(recorder_),
        .statement_ports = &static_cast<StatementFactPorts &>(recorder_),
        .namespace_ports = &static_cast<NamespacePassPorts &>(recorder_),
        .definition_ports = &static_cast<DefinitionScopeEmitter &>(recorder_),
        .evidence = &static_cast<EvidenceEmitter &>(recorder_),
        .presentation_intents =
            &static_cast<PresentationIntentEmitter &>(recorder_),
        .lifecycle = &static_cast<IndexingLifecycle &>(recorder_),
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
  }

  void record_pass_metrics(const PassExecutionReport &report) const {
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
  // A not-yet-indexed OWNED non-system header discovered by the include
  // recorder, with its freshly minted file row.
  struct PendingHeader {
    std::string path;
    int64_t file_id = -1;
    std::optional<double> mtime;
    std::optional<std::string> md5;
    bool covered_by_current_config = false;
    int stored = 0;
    std::vector<int64_t> symbol_ids;
    std::vector<int64_t> edge_ids;
    std::vector<int64_t> definition_ids;
  };

  void run_routed_symbol_pass(PassExecutionContext &execution) {
    routed_symbol_file_id_ = -1;
    recorder_.set_identity_translation_unit_config_id(
        state_.normalized_config_id, main_file_handle_);
    SymbolVisitor visitor(
        context_, symbol_emitter_, {}, &execution.metrics,
        [this](const std::string &path) { return route_symbol_file(path); });
    execution.metrics.note_whole_tu_traversal();
    profile::add_counter("root_traverse_decl_calls");
    visitor.TraverseDecl(tu_);
  }

  // The stable identity of this translation unit's normalized configuration.
  // Used as the owned-header claim key because a configuration ID is transient
  // until the writer mints its row, while this hash is not (S-074).
  [[nodiscard]] auto translation_unit_configuration_hash() const
      -> std::string {
    if (state_.publication_config == nullptr) {
      throw std::logic_error("publication configuration is unavailable");
    }
    return translation_unit_config_hash(*state_.publication_config);
  }

  [[nodiscard]] auto translation_unit_route_key() const -> std::string {
    return "config:" + translation_unit_configuration_hash() + "\x1fsource:" +
           db_.portable_source_identity_for_file(state_.rec->id);
  }

  [[nodiscard]] auto portable_file_identity(const std::string &path) const
      -> PortableFileIdentity {
    const std::string absolute = pathutil::normpath(pathutil::abspath(path));
    const std::optional<Component> component = db_.component_for_path(absolute);
    if (!component) {
      return {.file_name = absolute};
    }
    const std::string component_root =
        pathutil::normpath(db_.component_abs_base(*component));
    const std::string relative = pathutil::relpath(absolute, component_root);
    std::string directory = pathutil::dirname(relative);
    if (directory == ".") {
      directory.clear();
    }
    return {.component_path = component_root,
            .directory_path = std::move(directory),
            .file_name = pathutil::basename(relative)};
  }

  [[nodiscard]] auto fact_partition(const std::string &path) const
      -> FactPartitionKey {
    const std::int64_t universe_id =
        state_.ports->workspace.semantic_universe_for_file_id(state_.rec->id);
    const std::optional<SemanticUniverse> universe =
        state_.ports->workspace.get_semantic_universe_by_id(universe_id);
    return {
        .file = portable_file_identity(path),
        .configuration = {
            .semantic_universe = universe ? universe->key : "legacy",
            .translation_unit = translation_unit_route_key(),
            .normalized_configuration = include_config_digest(*state_.config),
            .identity_source = path,
            .content = {.driver = state_.config->driver,
                        .working_dir = state_.config->working_dir,
                        .arguments = state_.config->arguments,
                        .lang_mode = state_.config->lang_mode,
                        .resource_dir = state_.config->resource_dir}}};
  }

  [[nodiscard]] auto route_generation() const -> std::string {
    return std::to_string(state_.session_metrics->generation) + ':' +
           include_config_digest(*state_.config) + ':' +
           translation_unit_route_key();
  }

  [[nodiscard]] auto build_owned_header_route_plan() -> OwnedHeaderRoutePlan {
    const std::string translation_unit = translation_unit_route_key();
    std::vector<OwnedHeaderRouteCandidate> candidates;
    candidates.reserve(pending_headers_.size() + 1);
    candidates.push_back({.role = PlannedFileRole::translation_unit,
                          .translation_unit = translation_unit,
                          .translation_unit_file_id = state_.rec->id,
                          .path = state_.path,
                          .discovery_ordinal = 0,
                          .existing_file_id = state_.rec->id,
                          .snapshot = {.mtime = state_.out->source_mtime,
                                       .md5 = state_.out->source_md5},
                          .compile_options = state_.rec->compile_options,
                          .driver = state_.rec->driver,
                          .cleanup_symbols = true,
                          .partition = fact_partition(state_.path)});
    for (std::size_t index = 0; index < pending_headers_.size(); ++index) {
      const PendingHeader &header = pending_headers_[index];
      candidates.push_back(
          {.role = PlannedFileRole::owned_header,
           .translation_unit = translation_unit,
           .translation_unit_file_id = state_.rec->id,
           .path = header.path,
           .discovery_ordinal = index + 1,
           .existing_file_id = std::nullopt,
           .snapshot = {.mtime = header.mtime, .md5 = header.md5},
           .compile_options = state_.rec->compile_options,
           .driver = state_.rec->driver,
           .cleanup_symbols = header.covered_by_current_config,
           .partition = fact_partition(header.path)});
    }
    return plan_owned_header_routes(route_generation(), std::move(candidates));
  }

  // Planning and lifecycle publication complete before semantic pass
  // dispatch. The passes consume only the frozen path-to-file routing answer.
  [[nodiscard]] bool prepare_routed_files() {
    double routing_seconds = 0.0;
    double persistence_seconds = 0.0;
    {
      const profile::ScopedAccumulator routing(routing_seconds);
      pending_headers_ = discover_owned_headers(persistence_seconds);
      state_.route_plan = build_owned_header_route_plan();
      routed_file_ids_.clear();
      for (const PlannedFileRoute &route : state_.route_plan.routes()) {
        if (route.translation_unit != translation_unit_route_key() ||
            !route.extraction.transient_file_handle) {
          continue;
        }
        const std::int64_t handle = *route.extraction.transient_file_handle;
        recorder_.set_partition(route.extraction.partition, handle);
        routed_file_ids_.emplace(route.path, handle);
        if (route.role == PlannedFileRole::translation_unit) {
          main_file_handle_ = handle;
          continue;
        }
        const auto pending = std::ranges::find(pending_headers_, route.path,
                                               &PendingHeader::path);
        if (pending != pending_headers_.end()) {
          pending->file_id = handle;
        }
      }
      if (main_file_handle_ < 0) {
        state_.out->parse_failed = true;
        state_.out->error = "publication plan omitted the main file route";
        return false;
      }
    }
    profile::add_timing(profile::kRootSymbolRoutingTiming,
                        routing_seconds - persistence_seconds);
    profile::add_timing(profile::kRootSymbolPersistenceTiming,
                        persistence_seconds);
    return true;
  }

  bool route_symbol_file(const std::string &path) {
    const auto file = routed_file_ids_.find(path);
    if (file == routed_file_ids_.end()) {
      return false;
    }
    if (routed_symbol_file_id_ != file->second) {
      recorder_.set_current_file_id(file->second);
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
      recorder_.set_current_file_id(file->second);
      routed_fact_file_id_ = file->second;
    }
    return file->second;
  }

  // The same routing answer without route_fact_file's side effect of switching
  // the fact sink's current file. Recording events is not emitting facts, so
  // the collector must not move the sink; replay does that through the
  // visitors' own routers, in emission order.
  [[nodiscard]] auto route_fact_file_id(const std::string &path) const
      -> std::optional<int64_t> {
    const auto file = routed_file_ids_.find(path);
    return file == routed_file_ids_.end() ? std::nullopt
                                          : std::optional{file->second};
  }

  void configure_fact_file(int64_t file_id, bool reset) {
    recorder_.set_current_file_id(file_id);
    recorder_.set_identity_translation_unit_config_id(
        state_.normalized_config_id, main_file_handle_);
    if (reset) {
      recorder_.delete_edges_for_file(file_id);
      recorder_.delete_definitions_for_file(file_id);
    }
  }

  // The single rooted graph traversal. It records declaration,
  // definition-candidate and namespace-scope/use events and emits nothing; the
  // declaration, definition and namespace stages then replay it in logical
  // order. Statement bodies are neither traversed nor emitted here - they stay
  // in the separate statements.main phase, which owns no root budget.
  void collect_routed_graph_events(PassExecutionContext &execution) {
    graph_events_.reset();
    if (!state_.graph_enabled) {
      return;
    }
    // Bounded by the declared emitted-fact budget of the owning pass and
    // reported against its id. Metrics are deliberately not wired into the
    // recorder: every recorded declaration is counted once, by the visitor
    // that replays it, exactly as the separate root walks counted it.
    graph_events_ = std::make_unique<RoutedRootEventBuffer>(
        context_,
        [this](const std::string &path) { return route_fact_file_id(path); },
        kRoutedGraphEventBudget, "declarations.headers");
    execution.metrics.note_whole_tu_traversal();
    profile::add_counter("root_traverse_decl_calls");
    graph_events_->collect(tu_);
  }

  void run_routed_declaration_stage(PassExecutionContext &execution) {
    if (!state_.graph_enabled || graph_events_ == nullptr) {
      return;
    }
    routed_fact_file_id_ = -1;
    BudgetedDeclarationPassPorts ports(
        static_cast<DeclarationPassPorts &>(recorder_), execution.metrics);
    BudgetedPresentationIntentEmitter presentation_intents(
        static_cast<PresentationIntentEmitter &>(recorder_), execution.metrics);
    BudgetedDefinitionScopeEmitter definitions(
        static_cast<DefinitionScopeEmitter &>(recorder_), execution.metrics);
    DeclarationEdgeVisitor decls(
        context_, ports, {}, -1, &definitions, &execution.metrics,
        &presentation_intents,
        [this](const std::string &path) { return route_fact_file(path); });
    graph_events_->replay_declarations(decls);
  }

  auto collect_routed_definitions(PassExecutionContext &execution)
      -> std::unique_ptr<FunctionDefinitionVisitor> {
    if (!state_.graph_enabled || graph_events_ == nullptr) {
      return nullptr;
    }
    routed_fact_file_id_ = -1;
    auto visitor = std::make_unique<FunctionDefinitionVisitor>(
        context_, static_cast<DeclarationIdentityResolver &>(recorder_),
        static_cast<DefinitionScopeEmitter &>(recorder_), std::string{}, -1,
        &execution.metrics,
        [this](const std::string &path) { return route_fact_file(path); });
    graph_events_->replay_definitions(*visitor);
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
    routed_fact_file_id_ = -1;
    BudgetedStatementFactPorts ports(
        static_cast<StatementFactPorts &>(recorder_), execution.metrics);
    BudgetedDefinitionScopeEmitter definitions(
        static_cast<DefinitionScopeEmitter &>(recorder_), execution.metrics);
    visitor.run_statement_pass(ports, &execution.metrics, &definitions);
    static_cast<void>(edge_ids);
    static_cast<void>(definition_ids);
  }

  void run_routed_namespace_stage(PassExecutionContext &execution) {
    if (!state_.graph_enabled || graph_events_ == nullptr) {
      return;
    }
    routed_fact_file_id_ = -1;
    BudgetedNamespacePassPorts ports(
        static_cast<NamespacePassPorts &>(recorder_), execution.metrics);
    NamespaceUseVisitor ns(
        context_, ports, {}, -1, &execution.metrics,
        [this](const std::string &path) { return route_fact_file(path); });
    graph_events_->replay_namespaces(ns);
    // The recorded stream is valid only while this ASTContext lives, and the
    // last consumer has just run.
    graph_events_.reset();
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
  auto read_header_file_row(const std::string &abs, double &persistence_seconds)
      -> std::optional<cidx::File> {
    const profile::ScopedAccumulator persistence(persistence_seconds);
    return db_.get_file(abs);
  }

  // The already-indexed test, answered from the row this loop already read.
  // Storage::is_file_indexed(path, nullopt, md5) is exactly this predicate over
  // a fresh get_file(path); nothing writes that row between the two reads, so
  // repeating the query per header per translation unit is pure cost.
  static bool
  header_is_already_indexed(const std::optional<cidx::File> &existing,
                            const std::optional<std::string> &current_md5,
                            bool covered_by_current_config) {
    if (!covered_by_current_config || !existing || !existing->indexed) {
      return false;
    }
    return !current_md5 || existing->md5 == current_md5;
  }

  std::vector<PendingHeader>
  discover_owned_headers(double &persistence_seconds) {
    // Classification is identical in both modes; only WHO answers "is this
    // header already someone else's job" differs. Serially the committed
    // database answers, because the previous translation unit has already
    // published. Under parallel extraction the database has not yet seen the
    // in-flight translation units, so the scheduler's ordered oracle answers
    // instead -- with exactly the answer the serial run would have given.
    std::vector<PendingHeader> candidates;
    std::vector<cidx::index::HeaderClaimCandidate> claims;
    std::unordered_set<std::string> seen;
    for (const IncludeFact &f : state_.includes.includes) {
      if (!f.resolved) {
        continue; // no file was opened: nothing to index
      }
      const std::string abs = cidx::pathutil::abspath(f.dst_path);
      if (!seen.insert(abs).second) {
        continue;
      }
      classify_included_header(f.dst_path, abs, persistence_seconds, candidates,
                               claims);
    }
    if (state_.claims == nullptr) {
      return candidates;
    }
    return resolve_header_claims(std::move(candidates), claims);
  }

  // One inclusion: system / unowned / already-current / candidate. Split out of
  // the discovery loop so neither function carries both the iteration and the
  // classification.
  void classify_included_header(
      const std::string &inc, const std::string &abs,
      double &persistence_seconds, std::vector<PendingHeader> &candidates,
      std::vector<cidx::index::HeaderClaimCandidate> &claims) {
    cidx::HeaderStats &counts = state_.out->headers;
    if (is_system_header(inc)) {
      ++counts.system;
      return;
    }
    if (state_.ownership == nullptr || state_.session_metrics == nullptr ||
        !state_.ownership->owns(abs, *state_.session_metrics)) {
      ++counts.unowned;
      return;
    }
    ++state_.session_metrics->file_hash_reads;
    const std::optional<std::string> current_md5 = cidx::md5_of(abs);
    const std::optional<cidx::File> existing =
        read_header_file_row(abs, persistence_seconds);
    const bool covered_by_current_config =
        header_covered_by_current_config(existing);
    const std::optional<std::string> parsed_md5 =
        parsed_file_md5(context_.getSourceManager(), abs);
    const bool already_in_database =
        current_md5 && parsed_md5 && current_md5 == parsed_md5 &&
        header_is_already_indexed(existing, current_md5,
                                  covered_by_current_config);
    // Serially the committed row is the whole answer. Under parallel
    // extraction the oracle still has to see the candidate, because it -- not
    // the database -- decides who owns it.
    if (already_in_database && state_.claims == nullptr) {
      ++counts.already;
      return;
    }
    ++state_.session_metrics->file_stat_reads;
    candidates.push_back(
        {.path = abs,
         .mtime = file_mtime(abs),
         .md5 = parsed_md5,
         .covered_by_current_config = covered_by_current_config,
         .stored = 0});
    if (state_.claims != nullptr) {
      claims.push_back({.path = abs,
                        .parsed_md5 = parsed_md5,
                        .already_indexed_in_database = already_in_database});
    }
  }

  // The parallel half of owned-header discovery, kept out of the classification
  // loop so neither function carries both concerns.
  //
  // Exactly one call per translation unit, and it blocks until every
  // lower-ranked translation unit has claimed. That ordered gate is what makes
  // the answer independent of completion order.
  std::vector<PendingHeader> resolve_header_claims(
      std::vector<PendingHeader> candidates,
      const std::vector<cidx::index::HeaderClaimCandidate> &claims) {
    const std::vector<bool> owned = state_.claims->claim(
        state_.rank, translation_unit_configuration_hash(), claims);
    state_.claimed = true;
    std::vector<PendingHeader> plan;
    plan.reserve(candidates.size());
    for (std::size_t i = 0; i < candidates.size(); ++i) {
      if (owned[i]) {
        plan.push_back(std::move(candidates[i]));
      } else {
        // Owned by a lower-ranked translation unit, or already current in the
        // database: either way this translation unit reuses that assignment and
        // reports it exactly as serial does.
        ++state_.out->headers.already;
      }
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
  FactBatchRecorder recorder_;
  SymbolEmitterAdapter symbol_emitter_;
  clang::Decl *tu_;
  std::vector<PendingHeader> pending_headers_;
  std::unordered_map<std::string, int64_t> routed_file_ids_;
  int64_t routed_symbol_file_id_ = -1;
  int64_t routed_fact_file_id_ = -1;
  int64_t main_file_handle_ = -1;
  std::unique_ptr<FunctionDefinitionVisitor> routed_definition_visitor_;
  // The one rooted graph traversal's recorded event stream, alive only between
  // its collection and the last replay of the same translation unit.
  std::unique_ptr<RoutedRootEventBuffer> graph_events_;
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
      case cidx::storage::FailurePoint::symbol_capture_complete:
        return IndexFailurePoint::symbol_capture_complete;
      case cidx::storage::FailurePoint::declaration_replay:
        return IndexFailurePoint::declaration_replay;
      case cidx::storage::FailurePoint::definition_replay:
        return IndexFailurePoint::definition_replay;
      case cidx::storage::FailurePoint::statement_body_replay:
        return IndexFailurePoint::statement_body_replay;
      case cidx::storage::FailurePoint::namespace_replay:
        return IndexFailurePoint::namespace_replay;
      case cidx::storage::FailurePoint::header_association:
        return IndexFailurePoint::header_association;
      case cidx::storage::FailurePoint::main_association:
        return IndexFailurePoint::main_association;
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
    case IndexFailurePoint::symbol_capture_complete:
      return "symbol capture completion";
    case IndexFailurePoint::declaration_replay:
      return "declaration replay";
    case IndexFailurePoint::definition_replay:
      return "definition replay";
    case IndexFailurePoint::statement_body_replay:
      return "statement body replay";
    case IndexFailurePoint::namespace_replay:
      return "namespace replay";
    case IndexFailurePoint::header_association:
      return "header association";
    case IndexFailurePoint::main_association:
      return "main association";
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
    data_version_ = db_.database_data_version();
    ++metrics_.generation;
    ++metrics_.snapshot_rebuilds;
    if (count_invalidation) {
      note_invalidation(reason);
    }
  }

  void refresh_external_state(const std::string &path, std::int64_t file_id) {
    const std::int64_t current_version = db_.database_data_version();
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
      if (found->second < 0) {
        if (const auto id =
                db_.translation_unit_config_id_by_hash(configuration_hash)) {
          configuration_id_cache_.insert_or_assign(configuration_hash, *id);
          return *id;
        }
      }
      return found->second;
    }
    ++metrics_.configuration_id_misses;
    const auto persisted =
        db_.translation_unit_config_id_by_hash(configuration_hash);
    std::int64_t id = -static_cast<std::int64_t>(
        stable_fact_hash(configuration_hash) & 0x3fff'ffff'ffff'ffffULL);
    if (id == 0) {
      id = -1;
    }
    if (persisted) {
      id = *persisted;
    }
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

TranslationUnitCacheInputs IndexSession::translation_unit_cache_inputs(
    const std::string &path, std::int64_t file_id,
    const std::optional<std::string> &source_md5, bool no_front_end_reuse) {
  const TranslationUnitDescriptor &descriptor =
      impl_->prepared_descriptor(path, file_id, source_md5);
  const TranslationUnitConfig resolved =
      configuration_without_source(descriptor);
  const FrontEndReusePlan reuse =
      plan_front_end_reuse(resolved, no_front_end_reuse);
  // The reuse identity already covers the normalized configuration; the
  // explicit enabled/disabled suffix keeps an explicitly disabled run in its
  // own cache slot even while ADR-014 selects the `none` mechanism for both.
  std::string clang_identity =
      "clang-cpp/" + std::to_string(clang_version_major());
  for (const std::optional<std::string> &field :
       {resolved.target, resolved.resource_dir, resolved.sysroot,
        resolved.language, resolved.standard, resolved.diagnostics_policy}) {
    clang_identity += '\x1f';
    clang_identity += field.value_or("");
  }
  for (const std::string &option : resolved.abi_options) {
    clang_identity += '\x1f';
    clang_identity += option;
  }
  std::vector<std::string> generated_inputs;
  generated_inputs.reserve(resolved.generated_inputs.size());
  for (const std::string &input : resolved.generated_inputs) {
    generated_inputs.push_back(pathutil::abspath(
        pathutil::join(resolved.working_dir.value_or("."), input)));
  }
  return {.workspace_identity = descriptor.workspace_identity,
          .source_identity = pathutil::normpath(pathutil::abspath(path)),
          .configuration_identity = descriptor.semantic_hash,
          .configuration_hash = translation_unit_config_hash(resolved),
          .front_end_reuse_identity =
              reuse.identity.version + ':' + reuse.identity.mechanism + ':' +
              reuse.identity.sha256 +
              (no_front_end_reuse ? ":disabled" : ":enabled"),
          .clang_identity = std::move(clang_identity),
          .environment = resolved.relevant_environment,
          .generated_inputs = std::move(generated_inputs),
          .configuration = resolved,
          .configuration_id = impl_->configuration_id(descriptor)};
}

void record_pass_timings(const IndexOneOutcome &out) {
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
}

void record_session_profile(
    const std::string &path, const IndexSessionMetrics &session_before,
    const IndexSessionMetrics &current, const IndexOneOutcome &out,
    const EngineState &state, std::uint64_t start_position,
    const std::pair<std::int64_t, std::int64_t> &cardinality_before,
    ProfileClock::time_point wall_started, std::clock_t cpu_started,
    double child_wall_before) {
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
}

void record_index_profile(
    bool profiling, const std::string &path,
    const IndexSessionMetrics &session_before,
    const IndexSessionMetrics &current, const IndexOneOutcome &out,
    const EngineState &state, std::uint64_t start_position,
    const std::pair<std::int64_t, std::int64_t> &cardinality_before,
    ProfileClock::time_point wall_started, std::clock_t cpu_started,
    double child_wall_before) {
  if (!profiling) {
    return;
  }
  record_pass_timings(out);
  record_session_profile(path, session_before, current, out, state,
                         start_position, cardinality_before, wall_started,
                         cpu_started, child_wall_before);
}

void execute_index_one_frontend(CompilationSetup &setup, EngineState &state,
                                IndexOneOutcome &out, bool profiling) {
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
}

struct IndexOneSetup {
  IndexOneSetup(cidx::Storage &db, const cidx::File &rec,
                const std::string &path, bool graph_enabled,
                IndexFailurePoint failure, IndexOneOutcome &out,
                TranslationUnitConfig resolved_config,
                TranslationUnitConfig publication_config,
                ComponentOwnershipIndex &ownership,
                IndexSessionMetrics &metrics)
      : resolved(std::move(resolved_config)),
        publication(std::move(publication_config)),
        setup(resolved.arguments, path), collector(out.diagnostics),
        injector(failure), ports(db, &injector),
        ast_ports{.workspace = ports.workspace_catalog_read(),
                  .source = ports.source_read(),
                  .source_write = ports.source_write(),
                  .symbols_read = ports.symbol_read(),
                  .symbols_write = ports.symbol_write(),
                  .types_write = ports.type_write(),
                  .facts_write = ports.fact_write(),
                  .definitions_write = ports.definition_write(),
                  .unit_of_work = ports.unit_of_work()} {
    setup.tool.setDiagnosticConsumer(&collector);
    config.tu_file_id = rec.id;
    config.driver = resolved.driver;
    config.working_dir = resolved.working_dir;
    config.arguments = resolved.arguments;
    config.lang_mode = resolved.language;
    config.resource_dir = resolved.resource_dir;
    state.db = &db;
    state.ports = &ast_ports;
    state.rec = &rec;
    state.path = path;
    state.graph_enabled = graph_enabled;
    state.strict = read_strict_mode();
    state.failure_injector = &injector;
    state.out = &out;
    state.config = &config;
    state.publication_config = &publication;
    state.ownership = &ownership;
    state.session_metrics = &metrics;
    state.requested_failure = failure;
  }

  TranslationUnitConfig resolved;
  TranslationUnitConfig publication;
  CompilationSetup setup;
  DiagCollector collector;
  PipelineFailureInjector injector;
  cidx::storage::SqliteStoragePorts ports;
  cidx::IncludeConfig config;
  cidx::storage::AstStoragePorts ast_ports;
  EngineState state;
};

IndexOneOutcome run_index_one(cidx::Storage &db, const cidx::File &rec,
                              const std::string &path, bool graph_enabled,
                              IndexFailurePoint failure,
                              bool no_front_end_reuse) {
  IndexSession session(db);
  return run_index_one(db, session, rec, path, graph_enabled, failure,
                       no_front_end_reuse);
}

void record_final_profile(
    IndexSession &session, IndexSessionMetrics &profiled_metrics,
    bool profiling, const std::string &path,
    const IndexSessionMetrics &session_before, const IndexOneOutcome &out,
    const EngineState &state, std::uint64_t start_position,
    const std::pair<std::int64_t, std::int64_t> &cardinality_before,
    ProfileClock::time_point wall_started, std::clock_t cpu_started,
    double child_wall_before) {
  if (!profiling) {
    return;
  }
  const IndexSessionMetrics current = session.metrics();
  profiled_metrics = current;
  record_index_profile(profiling, path, session_before, current, out, state,
                       start_position, cardinality_before, wall_started,
                       cpu_started, child_wall_before);
}

void note_index_one_rollback(bool profiling) {
  if (profiling) {
    profile::note_transaction_rollback();
  }
}

auto writer_failure(IndexFailurePoint failure)
    -> cidx::storage::FactBatchWriterFailurePoint {
  switch (failure) {
  case IndexFailurePoint::begin:
    return cidx::storage::FactBatchWriterFailurePoint::temporary_load;
  case IndexFailurePoint::adapter:
    return cidx::storage::FactBatchWriterFailurePoint::entity_apply;
  case IndexFailurePoint::commit:
    return cidx::storage::FactBatchWriterFailurePoint::commit;
  default:
    return cidx::storage::FactBatchWriterFailurePoint::none;
  }
}

void record_writer_profile(const cidx::storage::FactBatchWriterReport &report,
                           bool profiling) {
  if (!profiling) {
    return;
  }
  profile::add_counter("fact_batch_writer.statements_prepared",
                       report.statements_prepared);
  profile::add_counter("fact_batch_writer.statements_reused",
                       report.statements_reused);
  profile::add_counter("fact_batch_writer.statement_executions",
                       report.statement_executions);
  profile::add_counter("fact_batch_writer.virtual_machine_steps",
                       report.virtual_machine_steps);
  profile::add_timing("fact_batch_writer.prepare", report.prepare_seconds);
  profile::add_timing("fact_batch_writer.virtual_machine",
                      report.virtual_machine_seconds);
  profile::add_timing("fact_batch_writer.commit", report.commit_seconds);
  std::uint64_t staged = 0;
  std::uint64_t inserted = 0;
  std::uint64_t updated = 0;
  std::uint64_t ignored = 0;
  std::uint64_t deleted = 0;
  for (const auto &[_, rows] : report.families) {
    staged += rows.staged;
    inserted += rows.inserted;
    updated += rows.updated;
    ignored += rows.ignored;
    deleted += rows.deleted;
  }
  profile::add_counter("fact_batch_writer.rows_staged", staged);
  profile::add_counter("fact_batch_writer.rows_inserted", inserted);
  profile::add_counter("fact_batch_writer.rows_updated", updated);
  profile::add_counter("fact_batch_writer.rows_ignored", ignored);
  profile::add_counter("fact_batch_writer.rows_deleted", deleted);
}

bool prepare_front_end_reuse(const TranslationUnitConfig &resolved,
                             bool no_front_end_reuse, bool profiling,
                             IndexOneOutcome &out) {
  const std::vector<std::string> &args = resolved.arguments;
  const FrontEndReusePlan reuse_plan =
      plan_front_end_reuse(resolved, no_front_end_reuse);
  if (profiling) {
    profile::add_counter(
        "front_end_reuse.mechanism." + reuse_plan.identity.mechanism, 1);
    profile::add_counter("front_end_reuse.explicitly_disabled",
                         no_front_end_reuse ? 1 : 0);
    profile::add_counter("front_end_reuse.generated_artifacts", 0);
  }
  if (const auto diagnostic = preflight_build_declared_pch(resolved)) {
    out.parse_failed = true;
    out.error = *diagnostic;
    out.failed_flags = args;
    if (profiling) {
      profile::add_counter("front_end_reuse.build_declared_pch_diagnostics", 1);
    }
    return false;
  }
  return true;
}

// The publication half of finalize_index_one, kept separate so the serial
// in-place path and the extraction-only path differ by one call rather than by
// a branch threaded through the whole function.
void publish_extracted_batch(cidx::Storage &db, EngineState &state,
                             const ExtractedFactPublication &publication,
                             IndexOneOutcome &out, bool profiling) {
  cidx::storage::FactBatchWriter writer(db);
  const cidx::storage::FactBatchPublicationContext context{
      .route_plan = publication.route_plan,
      .translation_unit = publication.translation_unit,
      .expected_generation = publication.expected_generation,
      .source_is_current =
          [](const std::string &candidate,
             const PlannedSourceSnapshot &snapshot) {
            return SourceSnapshot{.mtime = snapshot.mtime, .md5 = snapshot.md5}
                .matches(candidate);
          },
      .configuration_id = publication.configuration_id,
      .configuration = publication.configuration,
      .failure = writer_failure(state.requested_failure)};
  const cidx::storage::FactBatchWriterResult result =
      writer.apply(publication.batch, context);
  record_writer_profile(result.report, profiling);
  if (result.ok()) {
    if (profiling) {
      profile::note_transaction_commit();
      profile::add_timing("commit", result.report.commit_seconds);
    }
    return;
  }
  out.dependency_facts = {};
  note_index_one_rollback(profiling);
  if (state.requested_failure != IndexFailurePoint::none) {
    throw std::runtime_error(
        result.error.value_or("injected FactBatch publication failure"));
  }
  if (result.error && (result.error->contains("readonly") ||
                       result.error->contains("read-only"))) {
    throw std::runtime_error(*result.error);
  }
  out.parse_failed = true;
  out.error = result.error.value_or("FactBatch publication failed");
}

IndexOneOutcome finalize_index_one(
    cidx::Storage &db, IndexSession &session, const std::string &path,
    const SourceSnapshot &source, const std::vector<std::string> &args,
    EngineState &state, IndexOneOutcome &out, bool profiling,
    const IndexSessionMetrics &session_before, IndexSessionMetrics &metrics,
    IndexSessionMetrics &profiled_metrics,
    const std::function<void(IndexInvalidationReason)> &rebuild,
    std::uint64_t start_position,
    const std::pair<std::int64_t, std::int64_t> &cardinality_before,
    ProfileClock::time_point wall_started, std::clock_t cpu_started,
    double child_wall_before, bool publish) {

  if (!state.tu_handled) {
    out.parse_failed = true;
    out.error = "cannot parse " + path;
    note_index_one_rollback(profiling);
    out.session_metrics = session.metrics();
    record_final_profile(session, profiled_metrics, profiling, path,
                         session_before, out, state, start_position,
                         cardinality_before, wall_started, cpu_started,
                         child_wall_before);
    return out;
  }
  const auto verification_started =
      profiling ? ProfileClock::now() : ProfileClock::time_point{};
  apply_diagnostic_policy(path, state.strict, args, out);
  ++metrics.source_change_checks;
  ++metrics.file_stat_reads;
  ++metrics.file_hash_reads;
  if (!source.matches(path)) {
    rebuild(IndexInvalidationReason::source_mutation);
    out.source_changed = true;
    out.error = path + ": source changed during indexing; retry required";
  } else if (out.source_changed && out.error.empty()) {
    out.error = path + ": source changed during indexing; retry required";
  }
  if (profiling) {
    profile::add_timing("verification", elapsed_seconds(verification_started));
  }
  if (!out.parse_failed && !out.source_changed) {
    if (!state.batch) {
      out.parse_failed = true;
      out.error = "FactBatch extraction produced no publication batch";
      note_index_one_rollback(profiling);
    } else if (state.route_plan.routes().empty()) {
      out.parse_failed = true;
      out.error = "FactBatch extraction produced no publication route";
      note_index_one_rollback(profiling);
    } else {
      const std::string translation_unit =
          state.route_plan.routes().front().translation_unit;
      out.publication = ExtractedFactPublication{
          .batch = *state.batch,
          .route_plan = state.route_plan,
          .translation_unit = translation_unit,
          .expected_generation = state.route_plan.generation(),
          .configuration_id = state.normalized_config_id,
          .configuration = *state.publication_config};
      // Dependency evidence travels with a committed publication only; the
      // failure branch below drops it so a rolled-back TU never leaves
      // evidence a cache entry could be built from.
      out.dependency_facts = state.includes;
      // Extraction-only stops here: the batch, its route plan and its
      // configuration are complete and immutable, and the SCHEDULER publishes
      // them through the single controlled writer in legacy apply order. A
      // worker that published here would make completion order into
      // persistence order.
      if (publish) {
        publish_extracted_batch(db, state, *out.publication, out, profiling);
      }
    }
  } else {
    note_index_one_rollback(profiling);
  }
  record_final_profile(session, profiled_metrics, profiling, path,
                       session_before, out, state, start_position,
                       cardinality_before, wall_started, cpu_started,
                       child_wall_before);
  out.session_metrics = session.metrics();
  return out;
}

IndexOneOutcome run_index_one(cidx::Storage &db, IndexSession &session,
                              const cidx::File &rec, const std::string &path,
                              bool graph_enabled,
                              const ExtractionControl &control) {
  const IndexFailurePoint failure = control.failure;
  const bool no_front_end_reuse = control.no_front_end_reuse;
  const IndexSessionMetrics session_before = session.impl_->profiled_metrics_;
  const bool profiling = profile::active();
  const auto wall_started =
      profiling ? ProfileClock::now() : ProfileClock::time_point{};
  const std::clock_t cpu_started = profiling ? std::clock() : 0;
  // The translation unit's position in the run, which the per-TU
  // cost-versus-corpus-position analysis is fitted against. Serially, "how
  // many units have been recorded so far" is that position. Under the bounded
  // parallel scheduler it is not: every worker starts before any has recorded,
  // so all of them would report position 0 and the analysis would have no
  // abscissa at all. The scheduler already knows the answer -- the dispatch
  // rank in legacy apply order -- and supplies it alongside the header claims.
  const std::uint64_t start_position = [&]() -> std::uint64_t {
    if (!profiling) {
      return 0;
    }
    if (control.claims != nullptr) {
      return static_cast<std::uint64_t>(control.rank);
    }
    return profile::next_translation_unit_position();
  }();
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
    cardinality_before = db.indexing_cardinality();
  }
  const auto workspace_started =
      profiling ? ProfileClock::now() : ProfileClock::time_point{};
  out.source_mtime = source.mtime;
  out.source_md5 = source.md5;
  const TranslationUnitDescriptor &descriptor =
      session.impl_->prepared_descriptor(path, rec.id, source.md5);
  TranslationUnitConfig resolved = configuration_without_source(descriptor);
  const TranslationUnitConfig publication_config = resolved;
  if (!prepare_front_end_reuse(resolved, no_front_end_reuse, profiling, out)) {
    return out;
  }
  if (profiling) {
    profile::add_timing("workspace_snapshot_configuration",
                        elapsed_seconds(workspace_started));
  }
  IndexOneSetup prepared(db, rec, path, graph_enabled, failure, out,
                         std::move(resolved), publication_config,
                         session.impl_->ownership(), session.impl_->metrics_);
  CompilationSetup &setup = prepared.setup;
  EngineState &state = prepared.state;
  const std::vector<std::string> &args = prepared.resolved.arguments;
  state.normalized_config_id = session.impl_->configuration_id(descriptor);
  state.claims = control.claims;
  state.rank = control.rank;

  if (profiling) {
    profile::note_transaction_begin();
  }

  execute_index_one_frontend(setup, state, out, profiling);
  IndexOneOutcome result = finalize_index_one(
      db, session, path, source, args, state, out, profiling, session_before,
      session.impl_->metrics_, session.impl_->profiled_metrics_,
      [&session](IndexInvalidationReason reason) {
        session.impl_->rebuild(reason);
      },
      start_position, cardinality_before, wall_started, cpu_started,
      child_wall_before, control.publish);
  // A translation unit that never reached owned-header discovery -- a failed
  // parse, a source that changed under us -- still holds its place in the
  // ordered gate. Release it here so its successors are not stalled by work
  // that will never claim.
  if (control.claims != nullptr && !state.claimed) {
    control.claims->release_unclaimed(control.rank);
  }
  return result;
}

IndexOneOutcome run_index_one(cidx::Storage &db, IndexSession &session,
                              const cidx::File &rec, const std::string &path,
                              bool graph_enabled, IndexFailurePoint failure,
                              bool no_front_end_reuse) {
  return run_index_one(
      db, session, rec, path, graph_enabled,
      ExtractionControl{.failure = failure,
                        .no_front_end_reuse = no_front_end_reuse});
}
} // namespace cidx::ast
