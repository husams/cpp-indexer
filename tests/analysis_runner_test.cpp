#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include <memory>

#include "analysis/facts.hpp"
#include "analysis/runner.hpp"

namespace {

using namespace cidx::analysis;

FactRelation symbols() {
  return FactRelation{
      .descriptor =
          RelationDescriptor{
              .name = "symbol",
              .version = 1,
              .catalog_version = 1,
              .columns = {{.name = "id", .type = FactType::integer},
                          {.name = "usr", .type = FactType::string},
                          {.name = "name", .type = FactType::string}}},
      .rows = {{std::int64_t{1}, "usr:A", "A"},
               {std::int64_t{2}, "usr:B", "B"}}};
}

FactRelation ast_symbols() {
  return FactRelation{
      .descriptor =
          RelationDescriptor{
              .name = "ast_symbol",
              .version = 1,
              .catalog_version = 1,
              .columns = {{.name = "id", .type = FactType::integer},
                          {.name = "usr", .type = FactType::string},
                          {.name = "name", .type = FactType::string}}},
      .rows = {{std::int64_t{10}, "usr:A", "A"},
               {std::int64_t{11}, "usr:missing", "Missing"}}};
}

FactSnapshot snapshot() {
  FactSnapshot result{.provider = "semantic-index",
                      .workspace_identity = "workspace:test",
                      .tu_identity = std::nullopt,
                      .applicability = "workspace",
                      .catalog_hash = "sha256:catalog",
                      .evidence_references = {},
                      .input_hashes = {"sha256:input"},
                      .artifact_path = std::nullopt,
                      .relations = {}};
  result.add_relation(symbols());
  return result;
}

class NoopEngine final : public AnalysisEngine {
public:
  [[nodiscard]] AnalysisRun
  execute(const AnalysisPackage &package, const FactSnapshot &snapshot_value,
          const AnalysisOptions &options) const override {
    (void)package;
    (void)snapshot_value;
    (void)options;
    return {};
  }
};

} // namespace

TEST_CASE("fact snapshots preserve typed deterministic identity") {
  const auto first = snapshot();
  const auto second = snapshot();
  CHECK_UNARY(first.stable_hash() == second.stable_hash());
  CHECK_UNARY(first.require_relation("symbol").rows.size() == 2);
  CHECK_UNARY(fact_value_text(first.require_relation("symbol").rows[0][1]) ==
              "usr:A");
}

TEST_CASE("composed facts retain unresolved stable-identity joins") {
  FactSnapshot raw{.provider = "astgraph",
                   .workspace_identity = "workspace:test",
                   .tu_identity = std::nullopt,
                   .applicability = "translation-unit",
                   .catalog_hash = "sha256:catalog",
                   .evidence_references = {},
                   .input_hashes = {"sha256:ast"},
                   .artifact_path = std::nullopt,
                   .relations = {}};
  raw.add_relation(ast_symbols());
  const FactSnapshot joined =
      compose_snapshots(raw, snapshot(),
                        {JoinSpec{.left_relation = "ast_symbol",
                                  .right_relation = "symbol",
                                  .left_key = "usr",
                                  .right_key = "usr",
                                  .output_relation = "analysis/symbol_join"}});

  const auto &result = joined.require_relation("analysis/symbol_join");
  REQUIRE_UNARY(result.rows.size() == 2);
  CHECK_UNARY(fact_value_text(result.rows[0].back()) == "resolved");
  CHECK_UNARY(fact_value_text(result.rows[1].back()) == "unresolved");
  CHECK_UNARY(joined.require_relation("analysis/unresolved_join").rows.size() ==
              1);
}

TEST_CASE("runner distinguishes package incompatibility and stale inputs") {
  AnalysisPackage package{.name = "demo",
                          .version = "1.0.0",
                          .entry_point = "demo",
                          .program = ".output result\n",
                          .content_hash = {},
                          .required_relations = {{.name = "missing",
                                                  .version = 1,
                                                  .catalog_version = 1,
                                                  .columns = {}}}};
  StaticFactProvider provider(snapshot());
  const AnalysisRun incompatible =
      AnalysisRunner(std::make_unique<NoopEngine>()).run(package, provider);
  CHECK_UNARY(incompatible.status == AnalysisStatus::unknown);
  REQUIRE_UNARY(incompatible.diagnostics.size() == 1);
  CHECK_UNARY(incompatible.diagnostics[0].code == "unsupported_relation");

  FactSnapshot stale = snapshot();
  stale.completeness = FactCompleteness::stale;
  StaticFactProvider stale_provider(stale);
  package.required_relations.clear();
  const AnalysisRun stale_result =
      AnalysisRunner(std::make_unique<NoopEngine>())
          .run(package, stale_provider);
  CHECK_UNARY(stale_result.status == AnalysisStatus::unknown);
  REQUIRE_UNARY(stale_result.diagnostics.size() == 1);
  CHECK_UNARY(stale_result.diagnostics[0].code == "stale_input");
}

TEST_CASE("same package, inputs, and options produce the same run identity") {
  const AnalysisPackage package{.name = "demo",
                                .version = "1.0.0",
                                .entry_point = "demo",
                                .program = ".output result\n",
                                .content_hash = {},
                                .required_relations = {}};
  StaticFactProvider provider(snapshot());
  const AnalysisOptions options{.jobs = 2, .step_budget = 100};
  const AnalysisRunner runner(std::make_unique<NoopEngine>());
  const AnalysisRun first = runner.run(package, provider, {}, options);
  const AnalysisRun second = runner.run(package, provider, {}, options);
  CHECK_UNARY(first.run_id == second.run_id);
  CHECK_UNARY(first.input_hash == second.input_hash);
  CHECK_UNARY(first.package_hash == second.package_hash);
}
