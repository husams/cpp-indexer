#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include "analysis/facts.hpp"
#include "analysis/runner.hpp"
#include "astgraph/schema.hpp"
#include "catalogs/generated_catalog.hpp"
#include "storage/sqlite.hpp"
#include "storage/storage.hpp"

namespace {

using namespace cidx::analysis;
namespace fs = std::filesystem;

std::string temp_dir() {
  std::string template_name = "/tmp/cidx_analysis_review_XXXXXX";
  std::vector<char> templ(template_name.begin(), template_name.end());
  templ.push_back('\0');
  char *path = ::mkdtemp(templ.data());
  REQUIRE_UNARY(path != nullptr);
  return path;
}

void put_meta(cidx::SqliteDb &db, const std::string &table,
              const std::string &key, const std::string &value) {
  auto statement =
      db.prepare("INSERT INTO " + table + "(key,value) VALUES (?,?)");
  statement.bind(1, std::string_view(key));
  statement.bind(2, std::string_view(value));
  statement.step_done();
}

std::string make_extension_db(const std::string &root,
                              const std::string &workspace) {
  const std::string path = root + "/banking.extension.db";
  cidx::SqliteDb db(path);
  db.exec("CREATE TABLE meta(key TEXT PRIMARY KEY, value TEXT NOT NULL)");
  db.exec(
      "CREATE TABLE artifact_meta(key TEXT PRIMARY KEY, value TEXT NOT NULL)");
  db.exec("CREATE TABLE banking_fact(usr TEXT, boundary TEXT, score INTEGER)");
  put_meta(db, "artifact_meta", "kind", "extension:banking");
  put_meta(db, "artifact_meta", "workspace_identity", workspace);
  put_meta(db, "artifact_meta", "applicability", "workspace");
  put_meta(db, "artifact_meta", "exposed_relations", "banking_fact");
  put_meta(db, "artifact_meta", "completeness", "complete");
  put_meta(db, "artifact_meta", "truncation", "none");
  put_meta(db, "meta", "schema_version", "1");
  put_meta(db, "meta", "catalog_version",
           std::to_string(cidx::catalog::kCatalogVersion));
  put_meta(db, "meta", "catalog_hash",
           std::string(cidx::catalog::kCatalogHash));
  db.exec("INSERT INTO banking_fact VALUES "
          "('usr:bank.deposit','payments',1),"
          "('usr:bank.transfer','payments',0)");
  return path;
}

std::string make_ast_db(const std::string &root, const std::string &workspace,
                        const std::string &tu) {
  const std::string path = root + "/banking.ast.db";
  cidx::SqliteDb db(path);
  db.exec("CREATE TABLE meta(key TEXT PRIMARY KEY, value TEXT NOT NULL)");
  db.exec(
      "CREATE TABLE artifact_meta(key TEXT PRIMARY KEY, value TEXT NOT NULL)");
  db.exec(
      "CREATE TABLE file(id INTEGER PRIMARY KEY, path TEXT, is_main INTEGER)");
  db.exec("CREATE TABLE node_kind(id INTEGER PRIMARY KEY, name TEXT, category "
          "TEXT)");
  db.exec("CREATE TABLE relation_kind(id INTEGER PRIMARY KEY, name TEXT)");
  db.exec("CREATE TABLE symbol(id INTEGER PRIMARY KEY, usr TEXT, name TEXT, "
          "kind_id INTEGER, linkage INTEGER)");
  db.exec(
      "CREATE TABLE node(id INTEGER PRIMARY KEY, kind_id INTEGER, symbol_id "
      "INTEGER, type_id INTEGER, spelling TEXT, file_id INTEGER, line "
      "INTEGER, col INTEGER, end_line INTEGER, end_col INTEGER, "
      "is_definition INTEGER)");
  db.exec("CREATE TABLE edge(src_id INTEGER, dst_id INTEGER, rel_id INTEGER, "
          "ord INTEGER)");
  put_meta(db, "meta", "schema_version",
           std::to_string(cidx::astgraph::kSchemaVersion));
  put_meta(db, "meta", "catalog_version",
           std::to_string(cidx::catalog::kCatalogVersion));
  put_meta(db, "meta", "catalog_hash",
           std::string(cidx::catalog::kCatalogHash));
  put_meta(db, "meta", "status", "complete");
  put_meta(db, "artifact_meta", "workspace_identity", workspace);
  put_meta(db, "artifact_meta", "tu_identity", tu);
  put_meta(db, "artifact_meta", "completeness", "complete");
  put_meta(db, "artifact_meta", "truncation", "none");
  db.exec("INSERT INTO file VALUES (1,'banking.cpp',1)");
  db.exec("INSERT INTO node_kind VALUES (1,'function','decl')");
  db.exec("INSERT INTO relation_kind VALUES (1,'calls')");
  db.exec("INSERT INTO symbol VALUES (1,'usr:bank.deposit','deposit',1,0)");
  db.exec("INSERT INTO node VALUES "
          "(10,1,1,0,'deposit',1,4,1,4,8,1),"
          "(11,1,0,0,'near_miss',1,8,1,8,9,1)");
  return path;
}

class SnapshotEngine final : public AnalysisEngine {
public:
  [[nodiscard]] std::string engine_version() const override {
    return "test-snapshot-engine";
  }

  [[nodiscard]] AnalysisRun
  execute(const AnalysisPackage &package, const FactSnapshot &facts,
          const AnalysisOptions &options) const override {
    (void)package;
    (void)options;
    AnalysisRun result;
    result.relations = facts.relations;
    result.engine_version = engine_version();
    return result;
  }
};

class SlowEngine final : public AnalysisEngine {
public:
  [[nodiscard]] AnalysisRun
  execute(const AnalysisPackage &package, const FactSnapshot &facts,
          const AnalysisOptions &options) const override {
    (void)package;
    (void)options;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    AnalysisRun result;
    result.relations = facts.relations;
    return result;
  }
};

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
                      .source_revision = std::nullopt,
                      .source_fingerprint = std::nullopt,
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
                   .source_revision = std::nullopt,
                   .source_fingerprint = std::nullopt,
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
                          .prelude = {},
                          .include_catalog_prelude = true,
                          .content_hash = {},
                          .required_relations = {{.name = "missing",
                                                  .version = 1,
                                                  .catalog_version = 1,
                                                  .columns = {}}},
                          .output_relations = {}};
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
                                .prelude = {},
                                .include_catalog_prelude = true,
                                .content_hash = {},
                                .required_relations = {},
                                .output_relations = {}};
  StaticFactProvider provider(snapshot());
  const AnalysisOptions options{.jobs = 2,
                                .step_budget = 100,
                                .time_budget_ms = 600'000,
                                .output_budget = 0,
                                .artifact_root = std::nullopt,
                                .capture_budget = 1'048'576};
  const AnalysisRunner runner(std::make_unique<NoopEngine>());
  const AnalysisRun first = runner.run(package, provider, {}, options);
  const AnalysisRun second = runner.run(package, provider, {}, options);
  CHECK_UNARY(first.run_id == second.run_id);
  CHECK_UNARY(first.input_hash == second.input_hash);
  CHECK_UNARY(first.package_hash == second.package_hash);
}

TEST_CASE("real semantic provider reports workspace identity and freshness") {
  const std::string root = temp_dir();
  const std::string path = root + "/index.sqlite";
  std::string workspace;
  {
    cidx::Storage storage(path);
    storage.mint_symbol_id("usr:bank.deposit", "deposit", "deposit");
    storage.stamp_index_identity();
    workspace = storage.index_identity().workspace;
  }
  const FactSnapshot facts = SqliteFactProvider(path).snapshot({});
  CHECK_UNARY(facts.provider == "semantic-index");
  CHECK_UNARY(facts.workspace_identity == workspace);
  CHECK_UNARY(facts.workspace_identity != "unknown");
  CHECK_UNARY(facts.applicability == "workspace");
  CHECK_UNARY(facts.freshness == FactFreshness::current);
  CHECK_UNARY(facts.find_relation("symbol") != nullptr);
}

TEST_CASE("public service runs a real extension Souffle package") {
  const std::string root = temp_dir();
  const std::string path = make_extension_db(root, "workspace:banking");
  const AnalysisPackage package{
      .name = "banking-positive",
      .version = "1.0.0",
      .entry_point = "positive",
      .engine = "souffle",
      .program =
          ".decl banking_fact(usr:symbol, boundary:symbol, score:number)\n"
          ".input banking_fact\n"
          ".decl hit(usr:symbol)\n"
          "hit(usr) :- banking_fact(usr, \"payments\", 1).\n"
          ".output hit\n",
      .prelude = {},
      .include_catalog_prelude = false,
      .content_hash = {},
      .required_relations = {{.name = "banking_fact",
                              .version = 1,
                              .catalog_version = 1,
                              .columns = {}}},
      .output_relations = {
          {.name = "hit", .version = 1, .catalog_version = 1, .columns = {}}}};
  const AnalysisRequest request{
      .package = package,
      .provider = ProviderDeclaration{.kind = ProviderKind::extension,
                                      .path = path,
                                      .left = {},
                                      .right = {},
                                      .joins = {}},
      .facts = {},
      .options = AnalysisOptions{.jobs = 1,
                                 .step_budget = 0,
                                 .time_budget_ms = 600'000,
                                 .output_budget = 0,
                                 .artifact_root = root + "/retained",
                                 .capture_budget = 1'048'576}};
  const AnalysisRun result = AnalysisService().run(request);
  CHECK_UNARY(result.status == AnalysisStatus::complete);
  CHECK_UNARY(result.result_class == AnalysisResultClass::complete);
  REQUIRE_UNARY(result.relations.contains("hit"));
  CHECK_UNARY(result.relations.at("hit").rows.size() == 1);
  CHECK_UNARY(result.generated_inputs.size() >= 2);
  CHECK_UNARY(!result.engine_version.empty());
  CHECK_UNARY(fs::exists(root + "/retained"));
  const AnalysisRun replay = AnalysisService().run(request);
  CHECK_UNARY(replay.run_id == result.run_id);
  CHECK_UNARY(replay.generated_inputs == result.generated_inputs);
  CHECK_UNARY(replay.artifact_hash() == result.artifact_hash());
}

TEST_CASE("public service composes real AST and semantic providers") {
  const std::string root = temp_dir();
  const std::string semantic_path = root + "/index.sqlite";
  std::string workspace;
  {
    cidx::Storage storage(semantic_path);
    storage.mint_symbol_id("usr:bank.deposit", "deposit", "deposit");
    storage.stamp_index_identity();
    workspace = storage.index_identity().workspace;
  }
  const std::string ast_path = make_ast_db(root, workspace, "tu:banking:1");
  const AnalysisPackage package{.name = "banking-composed",
                                .version = "1.0.0",
                                .entry_point = "joined",
                                .engine = "capture",
                                .program = "capture",
                                .prelude = {},
                                .include_catalog_prelude = false,
                                .content_hash = {},
                                .required_relations = {},
                                .output_relations = {}};
  const auto left = std::make_shared<ProviderDeclaration>(
      ProviderDeclaration{.kind = ProviderKind::astgraph,
                          .path = ast_path,
                          .left = {},
                          .right = {},
                          .joins = {}});
  const auto right = std::make_shared<ProviderDeclaration>(
      ProviderDeclaration{.kind = ProviderKind::semantic_index,
                          .path = semantic_path,
                          .left = {},
                          .right = {},
                          .joins = {}});
  const AnalysisRequest request{
      .package = package,
      .provider =
          ProviderDeclaration{.kind = ProviderKind::composed,
                              .path = {},
                              .left = left,
                              .right = right,
                              .joins = {{.left_relation = "ast_node",
                                         .right_relation = "symbol",
                                         .left_key = "usr",
                                         .right_key = "usr",
                                         .output_relation = "banking/join"}}},
      .facts = {},
      .options = {}};
  const AnalysisRun result = AnalysisService([](const AnalysisPackage &) {
                               return std::make_unique<SnapshotEngine>();
                             }).run(request);
  CHECK_UNARY(result.status == AnalysisStatus::partial);
  REQUIRE_UNARY(result.relations.contains("banking/join"));
  CHECK_UNARY(result.relations.at("banking/join").rows.size() == 2);
  REQUIRE_UNARY(result.relations.contains("analysis/unresolved_join"));
  CHECK_UNARY(result.relations.at("analysis/unresolved_join").rows.size() == 1);
  CHECK_UNARY(
      fact_value_text(
          result.relations.at("analysis/unresolved_join").rows[0].back()) ==
      "missing_identity");
}

TEST_CASE("composition propagates both provider completeness and truncation") {
  const FactSnapshot left = snapshot();
  for (const auto completeness :
       {FactCompleteness::partial, FactCompleteness::unknown,
        FactCompleteness::stale}) {
    FactSnapshot right = snapshot();
    right.provider = "astgraph";
    right.applicability = "translation-unit";
    right.completeness = completeness;
    right.truncated = completeness == FactCompleteness::partial;
    if (completeness == FactCompleteness::stale) {
      right.freshness = FactFreshness::stale;
    } else if (completeness == FactCompleteness::unknown) {
      right.freshness = FactFreshness::unknown;
    } else {
      right.freshness = FactFreshness::current;
    }
    const FactSnapshot joined =
        compose_snapshots(left, right,
                          {{.left_relation = "symbol",
                            .right_relation = "symbol",
                            .left_key = "usr",
                            .right_key = "usr",
                            .output_relation = "banking/join"}});
    CHECK_UNARY(joined.completeness == completeness ||
                (completeness == FactCompleteness::partial &&
                 joined.completeness == FactCompleteness::partial));
    CHECK_UNARY(joined.truncated == right.truncated);
  }
}

TEST_CASE("budgets and typed provider or engine failures remain distinct") {
  const std::string root = temp_dir();
  const std::string semantic_path = root + "/index.sqlite";
  {
    cidx::Storage storage(semantic_path);
    storage.mint_symbol_id("usr:bank.deposit", "deposit", "deposit");
    storage.mint_symbol_id("usr:bank.transfer", "transfer", "transfer");
    storage.stamp_index_identity();
  }
  const AnalysisPackage package{.name = "budgeted",
                                .version = "1.0.0",
                                .entry_point = "run",
                                .engine = "capture",
                                .program = "capture",
                                .prelude = {},
                                .include_catalog_prelude = false,
                                .content_hash = {},
                                .required_relations = {},
                                .output_relations = {}};
  const AnalysisRequest step_request{
      .package = package,
      .provider = ProviderDeclaration{.kind = ProviderKind::semantic_index,
                                      .path = semantic_path,
                                      .left = {},
                                      .right = {},
                                      .joins = {}},
      .facts = {},
      .options = AnalysisOptions{.jobs = 1,
                                 .step_budget = 1,
                                 .time_budget_ms = 600'000,
                                 .output_budget = 0,
                                 .artifact_root = std::nullopt,
                                 .capture_budget = 1'048'576}};
  const AnalysisRun step = AnalysisService([](const AnalysisPackage &) {
                             return std::make_unique<SnapshotEngine>();
                           }).run(step_request);
  CHECK_UNARY(step.result_class == AnalysisResultClass::step_budget_exceeded);

  AnalysisRequest output_request = step_request;
  output_request.options.step_budget = 0;
  output_request.options.output_budget = 1;
  const AnalysisRun output = AnalysisService([](const AnalysisPackage &) {
                               return std::make_unique<SnapshotEngine>();
                             }).run(output_request);
  CHECK_UNARY(output.result_class ==
              AnalysisResultClass::output_budget_exceeded);

  AnalysisRequest timeout_request = step_request;
  timeout_request.options.step_budget = 0;
  timeout_request.options.time_budget_ms = 1;
  const AnalysisRun timeout = AnalysisService([](const AnalysisPackage &) {
                                return std::make_unique<SlowEngine>();
                              }).run(timeout_request);
  CHECK_UNARY(timeout.result_class == AnalysisResultClass::timeout);

  AnalysisPackage missing_package = package;
  missing_package.engine = "souffle";
  const AnalysisRun missing = AnalysisService().run(AnalysisRequest{
      .package = missing_package,
      .provider = ProviderDeclaration{.kind = ProviderKind::astgraph,
                                      .path = "/tmp/does-not-exist.db",
                                      .left = {},
                                      .right = {},
                                      .joins = {}},
      .facts = {},
      .options = {}});
  CHECK_UNARY(missing.result_class == AnalysisResultClass::missing_tu);

  AnalysisPackage invalid_package = package;
  invalid_package.program.clear();
  const AnalysisRun invalid_package_result =
      AnalysisService([](const AnalysisPackage &) {
        return std::make_unique<SnapshotEngine>();
      })
          .run(AnalysisRequest{
              .package = invalid_package,
              .provider =
                  ProviderDeclaration{.kind = ProviderKind::semantic_index,
                                      .path = semantic_path,
                                      .left = {},
                                      .right = {},
                                      .joins = {}},
              .facts = {},
              .options = {}});
  CHECK_UNARY(invalid_package_result.result_class ==
              AnalysisResultClass::invalid_package);

  const AnalysisRun invalid =
      AnalysisService([](const AnalysisPackage &) {
        throw AnalysisEngineError("engine_failure", "banking engine failed");
        return std::unique_ptr<AnalysisEngine>{};
      })
          .run(AnalysisRequest{.package = package,
                               .provider = ProviderDeclaration{},
                               .facts = {},
                               .options = {}});
  CHECK_UNARY(invalid.result_class == AnalysisResultClass::engine_failure);
}

TEST_CASE(
    "fact publication maps names safely and invalidates prior generations") {
  const std::string root = temp_dir();
  FactSnapshot facts = snapshot();
  facts.add_relation(FactRelation{
      .descriptor = RelationDescriptor{.name = "analysis/unresolved_join",
                                       .version = 1,
                                       .catalog_version = 1,
                                       .columns = {{.name = "key",
                                                    .type = FactType::string}}},
      .rows = {{"usr:bank.deposit"}}});
  const std::string fact_dir = root + "/facts";
  (void)write_fact_files(facts, fact_dir, "");
  if (!fs::exists(fact_dir + "/cidx_facts.map") ||
      fs::exists(fact_dir + "/analysis") ||
      fact_file_name("analysis/unresolved_join") ==
          "analysis/unresolved_join.facts") {
    throw std::runtime_error("fact-file mapping escaped its output directory");
  }

  const std::string db_path = root + "/index.sqlite";
  cidx::Storage storage(db_path);
  AnalysisRun run;
  run.run_id = "stable-run";
  run.input_hash = "input-hash";
  run.package_hash = "package-hash";
  run.engine_version = "souffle:1";
  run.relations = facts.relations;
  const auto first = AnalysisPublisher::publish(
      storage, root + "/artifacts", "banking", run, "workspace:banking");
  run.relations["symbol"].rows.push_back({3, "usr:bank.new", "new"});
  const auto second = AnalysisPublisher::publish(
      storage, root + "/artifacts", "banking", run, "workspace:banking");
  if (first.content_hash == second.content_hash) {
    throw std::runtime_error("publication did not create a new generation");
  }
  cidx::ArtifactStore artifacts(storage, root + "/artifacts");
  const auto current = artifacts.current("analysis:banking:stable-run");
  if (!current || current->content_hash != second.content_hash ||
      !artifacts.validate("analysis:banking:stable-run").usable()) {
    throw std::runtime_error("published analysis generation is not current");
  }
  bool rejected = false;
  try {
    (void)AnalysisPublisher::publish(storage, root + "/artifacts", "core", run,
                                     "workspace:banking");
  } catch (const std::exception &) {
    rejected = true;
  }
  CHECK_UNARY(rejected);
}
