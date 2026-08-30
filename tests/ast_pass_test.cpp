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
#include "ast/routed_fact_extractor.hpp"
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
#include <type_traits>
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
  REQUIRE(batch.records().symbols.size() == 2);
  CHECK(batch.records().symbols[0].usr == "usr-a");
  REQUIRE(batch.records().relations.size() == 1);
  CHECK(batch.records().relations[0].count == 2);
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
  REQUIRE(forward.records().symbols.size() == reverse.records().symbols.size());
  REQUIRE(forward.records().relations.size() ==
          reverse.records().relations.size());
  CHECK(forward.records().symbols[0].usr == reverse.records().symbols[0].usr);
  CHECK(forward.records().symbols[1].usr == reverse.records().symbols[1].usr);
  CHECK(forward.records().relations[0].src_id ==
        reverse.records().relations[0].src_id);
  CHECK(forward.records().relations[0].dst_id ==
        reverse.records().relations[0].dst_id);
  CHECK(forward.symbol_keys() == reverse.symbol_keys());
  CHECK(forward.relation_keys() == reverse.relation_keys());
  REQUIRE(forward.records().symbol_order.size() == 2);
  REQUIRE(forward.records().symbol_order.size() ==
          reverse.records().symbol_order.size());
  for (std::size_t index = 0; index < forward.records().symbol_order.size();
       ++index) {
    const SymbolEmissionMetadata &left = forward.records().symbol_order[index];
    const SymbolEmissionMetadata &right = reverse.records().symbol_order[index];
    CHECK(left.symbol == right.symbol);
    CHECK(left.apply_order == right.apply_order);
    CHECK(left.record_key == right.record_key);
    CHECK(left.first_seen == right.first_seen);
    CHECK(left.last_seen == right.last_seen);
    CHECK(left.first_seen == 0);
  }
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
  REQUIRE(batch.records().symbols.size() == 2);
  CHECK(batch.records().symbols[0].kind != batch.records().symbols[1].kind);
  CHECK(batch.records().symbols[0].display_name !=
        batch.records().symbols[1].display_name);
  REQUIRE(batch.records().symbol_order.size() == 2);
  CHECK(batch.records().symbol_order[0].record_key.find("f()") !=
        std::string::npos);
  CHECK(batch.records().symbol_order[1].record_key.find("f(int)") !=
        std::string::npos);
  CHECK(batch.records().symbol_order[0].apply_order.first_seen == 0);
  CHECK(batch.records().symbol_order[1].apply_order.first_seen == 1);

  FactBatchRecorder reversed("declaration-pass");
  SymbolRecord second_conflict;
  second_conflict.file = "test.cpp";
  second_conflict.usr = "usr-conflict";
  second_conflict.spelling = "f";
  second_conflict.kind = 2;
  second_conflict.display_name = "f(int)";
  reversed.emit(second_conflict);
  SymbolRecord first_conflict;
  first_conflict.file = "test.cpp";
  first_conflict.usr = "usr-conflict";
  first_conflict.spelling = "f";
  first_conflict.kind = 1;
  first_conflict.display_name = "f()";
  reversed.emit(first_conflict);
  const FactBatch reversed_batch = reversed.canonical_batch();
  REQUIRE(reversed_batch.records().symbol_order.size() == 2);
  CHECK(reversed_batch.records().symbol_order[0].record_key.find("f(int)") !=
        std::string::npos);
  CHECK(reversed_batch.records().symbol_order[1].record_key.find("f()") !=
        std::string::npos);
  CHECK(reversed_batch.records().symbol_order[0].apply_order.first_seen == 0);
  CHECK(reversed_batch.records().symbol_order[1].apply_order.first_seen == 1);
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
  CHECK(std::ranges::any_of(
      result.batch.records().relations,
      [](const EdgeRecord &edge) { return edge.kind == 1; }));
  CHECK(std::ranges::any_of(
      result.batch.records().edge_sites,
      [](const EdgeSiteRecord &site) { return site.line == 2; }));
  CHECK(result.emitted == 6);
  CHECK(result.batch.records().symbols.size() == 2);
  CHECK(result.batch.records().parameters.size() == 1);
  CHECK(result.batch.records().type_nodes.size() == 1);
  CHECK(result.batch.records().symbol_types.size() == 1);
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
  CHECK(recorder.batch().records().evidence.size() == 1);
  CHECK(recorder.batch().records().evidence.front().construct == "CallExpr");
}

TEST_CASE("frontend pass providers preserve contracts on batch-backed ports") {
  clear_frontend_pass_providers();
  register_frontend_pass_provider([](FrontendSession &session,
                                     ExtractionPassRegistry &registry,
                                     IndexingPlan &plan) {
    if (session.declaration_ports == nullptr) {
      throw std::logic_error("provider requires declaration ports");
    }
    auto descriptor = valid_descriptor("extension.batch");
    descriptor.required_capabilities.clear();
    descriptor.consumed_fact_families = {"symbols"};
    descriptor.produced_fact_families = {"symbols"};
    descriptor.completeness = FactCompleteness::partial;
    descriptor.trust = FactTrust::inferred;
    registry.register_pass(descriptor, [](PassExecutionContext &context) {
      MintRequest request;
      request.usr = "provider-usr";
      request.spelling = "provider";
      request.kind_name = "function";
      static_cast<void>(
          context.session->declaration_ports->mint_symbol(request));
    });
    plan.add(descriptor.id);
  });

  FactBatchRecorder recorder("provider-test");
  FrontendSession session;
  session.declaration_ports = &recorder;
  ExtractionPassRegistry registry;
  IndexingPlan plan;
  for (const FrontendPassProvider &provider : frontend_pass_providers()) {
    provider(session, registry, plan);
  }
  const PassExecutionReport report = registry.run(plan, &session);
  clear_frontend_pass_providers();

  REQUIRE(report.passes.size() == 1);
  CHECK(report.passes.front().descriptor.produced_fact_families ==
        std::vector<std::string>{"symbols"});
  CHECK(report.passes.front().descriptor.completeness ==
        FactCompleteness::partial);
  CHECK(report.passes.front().descriptor.trust == FactTrust::inferred);
  CHECK(report.passes.front().metrics.emitted_facts == 1);
  CHECK(report.passes.front().metrics.whole_tu_traversals == 0);
  CHECK(recorder.canonical_batch().records().symbols.size() == 1);
}

namespace {

FactPartitionKey fact_partition(std::string file, std::string universe,
                                std::string configuration, std::string source) {
  return {
      .file = {.component_path = "/repo",
               .directory_path = "src",
               .file_name = std::move(file)},
      .configuration = {.semantic_universe = std::move(universe),
                        .translation_unit = "src/main.cpp",
                        .normalized_configuration = std::move(configuration),
                        .identity_source = std::move(source)}};
}

void emit_test_symbol(FactBatchRecorder &recorder,
                      const FactPartitionKey &partition, std::string usr,
                      std::string spelling, int kind,
                      std::optional<std::string> qualified = std::nullopt) {
  SymbolRecord symbol;
  symbol.file = partition.file.portable_path();
  symbol.usr = std::move(usr);
  symbol.spelling = std::move(spelling);
  symbol.kind = kind;
  symbol.qual_name = std::move(qualified);
  symbol.identity_source = partition.configuration.identity_source;
  recorder.emit(symbol);
}

} // namespace

TEST_CASE("natural fact handles remain distinct under adversarial collisions") {
  CollisionSafeHandleIndex index(
      [](std::string_view) -> std::uint64_t { return 7; });
  const std::int64_t first = index.find_or_insert("symbol:first");
  const std::int64_t second = index.find_or_insert("symbol:second");

  CHECK(first != second);
  CHECK(index.find("symbol:first") == first);
  CHECK(index.find("symbol:second") == second);
  CHECK(index.key_for(first) == "symbol:first");
  CHECK(index.key_for(second) == "symbol:second");
}

TEST_CASE("partition identity prevents file and configuration aliasing") {
  const FactPartitionKey first =
      fact_partition("one.cpp", "workspace", "debug", "src/one.cpp");
  const FactPartitionKey second =
      fact_partition("two.cpp", "dependency", "release", "src/two.cpp");
  FactBatchRecorder recorder("partition-test");
  recorder.set_partition(first, 11);
  emit_test_symbol(recorder, first, "same-usr", "one", 8);
  const auto first_id = recorder.lookup_symbol_id("same-usr", "src/one.cpp");
  if (!first_id) {
    FAIL("first partition symbol was not indexed");
    return;
  }
  const FactBatch published = recorder.canonical_batch();

  recorder.set_partition(second, 22);
  emit_test_symbol(recorder, second, "same-usr", "two", 8);
  const auto second_id = recorder.lookup_symbol_id("same-usr", "src/two.cpp");
  if (!second_id) {
    FAIL("second partition symbol was not indexed");
    return;
  }
  const std::int64_t first_handle = first_id.value();
  const std::int64_t second_handle = second_id.value();
  REQUIRE(first_handle != second_handle);

  recorder.set_partition(first, 11);
  emit_test_symbol(recorder, second, "second-while-first-current", "two-routed",
                   8);
  emit_test_symbol(recorder, first, "first-again-usr", "one-again", 8);
  recorder.add_edge_site({.edge_id = 73,
                          .file_id = 22,
                          .line = 5,
                          .col = 7,
                          .conditional = 0,
                          .recv_src_kind = std::nullopt,
                          .recv_type_usr = std::nullopt,
                          .recv_decl_usr = std::nullopt,
                          .recv_param_pos = std::nullopt,
                          .recv_type_is_value = std::nullopt});

  CHECK(published.records().symbols.size() == 1);
  REQUIRE(published.file_keys().contains(11));
  CHECK(published.file_keys().at(11) == first);
  const FactBatch complete = recorder.canonical_batch();
  const FactBatch repeated = recorder.canonical_batch();
  REQUIRE(complete.partitions().size() == 2);
  CHECK(complete.records().symbols.size() == 4);
  REQUIRE(complete.records().edge_sites.size() == 1);
  CHECK(complete.symbol_keys().contains(first_handle));
  CHECK(complete.symbol_keys().contains(second_handle));
  std::size_t symbol_memberships = 0;
  std::size_t edge_site_memberships = 0;
  bool cross_file_symbol_routed = false;
  for (const FileFactPartition &partition : complete.partitions()) {
    REQUIRE(partition.members.contains(FactFamily::symbols));
    for (const std::size_t index : partition.members.at(FactFamily::symbols)) {
      ++symbol_memberships;
      REQUIRE(index < complete.records().symbols.size());
      CHECK(complete.records().symbols[index].file ==
            partition.key.file.portable_path());
      if (complete.records().symbols[index].usr ==
          "second-while-first-current") {
        cross_file_symbol_routed = true;
        CHECK(partition.key == second);
      }
    }
    if (partition.members.contains(FactFamily::edge_sites)) {
      for (const std::size_t index :
           partition.members.at(FactFamily::edge_sites)) {
        ++edge_site_memberships;
        REQUIRE(index < complete.records().edge_sites.size());
        CHECK(partition.key == second);
        CHECK(complete.records().edge_sites[index].file_id == 22);
      }
    }
  }
  CHECK(cross_file_symbol_routed);
  CHECK(symbol_memberships == complete.records().symbols.size());
  CHECK(edge_site_memberships == complete.records().edge_sites.size());

  REQUIRE(repeated.partitions().size() == complete.partitions().size());
  REQUIRE(repeated.records().symbols.size() ==
          complete.records().symbols.size());
  for (std::size_t index = 0; index < complete.partitions().size(); ++index) {
    CHECK(repeated.partitions()[index].key == complete.partitions()[index].key);
    CHECK(repeated.partitions()[index].members ==
          complete.partitions()[index].members);
  }
  for (std::size_t index = 0; index < complete.records().symbols.size();
       ++index) {
    CHECK(stable_symbol_record_key(repeated.records().symbols[index]) ==
          stable_symbol_record_key(complete.records().symbols[index]));
  }
}

TEST_CASE("symbol identities match external coalescing and local splitting") {
  const FactPartitionKey header =
      fact_partition("header.hpp", "workspace", "debug", "src/header.hpp");
  const FactPartitionKey main =
      fact_partition("main.cpp", "workspace", "debug", "src/main.cpp");
  FactBatchRecorder recorder("identity-oracle-test");

  auto emit = [&recorder](const FactPartitionKey &owner, std::string usr,
                          std::string linkage) {
    recorder.set_partition(owner);
    SymbolRecord record;
    record.file = owner.file.portable_path();
    record.usr = std::move(usr);
    record.spelling = "shared";
    record.kind = 8;
    record.linkage = std::move(linkage);
    record.identity_source = owner.configuration.identity_source;
    recorder.emit(record);
    const auto id = recorder.lookup_symbol_id(
        record.usr, owner.configuration.identity_source);
    if (!id) {
      throw std::logic_error("identity fixture symbol was not indexed");
    }
    return id.value();
  };

  const std::int64_t external_header = emit(header, "shared-usr", "external");
  const std::int64_t external_main = emit(main, "shared-usr", "external");
  CHECK(external_header == external_main);

  const std::int64_t internal_header = emit(header, "local-usr", "internal");
  const std::int64_t internal_main = emit(main, "local-usr", "internal");
  CHECK(internal_header != internal_main);
  recorder.set_partition(main);
  bool refused_ambiguity = false;
  try {
    static_cast<void>(recorder.lookup_symbol_id("local-usr"));
  } catch (const std::runtime_error &) {
    refused_ambiguity = true;
  }
  CHECK(refused_ambiguity);
  CHECK(recorder.counters().records_touched.at("lookup_symbol_sourceless") ==
        2);
}

TEST_CASE("inferred file identities enforce component path boundaries") {
  FactBatchRecorder recorder("inferred-file-boundary-test");
  const FactPartitionKey owner =
      fact_partition("main.cpp", "workspace", "debug", "src/main.cpp");
  recorder.set_partition(owner);

  auto emit = [&recorder](std::string file, std::string usr) {
    SymbolRecord record;
    record.file = std::move(file);
    record.usr = std::move(usr);
    record.spelling = "inferred";
    record.kind = 8;
    record.linkage = "external";
    record.identity_source = record.file;
    recorder.emit(record);
  };
  emit("/repo/include/inside.hpp", "inside-usr");
  emit("/repository/include/prefix.hpp", "prefix-usr");
  emit("/usr/include/foreign.hpp", "foreign-usr");

  const FactBatch batch = recorder.canonical_batch();
  const auto find_file = [&batch](std::string_view name) {
    return std::ranges::find_if(batch.partitions(),
                                [name](const FileFactPartition &partition) {
                                  return partition.key.file.file_name == name;
                                });
  };
  const auto inside = find_file("inside.hpp");
  const auto prefix = find_file("prefix.hpp");
  const auto foreign = find_file("foreign.hpp");
  REQUIRE(inside != batch.partitions().end());
  REQUIRE(prefix != batch.partitions().end());
  REQUIRE(foreign != batch.partitions().end());
  CHECK(inside->key.file.component_path == "/repo");
  CHECK(inside->key.file.directory_path == "include");
  CHECK(prefix->key.file.component_path.empty());
  CHECK(prefix->key.file.directory_path == "/repository/include");
  CHECK(foreign->key.file.component_path.empty());
  CHECK(foreign->key.file.directory_path == "/usr/include");
}

TEST_CASE("finalized batches expose every fact family through const records") {
  static_assert(
      std::is_same_v<decltype(std::declval<const FactBatch &>().records()),
                     const FactRecords &>);
  FactPartitionKey partition =
      fact_partition("main.cpp", "workspace", "debug", "src/main.cpp");
  partition.configuration.content.driver = "/usr/bin/clang++";
  partition.configuration.content.working_dir = "/repo";
  partition.configuration.content.arguments = {"-std=c++23", "-Iinclude"};
  partition.configuration.content.lang_mode = "c++";
  partition.configuration.content.resource_dir = "/opt/llvm/resource";
  SymbolNaturalKey symbol;
  symbol.partition = partition;
  symbol.usr = "usr-main";
  FactBatchRecorder recorder("coverage-test");
  recorder.set_partition(partition, 41);
  emit_test_symbol(recorder, partition, symbol.usr, "main", 8);
  DeclarationSiteRecord declaration;
  declaration.symbol = symbol;
  declaration.partition = partition;
  declaration.line = 1;
  declaration.col = 1;
  declaration.end_line = 1;
  declaration.end_col = 9;
  declaration.is_definition = true;
  recorder.emit(declaration);
  IncludeDirectiveRecord include;
  include.partition = partition;
  include.source = partition.file;
  include.destination = PortableFileIdentity{.component_path = "/repo",
                                             .directory_path = "include",
                                             .file_name = "a.hpp"};
  include.destination_path = "include/a.hpp";
  include.spelling = "<a.hpp>";
  include.directive = IncludeDirectiveKind::include_next;
  include.line = 1;
  include.col = 1;
  include.begin_offset = 4;
  include.end_offset = 19;
  include.conditional_fingerprint = "defined(FEATURE)";
  include.is_angled = true;
  include.resolved = true;
  include.is_system = false;
  include.guarded = true;
  recorder.emit(include);
  MacroUseRecord macro;
  macro.partition = partition;
  macro.source = partition.file;
  macro.definition = include.destination;
  macro.definition_path = "include/a.hpp";
  macro.name = "A";
  macro.count = 3;
  recorder.emit(macro);
  MacroUseRecord foreign_macro = macro;
  foreign_macro.definition.reset();
  foreign_macro.definition_path = "/usr/include/foreign.h";
  foreign_macro.name = "FOREIGN";
  foreign_macro.count = 1;
  recorder.emit(foreign_macro);
  DiagnosticFactRecord diagnostic;
  diagnostic.partition = partition;
  diagnostic.severity = DiagnosticSeverity::warning;
  diagnostic.spelling = "warning";
  diagnostic.location_file = partition.file;
  diagnostic.line = 2;
  diagnostic.col = 3;
  recorder.emit(diagnostic);
  LifecycleCleanupIntent cleanup;
  cleanup.partition = partition;
  cleanup.kind = LifecycleCleanupKind::relations;
  cleanup.target = partition.file;
  cleanup.prior_generation.token = "previous-content-digest";
  recorder.emit(cleanup);
  ApplicabilityOwnershipRecord applicability;
  applicability.partition = partition;
  applicability.file = partition.file;
  applicability.role = ApplicabilityRole::translation_unit;
  applicability.state = ApplicabilityState::registered;
  applicability.reason = "owned translation unit";
  applicability.generation.token = "current-content-digest";
  recorder.emit(applicability);

  const FactBatch batch = recorder.canonical_batch();
  const FactRecords &records = batch.records();
  CHECK(records.symbols.size() == 1);
  REQUIRE(records.declaration_sites.size() == 1);
  const DeclarationSiteRecord &published_declaration =
      records.declaration_sites.front();
  CHECK(published_declaration.symbol == declaration.symbol);
  CHECK(published_declaration.partition == partition);
  CHECK(published_declaration.line == 1);
  CHECK(published_declaration.col == 1);
  CHECK(published_declaration.end_line == 1);
  CHECK(published_declaration.end_col == 9);
  CHECK(published_declaration.is_definition);

  REQUIRE(records.includes.size() == 1);
  const IncludeDirectiveRecord &published_include = records.includes.front();
  CHECK(published_include.partition == partition);
  CHECK(published_include.source == partition.file);
  CHECK(published_include.destination == include.destination);
  CHECK(published_include.destination_path == "include/a.hpp");
  CHECK(published_include.spelling == "<a.hpp>");
  CHECK(published_include.directive == IncludeDirectiveKind::include_next);
  CHECK(published_include.line == 1);
  CHECK(published_include.col == 1);
  CHECK(published_include.begin_offset == 4);
  CHECK(published_include.end_offset == 19);
  CHECK(published_include.conditional_fingerprint == "defined(FEATURE)");
  CHECK(published_include.is_angled);
  CHECK(published_include.resolved);
  CHECK(!published_include.is_system);
  CHECK(published_include.guarded);

  REQUIRE(records.macros.size() == 2);
  const auto published_macro =
      std::ranges::find_if(records.macros, [](const MacroUseRecord &record) {
        return record.name == "A";
      });
  const auto published_foreign_macro =
      std::ranges::find_if(records.macros, [](const MacroUseRecord &record) {
        return record.name == "FOREIGN";
      });
  REQUIRE(published_macro != records.macros.end());
  REQUIRE(published_foreign_macro != records.macros.end());
  CHECK(published_macro->partition == partition);
  CHECK(published_macro->source == partition.file);
  CHECK(published_macro->definition == include.destination);
  CHECK(published_macro->definition_path == "include/a.hpp");
  CHECK(published_macro->count == 3);
  CHECK(!published_foreign_macro->definition);
  CHECK(published_foreign_macro->definition_path == "/usr/include/foreign.h");
  CHECK(published_foreign_macro->count == 1);

  REQUIRE(records.diagnostics.size() == 1);
  const DiagnosticFactRecord &published_diagnostic =
      records.diagnostics.front();
  CHECK(published_diagnostic.partition == partition);
  CHECK(published_diagnostic.severity == DiagnosticSeverity::warning);
  CHECK(published_diagnostic.spelling == "warning");
  CHECK(published_diagnostic.location_file == partition.file);
  CHECK(published_diagnostic.line == 2);
  CHECK(published_diagnostic.col == 3);

  REQUIRE(records.lifecycle_cleanup.size() == 1);
  const LifecycleCleanupIntent &published_cleanup =
      records.lifecycle_cleanup.front();
  CHECK(published_cleanup.partition == partition);
  CHECK(published_cleanup.kind == LifecycleCleanupKind::relations);
  CHECK(published_cleanup.target == partition.file);
  CHECK(published_cleanup.prior_generation.token == "previous-content-digest");

  REQUIRE(records.applicability.size() == 1);
  const ApplicabilityOwnershipRecord &published_applicability =
      records.applicability.front();
  CHECK(published_applicability.partition == partition);
  CHECK(published_applicability.file == partition.file);
  CHECK(published_applicability.role == ApplicabilityRole::translation_unit);
  CHECK(published_applicability.state == ApplicabilityState::registered);
  CHECK(published_applicability.reason == "owned translation unit");
  CHECK(published_applicability.generation.token == "current-content-digest");

  CHECK(std::to_underlying(IncludeDirectiveKind::include) == 1);
  CHECK(std::to_underlying(IncludeDirectiveKind::include_next) == 2);
  CHECK(std::to_underlying(IncludeDirectiveKind::import) == 3);
  CHECK(std::to_underlying(IncludeDirectiveKind::include_macros) == 4);
  CHECK(std::to_underlying(IncludeDirectiveKind::unknown) == 5);
}

TEST_CASE("portable configuration identity ignores database row ids") {
  FactPartitionKey partition =
      fact_partition("main.cpp", "workspace", "config-digest", "src/main.cpp");
  partition.configuration.content.driver = "/usr/bin/clang++";
  partition.configuration.content.working_dir = "/repo";
  partition.configuration.content.arguments = {"-std=c++23", "-Iinclude"};
  partition.configuration.content.lang_mode = "c++";
  partition.configuration.content.resource_dir = "/opt/llvm/resource";

  FactBatchRecorder first("configuration-id-first");
  first.set_partition(partition, 41);
  first.set_identity_translation_unit_config_id(7, 41);
  emit_test_symbol(first, partition, "portable-config-usr", "first", 8);

  FactBatchRecorder second("configuration-id-second");
  second.set_partition(partition, 41);
  second.set_identity_translation_unit_config_id(9001, 41);
  emit_test_symbol(second, partition, "portable-config-usr", "first", 8);

  const FactBatch first_batch = first.canonical_batch();
  const FactBatch second_batch = second.canonical_batch();
  REQUIRE(first_batch.partitions().size() == 1);
  REQUIRE(second_batch.partitions().size() == 1);
  CHECK(first_batch.partitions().front().key ==
        second_batch.partitions().front().key);
  CHECK(first_batch.partitions().front().key.configuration.content ==
        partition.configuration.content);

  FactPartitionKey changed = partition;
  changed.configuration.content.driver = "/opt/other/clang++";
  CHECK(changed.stable_string() != partition.stable_string());
}

TEST_CASE("symbol lookup indexes honor source name qualification and kind") {
  FactBatchRecorder recorder("lookup-test");
  const FactPartitionKey partition =
      fact_partition("main.cpp", "workspace", "debug", "src/main.cpp");
  recorder.set_partition(partition);
  emit_test_symbol(recorder, partition, "usr-function", "item", 8, "ns::item");
  emit_test_symbol(recorder, partition, "usr-class", "item", 4, "ns::item");
  emit_test_symbol(recorder, partition, "usr-function", "item", 8, "ns::item");
  for (int index = 0; index < 128; ++index) {
    emit_test_symbol(recorder, partition, "noise-usr-" + std::to_string(index),
                     "noise-" + std::to_string(index), 8,
                     "ns::noise_" + std::to_string(index));
  }

  const auto by_source =
      recorder.lookup_symbol_id("usr-function", "src/main.cpp");
  const auto source_less = recorder.lookup_symbol_id("usr-function");
  REQUIRE(by_source);
  CHECK(source_less == by_source);
  const auto qualified_candidates =
      recorder.type_arg_candidates("ns::item", true);
  const auto unqualified_candidates =
      recorder.type_arg_candidates("item", false);
  REQUIRE(qualified_candidates.size() == 2);
  REQUIRE(unqualified_candidates.size() == 2);
  CHECK(qualified_candidates[0].kind_name == "function");
  CHECK(qualified_candidates[1].kind_name == "class");
  CHECK(unqualified_candidates == qualified_candidates);
  const auto functions =
      recorder.symbol_ids_by_qual_name_kind("ns::item", "function");
  const auto classes =
      recorder.symbol_ids_by_qual_name_kind("ns::item", "class");
  REQUIRE(functions.size() == 1);
  REQUIRE(classes.size() == 1);
  CHECK(functions.front() != classes.front());
  CHECK(recorder.counters().records_touched.at("lookup_symbol_exact") == 1);
  CHECK(recorder.counters().records_touched.at("lookup_symbol_sourceless") ==
        1);
  CHECK(recorder.counters().records_touched.at("type_arg_candidates") == 4);
  CHECK(recorder.counters().records_touched.at(
            "symbol_ids_by_qual_name_kind") == 2);
}

TEST_CASE("symbol lookup indexes isolate universes and repeated declarations") {
  FactBatchRecorder recorder("lookup-scope-test");
  const FactPartitionKey workspace =
      fact_partition("main.cpp", "workspace", "debug", "src/main.cpp");
  const FactPartitionKey dependency = fact_partition(
      "main.cpp", "dependency", "debug", "dependency/src/main.cpp");

  recorder.set_partition(workspace);
  emit_test_symbol(recorder, workspace, "shared-usr", "shared", 8,
                   "ns::shared");
  const auto workspace_exact =
      recorder.lookup_symbol_id("shared-usr", "src/main.cpp");
  const auto workspace_sourceless = recorder.lookup_symbol_id("shared-usr");
  REQUIRE(workspace_exact);
  CHECK(workspace_sourceless == workspace_exact);

  emit_test_symbol(recorder, workspace, "shared-usr", "shared", 8,
                   "ns::shared");
  CHECK(recorder.lookup_symbol_id("shared-usr", "src/main.cpp") ==
        workspace_exact);
  CHECK(recorder.type_arg_candidates("ns::shared", true).size() == 1);

  recorder.set_partition(dependency);
  emit_test_symbol(recorder, dependency, "shared-usr", "shared", 8,
                   "ns::shared");
  const auto dependency_exact =
      recorder.lookup_symbol_id("shared-usr", "dependency/src/main.cpp");
  const auto dependency_sourceless = recorder.lookup_symbol_id("shared-usr");
  REQUIRE(dependency_exact);
  CHECK(dependency_sourceless == dependency_exact);
  CHECK(dependency_exact != workspace_exact);

  recorder.set_partition(workspace);
  CHECK(recorder.lookup_symbol_id("shared-usr") == workspace_exact);
}

TEST_CASE("symbol lookup reuses only source-independent scoped identities") {
  FactBatchRecorder recorder("lookup-source-fallback-test");
  const FactPartitionKey partition =
      fact_partition("main.cpp", "workspace", "debug", "src/main.cpp");
  recorder.set_partition(partition);

  SymbolRecord external;
  external.file = partition.file.portable_path();
  external.usr = "external-usr";
  external.spelling = "external";
  external.kind = 8;
  external.linkage = "external";
  external.identity_source = "src/declaration.hpp";
  recorder.emit(external);
  const auto exact =
      recorder.lookup_symbol_id("external-usr", "src/declaration.hpp");
  REQUIRE(exact);
  CHECK(recorder.lookup_symbol_id("external-usr", "src/use.cpp") == exact);
  CHECK(recorder.mint_symbol({.usr = "external-usr",
                              .spelling = "external",
                              .kind_name = "function",
                              .is_instantiation = true,
                              .identity_source = "src/declaration.hpp",
                              .linkage = "external",
                              .parent_usr = "owner-usr"}) == *exact);

  SymbolRecord internal = external;
  internal.usr = "internal-usr";
  internal.spelling = "internal";
  internal.linkage = "internal";
  recorder.emit(internal);
  CHECK_FALSE(recorder.lookup_symbol_id("internal-usr", "src/use.cpp"));

  const FactBatch batch = recorder.canonical_batch();
  const auto enriched = std::ranges::find_if(
      batch.records().symbols,
      [](const SymbolRecord &record) { return record.usr == "external-usr"; });
  REQUIRE(enriched != batch.records().symbols.end());
  CHECK(enriched->is_instantiation);
  CHECK(enriched->parent_usr == "owner-usr");
}

TEST_CASE("mutation and aggregation operations touch only keyed buckets") {
  FactBatchRecorder recorder("complexity-test");
  recorder.set_partition(
      fact_partition("main.cpp", "workspace", "debug", "src/main.cpp"));
  std::vector<std::int64_t> ids;
  for (int i = 0; i < 100; ++i) {
    const std::string usr = "usr-" + std::to_string(i);
    const FactPartitionKey partition =
        fact_partition("main.cpp", "workspace", "debug", "src/main.cpp");
    emit_test_symbol(recorder, partition, usr, usr, 8);
    const auto id = recorder.lookup_symbol_id(usr);
    if (!id) {
      FAIL("complexity fixture symbol was not indexed");
      return;
    }
    ids.push_back(id.value());
  }
  emit_test_symbol(
      recorder,
      fact_partition("main.cpp", "workspace", "debug", "src/main.cpp"),
      "usr-50", "usr-50", 8);
  recorder.update_display_name(ids[50], "updated");
  CHECK(recorder.counters().records_touched.at("update_display_name") == 2);

  for (int i = 0; i < 10; ++i) {
    EdgeRecord edge;
    edge.src_id = ids[0];
    edge.dst_id = ids[i + 1];
    edge.kind = 1;
    recorder.add_edge(edge);
  }
  for (int i = 20; i < 80; ++i) {
    EdgeRecord edge;
    edge.src_id = ids[i];
    edge.dst_id = ids[i + 1];
    edge.kind = 9;
    recorder.add_edge(edge);
  }
  CHECK(recorder.body_edge_count(ids[0]) == 10);
  CHECK(recorder.counters().records_touched.at("body_edge_count") == 10);

  EdgeRecord duplicate;
  duplicate.src_id = ids[0];
  duplicate.dst_id = ids[1];
  duplicate.kind = 1;
  recorder.add_edge(duplicate);
  CHECK(recorder.counters().records_touched.at("add_edge") == 1);
  ParameterRecord first_parameter;
  first_parameter.position = 0;
  first_parameter.name = "first";
  ParameterRecord second_parameter;
  second_parameter.position = 1;
  second_parameter.name = "second";
  recorder.replace_parameters(ids[0], {first_parameter, second_parameter});
  ParameterRecord replacement;
  replacement.position = 0;
  replacement.name = "only";
  recorder.replace_parameters(ids[0], {replacement});
  CHECK(recorder.counters().records_touched.at("replace_parameters") == 3);
  ParameterRecord unrelated;
  unrelated.position = 0;
  unrelated.name = "unrelated";
  recorder.replace_parameters(ids[1], {unrelated});

  TypeNodeRecord type_node;
  type_node.type_key = "builtin:int";
  type_node.spelling = "int";
  type_node.kind = 1;
  const std::int64_t type_id = recorder.intern_type_node(type_node);
  CHECK(recorder.intern_type_node(type_node) == type_id);
  CHECK(recorder.counters().calls.at("intern_type_node") == 2);
  CHECK(recorder.counters().records_touched.at("intern_type_node") == 1);

  const auto exact = recorder.lookup_symbol_id("usr-0", "src/main.cpp");
  if (!exact) {
    FAIL("exact lookup did not find the indexed symbol");
    return;
  }
  CHECK(exact.value() == ids[0]);
  CHECK(recorder.counters().records_touched.at("lookup_symbol_exact") == 1);

  const std::int64_t definition_id =
      recorder.get_or_create_definition(ids[0], 0, 3, 4, 3, 12, std::nullopt);
  CHECK(recorder.get_or_create_definition(ids[0], 0, 3, 4, 3, 12,
                                          std::nullopt) == definition_id);
  CHECK(recorder.counters().calls.at("get_or_create_definition") == 2);
  CHECK(recorder.counters().records_touched.at("get_or_create_definition") ==
        1);
  recorder.copy_body_edges_to_def_edge(definition_id, ids[0]);
  CHECK(recorder.counters().records_touched.at("copy_body_edges_to_def_edge") ==
        10);

  const FactBatch batch = recorder.canonical_batch();
  REQUIRE(batch.records().parameters.size() == 2);
  const auto target_parameter = std::ranges::find_if(
      batch.records().parameters, [&](const ParameterFactRecord &parameter) {
        return parameter.owner_id == ids[0];
      });
  REQUIRE(target_parameter != batch.records().parameters.end());
  CHECK(target_parameter->parameter.name == "only");
  const auto updated_symbol = std::ranges::find_if(
      batch.records().symbols,
      [](const SymbolRecord &symbol) { return symbol.usr == "usr-50"; });
  REQUIRE(updated_symbol != batch.records().symbols.end());
  CHECK(updated_symbol->display_name == "updated");
  REQUIRE(batch.records().relations.size() == 70);
  const auto aggregated_edge = std::ranges::find_if(
      batch.records().relations, [&](const EdgeRecord &edge) {
        return edge.src_id == ids[0] && edge.dst_id == ids[1] && edge.kind == 1;
      });
  REQUIRE(aggregated_edge != batch.records().relations.end());
  CHECK(aggregated_edge->count == 2);
  CHECK(batch.records().definition_edges.size() == 10);
}

TEST_CASE(
    "candidate emission probes keyed membership for large overload sets") {
  FactBatchRecorder recorder("candidate-membership-test");
  const FactPartitionKey partition = fact_partition(
      "overloads.cpp", "workspace", "debug", "src/overloads.cpp");
  recorder.set_partition(partition);

  constexpr int overload_count = 256;
  for (int index = 0; index < overload_count; ++index) {
    emit_test_symbol(recorder, partition,
                     "overload-usr-" + std::to_string(index), "overload", 8,
                     "ns::overload");
  }
  emit_test_symbol(recorder, partition, "overload-usr-0", "overload", 8,
                   "ns::overload");

  constexpr std::uint64_t memberships_per_qualified_symbol = 3;
  const std::uint64_t expected_probes =
      memberships_per_qualified_symbol * (overload_count + 1);
  CHECK(recorder.counters().calls.at("emit_candidate_membership") ==
        expected_probes);
  CHECK(recorder.counters().records_touched.at("emit_candidate_membership") ==
        expected_probes);
  CHECK(recorder.type_arg_candidates("overload", false).size() ==
        overload_count);
  CHECK(recorder.type_arg_candidates("ns::overload", true).size() ==
        overload_count);
  CHECK(recorder.symbol_ids_by_qual_name_kind("ns::overload", "function")
            .size() == overload_count);
}
