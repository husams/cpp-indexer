// Focused contracts for the HSE-63 extraction registry and typed recorders.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "ast/declaration_edge_visitor.hpp"
#include "ast/fact_batch.hpp"
#include "ast/function_definition_visitor.hpp"
#include "ast/instantiation_edges.hpp"
#include "ast/location.hpp"
#include "ast/namespace_use_visitor.hpp"
#include "ast/pass_registry.hpp"
#include "ast/routed_root_events.hpp"
#include "ast/statement_edge_visitor.hpp"
#include "ast/symbol_emitter.hpp"
#include "ast/symbol_visitor.hpp"
#include "ast/usr.hpp"

#include "clang/AST/ASTConsumer.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/Analysis/CFG.h"
#include "clang/Frontend/FrontendAction.h"
#include "clang/Tooling/Tooling.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace cidx::ast;

namespace {

ExtractionPassDescriptor
valid_descriptor(std::string id, std::vector<std::string> dependencies = {}) {
  return {.id = std::move(id),
          .version = 1,
          .required_capabilities = {FrontendCapability::ast},
          .consumed_fact_families = {"input"},
          .produced_fact_families = {"facts"},
          .catalog_versions = {1},
          .dependencies = std::move(dependencies),
          .scope = PassScope::translation_unit,
          .traversal = TraversalMode::declaration,
          .completeness = FactCompleteness::complete,
          .trust = FactTrust::trusted,
          .budget = {.max_visited_constructs = 10,
                     .max_emitted_facts = 10,
                     .max_diagnostics = 10,
                     .declared = true}};
}

struct StatementRecordingResult {
  FactBatch batch;
  PassMetrics::FactFamily relation_metrics;
  PassMetrics::FactFamily symbol_metrics;
  PassMetrics::FactFamily type_metrics;
  bool found = false;
  std::size_t emitted = 0;
};

// This recorder intentionally implements only the focused statement port.
// In particular it has no presentation-normalization or lifecycle methods;
// the parsed-AST test therefore catches accidental widening of the body pass.
class MinimalStatementRecorder final : public StatementFactPorts {
public:
  void emit(const SymbolRecord &symbol) { backend_.emit(symbol); }
  void emit(const EvidenceRecord &evidence) override {
    backend_.emit(evidence);
  }

  auto lookup_symbol_id(const std::string &usr,
                        const std::optional<std::string> &source = std::nullopt)
      -> std::optional<std::int64_t> override {
    return backend_.lookup_symbol_id(usr, source);
  }
  auto mint_symbol(const MintRequest &request) -> std::int64_t override {
    return backend_.mint_symbol(request);
  }
  auto file_id_for_path(const std::string &path)
      -> std::optional<std::int64_t> override {
    return backend_.file_id_for_path(path);
  }
  auto type_arg_candidates(const std::string &name, bool qualified)
      -> std::vector<TypeArgCandidate> override {
    return backend_.type_arg_candidates(name, qualified);
  }
  auto symbol_ids_by_qual_name_kind(const std::string &qual_name,
                                    const std::string &kind_name)
      -> std::vector<std::int64_t> override {
    return backend_.symbol_ids_by_qual_name_kind(qual_name, kind_name);
  }
  auto add_edge(const EdgeRecord &edge) -> std::int64_t override {
    return backend_.add_edge(edge);
  }
  auto ensure_edge(const EdgeRecord &edge) -> std::int64_t override {
    return backend_.ensure_edge(edge);
  }
  void add_edge_site(const EdgeSiteRecord &site) override {
    backend_.add_edge_site(site);
  }
  void add_call_arg(const CallArgRecord &arg) override {
    backend_.add_call_arg(arg);
  }
  void add_template_param(const TemplateParamRecord &param) override {
    backend_.add_template_param(param);
  }
  void add_template_arg(const TemplateArgRecord &arg) override {
    backend_.add_template_arg(arg);
  }
  auto intern_type_node(const TypeNodeRecord &node) -> std::int64_t override {
    return backend_.intern_type_node(node);
  }
  void add_type_edge(std::int64_t src_id, std::int64_t kind,
                     std::int64_t position, std::int64_t dst_id) override {
    backend_.add_type_edge(src_id, kind, position, dst_id);
  }
  void
  replace_parameters(std::int64_t owner_id,
                     const std::vector<ParameterRecord> &parameters) override {
    backend_.replace_parameters(owner_id, parameters);
  }
  void add_symbol_type(std::int64_t symbol_id, std::int64_t kind,
                       std::int64_t type_id) override {
    backend_.add_symbol_type(symbol_id, kind, type_id);
  }

  [[nodiscard]] auto canonical_batch() const -> FactBatch {
    return backend_.canonical_batch();
  }

private:
  FactBatchRecorder backend_{"statement-pass"};
};

class StatementRecordingConsumer final : public clang::ASTConsumer {
public:
  explicit StatementRecordingConsumer(StatementRecordingResult &result)
      : result_(result) {}

  void HandleTranslationUnit(clang::ASTContext &context) override {
    const auto *translation_unit = context.getTranslationUnitDecl();
    const clang::FunctionDecl *callee = nullptr;
    const clang::FunctionDecl *caller = nullptr;
    for (const clang::Decl *decl : translation_unit->decls()) {
      const auto *function = llvm::dyn_cast<clang::FunctionDecl>(decl);
      if (function == nullptr) {
        continue;
      }
      if (function->getNameAsString() == "callee") {
        callee = function;
      } else if (function->getNameAsString() == "caller" &&
                 function->doesThisDeclarationHaveABody()) {
        caller = function;
      }
    }
    if (callee == nullptr || caller == nullptr) {
      return;
    }

    MinimalStatementRecorder recorder;
    recorder.emit(SymbolRecord{.file = "test.cpp",
                               .usr = usr_for_decl(caller),
                               .spelling = "caller",
                               .kind = 1});
    const auto caller_id = recorder.lookup_symbol_id(usr_for_decl(caller));
    if (!caller_id) {
      return;
    }
    PassMetrics metrics;
    metrics.bind("statement-test", PassBudget{.declared = true});
    BudgetedStatementFactPorts ports(recorder, metrics);
    StatementEdgeVisitor visitor(context, ports, *caller_id, 1, "test.cpp",
                                 &metrics);
    visitor.walk(caller);
    result_.batch = recorder.canonical_batch();
    result_.relation_metrics = metrics.fact_families.at("relations");
    result_.symbol_metrics = metrics.fact_families.at("symbols");
    result_.type_metrics = metrics.fact_families.at("types");
    result_.emitted = metrics.emitted_facts;
    result_.found = true;
  }

private:
  StatementRecordingResult &result_;
};

class StatementRecordingAction final : public clang::ASTFrontendAction {
public:
  explicit StatementRecordingAction(StatementRecordingResult &result)
      : result_(result) {}

  std::unique_ptr<clang::ASTConsumer>
  CreateASTConsumer(clang::CompilerInstance &, llvm::StringRef) override {
    return std::make_unique<StatementRecordingConsumer>(result_);
  }

private:
  StatementRecordingResult &result_;
};

} // namespace

TEST_CASE("pass registry preserves explicit dependencies and records metrics") {
  ExtractionPassRegistry registry;
  registry.register_pass(
      [] {
        auto descriptor = valid_descriptor("symbols");
        descriptor.version = 2;
        descriptor.produced_fact_families = {"symbols"};
        return descriptor;
      }(),
      [](PassExecutionContext &context) {
        context.metrics.note_visited(3);
        context.metrics.note_emitted(2);
      });
  auto relations = valid_descriptor("relations", {"symbols"});
  relations.produced_fact_families = {"relations"};
  registry.register_pass(relations, [](PassExecutionContext &context) {
    context.metrics.note_visited();
    context.metrics.note_emitted();
  });

  IndexingPlan plan;
  plan.add("symbols");
  plan.add("relations");
  FrontendSession session;
  session.ast_context = reinterpret_cast<clang::ASTContext *>(1);
  const PassExecutionReport report = registry.run(plan, &session);

  REQUIRE(report.passes.size() == 2);
  CHECK(report.passes[0].descriptor.stable_key().starts_with(
      "id=7:symbols|version=2"));
  CHECK(report.passes[0].metrics.visited_constructs == 3);
  CHECK(report.passes[1].metrics.emitted_facts == 1);
}

TEST_CASE("pass registry rejects an order that violates dependencies") {
  ExtractionPassRegistry registry;
  registry.register_pass(valid_descriptor("dependent", {"base"}),
                         [](PassExecutionContext &) {});
  registry.register_pass(valid_descriptor("base"),
                         [](PassExecutionContext &) {});
  IndexingPlan plan;
  plan.add("dependent");
  plan.add("base");
  CHECK_THROWS_AS(static_cast<void>(registry.run(plan)), std::invalid_argument);
}

TEST_CASE("fact batches canonicalize traversal order and remove duplicates") {
  FactBatchRecorder recorder("statement-pass");
  recorder.emit(SymbolRecord{.file = "b.cpp", .usr = "usr-b", .spelling = "b"});
  recorder.emit(SymbolRecord{.file = "a.cpp", .usr = "usr-a", .spelling = "a"});
  recorder.emit(SymbolRecord{.file = "a.cpp", .usr = "usr-a", .spelling = "a"});
  const auto a_id = recorder.lookup_symbol_id("usr-a");
  const auto b_id = recorder.lookup_symbol_id("usr-b");
  REQUIRE(a_id);
  REQUIRE(b_id);
  recorder.add_edge({.src_id = *b_id, .dst_id = *a_id, .kind = 1});
  recorder.add_edge({.src_id = *b_id, .dst_id = *a_id, .kind = 1});

  const FactBatch batch = recorder.canonical_batch();
  REQUIRE(batch.symbols.size() == 2);
  CHECK(batch.symbols[0].usr == "usr-a");
  REQUIRE(batch.relations.size() == 1);
  CHECK(batch.relations[0].count == 2);
}

TEST_CASE("pass descriptors require metadata and bind every budget") {
  ExtractionPassRegistry missing_metadata;
  CHECK_THROWS_AS(missing_metadata.register_pass({.id = "missing"},
                                                 [](PassExecutionContext &) {}),
                  std::invalid_argument);

  auto run_over_budget = [](const PassBudget &budget, auto record) {
    ExtractionPassRegistry registry;
    auto descriptor = valid_descriptor("bounded");
    descriptor.budget = budget;
    registry.register_pass(descriptor, record);
    IndexingPlan plan;
    plan.add("bounded");
    FrontendSession session;
    session.ast_context = reinterpret_cast<clang::ASTContext *>(1);
    CHECK_THROWS_AS(static_cast<void>(registry.run(plan, &session)),
                    PassBudgetExceeded);
  };

  run_over_budget(
      {.max_visited_constructs = 1,
       .max_emitted_facts = 10,
       .max_diagnostics = 10,
       .declared = true},
      [](PassExecutionContext &context) { context.metrics.note_visited(2); });
  run_over_budget(
      {.max_visited_constructs = 10,
       .max_emitted_facts = 1,
       .max_diagnostics = 10,
       .declared = true},
      [](PassExecutionContext &context) { context.metrics.note_emitted(2); });
  run_over_budget({.max_visited_constructs = 10,
                   .max_emitted_facts = 10,
                   .max_diagnostics = 1,
                   .declared = true},
                  [](PassExecutionContext &context) {
                    context.metrics.note_diagnostic("first");
                    context.metrics.note_diagnostic("second");
                  });
  run_over_budget({.max_visited_constructs = 10,
                   .max_emitted_facts = 10,
                   .max_diagnostics = 10,
                   .max_whole_tu_traversals = 0,
                   .declared = true},
                  [](PassExecutionContext &context) {
                    context.metrics.note_whole_tu_traversal();
                  });
}

TEST_CASE(
    "pass registry rejects unavailable frontend capabilities before running") {
  ExtractionPassRegistry registry;
  auto descriptor = valid_descriptor("cfg-pass");
  descriptor.required_capabilities = {FrontendCapability::cfg};
  bool ran = false;
  registry.register_pass(descriptor,
                         [&](PassExecutionContext &) { ran = true; });
  IndexingPlan plan;
  plan.add("cfg-pass");
  CHECK_THROWS_AS(static_cast<void>(registry.run(plan)),
                  FrontendSessionRequired);
  CHECK(!ran);
  FrontendSession session;
  session.ast_context = reinterpret_cast<clang::ASTContext *>(1);
  CHECK(!session.supports(FrontendCapability::templates));
  CHECK(!session.supports(FrontendCapability::cfg));
  CHECK_THROWS_AS(static_cast<void>(registry.run(plan, &session)),
                  FrontendCapabilityUnavailable);
  CHECK(!ran);

  session.cfg_builder =
      [](const clang::FunctionDecl *) -> std::unique_ptr<clang::CFG> {
    return nullptr;
  };
  session.template_arguments =
      [](const clang::FunctionDecl *) -> const clang::TemplateArgumentList * {
    return nullptr;
  };
  CHECK(session.supports(FrontendCapability::templates));
  CHECK_NOTHROW(static_cast<void>(registry.run(plan, &session)));
  CHECK(ran);
}

TEST_CASE("pass registry rejects every unavailable frontend capability") {
  for (const FrontendCapability capability :
       {FrontendCapability::ast, FrontendCapability::preprocessor,
        FrontendCapability::cfg, FrontendCapability::templates}) {
    ExtractionPassRegistry registry;
    auto descriptor =
        valid_descriptor("missing-capability-" +
                         std::to_string(static_cast<unsigned>(capability)));
    descriptor.required_capabilities = {capability};
    bool ran = false;
    registry.register_pass(descriptor,
                           [&](PassExecutionContext &) { ran = true; });
    IndexingPlan plan;
    plan.add(descriptor.id);
    FrontendSession session;
    CHECK_THROWS_AS(static_cast<void>(registry.run(plan, &session)),
                    FrontendCapabilityUnavailable);
    CHECK(!ran);
  }
}

TEST_CASE("pass registry permits an explicitly capability-free pass") {
  ExtractionPassRegistry registry;
  auto descriptor = valid_descriptor("capability-free");
  descriptor.required_capabilities.clear();
  bool ran = false;
  registry.register_pass(descriptor, [&](PassExecutionContext &context) {
    CHECK(context.session == nullptr);
    ran = true;
  });
  IndexingPlan plan;
  plan.add(descriptor.id);
  CHECK_NOTHROW(static_cast<void>(registry.run(plan)));
  CHECK(ran);
}

TEST_CASE("pass stable keys include the complete descriptor contract") {
  const ExtractionPassDescriptor base = valid_descriptor("contract");
  auto catalog_changed = base;
  catalog_changed.catalog_versions = {2};
  CHECK(base.stable_key() != catalog_changed.stable_key());
  auto dependency_changed = base;
  dependency_changed.dependencies = {"other"};
  CHECK(base.stable_key() != dependency_changed.stable_key());
  auto input_changed = base;
  input_changed.consumed_fact_families = {"other-input"};
  CHECK(base.stable_key() != input_changed.stable_key());
  auto contract_changed = base;
  contract_changed.completeness = FactCompleteness::partial;
  contract_changed.trust = FactTrust::inferred;
  contract_changed.budget.max_emitted_facts = 11;
  CHECK(base.stable_key() != contract_changed.stable_key());
}

TEST_CASE("fact batch IDs and references are traversal-order independent") {
  const auto record = [](bool reverse) {
    FactBatchRecorder recorder("statement-pass");
    const SymbolRecord a{
        .file = "test.cpp", .usr = "usr-a", .spelling = "a", .kind = 1};
    const SymbolRecord b{
        .file = "test.cpp", .usr = "usr-b", .spelling = "b", .kind = 1};
    if (reverse) {
      recorder.emit(b);
      recorder.emit(a);
    } else {
      recorder.emit(a);
      recorder.emit(b);
    }
    const auto a_id = recorder.lookup_symbol_id("usr-a");
    const auto b_id = recorder.lookup_symbol_id("usr-b");
    REQUIRE(a_id);
    REQUIRE(b_id);
    recorder.add_edge({.src_id = *a_id, .dst_id = *b_id, .kind = 7});
    return recorder.canonical_batch();
  };

  const FactBatch forward = record(false);
  const FactBatch reverse = record(true);
  REQUIRE(forward.symbols.size() == reverse.symbols.size());
  REQUIRE(forward.relations.size() == reverse.relations.size());
  CHECK(forward.symbols[0].usr == reverse.symbols[0].usr);
  CHECK(forward.symbols[1].usr == reverse.symbols[1].usr);
  CHECK(forward.relations[0].src_id == reverse.relations[0].src_id);
  CHECK(forward.relations[0].dst_id == reverse.relations[0].dst_id);
}

TEST_CASE("fact batch keeps conflicting symbol semantics lossless") {
  FactBatchRecorder recorder("declaration-pass");
  recorder.emit(SymbolRecord{.file = "test.cpp",
                             .usr = "usr-conflict",
                             .spelling = "f",
                             .kind = 1,
                             .display_name = std::string("f()")});
  recorder.emit(SymbolRecord{.file = "test.cpp",
                             .usr = "usr-conflict",
                             .spelling = "f",
                             .kind = 2,
                             .display_name = std::string("f(int)")});
  const FactBatch batch = recorder.canonical_batch();
  REQUIRE(batch.symbols.size() == 2);
  CHECK(batch.symbols[0].kind != batch.symbols[1].kind);
  CHECK(batch.symbols[0].display_name != batch.symbols[1].display_name);
}

TEST_CASE("statement pass records calls from a parsed AST") {
  StatementRecordingResult result;
  const bool ran = clang::tooling::runToolOnCode(
      std::make_unique<StatementRecordingAction>(result),
      "int callee(int value) { return value; }\n"
      "void caller() { (void)callee(1); }\n",
      "test.cpp");

  REQUIRE(ran);
  REQUIRE(result.found);
  CHECK(std::ranges::any_of(result.batch.relations, [](const EdgeRecord &edge) {
    return edge.kind == 1;
  }));
  CHECK(std::ranges::any_of(
      result.batch.edge_sites,
      [](const EdgeSiteRecord &site) { return site.line == 2; }));
  CHECK(result.emitted == 6);
  CHECK(result.batch.symbols.size() == 2);
  CHECK(result.batch.parameters.size() == 1);
  CHECK(result.batch.type_nodes.size() == 1);
  CHECK(result.batch.symbol_types.size() == 1);
  CHECK(result.relation_metrics.attempted == 2);
  CHECK(result.relation_metrics.persisted == 2);
  CHECK(result.symbol_metrics.attempted == 1);
  CHECK(result.symbol_metrics.persisted == 1);
  CHECK(result.type_metrics.attempted == 3);
  CHECK(result.type_metrics.persisted == 3);
}

TEST_CASE("focused ports are independently usable by a fact recorder") {
  FactBatchRecorder recorder("call-pass");
  SymbolFactEmitter &symbols = recorder;
  RelationFactEmitter &relations = recorder;
  EvidenceEmitter &evidence = recorder;

  symbols.emit(SymbolRecord{.file = "tu.cpp", .usr = "u", .spelling = "f"});
  const auto edge_id =
      relations.add_edge({.src_id = 1, .dst_id = 2, .kind = 1});
  evidence.emit(EvidenceRecord{.producer = "call-pass",
                               .construct = "CallExpr",
                               .file = "tu.cpp",
                               .line = 4,
                               .col = 2});

  CHECK(edge_id > 0);
  CHECK(recorder.batch().evidence.size() == 1);
  CHECK(recorder.batch().evidence.front().construct == "CallExpr");
}

// --- T-109: bounded routed root-event replay contract ------------------------
//
// The four routed root passes each run their own whole-TU RecursiveASTVisitor.
// These cases pin the contract that lets one recorded event stream drive the
// same non-recursive handlers: the replayed facts must be byte-identical to the
// standalone walks, the recorder must apply S-071's routing exclusions, and it
// must fail with a named pass-budget diagnostic instead of growing unbounded.
namespace {

// The routed set S-071 builds in prepare_routed_files(): the main file plus the
// owned headers this TU is about to index. Anything else — a system header, a
// file outside every component, a header another TU already indexed — is simply
// absent from that map, so the router answers nothing for it.
constexpr std::int64_t kRoutedMainFileId = 11;
constexpr std::int64_t kRoutedOwnedAFileId = 22;
constexpr std::int64_t kRoutedOwnedBFileId = 33;

std::optional<std::int64_t> route_fixture_file(const std::string &path) {
  if (path.ends_with("routed_main.cpp")) {
    return kRoutedMainFileId;
  }
  if (path.ends_with("owned_a.hpp")) {
    return kRoutedOwnedAFileId;
  }
  if (path.ends_with("owned_b.hpp")) {
    return kRoutedOwnedBFileId;
  }
  // unowned.hpp (outside every component), already_indexed.hpp (owned by an
  // earlier TU) and every system header stay unrouted.
  return std::nullopt;
}

std::string basename_of(const std::string &path) {
  const auto slash = path.find_last_of('/');
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

class BatchSymbolEmitter final : public SymbolEmitter {
public:
  explicit BatchSymbolEmitter(FactBatchRecorder &recorder)
      : recorder_(recorder) {}
  void emit(const SymbolRecord &symbol) override { recorder_.emit(symbol); }

private:
  FactBatchRecorder &recorder_;
};

// A deterministic, EMISSION-ORDERED rendering of everything the four root
// passes produced. Canonicalization is deliberately skipped: the point is to
// compare the ordered handler outcome, not a sorted set.
std::string render_batch(const FactBatch &batch) {
  std::ostringstream out;
  for (const SymbolRecord &symbol : batch.symbols) {
    out << "symbol\t" << basename_of(symbol.file) << '\t' << symbol.usr << '\t'
        << symbol.kind << '\t' << symbol.line << '\t' << symbol.col << '\t'
        << symbol.end_line << '\t' << symbol.end_col << '\t'
        << symbol.decl_line.value_or(-1) << '\t' << symbol.decl_col.value_or(-1)
        << '\t' << symbol.is_definition << symbol.is_instantiation << '\n';
  }
  for (const EdgeRecord &edge : batch.relations) {
    out << "edge\t" << edge.src_id << '\t' << edge.dst_id << '\t' << edge.kind
        << '\t' << edge.count << '\n';
  }
  for (const EdgeSiteRecord &site : batch.edge_sites) {
    out << "edge_site\t" << site.edge_id << '\t' << site.file_id << '\t'
        << site.line << '\t' << site.col << '\n';
  }
  for (const DefinitionFactRecord &definition : batch.definitions) {
    out << "definition\t" << definition.symbol_id << '\t' << definition.file_id
        << '\t' << definition.line << '\t' << definition.col << '\t'
        << definition.end_line << '\t' << definition.end_col << '\n';
  }
  for (const DefinitionEdgeRecord &edge : batch.definition_edges) {
    out << "def_edge\t" << edge.definition_id << '\t' << edge.destination_id
        << '\t' << edge.kind << '\n';
  }
  for (const TemplateParamRecord &param : batch.template_params) {
    out << "template_param\t" << param.owner_id << '\t' << param.position
        << '\t' << param.param_kind << '\t' << param.name.value_or("") << '\n';
  }
  for (const TemplateArgRecord &arg : batch.template_args) {
    out << "template_arg\t" << arg.owner_id << '\t' << arg.position << '\t'
        << arg.arg_kind << '\t' << arg.literal.value_or("") << '\n';
  }
  for (const ParameterFactRecord &parameter : batch.parameters) {
    out << "parameter\t" << parameter.owner_id << '\t'
        << parameter.parameter.position << '\t'
        << parameter.parameter.name.value_or("") << '\n';
  }
  for (const TypeNodeRecord &node : batch.type_nodes) {
    out << "type_node\t" << node.type_key << '\t' << node.kind << '\n';
  }
  for (const TypeEdgeRecord &edge : batch.type_edges) {
    out << "type_edge\t" << edge.src_id << '\t' << edge.kind << '\t'
        << edge.position << '\t' << edge.dst_id << '\n';
  }
  for (const SymbolTypeRecord &symbol_type : batch.symbol_types) {
    out << "symbol_type\t" << symbol_type.symbol_id << '\t' << symbol_type.kind
        << '\t' << symbol_type.type_id << '\n';
  }
  for (const PresentationIntent &intent : batch.presentation_intents) {
    out << "presentation\t" << intent.symbol_id << '\t'
        << intent.display_args.size() << '\n';
  }
  return out.str();
}

// The four routed root visitors, constructed exactly as index_engine's routed
// stages construct them: whole-TU mode (empty target file, file id -1) with the
// routing port doing the per-file selection.
void run_root_visitors(clang::ASTContext &context, FactBatchRecorder &sink,
                       const RoutedRootEventBuffer *replay) {
  clang::Decl *root = context.getTranslationUnitDecl();

  BatchSymbolEmitter symbol_sink(sink);
  SymbolVisitor symbols(context, symbol_sink, {}, nullptr,
                        [](const std::string &path) {
                          return route_fixture_file(path).has_value();
                        });
  DeclarationEdgeVisitor declarations(
      context, static_cast<DeclarationPassPorts &>(sink), {}, -1,
      static_cast<DefinitionScopeEmitter *>(&sink), nullptr,
      static_cast<PresentationIntentEmitter *>(&sink), route_fixture_file);
  FunctionDefinitionVisitor definitions(
      context, static_cast<DeclarationIdentityResolver &>(sink),
      static_cast<DefinitionScopeEmitter &>(sink), {}, -1, nullptr,
      route_fixture_file);
  NamespaceUseVisitor namespaces(context,
                                 static_cast<NamespacePassPorts &>(sink), {},
                                 -1, nullptr, route_fixture_file);

  if (replay == nullptr) {
    symbols.TraverseDecl(root);
    declarations.TraverseDecl(root);
    definitions.TraverseDecl(root);
    namespaces.TraverseDecl(root);
    return;
  }
  replay->replay_symbols(symbols);
  replay->replay_declarations(declarations);
  replay->replay_definitions(definitions);
  replay->replay_namespaces(namespaces);
}

struct RoutedRootProbe {
  bool found = false;
  std::string standalone;
  std::string replayed;
  std::size_t symbols = 0;
  std::size_t relations = 0;
  std::size_t namespace_use_edges = 0;
  std::size_t definitions = 0;
  std::size_t events = 0;
  std::size_t declarations = 0;
  std::size_t excluded_declarations = 0;
  std::vector<std::int64_t> routed_file_ids;
  std::set<std::string> recorded_files;
  std::set<std::string> instantiation_owner_files;
  std::set<std::string> instantiation_owner_names;
  std::set<std::string> instantiation_anchor_files;
  int max_scope_depth = 0;
  bool balanced_scopes = true;
  // Budget probes.
  std::string event_budget_pass;
  std::string event_budget_dimension;
  std::size_t event_budget_residue = 0;
  std::string visited_budget_dimension;
  bool visited_budget_exhausted = false;
  // Contract validation, exercised against a live ASTContext.
  bool rejected_empty_router = false;
  bool rejected_zero_event_bound = false;
  bool rejected_empty_pass_id = false;
};

class RoutedRootConsumer final : public clang::ASTConsumer {
public:
  explicit RoutedRootConsumer(RoutedRootProbe &probe) : probe_(probe) {}

  void HandleTranslationUnit(clang::ASTContext &context) override {
    RoutedRootEventBuffer buffer(context, route_fixture_file, 1'000'000,
                                 "graph.headers.probe");
    buffer.collect(context.getTranslationUnitDecl());
    probe_.events = buffer.size();
    probe_.declarations = buffer.declaration_count();
    probe_.excluded_declarations = buffer.excluded_declaration_count();
    probe_.routed_file_ids = buffer.routed_file_ids();
    inspect_events(context, buffer);

    FactBatchRecorder standalone("standalone-root-passes");
    run_root_visitors(context, standalone, nullptr);
    FactBatchRecorder replayed("replayed-root-passes");
    run_root_visitors(context, replayed, &buffer);

    probe_.standalone = render_batch(standalone.batch());
    probe_.replayed = render_batch(replayed.batch());
    probe_.symbols = standalone.batch().symbols.size();
    probe_.relations = standalone.batch().relations.size();
    probe_.definitions = standalone.batch().definitions.size();
    probe_.namespace_use_edges = static_cast<std::size_t>(std::ranges::count_if(
        standalone.batch().relations,
        [](const EdgeRecord &edge) { return edge.kind == 7; }));
    probe_size_budgets(context);
    probe_contract_validation(context);
    probe_.found = true;
  }

private:
  void probe_contract_validation(clang::ASTContext &context) {
    const auto rejects = [&context](RoutedRootEventBuffer::FileRouter router,
                                    std::size_t max_events,
                                    std::string pass_id) {
      try {
        RoutedRootEventBuffer buffer(context, std::move(router), max_events,
                                     std::move(pass_id));
        return false;
      } catch (const std::invalid_argument &) {
        return true;
      }
    };
    probe_.rejected_empty_router = rejects({}, 10, "pass");
    probe_.rejected_zero_event_bound = rejects(route_fixture_file, 0, "pass");
    probe_.rejected_empty_pass_id = rejects(route_fixture_file, 10, "");
  }

  void inspect_events(clang::ASTContext &context,
                      const RoutedRootEventBuffer &buffer) {
    int depth = 0;
    for (const RoutedRootEvent &event : buffer.events()) {
      if (event.kind == RoutedRootEventKind::enter_decl) {
        ++depth;
        probe_.max_scope_depth = std::max(probe_.max_scope_depth, depth);
      } else if (event.kind == RoutedRootEventKind::leave_decl) {
        --depth;
        if (depth < 0) {
          probe_.balanced_scopes = false;
        }
      }
      if (event.decl == nullptr) {
        continue;
      }
      const std::string file =
          basename_of(expansion_loc(context, event.decl->getLocation()).file);
      probe_.recorded_files.insert(file);
      if (!event.owns_routed_instantiation) {
        continue;
      }
      probe_.instantiation_owner_files.insert(file);
      if (const auto *named = llvm::dyn_cast<clang::NamedDecl>(event.decl)) {
        probe_.instantiation_owner_names.insert(named->getNameAsString());
        record_instantiation_anchors(context, named);
      }
    }
    if (depth != 0) {
      probe_.balanced_scopes = false;
    }
  }

  void record_instantiation_anchors(clang::ASTContext &context,
                                    const clang::NamedDecl *decl) {
    const auto anchor = [this,
                         &context](const clang::FunctionDecl *instantiation) {
      const clang::SourceLocation poi =
          instantiation->getPointOfInstantiation();
      if (poi.isInvalid()) {
        return;
      }
      probe_.instantiation_anchor_files.insert(
          basename_of(expansion_loc(context, poi).file));
    };
    if (const auto *fn = llvm::dyn_cast<clang::FunctionTemplateDecl>(decl)) {
      for_each_explicit_callable_instantiation(fn, anchor);
    } else if (const auto *rec =
                   llvm::dyn_cast<clang::ClassTemplateDecl>(decl)) {
      for_each_explicit_callable_instantiation(rec, anchor);
    }
  }

  void probe_size_budgets(clang::ASTContext &context) {
    RoutedRootEventBuffer bounded(context, route_fixture_file, 4,
                                  "graph.headers.probe");
    try {
      bounded.collect(context.getTranslationUnitDecl());
    } catch (const PassBudgetExceeded &error) {
      probe_.event_budget_pass = error.pass_id();
      probe_.event_budget_dimension = error.dimension();
    }
    probe_.event_budget_residue = bounded.size();

    PassMetrics metrics;
    metrics.bind("graph.headers.probe",
                 PassBudget{.max_visited_constructs = 1, .declared = true});
    RoutedRootEventBuffer metered(context, route_fixture_file, 1'000'000,
                                  "graph.headers.probe", &metrics);
    try {
      metered.collect(context.getTranslationUnitDecl());
    } catch (const PassBudgetExceeded &error) {
      probe_.visited_budget_dimension = error.dimension();
    }
    probe_.visited_budget_exhausted = metrics.budget_exhausted;
  }

  RoutedRootProbe &probe_;
};

class RoutedRootAction final : public clang::ASTFrontendAction {
public:
  explicit RoutedRootAction(RoutedRootProbe &probe) : probe_(probe) {}

  std::unique_ptr<clang::ASTConsumer>
  CreateASTConsumer(clang::CompilerInstance &, llvm::StringRef) override {
    return std::make_unique<RoutedRootConsumer>(probe_);
  }

private:
  RoutedRootProbe &probe_;
};

// An interleaved TU: the main file, two owned headers that nest namespaces
// across each other, an unowned header, a header an earlier TU already
// indexed, and a system header. `template int ext::identity<int>(int);` is the
// discriminating case — its template lives in the UNROUTED header while the
// instantiation statement that owns it sits in the routed main file.
const clang::tooling::FileContentMappings &routed_fixture_headers() {
  static const clang::tooling::FileContentMappings headers{
      {"owned_a.hpp", "#pragma once\n"
                      "namespace geo {\n"
                      "namespace detail { struct Tag { int id; }; }\n"
                      "template <typename T> struct Point {\n"
                      "  T x;\n"
                      "  T y;\n"
                      "  T sum() const { return x + y; }\n"
                      "};\n"
                      "using IntPoint = Point<int>;\n"
                      "int area(const IntPoint &p);\n"
                      "} // namespace geo\n"},
      {"owned_b.hpp",
       "#pragma once\n"
       "#include \"owned_a.hpp\"\n"
       "namespace shape {\n"
       "using geo::detail::Tag;\n"
       "struct Base {\n"
       "  virtual ~Base();\n"
       "  virtual int kind() const;\n"
       "};\n"
       "struct Derived : Base {\n"
       "  int kind() const override;\n"
       "  geo::IntPoint origin;\n"
       "  friend struct Base;\n"
       "};\n"
       "template <typename T> struct Holder { T value; };\n"
       "template <typename T> struct Holder<T *> { T *value; };\n"
       "} // namespace shape\n"},
      {"unowned.hpp", "#pragma once\n"
                      "namespace ext {\n"
                      "struct Widget { int size; };\n"
                      "template <typename T> T identity(T v) { return v; }\n"
                      "} // namespace ext\n"},
      {"already_indexed.hpp",
       "#pragma once\n"
       "namespace cached { struct Entry { int slot; }; }\n"},
      {"sys.hpp", "#pragma GCC system_header\n"
                  "namespace sys { struct Buffer { int len; }; }\n"},
  };
  return headers;
}

const char *routed_fixture_main() {
  return "#include \"owned_b.hpp\"\n"
         "#include \"unowned.hpp\"\n"
         "#include \"already_indexed.hpp\"\n"
         "#include \"sys.hpp\"\n"
         "using namespace shape;\n"
         "namespace app {\n"
         "namespace inner {\n"
         "struct Runner {\n"
         "  geo::Point<double> at;\n"
         "  ext::Widget widget;\n"
         "  sys::Buffer buffer;\n"
         "  cached::Entry entry;\n"
         "  shape::Holder<int *> held;\n"
         "  int run() const;\n"
         "};\n"
         "} // namespace inner\n"
         "int inner::Runner::run() const { return static_cast<int>(at.sum()); "
         "}\n"
         "} // namespace app\n"
         "template int ext::identity<int>(int);\n"
         "int geo::area(const geo::IntPoint &p) { return p.x * p.y; }\n"
         "shape::Base::~Base() = default;\n"
         "int shape::Base::kind() const { return 0; }\n"
         "int shape::Derived::kind() const { return 1; }\n";
}

RoutedRootProbe run_routed_root_probe() {
  RoutedRootProbe probe;
  const bool ran = clang::tooling::runToolOnCodeWithArgs(
      std::make_unique<RoutedRootAction>(probe), routed_fixture_main(),
      {"-std=c++23"}, "routed_main.cpp", "clang-tool",
      std::make_shared<clang::PCHContainerOperations>(),
      routed_fixture_headers());
  REQUIRE(ran);
  REQUIRE(probe.found);
  return probe;
}

} // namespace

TEST_CASE("routed root event replay reproduces every standalone root pass") {
  const RoutedRootProbe probe = run_routed_root_probe();

  // Non-vacuity: the comparison below is only meaningful if the standalone
  // walks actually produced symbols, relations and definitions.
  REQUIRE(probe.symbols > 10);
  REQUIRE(probe.relations > 10);
  REQUIRE(probe.definitions > 0);
  REQUIRE(probe.namespace_use_edges > 0);
  REQUIRE(probe.events > 100);

  CHECK(probe.replayed == probe.standalone);
}

TEST_CASE("routed root events exclude system, unowned and already-indexed "
          "files") {
  const RoutedRootProbe probe = run_routed_root_probe();

  CHECK(probe.routed_file_ids ==
        std::vector<std::int64_t>{kRoutedMainFileId, kRoutedOwnedAFileId,
                                  kRoutedOwnedBFileId});
  // Declarations the router rejected were traversed but never recorded.
  CHECK(probe.excluded_declarations > 0);
  CHECK(probe.declarations > probe.excluded_declarations);

  // unowned.hpp appears only through the explicit-instantiation exception;
  // already_indexed.hpp and the system header never appear at all.
  CHECK(probe.recorded_files ==
        std::set<std::string>{"routed_main.cpp", "owned_a.hpp", "owned_b.hpp",
                              "unowned.hpp"});
  CHECK(probe.instantiation_owner_files ==
        std::set<std::string>{"unowned.hpp"});
  CHECK(!probe.recorded_files.contains("already_indexed.hpp"));
  CHECK(!probe.recorded_files.contains("sys.hpp"));
}

TEST_CASE("routed root events preserve namespace enter/exit nesting") {
  const RoutedRootProbe probe = run_routed_root_probe();

  CHECK(probe.balanced_scopes);
  // The translation-unit decl has no expansion file, so it is unrouted and
  // opens no scope. The deepest retained nesting is therefore
  // namespace app -> namespace inner -> struct Runner -> one of its members.
  CHECK(probe.max_scope_depth == 4);
  CHECK(probe.namespace_use_edges > 0);
}

TEST_CASE("routed root events keep explicit instantiations owned by their "
          "point of instantiation") {
  const RoutedRootProbe probe = run_routed_root_probe();

  // The template declaration itself is unrouted; only its instantiation
  // statement is in a routed file, so the declaration is retained on that
  // ownership alone.
  CHECK(probe.instantiation_owner_names == std::set<std::string>{"identity"});
  CHECK(probe.instantiation_owner_files ==
        std::set<std::string>{"unowned.hpp"});
  CHECK(probe.instantiation_anchor_files ==
        std::set<std::string>{"routed_main.cpp"});
  CHECK(probe.standalone.find("identity") != std::string::npos);
  CHECK(probe.replayed == probe.standalone);
}

TEST_CASE("routed root event budgets fail with a named pass diagnostic and "
          "leave no partial stream") {
  const RoutedRootProbe probe = run_routed_root_probe();

  CHECK(probe.event_budget_pass == "graph.headers.probe");
  CHECK(probe.event_budget_dimension == "routed_root_events");
  // A truncated stream is not a traversal; the buffer must be empty so no
  // caller can replay half a translation unit.
  CHECK(probe.event_budget_residue == 0);

  // The declared visited-construct budget bounds the same recorder.
  CHECK(probe.visited_budget_dimension == "visited");
  CHECK(probe.visited_budget_exhausted);
}

TEST_CASE("routed root event buffers refuse an unusable contract") {
  const RoutedRootProbe probe = run_routed_root_probe();

  CHECK(probe.rejected_empty_router);
  CHECK(probe.rejected_zero_event_bound);
  CHECK(probe.rejected_empty_pass_id);
}
