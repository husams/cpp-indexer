#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "storage/ports.hpp"
#include "storage/sqlite_adapters.hpp"
#include "storage/storage.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <set>
#include <stdexcept>
#include <type_traits>

using namespace cidx;
using namespace cidx::storage;

TEST_CASE("focused ports expose domain records without SQLite") {
  static_assert(std::is_abstract_v<WorkspaceCatalogReadPort>);
  static_assert(std::is_abstract_v<WorkspaceCatalogWritePort>);
  static_assert(std::is_abstract_v<SourceStoreReadPort>);
  static_assert(std::is_abstract_v<SymbolReadPort>);
  static_assert(std::is_abstract_v<SymbolWritePort>);
  static_assert(std::is_abstract_v<TypeReadPort>);
  static_assert(std::is_abstract_v<FactWritePort>);
  static_assert(std::is_abstract_v<DefinitionWritePort>);
  static_assert(std::is_abstract_v<IncludeReadPort>);
  static_assert(std::is_abstract_v<SchemaCatalogReadPort>);
  static_assert(std::is_abstract_v<UnitOfWork>);
}

TEST_CASE("SQLite adapters preserve domain records and separate writes") {
  Storage db(":memory:");
  SqliteWorkspaceCatalogAdapter catalog(db);
  SqliteSourceStoreAdapter source(db);
  SqliteSymbolStoreAdapter symbols(db);

  const ComponentWriteRecord component{"ports", "/tmp/ports", "repo", "v1"};
  const auto component_id = catalog.add_component(component);
  const auto directory_id = db.add_directory(component_id, "");
  const auto file_id = source.add_file(directory_id, "main.cpp");
  CHECK(file_id == db.add_file(directory_id, "main.cpp"));

  Symbol symbol;
  symbol.usr = "ports:@F@main";
  symbol.spelling = "main";
  symbol.kind = "function";
  symbol.file_id = file_id;
  symbol.is_definition = true;
  const auto symbol_id = symbols.add_symbol(symbol);

  const auto read_port = static_cast<SymbolReadPort *>(&symbols);
  REQUIRE(read_port->lookup_symbol_by_id(symbol_id).has_value());
  CHECK(read_port->lookup_symbol_by_id(symbol_id)->usr == symbol.usr);
  const auto stored_component = catalog.get_component_by_id(component_id);
  REQUIRE(stored_component.has_value());
  CHECK(stored_component->name == component.name);
  CHECK(stored_component->path == component.path);
  CHECK(stored_component->kind == component.kind);
  CHECK(stored_component->version == component.version);

  const auto universe_id =
      db.add_semantic_universe("ports-universe", "Ports", "explicit");
  const RepositoryWriteRecord repository{
      "ports-repo", "repo", "https://example.invalid/ports", universe_id};
  const auto repository_id = catalog.add_repository(repository);
  const auto stored_repository = db.get_repository_by_id(repository_id);
  REQUIRE(stored_repository.has_value());
  CHECK(stored_repository->name == repository.name);
  CHECK(stored_repository->kind == repository.kind);
  CHECK(stored_repository->remote_url == repository.remote_url);
  CHECK(stored_repository->semantic_universe_id ==
        repository.semantic_universe_id);

  const SymbolIdentityRecord identity{"ports:@F@minted",
                                      "minted",
                                      "ports::minted",
                                      "ports::minted()",
                                      "function",
                                      std::nullopt,
                                      11,
                                      7,
                                      "system/ports.hpp",
                                      true,
                                      true,
                                      "int ()",
                                      universe_id,
                                      "system/ports.hpp",
                                      "internal",
                                      "ports-tu"};
  const auto minted_id = symbols.mint_symbol_id(identity);
  const auto minted = db.lookup_symbol_by_id(minted_id);
  REQUIRE(minted.has_value());
  CHECK(minted->usr == identity.usr);
  CHECK(minted->spelling == identity.spelling);
  CHECK(minted->qual_name == identity.qual_name);
  CHECK(minted->display_name == identity.display_name);
  CHECK(minted->type_info == identity.type_info);
  CHECK(minted->decl_line == identity.decl_line);
  CHECK(minted->decl_col == identity.decl_col);
  CHECK(minted->decl_path == identity.decl_path);
  CHECK(minted->is_instantiation == identity.is_instantiation);
  CHECK(minted->semantic_universe_id == *identity.semantic_universe_id);
  CHECK(minted->identity_key.contains("system/ports.hpp"));
  CHECK(minted->identity_key.contains("ports-tu"));

  auto &facade_catalog = db.workspace_catalog_write();
  const auto facade_component_id = facade_catalog.add_component(
      ComponentWriteRecord{"facade", "/tmp/facade", "repo", std::nullopt});
  const auto facade_component =
      db.workspace_catalog_read().get_component_by_id(facade_component_id);
  REQUIRE(facade_component.has_value());
  CHECK(facade_component->name == "facade");
}

TEST_CASE("unit of work port commits and rolls back as a boundary") {
  Storage db(":memory:");
  SqliteWorkspaceCatalogAdapter catalog(db);
  SqliteUnitOfWorkFactory units(db);

  {
    auto unit = units.begin();
    catalog.add_component(
        ComponentWriteRecord{"rolled-back", "/tmp/rolled-back", "repo", {}});
    unit->rollback();
  }
  CHECK_FALSE(catalog.get_component("/tmp/rolled-back").has_value());

  {
    auto unit = units.begin();
    catalog.add_component(
        ComponentWriteRecord{"committed", "/tmp/committed", "repo", {}});
    unit->commit();
  }
  CHECK(catalog.get_component("/tmp/committed").has_value());
}

TEST_CASE("unit of work rolls back a failed one-TU publication") {
  Storage db(":memory:");
  SqliteWorkspaceCatalogAdapter catalog(db);
  SqliteUnitOfWorkFactory units(db);

  try {
    auto unit = units.begin();
    catalog.add_component(
        ComponentWriteRecord{"failed-tu", "/tmp/failed-tu", "repo", {}});
    throw std::runtime_error("injected publication failure");
  } catch (const std::runtime_error &) {
  }

  CHECK_FALSE(catalog.get_component("/tmp/failed-tu").has_value());
}

TEST_CASE("transform registry declares a deterministic dependency order") {
  TransformRegistry registry;
  registry.register_source_fact(TransformSourceFact{.name = "raw"});
  TransformDescriptor source;
  source.id = "source";
  source.version = 1;
  source.input_facts = {"raw"};
  source.produced_facts = {"source.fact"};
  source.invalidation_keys = {"source"};
  source.invalidation_inputs = {
      TransformInvalidationInput{.name = "source",
                                 .kind = TransformInputKind::source,
                                 .provider_id = "test.source.v1",
                                 .value_query = {},
                                 .static_value = "source"}};
  source.options = {"deterministic-sql-v1"};
  source.input_queries = {"SELECT 1"};
  source.output_queries = {"SELECT 1"};
  source.output_count_query = "SELECT 1";
  source.implementation_provider =
      TransformImplementationProvider{.provider_id = "test.source.executor.v1",
                                      .version = 1,
                                      .content = "source-executor"};
  source.fact_set_requirements = {TransformFactSetRequirement{
      .name = "source.fact", .facts = {"source.fact"}}};
  registry.register_transform(source);
  TransformDescriptor derived;
  derived.id = "derived";
  derived.version = 1;
  derived.input_facts = {"source.fact"};
  derived.produced_facts = {"derived.fact"};
  derived.dependencies = {"source"};
  derived.invalidation_keys = {"derived"};
  derived.invalidation_inputs = {
      TransformInvalidationInput{.name = "derived",
                                 .kind = TransformInputKind::source,
                                 .provider_id = "test.derived.v1",
                                 .value_query = {},
                                 .static_value = "derived"}};
  derived.options = {"deterministic-sql-v1"};
  derived.input_queries = {"SELECT 1"};
  derived.output_queries = {"SELECT 1"};
  derived.output_count_query = "SELECT 1";
  derived.implementation_provider =
      TransformImplementationProvider{.provider_id = "test.derived.executor.v1",
                                      .version = 1,
                                      .content = "derived-executor"};
  derived.fact_set_requirements = {TransformFactSetRequirement{
      .name = "derived.fact", .facts = {"derived.fact"}}};
  registry.register_transform(derived);

  const auto order = registry.execution_order();
  REQUIRE(order.size() == 2);
  CHECK(order[0]->id == "source");
  CHECK(order[1]->id == "derived");

  TransformRegistry invalid;
  TransformDescriptor missing_provider = source;
  missing_provider.id = "missing-provider";
  missing_provider.produced_facts = {"missing.fact"};
  missing_provider.fact_set_requirements = {TransformFactSetRequirement{
      .name = "missing.fact", .facts = {"missing.fact"}}};
  missing_provider.invalidation_inputs.clear();
  invalid.register_transform(std::move(missing_provider));
  bool rejected = false;
  try {
    (void)invalid.execution_order();
  } catch (const std::invalid_argument &) {
    rejected = true;
  }
  CHECK(rejected);
}

TEST_CASE("transform registry rejects invalid named fact graphs") {
  const auto fact_transform =
      [](std::string id, std::vector<std::string> inputs,
         std::vector<std::string> outputs, std::vector<std::string> deps = {}) {
        TransformDescriptor result;
        result.id = std::move(id);
        result.version = 1;
        result.input_facts = std::move(inputs);
        result.produced_facts = std::move(outputs);
        result.dependencies = std::move(deps);
        result.invalidation_keys = {"source"};
        result.invalidation_inputs = {
            TransformInvalidationInput{.name = "source",
                                       .kind = TransformInputKind::source,
                                       .provider_id = "test.source.v1",
                                       .static_value = "source"}};
        result.options = {"deterministic-sql-v1"};
        result.input_queries = {"SELECT 1"};
        result.output_queries = {"SELECT 1"};
        result.output_count_query = "SELECT 1";
        result.implementation_provider = TransformImplementationProvider{
            .provider_id = result.id + ".executor.v1",
            .version = 1,
            .content = result.id + "-executor"};
        for (const auto &fact : result.produced_facts) {
          result.fact_set_requirements.push_back(
              TransformFactSetRequirement{.name = fact, .facts = {fact}});
        }
        return result;
      };
  const auto expect_rejected = [](TransformRegistry registry) {
    bool rejected = false;
    try {
      (void)registry.execution_order();
    } catch (const std::invalid_argument &) {
      rejected = true;
    }
    CHECK(rejected);
  };

  {
    TransformRegistry registry;
    registry.register_source_fact(TransformSourceFact{.name = "raw"});
    auto transform = fact_transform("bad-alias", {"raw"}, {"actual.fact"});
    transform.fact_set_requirements.push_back(TransformFactSetRequirement{
        .name = "bad-alias", .facts = {"undeclared.fact"}});
    registry.register_transform(std::move(transform));
    expect_rejected(std::move(registry));
  }
  {
    TransformRegistry registry;
    registry.register_source_fact(TransformSourceFact{.name = "raw"});
    auto transform = fact_transform("incompatible", {"raw"}, {"fact"});
    transform.input_catalog = "other-catalog";
    registry.register_transform(std::move(transform));
    expect_rejected(std::move(registry));
  }
  {
    TransformRegistry registry;
    registry.register_source_fact(TransformSourceFact{.name = "raw"});
    registry.register_transform(
        fact_transform("producer-a", {"raw"}, {"duplicate.fact"}));
    registry.register_transform(
        fact_transform("producer-b", {"raw"}, {"duplicate.fact"}));
    expect_rejected(std::move(registry));
  }
  {
    TransformRegistry registry;
    registry.register_source_fact(TransformSourceFact{.name = "raw"});
    registry.register_transform(
        fact_transform("missing-dependency", {"raw"}, {"derived.fact"}, {}));
    auto consumer =
        fact_transform("consumer", {"derived.fact"}, {"consumer.fact"});
    registry.register_transform(std::move(consumer));
    expect_rejected(std::move(registry));
  }
  {
    TransformRegistry registry;
    registry.register_source_fact(TransformSourceFact{.name = "raw"});
    registry.register_transform(
        fact_transform("undeclared-input", {"missing.fact"}, {"fact"}));
    expect_rejected(std::move(registry));
  }
  {
    TransformRegistry registry;
    auto first = fact_transform("cycle-a", {"cycle-b.fact"}, {"cycle-a.fact"},
                                {"cycle-b"});
    auto second = fact_transform("cycle-b", {"cycle-a.fact"}, {"cycle-b.fact"},
                                 {"cycle-a"});
    registry.register_transform(std::move(first));
    registry.register_transform(std::move(second));
    expect_rejected(std::move(registry));
  }
}

TEST_CASE("named transform pipeline reuses identical content identities") {
  Storage db(":memory:");

  Symbol source;
  source.usr = "transform:@F@nonempty-source";
  source.spelling = "nonempty-source";
  source.kind = "function";
  source.is_definition = true;
  source.resolved = true;
  const auto source_id = db.add_symbol(source);
  Symbol target;
  target.usr = "transform:@F@nonempty-target";
  target.spelling = "nonempty-target";
  target.kind = "function";
  target.is_definition = true;
  target.resolved = true;
  const auto target_id = db.add_symbol(target);
  Edge edge;
  edge.src_id = source_id;
  edge.dst_id = target_id;
  edge.kind = 1;
  db.add_edge(edge);

  const auto first = db.run_transform_pipeline();
  REQUIRE(first.runs.size() == 9);
  CHECK(first.still_stub_count == 0);
  for (const auto &run : first.runs) {
    CHECK((run.status == TransformRunStatus::ran ||
           run.status == TransformRunStatus::skipped));
    CHECK_FALSE(run.input_identity.empty());
    CHECK_FALSE(run.output_identity.empty());
  }

  const auto second = db.run_transform_pipeline();
  REQUIRE(second.runs.size() == first.runs.size());
  for (const auto &run : second.runs) {
    CHECK(run.status == TransformRunStatus::reused);
  }
}

TEST_CASE("reused attempt and last-run history survive close and reopen") {
  const auto path =
      std::filesystem::temp_directory_path() / "cidx-hse67-reused-history.db";
  std::filesystem::remove(path);
  {
    Storage db(path.string());
    REQUIRE(db.run_transform_pipeline().complete);
    const auto reused = db.run_transform_pipeline();
    REQUIRE(reused.complete);
    CHECK(std::ranges::all_of(reused.runs, [](const TransformRun &run) {
      return run.status == TransformRunStatus::reused;
    }));
    auto history =
        db.raw_db().prepare("SELECT value FROM meta WHERE key = "
                            "'transform.entity-graph-rollup.history.last_run'");
    REQUIRE(history.step());
    CHECK(history.col_text(0).find("reused|") == 0);
    auto published_generation = db.raw_db().prepare(
        "SELECT value FROM meta WHERE key = "
        "'transform.entity-graph-rollup.published.generation'");
    REQUIRE(published_generation.step());
    CHECK(published_generation.col_text(0) == "1");
  }
  {
    Storage db(path.string());
    const auto status = db.transform_status("entity-graph");
    REQUIRE(status.runs.size() == 1);
    CHECK(status.runs.front().status == TransformRunStatus::reused);
    CHECK(db.transform_explain("entity-graph").find("reused") !=
          std::string::npos);
  }
  std::filesystem::remove(path);
}

TEST_CASE("transform requalifies mutated derived output before reuse") {
  Storage db(":memory:");
  Symbol src;
  src.usr = "transform:@F@mutation-src";
  src.spelling = "mutation-src";
  src.kind = "function";
  src.is_definition = true;
  const auto src_id = db.add_symbol(src);
  Symbol dst = src;
  dst.usr = "transform:@F@mutation-dst";
  dst.spelling = "mutation-dst";
  const auto dst_id = db.add_symbol(dst);
  Edge edge;
  edge.src_id = src_id;
  edge.dst_id = dst_id;
  edge.kind = 1;
  const auto edge_id = db.add_edge(edge);
  const auto component_id = db.add_component("mutation", "/tmp/mutation");
  const auto directory_id = db.add_directory(component_id, "");
  const auto file_id = db.add_file(directory_id, "mutation.cpp");
  db.add_edge_site(
      EdgeSite{.edge_id = edge_id, .file_id = file_id, .line = 1, .col = 1});
  REQUIRE(db.run_transform_pipeline().complete);

  db.raw_db().exec("UPDATE edge SET count = count + 99 WHERE kind = 1");
  const auto repaired = db.run_transform_pipeline();
  const auto it =
      std::ranges::find_if(repaired.runs, [](const TransformRun &run) {
        return run.transform_id == "edge-site-count-rollup";
      });
  REQUIRE(it != repaired.runs.end());
  CHECK(it->status == TransformRunStatus::ran);
  CHECK(it->diagnostic.find("mutation") != std::string::npos);
  CHECK(db.run_transform_pipeline().complete);
}

TEST_CASE("non-reproducing derived mutation fails qualification and preserves "
          "publication") {
  const auto path = std::filesystem::temp_directory_path() /
                    "cidx-hse67-qualification-rollback.db";
  std::filesystem::remove(path);
  std::string published_identity;
  int64_t edge_id = -1;
  {
    Storage db(path.string());
    Symbol src;
    src.usr = "transform:@F@nondeterministic-src";
    src.spelling = "nondeterministic-src";
    src.kind = "function";
    src.is_definition = true;
    const auto src_id = db.add_symbol(src);
    Symbol dst = src;
    dst.usr = "transform:@F@nondeterministic-dst";
    dst.spelling = "nondeterministic-dst";
    const auto dst_id = db.add_symbol(dst);
    edge_id = db.add_edge(Edge{.src_id = src_id,
                               .dst_id = dst_id,
                               .kind = 1,
                               .count = 1,
                               .base_access = std::nullopt,
                               .is_virtual = std::nullopt});
    const auto component_id =
        db.add_component("qualification", "/tmp/qualification");
    const auto directory_id = db.add_directory(component_id, "");
    const auto file_id = db.add_file(directory_id, "qualification.cpp");
    db.add_edge_site(
        EdgeSite{.edge_id = edge_id, .file_id = file_id, .line = 1, .col = 1});
    REQUIRE(db.run_transform_pipeline().complete);
    auto published = db.raw_db().prepare(
        "SELECT value FROM meta WHERE key = "
        "'transform.edge-site-count-rollup.published.output'");
    REQUIRE(published.step());
    published_identity = published.col_text(0);

    db.raw_db().exec("UPDATE edge SET count = count + 99 WHERE kind = 1");
    db.inject_transform_nondeterminism_for_testing("edge-site-count-rollup");
    const auto failed = db.run_transform_pipeline();
    CHECK(failed.failed);
    const auto status = db.transform_fact_set_status("edge.count");
    CHECK(status.known);
    CHECK_FALSE(status.ready);
    CHECK(status.status == TransformRunStatus::failed);
    CHECK(db.transform_explain("edge.count").find("qualification") !=
          std::string::npos);
    auto retained = db.raw_db().prepare(
        "SELECT value FROM meta WHERE key = "
        "'transform.edge-site-count-rollup.published.output'");
    REQUIRE(retained.step());
    CHECK(retained.col_text(0) == published_identity);
  }
  {
    Storage fresh(path.string());
    auto row = fresh.raw_db().prepare("SELECT count FROM edge WHERE id = ?");
    row.bind(1, edge_id);
    REQUIRE(row.step());
    CHECK(row.col_int64(0) == 1);
    const auto status = fresh.transform_status("edge.count");
    REQUIRE(status.runs.size() == 1);
    CHECK(status.runs.front().output_identity == published_identity);
  }
  std::filesystem::remove(path);
}

TEST_CASE("site-less call and use counts retain indexed values") {
  Storage db(":memory:");
  Symbol src;
  src.usr = "transform:@F@site-less-src";
  src.spelling = "site-less-src";
  src.kind = "function";
  src.is_definition = true;
  const auto src_id = db.add_symbol(src);
  Symbol dst = src;
  dst.usr = "transform:@F@site-less-dst";
  dst.spelling = "site-less-dst";
  const auto dst_id = db.add_symbol(dst);
  const auto call_id = db.add_edge(
      Edge{.src_id = src_id, .dst_id = dst_id, .kind = 1, .count = 7});
  const auto use_id = db.add_edge(
      Edge{.src_id = src_id, .dst_id = dst_id, .kind = 7, .count = 11});
  REQUIRE(db.run_transform_pipeline().complete);
  for (const auto &[edge_id, expected] :
       std::array<std::pair<int64_t, int64_t>, 2>{
           {{call_id, 7}, {use_id, 11}}}) {
    auto row = db.raw_db().prepare("SELECT count FROM edge WHERE id = ?");
    row.bind(1, edge_id);
    REQUIRE(row.step());
    CHECK(row.col_int64(0) == expected);
  }
}

TEST_CASE("dynamic target changes rebuild semantic dependents only") {
  Storage db(":memory:");
  Symbol caller;
  caller.usr = "transform:@F@dynamic-caller";
  caller.spelling = "dynamic-caller";
  caller.kind = "function";
  caller.is_definition = true;
  const auto caller_id = db.add_symbol(caller);
  Symbol target;
  target.usr = "transform:@F@dynamic-target";
  target.spelling = "dynamic-target";
  target.kind = "function";
  target.is_definition = true;
  const auto target_id = db.add_symbol(target);
  const auto caller_def = db.get_or_create_definition(caller_id, std::nullopt);
  (void)db.get_or_create_definition(target_id, std::nullopt);
  (void)db.get_or_create_definition(target_id, std::nullopt);
  db.add_def_edge(caller_def, target_id, 1);
  REQUIRE(db.run_transform_pipeline().complete);

  db.add_def_edge(caller_def, target_id, 1, 1);
  const auto changed = db.run_transform_pipeline();
  const auto status = [&](std::string_view id) {
    const auto it =
        std::ranges::find_if(changed.runs, [&](const TransformRun &run) {
          return run.transform_id == id;
        });
    REQUIRE(it != changed.runs.end());
    return it->status;
  };
  CHECK(status("possible-call-materialization") == TransformRunStatus::ran);
  CHECK(status("hse-66-effect-registration") == TransformRunStatus::ran);
  CHECK(status("hse-66-proof-registration") == TransformRunStatus::ran);
  CHECK(status("include-fact-readiness") == TransformRunStatus::reused);
  CHECK(status("type-fact-readiness") == TransformRunStatus::reused);
}

TEST_CASE("implementation version change closes only declared downstream") {
  Storage db(":memory:");
  REQUIRE(db.run_transform_pipeline().complete);
  // Change the registered provider version, which is coupled to the
  // edge-site executor and participates in the current input identity.
  db.set_transform_implementation_provider_for_testing("edge-site-count-rollup",
                                                       2);
  const auto changed = db.run_transform_pipeline();
  const auto status = [&](std::string_view id) {
    const auto it =
        std::ranges::find_if(changed.runs, [&](const TransformRun &run) {
          return run.transform_id == id;
        });
    REQUIRE(it != changed.runs.end());
    return it->status;
  };
  CHECK(status("edge-site-count-rollup") == TransformRunStatus::ran);
  CHECK(status("virtual-dispatch-call-materialization") ==
        TransformRunStatus::ran);
  CHECK(status("entity-graph-rollup") == TransformRunStatus::ran);
  CHECK(status("hse-66-effect-registration") == TransformRunStatus::ran);
  CHECK(status("hse-66-proof-registration") == TransformRunStatus::ran);
  CHECK(status("multi-definition-classification") ==
        TransformRunStatus::reused);
  CHECK(status("include-fact-readiness") == TransformRunStatus::reused);
  CHECK(status("type-fact-readiness") == TransformRunStatus::reused);
}

TEST_CASE(
    "transform budgets fail atomically and retain the published generation") {
  Storage db(":memory:");
  Symbol a;
  a.usr = "transform:@F@budget-a";
  a.spelling = "budget-a";
  a.kind = "function";
  a.is_definition = true;
  const auto a_id = db.add_symbol(a);
  Symbol b = a;
  b.usr = "transform:@F@budget-b";
  b.spelling = "budget-b";
  const auto b_id = db.add_symbol(b);
  Symbol c = a;
  c.usr = "transform:@F@budget-c";
  c.spelling = "budget-c";
  const auto c_id = db.add_symbol(c);
  Edge first{.src_id = a_id,
             .dst_id = b_id,
             .kind = 1,
             .count = 1,
             .base_access = std::nullopt,
             .is_virtual = std::nullopt,
             .vtable_slot = std::nullopt};
  Edge second{.src_id = a_id,
              .dst_id = c_id,
              .kind = 1,
              .count = 1,
              .base_access = std::nullopt,
              .is_virtual = std::nullopt,
              .vtable_slot = std::nullopt};
  db.add_edge(first);
  db.add_edge(second);
  REQUIRE(db.run_transform_pipeline().complete);
  db.set_transform_budget_for_testing("edge-site-count-rollup", 1, 0);
  db.set_transform_invalidation_for_testing("source", "budget-boundary");
  const auto failed = db.run_transform_pipeline();
  CHECK(failed.failed);
  const auto it =
      std::ranges::find_if(failed.runs, [](const TransformRun &run) {
        return run.transform_id == "edge-site-count-rollup";
      });
  REQUIRE(it != failed.runs.end());
  CHECK(it->status == TransformRunStatus::failed);
  CHECK(it->diagnostic.find("max_rows") != std::string::npos);
  auto published = db.raw_db().prepare(
      "SELECT value FROM meta WHERE key = "
      "'transform.edge-site-count-rollup.published.status'");
  REQUIRE(published.step());
  CHECK(published.col_text(0) == "ran");
}

TEST_CASE(
    "failed transform preserves prior derived rows and records stale input") {
  Storage db(":memory:");
  Symbol src;
  src.usr = "transform:@F@src";
  src.spelling = "src";
  src.kind = "function";
  src.is_definition = true;
  const auto src_id = db.add_symbol(src);
  Symbol dst;
  dst.usr = "transform:@F@dst";
  dst.spelling = "dst";
  dst.kind = "function";
  dst.is_definition = true;
  const auto dst_id = db.add_symbol(dst);
  Edge dispatch_edge;
  dispatch_edge.src_id = src_id;
  dispatch_edge.dst_id = dst_id;
  dispatch_edge.kind = 18;
  db.add_edge(dispatch_edge);

  const auto baseline = db.run_transform_pipeline();
  REQUIRE(baseline.runs.size() == 9);

  Edge earlier_edge;
  earlier_edge.src_id = src_id;
  earlier_edge.dst_id = dst_id;
  earlier_edge.kind = 1;
  db.add_edge(earlier_edge);
  db.raw_db().exec("INSERT OR IGNORE INTO entity_node(id, kind) VALUES (" +
                   std::to_string(src_id) + ", 1), (" + std::to_string(dst_id) +
                   ", 1)");
  db.add_entity_edge(src_id, dst_id, 1);
  auto before = db.raw_db().prepare("SELECT COUNT(*) FROM entity_edge");
  REQUIRE(before.step());
  CHECK(before.col_int64(0) == 1);

  Edge override_edge;
  override_edge.src_id = src_id;
  override_edge.dst_id = dst_id;
  override_edge.kind = 6;
  db.add_edge(override_edge);
  db.inject_transform_failure_for_testing("entity-graph-rollup");
  const auto failed = db.run_transform_pipeline();
  REQUIRE(failed.runs.size() == 9);
  std::set<std::string> final_ids;
  for (const auto &run : failed.runs) {
    final_ids.insert(run.transform_id);
  }
  CHECK(final_ids.size() == failed.runs.size());
  const auto entity =
      std::ranges::find_if(failed.runs, [](const TransformRun &run) {
        return run.transform_id == "entity-graph-rollup";
      });
  REQUIRE(entity != failed.runs.end());
  CHECK(entity->status == TransformRunStatus::failed);
  CHECK_FALSE(entity->diagnostic.empty());
  const auto earlier_record =
      std::ranges::find_if(failed.runs, [](const TransformRun &run) {
        return run.transform_id == "edge-site-count-rollup";
      });
  REQUIRE(earlier_record != failed.runs.end());
  CHECK(earlier_record->status == TransformRunStatus::stale);
  const auto downstream_record =
      std::ranges::find_if(failed.runs, [](const TransformRun &run) {
        return run.transform_id == "hse-66-effect-registration";
      });
  REQUIRE(downstream_record != failed.runs.end());
  CHECK(downstream_record->status == TransformRunStatus::stale);

  auto state = db.raw_db().prepare("SELECT value FROM meta WHERE key = "
                                   "'transform.entity-graph-rollup.status'");
  REQUIRE(state.step());
  CHECK(state.col_text(0) == "failed");
  auto published =
      db.raw_db().prepare("SELECT value FROM meta WHERE key = "
                          "'transform.entity-graph-rollup.published.status'");
  REQUIRE(published.step());
  CHECK(published.col_text(0) == "ran");
  auto attempt_input =
      db.raw_db().prepare("SELECT value FROM meta WHERE key = "
                          "'transform.entity-graph-rollup.attempt.input'");
  REQUIRE(attempt_input.step());
  CHECK_FALSE(attempt_input.col_text(0).empty());
  auto after = db.raw_db().prepare("SELECT COUNT(*) FROM entity_edge");
  REQUIRE(after.step());
  CHECK(after.col_int64(0) == 1);
  const auto status = db.transform_status();
  const auto effect =
      std::ranges::find_if(status.runs, [](const TransformRun &run) {
        return run.transform_id == "hse-66-effect-registration";
      });
  REQUIRE(effect != status.runs.end());
  CHECK(effect->status == TransformRunStatus::stale);
  CHECK(effect->diagnostic.find("entity-graph-rollup") != std::string::npos);
  auto edge_attempt =
      db.raw_db().prepare("SELECT value FROM meta WHERE key = "
                          "'transform.edge-site-count-rollup.attempt.status'");
  REQUIRE(edge_attempt.step());
  CHECK(edge_attempt.col_text(0) == "stale");
  const auto earlier = db.transform_fact_set_status("edge.count");
  CHECK(earlier.known);
  CHECK_FALSE(earlier.ready);
  CHECK(earlier.status == TransformRunStatus::stale);
  const auto downstream = db.transform_fact_set_status("effect");
  CHECK(downstream.known);
  CHECK_FALSE(downstream.ready);
  CHECK(downstream.status == TransformRunStatus::stale);
}

TEST_CASE("transform invalidation is typed and has minimum closure") {
  Storage db(":memory:");
  const auto first = db.run_transform_pipeline();
  REQUIRE(first.complete);

  db.set_transform_invalidation_for_testing("catalog", "catalog-v2");
  const auto catalog_change = db.run_transform_pipeline();
  CHECK(catalog_change.runs[0].status == TransformRunStatus::reused);
  CHECK(catalog_change.runs[4].status == TransformRunStatus::ran);
  CHECK(catalog_change.runs[5].status == TransformRunStatus::reused);
  CHECK(catalog_change.runs[6].status == TransformRunStatus::ran);
  CHECK(catalog_change.runs[7].status == TransformRunStatus::ran);

  db.set_transform_invalidation_for_testing("catalog", "catalog-v1");
  db.set_transform_invalidation_for_testing("source", "source-v2");
  const auto source_change = db.run_transform_pipeline();
  CHECK(source_change.runs[0].status == TransformRunStatus::ran);
  CHECK(source_change.runs[1].status == TransformRunStatus::ran);
  CHECK(source_change.runs[2].status == TransformRunStatus::ran);
  CHECK(source_change.runs[3].status == TransformRunStatus::ran);
  CHECK(source_change.runs[4].status == TransformRunStatus::ran);
  CHECK(source_change.runs[5].status == TransformRunStatus::ran);
  CHECK(source_change.runs[6].status == TransformRunStatus::ran);
}

TEST_CASE("clean and incremental publication have equal identities") {
  struct Fixture {
    int64_t directory_id = 0;
    int64_t file_id = 0;
    int64_t caller_id = 0;
    int64_t base_id = 0;
    int64_t override_id = 0;
    int64_t caller_definition_id = 0;
  };
  const auto seed = [](Storage &db) {
    const auto component_id = db.add_component("incremental", "/tmp");
    const auto directory_id = db.add_directory(component_id, "");
    const auto file_id = db.add_file(directory_id, "incremental.cpp");
    Symbol caller;
    caller.usr = "transform:@F@incremental-caller";
    caller.spelling = "incremental-caller";
    caller.kind = "function";
    caller.is_definition = true;
    caller.resolved = true;
    caller.file_id = file_id;
    const auto caller_id = db.add_symbol(caller);
    Symbol base = caller;
    base.usr = "transform:@F@incremental-base";
    base.spelling = "incremental-base";
    const auto base_id = db.add_symbol(base);
    Symbol override = caller;
    override.usr = "transform:@F@incremental-override";
    override.spelling = "incremental-override";
    const auto override_id = db.add_symbol(override);
    const auto caller_definition_id =
        db.get_or_create_definition(caller_id, file_id);
    (void)db.get_or_create_definition(base_id, file_id);
    db.add_def_edge(caller_definition_id, base_id, 1);
    return Fixture{.directory_id = directory_id,
                   .file_id = file_id,
                   .caller_id = caller_id,
                   .base_id = base_id,
                   .override_id = override_id,
                   .caller_definition_id = caller_definition_id};
  };
  const auto mutate = [](Storage &db, const Fixture &fixture,
                         bool record_changes) {
    const auto second_file =
        db.add_file(fixture.directory_id, "incremental-second.cpp");
    const auto target_definition =
        db.get_or_create_definition(fixture.base_id, second_file);
    const auto call_edge = db.add_edge(Edge{.src_id = fixture.caller_id,
                                            .dst_id = fixture.base_id,
                                            .kind = 1,
                                            .count = 1});
    const auto override_edge = db.add_edge(Edge{.src_id = fixture.override_id,
                                                .dst_id = fixture.base_id,
                                                .kind = 6,
                                                .count = 1});
    db.add_edge_site(EdgeSite{
        .edge_id = call_edge, .file_id = fixture.file_id, .line = 1, .col = 1});
    if (record_changes) {
      db.note_transform_changes(
          fixture.file_id,
          {fixture.caller_id, fixture.base_id, fixture.override_id},
          {call_edge, override_edge},
          {fixture.caller_definition_id, target_definition});
    }
  };
  Storage clean(":memory:");
  Storage incremental(":memory:");
  const Fixture clean_fixture = seed(clean);
  const Fixture incremental_fixture = seed(incremental);
  REQUIRE(clean.run_transform_pipeline().complete);
  REQUIRE(incremental.run_transform_pipeline().complete);
  auto baseline_mode = clean.raw_db().prepare(
      "SELECT value FROM meta WHERE key = 'transform.pipeline.execution_mode'");
  REQUIRE(baseline_mode.step());
  CHECK(baseline_mode.col_text(0) == "full");
  mutate(clean, clean_fixture, false);
  mutate(incremental, incremental_fixture, true);
  const auto clean_run = clean.run_transform_pipeline();
  const auto incremental_run = incremental.run_transform_pipeline();
  REQUIRE(clean_run.complete);
  REQUIRE(incremental_run.complete);
  REQUIRE(clean_run.runs.size() == incremental_run.runs.size());
  for (std::size_t i = 0; i < clean_run.runs.size(); ++i) {
    CHECK(clean_run.runs[i].output_identity ==
          incremental_run.runs[i].output_identity);
  }
  for (const std::string_view id :
       {"edge-site-count-rollup", "multi-definition-classification",
        "possible-call-materialization",
        "virtual-dispatch-call-materialization"}) {
    const auto run = std::ranges::find_if(incremental_run.runs,
                                          [id](const TransformRun &candidate) {
                                            return candidate.transform_id == id;
                                          });
    REQUIRE(run != incremental_run.runs.end());
    CHECK(run->execution_mode == TransformExecutionMode::incremental);
    CHECK(run->work.input_identity_rows_scanned == 0);
    CHECK(run->diagnostic.find("mode=incremental") != std::string::npos);
  }
  const auto entity = std::ranges::find_if(
      incremental_run.runs, [](const TransformRun &candidate) {
        return candidate.transform_id == "entity-graph-rollup";
      });
  REQUIRE(entity != incremental_run.runs.end());
  CHECK(entity->execution_mode == TransformExecutionMode::full);
  CHECK(entity->fallback_reason == "generation-gated full rebuild contract");
  auto mixed_mode = incremental.raw_db().prepare(
      "SELECT value FROM meta WHERE key = 'transform.pipeline.execution_mode'");
  REQUIRE(mixed_mode.step());
  CHECK(mixed_mode.col_text(0) == "mixed");
}

TEST_CASE("change capture skips work before a published baseline exists") {
  Storage db(":memory:");
  const auto component = db.add_component("capture", "/tmp/capture");
  const auto directory = db.add_directory(component, "");
  const auto file = db.add_file(directory, "capture.cpp");
  db.capture_transform_changes_for_file(file);
  db.note_transform_changes(file, {41}, {42}, {43});
  CHECK(db.pending_transform_changes().empty());
}

TEST_CASE("removing file facts preserves full and incremental equivalence") {
  struct Fixture {
    int64_t file_id = 0;
  };
  const auto seed = [](Storage &db) {
    const auto component = db.add_component("removal", "/tmp/removal");
    const auto directory = db.add_directory(component, "");
    const auto first_file = db.add_file(directory, "first.cpp");
    const auto second_file = db.add_file(directory, "second.cpp");
    Symbol base;
    base.usr = "transform:@F@removal-base";
    base.spelling = "removal-base";
    base.kind = "function";
    base.is_definition = true;
    base.resolved = true;
    base.file_id = first_file;
    const auto base_id = db.add_symbol(base);
    Symbol caller = base;
    caller.usr = "transform:@F@removal-caller";
    caller.spelling = "removal-caller";
    caller.file_id = second_file;
    const auto caller_id = db.add_symbol(caller);
    Symbol override = caller;
    override.usr = "transform:@F@removal-override";
    override.spelling = "removal-override";
    const auto override_id = db.add_symbol(override);
    (void)db.get_or_create_definition(base_id, first_file);
    (void)db.get_or_create_definition(base_id, second_file);
    const auto caller_definition =
        db.get_or_create_definition(caller_id, second_file);
    db.add_def_edge(caller_definition, base_id, 1);
    const auto call = db.add_edge(
        Edge{.src_id = caller_id, .dst_id = base_id, .kind = 1, .count = 1});
    (void)db.add_edge(
        Edge{.src_id = override_id, .dst_id = base_id, .kind = 6, .count = 1});
    db.add_edge_site(
        EdgeSite{.edge_id = call, .file_id = second_file, .line = 1, .col = 1});
    return Fixture{.file_id = second_file};
  };
  const auto remove = [](Storage &db, const Fixture &fixture,
                         bool capture_changes) {
    if (capture_changes) {
      db.capture_transform_changes_for_file(fixture.file_id);
    }
    db.delete_edges_for_file(fixture.file_id);
    db.delete_definitions_for_file(fixture.file_id);
    db.delete_symbols_for_file(fixture.file_id);
    if (capture_changes) {
      db.capture_transform_changes_for_file(fixture.file_id);
    }
  };
  Storage full(":memory:");
  Storage incremental(":memory:");
  const Fixture full_fixture = seed(full);
  const Fixture incremental_fixture = seed(incremental);
  REQUIRE(full.run_transform_pipeline().complete);
  REQUIRE(incremental.run_transform_pipeline().complete);
  remove(full, full_fixture, false);
  remove(incremental, incremental_fixture, true);
  const auto full_run = full.run_transform_pipeline();
  const auto incremental_run = incremental.run_transform_pipeline();
  REQUIRE(full_run.complete);
  REQUIRE(incremental_run.complete);
  REQUIRE(full_run.runs.size() == incremental_run.runs.size());
  for (std::size_t i = 0; i < full_run.runs.size(); ++i) {
    CHECK(full_run.runs[i].output_identity ==
          incremental_run.runs[i].output_identity);
  }
}

TEST_CASE("legacy resolve propagates a failed transform") {
  Storage db(":memory:");
  REQUIRE(db.run_transform_pipeline().complete);
  db.set_transform_invalidation_for_testing("source", "resolve-failure");
  db.inject_transform_failure_for_testing("entity-graph-rollup");
  bool threw = false;
  try {
    (void)db.resolve_pass();
  } catch (const std::exception &) {
    threw = true;
  }
  CHECK(threw);
  CHECK_FALSE(db.graph_resolved());
}

TEST_CASE("pending transform publication stays stale until graph extraction") {
  Storage db(":memory:");
  REQUIRE(db.run_transform_pipeline().complete);
  db.stamp_graph_resolved();
  db.mark_transform_pipeline_pending("selected file remains pending");
  const auto status = db.transform_status();
  CHECK_FALSE(status.complete);
  CHECK_FALSE(db.graph_resolved());
  CHECK(std::ranges::all_of(status.runs, [](const TransformRun &run) {
    return run.status == TransformRunStatus::stale &&
           run.completeness == TransformCompleteness::pending;
  }));
}

TEST_CASE("graph-disabled extraction cannot be published by resolve") {
  Storage db(":memory:");
  REQUIRE(db.run_transform_pipeline().complete);
  db.mark_transform_pipeline_pending("graph extraction disabled");
  CHECK_THROWS_WITH(db.resolve_pass(),
                    "graph extraction was disabled; re-index with graph "
                    "extraction before resolve");
  CHECK_FALSE(db.transform_status().complete);
  CHECK_FALSE(db.graph_resolved());
}

TEST_CASE("deferred publication resolves from persisted Layer-0 facts") {
  Storage db(":memory:");
  REQUIRE(db.run_transform_pipeline().complete);
  Symbol symbol;
  symbol.usr = "transform:@F@deferred";
  symbol.spelling = "deferred";
  symbol.kind = "function";
  symbol.is_definition = true;
  const auto symbol_id = db.add_symbol(symbol);
  db.note_transform_changes(1, {symbol_id}, {}, {});
  db.mark_transform_pipeline_pending("derived publication pending");
  CHECK_FALSE(db.transform_status().complete);
  CHECK(db.transform_explain().find("derived publication pending") !=
        std::string::npos);
  CHECK(db.resolve_pass() >= 0);
  CHECK(db.transform_status().complete);
}

TEST_CASE("pending state overrides reused attempts after reopen") {
  const auto path =
      std::filesystem::temp_directory_path() / "cidx-hse67-pending-reopen.db";
  std::filesystem::remove(path);
  {
    Storage db(path.string());
    REQUIRE(db.run_transform_pipeline().complete);
    const auto reused = db.run_transform_pipeline();
    REQUIRE(reused.complete);
    CHECK(std::ranges::all_of(reused.runs, [](const TransformRun &run) {
      return run.status == TransformRunStatus::reused;
    }));
    db.mark_transform_pipeline_pending("selected file remains pending");
  }
  {
    Storage db(path.string());
    const auto status = db.transform_status();
    REQUIRE(status.runs.size() == 9);
    CHECK(std::ranges::all_of(status.runs, [](const TransformRun &run) {
      return run.status == TransformRunStatus::stale &&
             run.completeness == TransformCompleteness::pending;
    }));
    const auto fact_status = db.transform_fact_set_status("entity-graph");
    CHECK(fact_status.known);
    CHECK_FALSE(fact_status.ready);
    CHECK(fact_status.status == TransformRunStatus::stale);
    const auto explanation = db.transform_explain("entity-graph");
    CHECK(explanation.find("entity-graph-rollup: stale, pending") !=
          std::string::npos);
    CHECK(explanation.find("cause=selected file remains pending") !=
          std::string::npos);
  }
  std::filesystem::remove(path);
}

TEST_CASE("fact-set readiness resolves named producers and unknowns") {
  Storage db(":memory:");
  REQUIRE(db.run_transform_pipeline().complete);
  const auto ready = db.transform_fact_set_status("entity-graph");
  CHECK(ready.known);
  CHECK(ready.ready);
  CHECK(db.transform_status("entity-graph").runs.size() == 1);
  CHECK(db.transform_explain("entity-graph").find("entity-graph-rollup") !=
        std::string::npos);
  const auto unknown = db.transform_fact_set_status("does-not-exist");
  CHECK_FALSE(unknown.known);
  CHECK_FALSE(unknown.ready);
}
