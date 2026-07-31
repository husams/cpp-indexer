#include "ast/index_engine.hpp"

#include "ast/declaration_edge_visitor.hpp"
#include "ast/display_name_rewrite.hpp"
#include "ast/function_definition_visitor.hpp"
#include "ast/include_facts.hpp"
#include "ast/location.hpp"
#include "ast/namespace_use_visitor.hpp"
#include "ast/pass_registry.hpp"
#include "ast/storage_edge_sink.hpp"
#include "ast/storage_symbol_sink.hpp"
#include "ast/symbol_visitor.hpp"

#include "catalogs/generated_catalog.hpp"
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
#include <memory>
#include <stdexcept>
#include <tuple>
#include <unordered_map>
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
  std::vector<PresentationIntent> presentation_intents;
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
           FactTrust trust = FactTrust::trusted) -> ExtractionPassDescriptor {
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
          .budget = {.max_visited_constructs = 1'000'000,
                     .max_emitted_facts = 2'000'000,
                     .max_diagnostics = 1'024,
                     .declared = true}};
    };
    registry.register_pass(
        descriptor("symbols.main", {FrontendCapability::ast}, {}, {"symbols"},
                   {}, PassScope::main_file, TraversalMode::declaration),
        [this](PassExecutionContext &execution) -> void {
          for (const cidx::Diagnostic &diagnostic : state_.out->diagnostics) {
            execution.metrics.note_diagnostic(diagnostic.spelling);
          }
          state_.out->stored =
              run_symbol_pass(state_.path, state_.rec->id, execution);
          main_symbol_ids_ = symbols_.symbol_ids();
        });
    registry.register_pass(
        descriptor("symbols.headers",
                   {FrontendCapability::ast, FrontendCapability::preprocessor},
                   {"includes"}, {"symbols"}, {"symbols.main"},
                   PassScope::owned_header, TraversalMode::declaration),
        [this](PassExecutionContext &execution) -> void {
          pending_headers_ = plan_owned_headers();
          for (PendingHeader &header : pending_headers_) {
            if (header.covered_by_current_config) {
              db_.delete_symbols_for_file(header.file_id);
            }
            header.stored =
                run_symbol_pass(header.path, header.file_id, execution);
            header.symbol_ids = symbols_.symbol_ids();
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
        descriptor(
            "declarations.headers", {FrontendCapability::ast},
            {"symbols", "fact_lifecycle"},
            {"relations", "types", "definitions", "presentation_intents"},
            {"symbols.headers", "lifecycle.headers"}, PassScope::owned_header,
            TraversalMode::declaration),
        [this](PassExecutionContext &execution) -> void {
          for (PendingHeader &header : pending_headers_) {
            run_declaration_stage(header.path, header.file_id, execution,
                                  &header.edge_ids, &header.definition_ids);
          }
        });
    registry.register_pass(
        descriptor("definitions.headers", {FrontendCapability::ast},
                   {"symbols"}, {"definitions"}, {"declarations.headers"},
                   PassScope::owned_header, TraversalMode::declaration),
        [this](PassExecutionContext &execution) -> void {
          header_definition_visitors_.clear();
          for (PendingHeader &header : pending_headers_) {
            collect_definitions(header_definition_visitors_, header.path,
                                header.file_id, execution, &header.edge_ids,
                                &header.definition_ids);
          }
        });
    registry.register_pass(
        descriptor("statements.headers",
                   {FrontendCapability::ast, FrontendCapability::templates},
                   {"definitions", "relations", "types"},
                   {"relations", "types", "evidence", "definitions", "symbols"},
                   {"definitions.headers"}, PassScope::owned_header,
                   TraversalMode::body, FactCompleteness::partial,
                   FactTrust::inferred),
        [this](PassExecutionContext &execution) -> void {
          for (std::size_t index = 0;
               index < header_definition_visitors_.size(); ++index) {
            run_statement_stage(*header_definition_visitors_[index], execution,
                                &pending_headers_[index].edge_ids,
                                &pending_headers_[index].definition_ids);
          }
        });
    registry.register_pass(
        descriptor("namespaces.headers", {FrontendCapability::ast},
                   {"symbols", "relations"}, {"relations"},
                   {"statements.headers"}, PassScope::owned_header,
                   TraversalMode::declaration),
        [this](PassExecutionContext &execution) -> void {
          for (PendingHeader &header : pending_headers_) {
            run_namespace_stage(header.path, header.file_id, execution,
                                &header.edge_ids, &header.definition_ids);
          }
        });
    auto header_association = descriptor(
        "headers.associate", {}, {"symbols", "relations", "definitions"},
        {"file_associations"},
        {"symbols.headers", "statements.headers", "namespaces.headers"},
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
            execution.metrics.note_emitted(db_.association_fact_count(
                header.file_id, header.symbol_ids, header.edge_ids,
                header.definition_ids));
            db_.associate_facts_for_file(
                header.file_id, state_.normalized_config_id, header.symbol_ids,
                header.edge_ids, header.definition_ids);
            db_.mark_file_indexed(header.file_id, header.mtime, header.md5);
            ++state_.out->headers.indexed;
            state_.out->headers.symbols += header.stored;
          }
        });
    registry.register_pass(descriptor("lifecycle.main", {}, {},
                                      {"fact_lifecycle"}, {"headers.associate"},
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
            {"headers.associate", "lifecycle.main"}, PassScope::main_file,
            TraversalMode::declaration),
        [this](PassExecutionContext &execution) -> void {
          run_declaration_stage(state_.path, state_.rec->id, execution,
                                &main_edge_ids_, &main_definition_ids_);
        });
    registry.register_pass(
        descriptor("definitions.main", {FrontendCapability::ast}, {"symbols"},
                   {"definitions"}, {"declarations.main"}, PassScope::main_file,
                   TraversalMode::declaration),
        [this](PassExecutionContext &execution) -> void {
          main_definition_visitors_.clear();
          collect_definitions(main_definition_visitors_, state_.path,
                              state_.rec->id, execution, &main_edge_ids_,
                              &main_definition_ids_);
        });
    registry.register_pass(
        descriptor("statements.main",
                   {FrontendCapability::ast, FrontendCapability::templates},
                   {"definitions", "relations", "types"},
                   {"relations", "types", "evidence", "definitions", "symbols"},
                   {"definitions.main"}, PassScope::main_file,
                   TraversalMode::body, FactCompleteness::partial,
                   FactTrust::inferred),
        [this](PassExecutionContext &execution) -> void {
          for (const auto &visitor : main_definition_visitors_) {
            run_statement_stage(*visitor, execution, &main_edge_ids_,
                                &main_definition_ids_);
          }
        });
    registry.register_pass(
        descriptor("namespaces.main", {FrontendCapability::ast},
                   {"symbols", "relations"}, {"relations"}, {"statements.main"},
                   PassScope::main_file, TraversalMode::declaration),
        [this](PassExecutionContext &execution) -> void {
          run_namespace_stage(state_.path, state_.rec->id, execution,
                              &main_edge_ids_, &main_definition_ids_);
        });
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
        });
    auto main_association = descriptor(
        "main.associate", {}, {"symbols", "relations", "definitions"},
        {"file_associations"},
        {"symbols.main", "lifecycle.main", "statements.main",
         "namespaces.main"},
        PassScope::main_file, TraversalMode::lifecycle);
    registry.register_pass(
        std::move(main_association),
        [this](PassExecutionContext &execution) -> void {
          execution.metrics.note_emitted(
              db_.association_fact_count(state_.rec->id, main_symbol_ids_,
                                         main_edge_ids_, main_definition_ids_));
          db_.associate_facts_for_file(
              state_.rec->id, state_.normalized_config_id, main_symbol_ids_,
              main_edge_ids_, main_definition_ids_);
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
          const IncludeFactCounts counts =
              include_fact_count(db_, state_.includes);
          execution.metrics.note_duplicate(counts.duplicates);
          execution.metrics.note_emitted(counts.emitted_facts);
          persist_include_facts(db_, state_.includes, *state_.config);
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
        });

    IndexingPlan plan;
    plan.add("symbols.main");
    plan.add("symbols.headers");
    plan.add("lifecycle.headers");
    plan.add("declarations.headers");
    plan.add("definitions.headers");
    plan.add("statements.headers");
    plan.add("namespaces.headers");
    plan.add("headers.associate");
    plan.add("lifecycle.main");
    plan.add("declarations.main");
    plan.add("definitions.main");
    plan.add("statements.main");
    plan.add("namespaces.main");
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
    for (const PassExecutionRecord &pass : report.passes) {
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

  using DefinitionVisitors =
      std::vector<std::unique_ptr<FunctionDefinitionVisitor>>;

  int run_symbol_pass(const std::string &file, int64_t file_id,
                      PassExecutionContext &execution) {
    symbols_.set_current_file_id(file_id);
    symbols_.set_identity_translation_unit_config_id(
        state_.normalized_config_id, state_.rec->id);
    symbols_.reset_counters();
    symbols_.set_metrics(&execution.metrics);
    SymbolVisitor visitor(context_, symbols_, file, &execution.metrics);
    visitor.TraverseDecl(tu_);
    return symbols_.stored_count();
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

  void run_declaration_stage(const std::string &file, int64_t file_id,
                             PassExecutionContext &execution,
                             std::vector<int64_t> *edge_ids,
                             std::vector<int64_t> *definition_ids) {
    if (!state_.graph_enabled) {
      return;
    }
    configure_fact_file(file_id, false);
    edges_.reset_fact_ids();
    BudgetedDeclarationPassPorts ports(
        static_cast<DeclarationPassPorts &>(edges_), execution.metrics);
    BudgetedPresentationIntentEmitter presentation_intents(
        static_cast<PresentationIntentEmitter &>(edges_), execution.metrics);
    BudgetedDefinitionScopeEmitter definitions(
        static_cast<DefinitionScopeEmitter &>(edges_), execution.metrics);
    DeclarationEdgeVisitor decls(context_, ports, file, file_id, &definitions,
                                 &execution.metrics, &presentation_intents);
    decls.TraverseDecl(tu_);
    if (edge_ids != nullptr) {
      append_fact_ids(*edge_ids, edges_.edge_ids());
    }
    if (definition_ids != nullptr) {
      append_fact_ids(*definition_ids, edges_.definition_ids());
    }
  }

  void collect_definitions(DefinitionVisitors &out, const std::string &file,
                           int64_t file_id, PassExecutionContext &execution,
                           std::vector<int64_t> *edge_ids,
                           std::vector<int64_t> *definition_ids) {
    if (!state_.graph_enabled) {
      return;
    }
    configure_fact_file(file_id, false);
    edges_.reset_fact_ids();
    auto visitor = std::make_unique<FunctionDefinitionVisitor>(
        context_, static_cast<DeclarationIdentityResolver &>(edges_),
        static_cast<DefinitionScopeEmitter &>(edges_), file, file_id,
        &execution.metrics);
    visitor->TraverseDecl(tu_);
    if (edge_ids != nullptr) {
      append_fact_ids(*edge_ids, edges_.edge_ids());
    }
    if (definition_ids != nullptr) {
      append_fact_ids(*definition_ids, edges_.definition_ids());
    }
    out.push_back(std::move(visitor));
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

  void run_namespace_stage(const std::string &file, int64_t file_id,
                           PassExecutionContext &execution,
                           std::vector<int64_t> *edge_ids,
                           std::vector<int64_t> *definition_ids) {
    if (!state_.graph_enabled) {
      return;
    }
    configure_fact_file(file_id, false);
    edges_.reset_fact_ids();
    BudgetedNamespacePassPorts ports(static_cast<NamespacePassPorts &>(edges_),
                                     execution.metrics);
    NamespaceUseVisitor ns(context_, ports, file, file_id, &execution.metrics);
    ns.TraverseDecl(tu_);
    if (edge_ids != nullptr) {
      append_fact_ids(*edge_ids, edges_.edge_ids());
    }
    if (definition_ids != nullptr) {
      append_fact_ids(*definition_ids, edges_.definition_ids());
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
  std::vector<int64_t> main_symbol_ids_;
  std::vector<int64_t> main_edge_ids_;
  std::vector<int64_t> main_definition_ids_;
  DefinitionVisitors header_definition_visitors_;
  DefinitionVisitors main_definition_visitors_;
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

class IndexSession::Impl {
public:
  Impl(cidx::Storage &db, cidx::Logger &log)
      : db_(db), workspace_data_(db), toolchain_(log) {
    rebuild();
  }

  void rebuild() {
    descriptor_cache_.clear();
    configuration_id_cache_.clear();
    context_ = std::make_unique<WorkspaceContext>(WorkspaceContext::borrow(
        workspace_data_, WorkspaceReadWriteMode::read_write));
    resolver_ = std::make_unique<TranslationUnitConfigurationService>(
        *context_, toolchain_);
    ++metrics_.generation;
    ++metrics_.snapshot_rebuilds;
    ++metrics_.component_scans;
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
    if (const auto found =
            configuration_id_cache_.find(descriptor.semantic_hash);
        found != configuration_id_cache_.end()) {
      ++metrics_.configuration_id_hits;
      return found->second;
    }
    ++metrics_.configuration_id_misses;
    const int64_t id =
        db_.add_translation_unit_config(descriptor.configuration);
    configuration_id_cache_.emplace(descriptor.semantic_hash, id);
    return id;
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
  IndexSessionMetrics metrics_;
};

IndexSession::IndexSession(cidx::Storage &db, cidx::Logger &log)
    : impl_(std::make_unique<Impl>(db, log)) {}
IndexSession::~IndexSession() = default;
IndexSession::IndexSession(IndexSession &&) noexcept = default;
IndexSession &IndexSession::operator=(IndexSession &&) noexcept = default;

void IndexSession::invalidate() { impl_->rebuild(); }

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
  IndexOneOutcome out;
  const SourceSnapshot source = SourceSnapshot::capture(path);
  ++session.impl_->metrics_.file_hash_reads;
  out.source_md5 = source.md5;
  const TranslationUnitDescriptor &descriptor = session.impl_->descriptor(path);
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
  state.normalized_config_id = session.impl_->configuration_id(descriptor);

  IndexFrontendActionFactory factory(state);
  try {
    (void)setup.tool.run(&factory);
  } catch (const PassBudgetExceeded &error) {
    out.parse_failed = true;
    out.error = error.what();
  }
  if (!state.tu_handled) {
    out.parse_failed = true;
    out.error = "cannot parse " + path;
    out.session_metrics = session.metrics();
    return out;
  }
  apply_diagnostic_policy(path, state.strict, args, out);
  ++session.impl_->metrics_.source_change_checks;
  ++session.impl_->metrics_.file_hash_reads;
  if (!source.matches(path)) {
    out.source_changed = true;
    out.error = path + ": source changed during indexing; retry required";
  } else if (out.source_changed && out.error.empty()) {
    out.error = path + ": source changed during indexing; retry required";
  }
  if (!out.parse_failed && !out.source_changed) {
    state.unit->commit();
  }
  out.session_metrics = session.metrics();
  return out;
}

} // namespace cidx::ast
