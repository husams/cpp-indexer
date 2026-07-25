// Focused contracts for the HSE-63 extraction registry and typed recorders.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "ast/fact_batch.hpp"
#include "ast/pass_registry.hpp"
#include "ast/statement_edge_visitor.hpp"
#include "ast/usr.hpp"

#include "clang/AST/ASTConsumer.h"
#include "clang/AST/Decl.h"
#include "clang/Frontend/FrontendAction.h"
#include "clang/Tooling/Tooling.h"

#include <algorithm>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

using namespace cidx::ast;

namespace {

ExtractionPassDescriptor
valid_descriptor(std::string id, std::vector<std::string> dependencies = {}) {
  return {.id = std::move(id),
          .version = 1,
          .required_capabilities = {FrontendCapability::ast},
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
  bool found = false;
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

    FactBatchRecorder recorder("statement-pass");
    recorder.emit(SymbolRecord{.file = "test.cpp",
                               .usr = usr_for_decl(caller),
                               .spelling = "caller",
                               .kind = 1});
    recorder.emit(SymbolRecord{.file = "test.cpp",
                               .usr = usr_for_decl(callee),
                               .spelling = "callee",
                               .kind = 1});
    const auto caller_id = recorder.lookup_symbol_id(usr_for_decl(caller));
    if (!caller_id) {
      return;
    }
    StatementEdgeVisitor visitor(context, recorder, *caller_id, 1, "test.cpp");
    visitor.walk(caller);
    result_.batch = recorder.canonical_batch();
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
  const PassExecutionReport report = registry.run(plan);

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
    CHECK_THROWS_AS(static_cast<void>(registry.run(plan)), PassBudgetExceeded);
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
}

TEST_CASE("pass stable keys include the complete descriptor contract") {
  const ExtractionPassDescriptor base = valid_descriptor("contract");
  auto catalog_changed = base;
  catalog_changed.catalog_versions = {2};
  CHECK(base.stable_key() != catalog_changed.stable_key());
  auto dependency_changed = base;
  dependency_changed.dependencies = {"other"};
  CHECK(base.stable_key() != dependency_changed.stable_key());
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
