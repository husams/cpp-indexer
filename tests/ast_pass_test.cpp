// Focused contracts for the HSE-63 extraction registry and typed recorders.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "ast/fact_batch.hpp"
#include "ast/pass_registry.hpp"

#include <stdexcept>

using namespace cidx::ast;

TEST_CASE("pass registry preserves explicit dependencies and records metrics") {
  ExtractionPassRegistry registry;
  registry.register_pass(
      {.id = "symbols", .version = 2, .produced_fact_families = {"symbols"}},
      [](PassExecutionContext &context) {
        context.metrics.note_visited(3);
        context.metrics.note_emitted(2);
      });
  registry.register_pass({.id = "relations",
                          .version = 1,
                          .produced_fact_families = {"relations"},
                          .dependencies = {"symbols"}},
                         [](PassExecutionContext &context) {
                           context.metrics.note_visited();
                           context.metrics.note_emitted();
                         });

  IndexingPlan plan;
  plan.add("symbols");
  plan.add("relations");
  const PassExecutionReport report = registry.run(plan);

  REQUIRE(report.passes.size() == 2);
  CHECK(report.passes[0].descriptor.stable_key().starts_with("symbols@2|"));
  CHECK(report.passes[0].metrics.visited_constructs == 3);
  CHECK(report.passes[1].metrics.emitted_facts == 1);
}

TEST_CASE("pass registry rejects an order that violates dependencies") {
  ExtractionPassRegistry registry;
  registry.register_pass({.id = "dependent", .dependencies = {"base"}},
                         [](PassExecutionContext &) {});
  registry.register_pass({.id = "base"}, [](PassExecutionContext &) {});
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
  recorder.add_edge({.src_id = 2, .dst_id = 1, .kind = 1});
  recorder.add_edge({.src_id = 2, .dst_id = 1, .kind = 1});

  const FactBatch batch = recorder.canonical_batch();
  REQUIRE(batch.symbols.size() == 2);
  CHECK(batch.symbols[0].usr == "usr-a");
  REQUIRE(batch.relations.size() == 1);
  CHECK(batch.relations[0].count == 2);
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
