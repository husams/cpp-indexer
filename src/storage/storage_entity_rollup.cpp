// materialise_entity_edges: the pure-DB roll-up of all 11 entity relation kinds, plus resolve_pass.
// Split out of storage.cpp; Storage's interface is unchanged.
#include "storage/storage.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstring>
#include <exception>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "catalogs/generated_catalog.hpp"
#include "util/version.hpp"
#include "compiledb/compiledb.hpp"
#include "profile/index_profile.hpp"
#include "storage/storage_detail.hpp"
#include "storage/storage_schema.hpp"
#include "util/errors.hpp"
#include "util/hashing.hpp"
#include "util/json_min.hpp"
#include "util/logger.hpp"
#include "util/pathutil.hpp"

namespace cidx {

using namespace detail;


namespace {

std::optional<std::string> read_transform_meta(SqliteDb &db,
                                               const std::string &key);

std::pair<std::string, std::string>
implementation_provider_for(std::string_view transform_id) {
  if (transform_id == "edge-site-count-rollup") {
    return {"executor.edge_site_count.v1",
            "SqliteStorageService::rollup_edge_counts"};
  }
  if (transform_id == "multi-definition-classification") {
    return {"executor.multi_definition.v1",
            "SqliteStorageService::set_multi_def"};
  }
  if (transform_id == "possible-call-materialization") {
    return {"executor.possible_call.v1",
            "SqliteStorageService::materialize_possible_calls"};
  }
  if (transform_id == "virtual-dispatch-call-materialization") {
    return {"executor.virtual_dispatch.v1",
            "SqliteStorageService::materialize_dispatch_calls"};
  }
  if (transform_id == "entity-graph-rollup") {
    return {"executor.entity_graph.v1",
            "SqliteStorageService::materialise_entity_edges"};
  }
  if (transform_id == "include-fact-readiness") {
    return {"readiness.include.v1", "readiness_transform(include)"};
  }
  if (transform_id == "hse-66-effect-registration") {
    return {"readiness.effect.v1", "readiness_transform(effect)"};
  }
  if (transform_id == "hse-66-proof-registration") {
    return {"readiness.proof.v1", "readiness_transform(proof)"};
  }
  if (transform_id == "type-fact-readiness") {
    return {"readiness.type.v1", "readiness_transform(type)"};
  }
  throw StorageError("no implementation provider for transform: " +
                     std::string(transform_id));
}

TransformDescriptor descriptor(std::string id, std::vector<std::string> inputs,
                               std::vector<std::string> outputs,
                               std::vector<std::string> dependencies,
                               std::vector<std::string> invalidation_keys,
                               std::vector<std::string> input_queries,
                               std::vector<std::string> output_queries,
                               std::string output_count_query) {
  TransformDescriptor result;
  result.id = std::move(id);
  result.version = 1;
  result.input_facts = std::move(inputs);
  result.produced_facts = std::move(outputs);
  result.dependencies = std::move(dependencies);
  result.invalidation_keys = std::move(invalidation_keys);
  result.options = {"deterministic-sql-v1"};
  result.budget = TransformBudget{.max_rows = 5'000'000,
                                  .max_milliseconds = 60'000};
  if (result.id == "include-fact-readiness" ||
      result.id == "hse-66-effect-registration" ||
      result.id == "hse-66-proof-registration") {
    result.completeness = TransformCompleteness::complete;
  }
  result.input_queries = std::move(input_queries);
  result.output_queries = std::move(output_queries);
  result.output_count_query = std::move(output_count_query);
  const auto [provider_id, provider_content] =
      implementation_provider_for(result.id);
  result.implementation_provider = TransformImplementationProvider{
      .provider_id = provider_id, .version = 1, .content = provider_content};
  for (const auto &fact : result.produced_facts) {
    result.fact_set_requirements.push_back(TransformFactSetRequirement{
        .name = fact,
        .facts = {fact},
        .schema_version = result.output_schema_version,
        .catalog = result.output_catalog});
  }
  const std::unordered_map<std::string, std::string> named_fact_sets = {
      {"entity-graph-rollup", "entity-graph"},
      {"possible-call-materialization", "possible-call"},
      {"include-fact-readiness", "include"},
      {"hse-66-effect-registration", "effect"},
      {"hse-66-proof-registration", "proof"},
      {"type-fact-readiness", "type"}};
  if (const auto alias = named_fact_sets.find(result.id);
      alias != named_fact_sets.end()) {
    result.fact_set_requirements.push_back(TransformFactSetRequirement{
        .name = alias->second,
        .facts = result.produced_facts,
        .schema_version = result.output_schema_version,
        .catalog = result.output_catalog});
  }
  for (const auto &key : result.invalidation_keys) {
    TransformInvalidationInput input;
    input.name = key;
    if (key == "source") {
      input.kind = TransformInputKind::source;
      input.provider_id = "source.files.content.v1";
      input.value_query =
          "SELECT id, name, mtime, md5, indexed, compile_options, driver "
          "FROM file ORDER BY id";
    } else if (key == "catalog") {
      input.kind = TransformInputKind::catalog;
      input.provider_id = "catalog.generated.hash.v1";
      input.value_query =
          "SELECT value FROM meta WHERE key = 'catalog_hash'";
      input.static_value = std::string(catalog::kCatalogHash);
    } else if (key == "schema") {
      input.kind = TransformInputKind::schema;
      input.provider_id = "schema.database.version.v1";
      input.value_query =
          "SELECT value FROM meta WHERE key = 'schema_version'";
    } else if (key == "applicability") {
      input.kind = TransformInputKind::applicability;
      input.provider_id = "facts.applicability.generation.v1";
      input.value_query =
          "SELECT fact_kind, fact_id, config_id, generation "
          "FROM fact_applicability ORDER BY fact_kind, fact_id, config_id";
    } else if (key == "configuration") {
      input.kind = TransformInputKind::configuration;
      input.provider_id = "source.compile-configuration.v1";
      input.value_query =
          "SELECT id, compile_options, driver FROM file ORDER BY id";
    } else if (key == "pass") {
      input.kind = TransformInputKind::pass;
      input.provider_id = "resolve.pass.api.v1";
      input.static_value = std::to_string(version::kApiVersion);
    } else if (key == "package") {
      input.kind = TransformInputKind::package;
      input.provider_id = "cidx.package.version.v1";
      input.static_value = std::string(version::kFullProductVersion);
    } else if (key == "model") {
      input.kind = TransformInputKind::model;
      input.provider_id = "semantic.catalog.hash.v1";
      input.static_value = std::string(catalog::kCatalogHash);
    } else if (key == "implementation") {
      input.kind = TransformInputKind::implementation;
      input.provider_id = result.implementation_provider.provider_id;
      input.static_value = result.implementation_provider.provider_id + "|" +
                           std::to_string(result.implementation_provider.version) +
                           "|" + result.implementation_provider.content;
    }
    result.invalidation_inputs.push_back(std::move(input));
  }
  return result;
}

TransformRegistry make_transform_registry(SqliteDb *db = nullptr) {
  TransformRegistry registry;
  for (const auto &source : {"edge", "edge_site", "symbol", "definition",
                             "def_edge", "type_edge", "include_config",
                             "include_edge", "include_site", "artifact",
                             "raw"}) {
    registry.register_source_fact(TransformSourceFact{.name = source});
  }
  registry.register_transform(
      descriptor("edge-site-count-rollup", {"edge", "edge_site"},
                 {"edge.count"}, {}, {"source", "schema", "implementation"},
                 {"SELECT e.id, e.kind, "
                  "COALESCE((SELECT COUNT(*) FROM edge_site es WHERE "
                  "es.edge_id = e.id), 0) "
                  "FROM edge e WHERE e.kind IN (1, 7) ORDER BY e.id"},
                 {"SELECT e.id, e.kind, e.count, "
                  "COALESCE((SELECT COUNT(*) FROM edge_site es WHERE "
                  "es.edge_id = e.id), 0) "
                  "FROM edge e WHERE e.kind IN (1, 7) ORDER BY e.id"},
                 "SELECT COUNT(*) FROM edge WHERE kind IN (1, 7)"));
  registry.register_transform(descriptor(
      "multi-definition-classification", {"symbol", "definition"},
      {"symbol.multi_def"}, {}, {"source", "schema", "implementation"},
      {"SELECT s.id, d.id, d.symbol_id, d.component_id, "
       "d.file_id FROM symbol s LEFT JOIN definition d ON d.symbol_id = s.id "
       "ORDER BY s.id, d.id"},
      {"SELECT id, multi_def FROM symbol ORDER BY id"},
      "SELECT COUNT(*) FROM symbol WHERE multi_def > 0"));
  registry.register_transform(descriptor(
      "possible-call-materialization",
      {"def_edge", "symbol.multi_def", "definition"}, {"possible_call"},
      {"multi-definition-classification"},
      {"source", "pass", "package", "model", "implementation"},
      {"SELECT de.src_def_id, de.dst_id, de.kind, de.count, "
       "td.id FROM def_edge de "
       "LEFT JOIN definition td ON td.symbol_id = de.dst_id "
       "WHERE de.kind = 1 ORDER BY de.src_def_id, de.dst_id, td.id"},
      {"SELECT src_def_id, dst_def_id, count FROM possible_call "
       "ORDER BY src_def_id, dst_def_id"},
      "SELECT COUNT(*) FROM possible_call"));
  registry.register_transform(descriptor(
      "virtual-dispatch-call-materialization", {"edge"}, {"dispatch_calls"},
      {"edge-site-count-rollup"},
      {"source", "pass", "package", "model", "implementation"},
      {"SELECT id, src_id, dst_id, kind FROM edge "
       "WHERE kind IN (1, 6) ORDER BY id"},
      {"SELECT src_id, dst_id, kind, count FROM edge WHERE kind = 18 "
       "ORDER BY src_id, dst_id"},
      "SELECT COUNT(*) FROM edge WHERE kind = 18"));
  registry.register_transform(
      descriptor("entity-graph-rollup", {"symbol", "edge", "type_edge"},
                 {"entity_node", "entity_edge"}, {"edge-site-count-rollup"},
                 {"source", "catalog", "applicability", "configuration",
                  "implementation"},
                 {"SELECT id, usr, kind, parent_usr, is_pure FROM symbol "
                  "ORDER BY id",
                  "SELECT id, src_id, dst_id, kind, base_access, "
                  "is_virtual FROM edge ORDER BY id",
                  "SELECT src_id, kind, position, dst_id FROM type_edge "
                  "ORDER BY src_id, kind, position, dst_id"},
                 {"SELECT id, kind FROM entity_node ORDER BY id",
                  "SELECT src_id, dst_id, kind, count, via_member_id, "
                  "multiplicity, access, is_virtual, create_form, partial "
                  "FROM entity_edge ORDER BY src_id, dst_id, kind"},
                 "SELECT COUNT(*) FROM entity_edge"));
  registry.register_transform(descriptor(
      "include-fact-readiness", {"include_config", "include_edge", "include_site"},
      {"include.fact_set"}, {}, {"source", "schema", "implementation"},
      {"SELECT id, tu_file_id, digest FROM include_config ORDER BY id",
       "SELECT id, src_file_id, dst_file_id, config_id FROM include_edge ORDER BY id",
       "SELECT edge_id, line, col, directive FROM include_site ORDER BY edge_id, line"},
      {"SELECT id FROM include_edge ORDER BY id"},
      "SELECT COUNT(*) FROM include_edge"));
  registry.register_transform(
      descriptor("hse-66-effect-registration", {"artifact"},
                 {"effect.fact_set"},
                 {"possible-call-materialization",
                  "virtual-dispatch-call-materialization", "entity-graph-rollup"},
                 {"catalog", "schema", "implementation"},
                 {"SELECT id, kind, artifact_schema, producer_version FROM artifact "
                  "WHERE evidence = 'derived' ORDER BY id"},
                 {"SELECT id FROM artifact WHERE evidence = 'derived' ORDER BY id"},
                 "SELECT COUNT(*) FROM artifact WHERE evidence = 'derived'"));
  registry.register_transform(
      descriptor("hse-66-proof-registration", {"artifact"},
                 {"proof.fact_set"},
                 {"possible-call-materialization",
                  "virtual-dispatch-call-materialization", "entity-graph-rollup"},
                 {"catalog", "schema", "implementation"},
                 {"SELECT id, kind, artifact_schema, producer_version FROM artifact "
                  "WHERE evidence = 'proof' ORDER BY id"},
                 {"SELECT id FROM artifact WHERE evidence = 'proof' ORDER BY id"},
                 "SELECT COUNT(*) FROM artifact WHERE evidence = 'proof'"));
  registry.register_transform(descriptor(
      "type-fact-readiness", {"type_edge"}, {"type.fact_set"}, {},
      {"source", "schema", "implementation"},
      {"SELECT src_id, kind, position, dst_id FROM type_edge ORDER BY src_id, kind, position, dst_id"},
      {"SELECT src_id, kind, position, dst_id FROM type_edge ORDER BY src_id, kind, position, dst_id"},
      "SELECT COUNT(*) FROM type_edge"));
  registry.validate();
  if (db != nullptr) {
    for (const auto &transform : registry.descriptors()) {
      if (const auto version = read_transform_meta(
              *db, "transform.implementation." + transform.id + ".version")) {
        registry.set_implementation_version(transform.id, std::stoi(*version));
      }
    }
  }
  return registry;
}

std::string transform_meta_key(const std::string &id, const char *field) {
  return "transform." + id + "." + field;
}

std::optional<std::string> read_transform_meta(SqliteDb &db,
                                               const std::string &key) {
  auto st = db.prepare("SELECT value FROM meta WHERE key = ?");
  st.bind(1, std::string_view(key));
  if (!st.step()) {
    return std::nullopt;
  }
  return st.col_text(0);
}

void write_transform_meta(SqliteDb &db, const std::string &key,
                          const std::string &value) {
  auto st =
      db.prepare("INSERT OR REPLACE INTO meta (key, value) VALUES (?, ?)");
  st.bind(1, std::string_view(key));
  st.bind(2, std::string_view(value));
  st.step_done();
}

std::optional<TransformRun> read_transform_run(SqliteDb &db,
                                               const TransformDescriptor &d) {
  const auto version = read_transform_meta(
      db, transform_meta_key(d.id, "published.version"));
  const auto input =
      read_transform_meta(db, transform_meta_key(d.id, "published.input"));
  const auto output = read_transform_meta(
      db, transform_meta_key(d.id, "published.output"));
  const auto status = read_transform_meta(
      db, transform_meta_key(d.id, "published.status"));
  const auto count = read_transform_meta(
      db, transform_meta_key(d.id, "published.count"));
  // Read metadata written by the first PR as a compatibility bridge. A failed
  // attempt is intentionally not a published generation.
  const auto legacy_version =
      read_transform_meta(db, transform_meta_key(d.id, "version"));
  const auto legacy_input =
      read_transform_meta(db, transform_meta_key(d.id, "input"));
  const auto legacy_output =
      read_transform_meta(db, transform_meta_key(d.id, "output"));
  const auto legacy_status =
      read_transform_meta(db, transform_meta_key(d.id, "status"));
  const auto legacy_count =
      read_transform_meta(db, transform_meta_key(d.id, "count"));
  const bool use_legacy = !version && legacy_status &&
                          *legacy_status != "failed";
  if (!version || !input || !output || !status || !count) {
    if (!use_legacy || !legacy_version || !legacy_input || !legacy_output ||
        !legacy_count) {
      return std::nullopt;
    }
  }
  TransformRun run;
  run.transform_id = d.id;
  run.version = std::stoi(use_legacy ? *legacy_version : *version);
  run.input_identity = use_legacy ? *legacy_input : *input;
  run.output_identity = use_legacy ? *legacy_output : *output;
  run.output_count = std::stoll(use_legacy ? *legacy_count : *count);
  const std::string published_status = use_legacy ? *legacy_status : *status;
  if (published_status == "ran") {
    run.status = TransformRunStatus::ran;
  } else if (published_status == "reused") {
    run.status = TransformRunStatus::reused;
  } else if (published_status == "skipped") {
    run.status = TransformRunStatus::skipped;
  } else {
    run.status = TransformRunStatus::stale;
  }
  run.generation = std::stoull(
      read_transform_meta(db, transform_meta_key(d.id, "published.generation"))
          .value_or("0"));
  run.published_generation = run.generation;
  run.diagnostic =
      read_transform_meta(db, transform_meta_key(d.id, "published.diagnostic"))
          .value_or("");
  run.diagnostic = use_legacy
                       ? read_transform_meta(
                             db, transform_meta_key(d.id, "diagnostic"))
                             .value_or(run.diagnostic)
                       : run.diagnostic;
  return run;
}

void write_transform_run(SqliteDb &db, const TransformRun &run) {
  const std::string generation = std::to_string(run.generation);
  for (const std::string &prefix : {std::string("attempt."),
                                    std::string("published.")}) {
    write_transform_meta(db,
                         transform_meta_key(run.transform_id,
                                            (prefix + "version").c_str()),
                         std::to_string(run.version));
    write_transform_meta(db,
                         transform_meta_key(run.transform_id,
                                            (prefix + "input").c_str()),
                         run.input_identity);
    write_transform_meta(db,
                         transform_meta_key(run.transform_id,
                                            (prefix + "output").c_str()),
                         run.output_identity);
    write_transform_meta(db,
                         transform_meta_key(run.transform_id,
                                            (prefix + "status").c_str()),
                         transform_run_status_name(run.status));
    write_transform_meta(db,
                         transform_meta_key(run.transform_id,
                                            (prefix + "count").c_str()),
                         std::to_string(run.output_count));
    write_transform_meta(db,
                         transform_meta_key(run.transform_id,
                                            (prefix + "generation").c_str()),
                         generation);
    write_transform_meta(db,
                         transform_meta_key(run.transform_id,
                                            (prefix + "diagnostic").c_str()),
                         run.diagnostic);
    write_transform_meta(
        db, transform_meta_key(run.transform_id,
                               (prefix + "applicability").c_str()),
        transform_applicability_name(run.applicability));
    write_transform_meta(
        db, transform_meta_key(run.transform_id,
                               (prefix + "completeness").c_str()),
        transform_completeness_name(run.completeness));
  }
  write_transform_meta(db, transform_meta_key(run.transform_id, "version"),
                       std::to_string(run.version));
  write_transform_meta(db, transform_meta_key(run.transform_id, "input"),
                       run.input_identity);
  write_transform_meta(db, transform_meta_key(run.transform_id, "output"),
                       run.output_identity);
  write_transform_meta(db, transform_meta_key(run.transform_id, "status"),
                       transform_run_status_name(run.status));
  write_transform_meta(db, transform_meta_key(run.transform_id, "count"),
                       std::to_string(run.output_count));
  write_transform_meta(db, transform_meta_key(run.transform_id, "diagnostic"),
                       run.diagnostic);
  std::string changed;
  for (const auto &input : run.changed_inputs) {
    if (!changed.empty()) {
      changed += ",";
    }
    changed += input;
  }
  write_transform_meta(db,
                       transform_meta_key(run.transform_id, "changed_inputs"),
                       changed);
  write_transform_meta(db, transform_meta_key(run.transform_id, "stale_cause"),
                       "");
  write_transform_meta(db,
                       transform_meta_key(run.transform_id,
                                          (std::string("history.") + generation)
                                              .c_str()),
                       std::string(transform_run_status_name(run.status)) +
                           "|" + run.input_identity + "|" + run.diagnostic);
  if (run.transform_id == "edge-site-count-rollup") {
    auto snapshot = db.prepare(
        "SELECT id, count FROM edge WHERE kind IN (1, 7) ORDER BY id");
    std::string rows;
    while (snapshot.step()) {
      rows += std::to_string(snapshot.col_int64(0)) + "=" +
              std::to_string(snapshot.col_int64(1)) + ";";
    }
    write_transform_meta(
        db, transform_meta_key(run.transform_id, "published.rows"), rows);
  }
}

bool restore_edge_count_snapshot(SqliteDb &db) {
  const auto snapshot = read_transform_meta(
      db, transform_meta_key("edge-site-count-rollup", "published.rows"));
  if (!snapshot) {
    return false;
  }
  std::size_t cursor = 0;
  while (cursor < snapshot->size()) {
    const auto end = snapshot->find(';', cursor);
    if (end == std::string::npos) {
      break;
    }
    const auto separator = snapshot->find('=', cursor);
    if (separator == std::string::npos || separator > end) {
      return false;
    }
    auto update = db.prepare("UPDATE edge SET count = ? WHERE id = ?");
    const auto count = static_cast<std::int64_t>(std::stoll(
        snapshot->substr(separator + 1, end - separator - 1)));
    const auto edge_id = static_cast<std::int64_t>(
        std::stoll(snapshot->substr(cursor, separator - cursor)));
    update.bind(1, count);
    update.bind(2, edge_id);
    update.step_done();
    cursor = end + 1;
  }
  return true;
}

void write_reused_attempt(SqliteDb &db, const TransformRun &run,
                          std::uint64_t published_generation) {
  const auto prefix = [&](const std::string &name) {
    return transform_meta_key(run.transform_id, ("attempt." + name).c_str());
  };
  write_transform_meta(db, prefix("status"), "reused");
  write_transform_meta(db, prefix("version"), std::to_string(run.version));
  write_transform_meta(db, prefix("input"), run.input_identity);
  write_transform_meta(db, prefix("output"), run.output_identity);
  write_transform_meta(db, prefix("count"), std::to_string(run.output_count));
  write_transform_meta(db, prefix("generation"),
                       std::to_string(run.generation));
  write_transform_meta(db, prefix("published_generation"),
                       std::to_string(published_generation));
  write_transform_meta(db, prefix("diagnostic"), run.diagnostic);
  write_transform_meta(db, prefix("applicability"),
                       transform_applicability_name(run.applicability));
  write_transform_meta(db, prefix("completeness"),
                       transform_completeness_name(run.completeness));
  write_transform_meta(db, transform_meta_key(run.transform_id, "status"),
                       "reused");
  write_transform_meta(db, transform_meta_key(run.transform_id, "diagnostic"),
                       run.diagnostic);
  std::string changed;
  for (const auto &input : run.changed_inputs) {
    if (!changed.empty()) {
      changed += ",";
    }
    changed += input;
  }
  write_transform_meta(db,
                       transform_meta_key(run.transform_id, "changed_inputs"),
                       changed);
  write_transform_meta(
      db, transform_meta_key(run.transform_id, "history.last_run"),
      std::string("reused|") + run.input_identity + "|" + run.diagnostic);
}

void write_failed_attempt(SqliteDb &db, const TransformRun &run) {
  write_transform_meta(db,
                       transform_meta_key(run.transform_id, "attempt.status"),
                       transform_run_status_name(run.status));
  write_transform_meta(db,
                       transform_meta_key(run.transform_id, "attempt.version"),
                       std::to_string(run.version));
  write_transform_meta(db,
                       transform_meta_key(run.transform_id, "attempt.input"),
                       run.input_identity);
  write_transform_meta(db,
                       transform_meta_key(run.transform_id, "attempt.output"),
                       run.output_identity);
  write_transform_meta(db,
                       transform_meta_key(run.transform_id, "attempt.count"),
                       std::to_string(run.output_count));
  write_transform_meta(
      db, transform_meta_key(run.transform_id, "attempt.generation"),
      std::to_string(run.generation));
  write_transform_meta(
      db, transform_meta_key(run.transform_id, "attempt.published_generation"),
      std::to_string(run.published_generation));
  write_transform_meta(db,
                       transform_meta_key(run.transform_id, "attempt.diagnostic"),
                       run.diagnostic);
  write_transform_meta(
      db, transform_meta_key(run.transform_id, "attempt.applicability"),
      transform_applicability_name(run.applicability));
  write_transform_meta(
      db, transform_meta_key(run.transform_id, "attempt.completeness"),
      transform_completeness_name(run.completeness));
  write_transform_meta(db,
                       transform_meta_key(run.transform_id, "stale_cause"),
                       run.diagnostic);
  write_transform_meta(db, transform_meta_key(run.transform_id, "status"),
                       transform_run_status_name(run.status));
  std::string changed;
  for (const auto &input : run.changed_inputs) {
    if (!changed.empty()) {
      changed += ",";
    }
    changed += input;
  }
  write_transform_meta(db,
                       transform_meta_key(run.transform_id, "changed_inputs"),
                       changed);
}

void write_invalidation_values(
    SqliteDb &db, const TransformDescriptor &d,
    const std::unordered_map<std::string, std::string> &values) {
  for (const auto &[name, value] : values) {
    write_transform_meta(
        db, "transform." + d.id + ".published.key." + name, value);
  }
}

void write_attempt_invalidation_values(
    SqliteDb &db, const TransformDescriptor &d,
    const std::unordered_map<std::string, std::string> &values) {
  for (const auto &[name, value] : values) {
    write_transform_meta(db, "transform." + d.id + ".attempt.key." + name,
                         value);
  }
}

std::string query_identity(SqliteDb &db,
                           const std::vector<std::string> &queries) {
  std::string canonical;
  for (const auto &query : queries) {
    auto st = db.prepare(query);
    while (st.step()) {
      canonical += "row\x1f";
      for (int column = 0; column < st.column_count(); ++column) {
        if (st.col_is_null(column)) {
          canonical += "null";
        } else {
          canonical += st.col_text(column);
        }
        canonical += '\x1e';
      }
    }
    canonical += "query\x1d";
  }
  return sha256_hex(canonical);
}

std::int64_t output_count(SqliteDb &db, const std::string &query) {
  auto st = db.prepare(query);
  return st.step() ? st.col_int64(0) : 0;
}

std::unordered_map<std::string, std::string>
current_invalidation_values(SqliteDb &db, const TransformDescriptor &d) {
  std::unordered_map<std::string, std::string> values;
  for (const auto &input : d.invalidation_inputs) {
    std::optional<std::string> override_value;
    if (input.kind != TransformInputKind::implementation) {
      override_value = read_transform_meta(
          db, "transform.input." + d.id + "." + input.name);
      if (!override_value) {
        override_value =
            read_transform_meta(db, "transform.input." + input.name);
      }
    }
    if (override_value) {
      values[input.name] = *override_value;
      continue;
    }
    std::string value = input.static_value;
    if (!input.value_query.empty()) {
      value += "|" + query_identity(db, {input.value_query});
    }
    values[input.name] = sha256_hex(value);
  }
  return values;
}

TransformBudget effective_budget(SqliteDb &db, const TransformDescriptor &d) {
  TransformBudget budget = d.budget;
  if (const auto rows = read_transform_meta(
          db, "transform.budget." + d.id + ".max_rows")) {
    budget.max_rows = std::stoll(*rows);
  }
  if (const auto milliseconds = read_transform_meta(
          db, "transform.budget." + d.id + ".max_milliseconds")) {
    budget.max_milliseconds = std::stoll(*milliseconds);
  }
  return budget;
}

bool readiness_transform(const TransformDescriptor &transform) {
  return transform.id == "include-fact-readiness" ||
         transform.id == "hse-66-effect-registration" ||
         transform.id == "hse-66-proof-registration" ||
         transform.id == "type-fact-readiness";
}

std::string dependency_token(const TransformRun &run) {
  // A generation number and changed-input labels are publication history, not
  // content. The stable input/output identities already carry provider and
  // dependency content changes to declared consumers.
  return sha256_hex(run.transform_id + "\x1f" + std::to_string(run.version) +
                    "\x1f" + run.output_identity + "\x1f" +
                    run.input_identity);
}

bool qualified_ready(const TransformRun &run) {
  return (run.status == TransformRunStatus::ran ||
          run.status == TransformRunStatus::reused ||
          run.status == TransformRunStatus::skipped) &&
         run.completeness == TransformCompleteness::complete;
}

std::string source_identity(
    SqliteDb &db, const TransformDescriptor &d,
    const std::unordered_map<std::string, std::string> &key_values) {
  std::string canonical = d.id + "\x1f" + std::to_string(d.version);
  std::vector<std::string> names;
  names.reserve(key_values.size());
  for (const auto &[name, unused] : key_values) {
    (void)unused;
    names.push_back(name);
  }
  std::ranges::sort(names);
  for (const auto &name : names) {
    canonical += "\x1e" + name + "\x1f" + key_values.at(name);
  }
  for (const auto &option : d.options) {
    canonical += "\x1eoption\x1f" + option;
  }
  // These are source facts only. Derived outputs are represented solely by
  // dependency identities below and can never feed a transform's own key.
  canonical += "\x1equery\x1f" + query_identity(db, d.input_queries);
  return sha256_hex(canonical);
}

std::string input_identity(
    SqliteDb &db, const TransformDescriptor &d,
    const std::unordered_map<std::string, std::string> &key_values,
    const std::unordered_map<std::string, std::string> &dependency_outputs) {
  std::string canonical = source_identity(db, d, key_values);
  for (const auto &dependency : d.dependencies) {
    canonical += "\x1e" "dependency\x1f" + dependency + "\x1f" +
                 dependency_outputs.at(dependency);
  }
  return sha256_hex(canonical);
}

std::vector<std::string> changed_inputs(
    SqliteDb &db, const TransformDescriptor &d,
    const std::unordered_map<std::string, std::string> &current) {
  std::vector<std::string> changed;
  for (const auto &[name, value] : current) {
    const auto previous = read_transform_meta(
        db, "transform." + d.id + ".published.key." + name);
    if (!previous || *previous != value) {
      changed.push_back(name);
    }
  }
  return changed;
}

int count_stubs(SqliteDb &db) {
  auto st = db.prepare(
      "SELECT COUNT(*) FROM symbol WHERE resolved = 0 AND file_id IS NULL "
      "AND decl_file_id IS NULL");
  return st.step() ? static_cast<int>(st.col_int64(0)) : 0;
}

void validate_implementation_provider(const TransformDescriptor &d) {
  const auto [provider_id, provider_content] =
      implementation_provider_for(d.id);
  if (d.implementation_provider.provider_id != provider_id ||
      d.implementation_provider.content != provider_content) {
    throw StorageError("implementation provider is not coupled to executor: " +
                       d.id);
  }
}

void run_transform(SqliteStorageService &storage,
                   const TransformDescriptor &d) {
  validate_implementation_provider(d);
  if (d.id == "edge-site-count-rollup") {
    storage.rollup_edge_counts();
  } else if (d.id == "multi-definition-classification") {
    storage.set_multi_def();
  } else if (d.id == "possible-call-materialization") {
    storage.materialize_possible_calls();
  } else if (d.id == "virtual-dispatch-call-materialization") {
    storage.materialize_dispatch_calls();
  } else if (d.id == "entity-graph-rollup") {
    storage.materialise_entity_edges();
  } else {
    throw StorageError("no executor registered for transform: " + d.id);
  }
}

} // namespace


// ---------------------------------------------------------------------------
// materialise_entity_edges: pure-DB roll-up of all 11 entity relation kinds.
// Mirrors indexer/entity_rollup.py:materialize_entity_edges() byte-identically.
// ---------------------------------------------------------------------------

// Per-pass precomputed lookups (perf: O(n^2) -> O(n)). The collapse +
// interface/abstractness helpers used to issue a fresh SQL query (often
// several) PER edge row across every phase -- on a large corpus that is the
// dominant cost of `resolve` and made the pass run for a very long time with no
// DB writes (read-heavy, so index.db mtime / journal never moved -- looking
// frozen). RollupState precomputes the same answers ONCE per materialise pass
// from tables that are READ-ONLY for the pass (edge, symbol), then the hot
// helpers become in-memory map/set lookups. Byte-identical to the old per-row
// queries (the parity gate + acceptance suite verify), so a pure speedup.
// Mirrors entity_rollup._RollupState (Python).
struct RollupState {
  std::unordered_map<int64_t, int64_t> next_hop;       // src -> first 4/5 dst
  std::unordered_map<int64_t, int64_t> collapse_cache; // memoised collapse
  std::unordered_set<std::string> non_pure_method_owners;
  std::unordered_set<std::string> field_owners;
  std::unordered_set<std::string> pure_method_owners;
  std::unordered_map<int64_t, std::string> usr_by_id; // entity-kind ids only

  explicit RollupState(cidx::SqliteDb &db) {
    // collapse next-hop: FIRST (kind, dst_id) per src among kind 4/5 edges ==
    // the old `WHERE src_id=? AND kind IN (4,5) ORDER BY kind, dst_id LIMIT 1`
    // for every src in one ordered scan (emplace keeps the first per key).
    {
      auto st = db.prepare("SELECT src_id, dst_id FROM edge WHERE kind IN (4, 5) "
                           "ORDER BY src_id, kind, dst_id");
      while (st.step()) {
        next_hop.emplace(st.col_int64(0), st.col_int64(1));
      }
    }
    // Interface / abstractness owner-sets keyed by parent_usr (the three
    // COUNT(*) probes the old is_interface ran PER call, hoisted to 3 scans).
    {
      auto st = db.prepare("SELECT DISTINCT parent_usr FROM symbol "
                           "WHERE kind = 21 AND is_pure = 0 AND parent_usr IS NOT NULL");
      while (st.step()) {
        non_pure_method_owners.insert(st.col_text(0));
      }
    }
    {
      auto st = db.prepare("SELECT DISTINCT parent_usr FROM symbol "
                           "WHERE kind = 6 AND parent_usr IS NOT NULL");
      while (st.step()) {
        field_owners.insert(st.col_text(0));
      }
    }
    {
      auto st = db.prepare("SELECT DISTINCT parent_usr FROM symbol "
                           "WHERE kind = 21 AND is_pure = 1 AND parent_usr IS NOT NULL");
      while (st.step()) {
        pure_method_owners.insert(st.col_text(0));
      }
    }
    {
      auto st = db.prepare("SELECT id, usr FROM symbol WHERE kind IN (2,3,4,5,31)");
      while (st.step()) {
        usr_by_id.emplace(st.col_int64(0), st.col_text(1));
      }
    }
  }

  int64_t collapse(int64_t sym_id) {
    auto hit = collapse_cache.find(sym_id);
    if (hit != collapse_cache.end()) {
      return hit->second;
    }
    std::set<int64_t> seen;
    int64_t cur = sym_id;
    while (!seen.contains(cur)) {
      seen.insert(cur);
      auto nit = next_hop.find(cur);
      if (nit == next_hop.end()) {
        break;
      }
      cur = nit->second;
    }
    collapse_cache.emplace(sym_id, cur);
    return cur;
  }

  [[nodiscard]] bool is_interface(int64_t sym_id) const {
    auto it = usr_by_id.find(sym_id);
    if (it == usr_by_id.end()) {
      return false;
    }
    const std::string &usr = it->second;
    if (non_pure_method_owners.contains(usr)) {
      return false;
    }
    if (field_owners.contains(usr)) {
      return false;
    }
    return pure_method_owners.contains(usr);
  }

  [[nodiscard]] bool has_pure(int64_t sym_id) const {
    auto it = usr_by_id.find(sym_id);
    return it != usr_by_id.end() && pure_method_owners.contains(it->second);
  }
};

// Module-global state for the in-progress pass. Set by materialise_entity_edges
// (and cpp_materialise_entity_nodes when called standalone for the v21->v22
// backfill); cleared by the matching CtxGuard destructor. resolve is
// single-threaded and the helpers only run synchronously within a pass.
RollupState *g_rollup_ctx = nullptr;

// RAII guard: builds + installs a RollupState only if none is active yet (the
// outermost caller "owns" it), and uninstalls on scope exit. A nested call
// (entity_nodes inside materialise_entity_edges) reuses the active state with
// no rebuild. Mirrors the owns_ctx logic in entity_rollup (Python).
struct CtxGuard {
  std::optional<RollupState> st;
  bool owned;
  explicit CtxGuard(cidx::SqliteDb &db) : owned(g_rollup_ctx == nullptr) {
    if (owned) {
      st.emplace(db);
      g_rollup_ctx = &*st;
    }
  }
  ~CtxGuard() {
    if (owned) {
      g_rollup_ctx = nullptr;
    }
  }
  CtxGuard(const CtxGuard &) = delete;
  CtxGuard &operator=(const CtxGuard &) = delete;
  CtxGuard(CtxGuard &&) = delete;
  CtxGuard &operator=(CtxGuard &&) = delete;
};

// Template-instance collapse (ADR-008 decision 6 / OQ-3): map a template
// instance/specialization symbol onto its primary template. Both the Layer-0
// instantiates(5) and specializes(4) edges point instance -> primary, so we
// follow an outgoing 4/5 edge until none remains. Returns sym_id unchanged
// when it is not an instance/specialization. Mirrors entity_rollup._collapse_to_primary.
// Delegates to the per-pass precomputed next-hop map; `db` is unused (kept for
// signature stability with the call sites).
static int64_t cpp_collapse_to_primary(cidx::SqliteDb &db, int64_t sym_id) {
  (void)db;
  return g_rollup_ctx->collapse(sym_id);
}

// Phase 1: generalizes(1) / implements(2) from inherits(2) edges.
static void cpp_materialise_inheritance(cidx::SqliteDb &db) {
  // Is sym_id a pure Interface? Delegates to the per-pass precomputed
  // owner-sets (RollupState), identical to the old per-row COUNT(*) probes.
  const auto is_interface = [](int64_t sym_id) -> bool {
    return g_rollup_ctx->is_interface(sym_id);
  };

  auto st = db.prepare(
      "SELECT e.src_id, e.dst_id, e.base_access, e.is_virtual "
      "FROM edge e "
      "JOIN symbol src ON src.id = e.src_id "
      "JOIN symbol dst ON dst.id = e.dst_id "
      "WHERE e.kind = 2 "
      "  AND src.kind IN (2,3,4,5) "
      "  AND dst.kind IN (2,3,4,5)");

  struct InhRow { int64_t src; int64_t dst; int64_t acc; int64_t virt; };
  std::vector<InhRow> rows;
  while (st.step()) {
    InhRow r;
    r.src  = st.col_int64(0);
    r.dst  = st.col_int64(1);
    r.acc  = st.col_int64(2);
    r.virt = st.col_int64(3);
    rows.push_back(r);
  }

  for (const auto &r : rows) {
    // Collapse the DERIVED side (src) onto its primary template, but keep the
    // BASE (dst) un-collapsed: a template used as a base
    // (`class Cache : public Singleton<Cache>`) is its OWN design entity, so we
    // want `Cache generalizes Singleton<Cache>` and let the separate
    // instantiates(11) edge carry `Singleton<Cache> -> Singleton`.  (Pre-CRTP-
    // fix this was moot -- no base specifier had an instantiates(5) Layer-0
    // edge, so collapsing the dst was always a no-op.)
    int64_t src = cpp_collapse_to_primary(db, r.src);
    int64_t dst = r.dst;
    if (src == dst) {
      continue; // no self-edge
    }
    int64_t ek = is_interface(dst) ? 2 : 1;  // implements=2 or generalizes=1
    auto ins = db.prepare(
        "INSERT INTO entity_edge "
        "(src_id, dst_id, kind, count, via_member_id, multiplicity, "
        " access, is_virtual, create_form, partial) "
        "VALUES (?, ?, ?, 1, NULL, 1, ?, ?, NULL, 0) "
        "ON CONFLICT(src_id, dst_id, kind, COALESCE(via_member_id, -1), COALESCE(create_form, -1)) DO UPDATE SET "
        "  access     = excluded.access, "
        "  is_virtual = excluded.is_virtual");
    ins.bind(1, src);
    ins.bind(2, dst);
    ins.bind(3, ek);
    ins.bind(4, r.acc);
    ins.bind(5, r.virt);
    ins.step_done();
  }
}

// Phase 2: specializes(3) from Layer-0 specializes(4) edges between entity
// symbols. These come ONLY from EXPLICIT / PARTIAL specializations (the
// extractor emits kind 4 for those and kind 5 (instantiates) for plain
// instantiations, so the two are disjoint at Layer-0). Mirrors
// entity_rollup._materialise_specializes.
static void cpp_materialise_specializes(cidx::SqliteDb &db) {
  auto st = db.prepare(
      "SELECT e.src_id, e.dst_id "
      "FROM edge e "
      "JOIN symbol src ON src.id = e.src_id "
      "JOIN symbol dst ON dst.id = e.dst_id "
      "WHERE e.kind = 4 "
      "  AND src.kind IN (2,3,4,5,31) "
      "  AND dst.kind IN (2,3,4,5,31)");
  std::vector<std::pair<int64_t,int64_t>> rows;
  while (st.step()) {
    rows.emplace_back(st.col_int64(0), st.col_int64(1));
  }
  for (const auto &[src0, dst0] : rows) {
    // The specialization is its OWN design entity -- do NOT collapse the SOURCE
    // onto the primary (that would self-suppress the edge). Collapse only the
    // destination (already the primary; this is a no-op there but keeps the
    // phase robust to chains).
    int64_t src = src0;
    int64_t dst = cpp_collapse_to_primary(db, dst0);
    if (src == dst) {
      continue;
    }
    auto ins = db.prepare(
        "INSERT INTO entity_edge "
        "(src_id, dst_id, kind, count, via_member_id, multiplicity, "
        " access, is_virtual, create_form, partial) "
        "VALUES (?, ?, 3, 1, NULL, 1, 0, 0, NULL, 0) "
        "ON CONFLICT(src_id, dst_id, kind, COALESCE(via_member_id, -1), COALESCE(create_form, -1)) DO UPDATE SET "
        "  count = entity_edge.count + 1");
    ins.bind(1, src);
    ins.bind(2, dst);
    ins.step_done();
  }
}

// Phase 2b: instantiates(11) from Layer-0 instantiates(5) edges between entity
// symbols. src = the concrete instance `X<B>`, dst = the primary template `X`.
// An implicit instantiation is a distinct design entity (UML <<bind>>), so --
// exactly like specializes -- the SOURCE is kept un-collapsed (collapsing it
// would follow its own kind-5 edge to the primary and self-suppress the row).
// Mirrors entity_rollup._materialise_instantiates.
static void cpp_materialise_instantiates(cidx::SqliteDb &db) {
  auto st = db.prepare(
      "SELECT e.src_id, e.dst_id "
      "FROM edge e "
      "JOIN symbol src ON src.id = e.src_id "
      "JOIN symbol dst ON dst.id = e.dst_id "
      "WHERE e.kind = 5 "
      "  AND src.kind IN (2,3,4,5,31) "
      "  AND dst.kind IN (2,3,4,5,31)");
  std::vector<std::pair<int64_t,int64_t>> rows;
  while (st.step()) {
    rows.emplace_back(st.col_int64(0), st.col_int64(1));
  }
  for (const auto &[src0, dst0] : rows) {
    int64_t src = src0;
    int64_t dst = cpp_collapse_to_primary(db, dst0);
    if (src == dst) {
      continue;
    }
    auto ins = db.prepare(
        "INSERT INTO entity_edge "
        "(src_id, dst_id, kind, count, via_member_id, multiplicity, "
        " access, is_virtual, create_form, partial) "
        "VALUES (?, ?, 11, 1, NULL, 1, 0, 0, NULL, 0) "
        "ON CONFLICT(src_id, dst_id, kind, COALESCE(via_member_id, -1), COALESCE(create_form, -1)) DO UPDATE SET "
        "  count = entity_edge.count + 1");
    ins.bind(1, src);
    ins.bind(2, dst);
    ins.step_done();
  }
}

// Classify field type spelling → (entity_edge kind, multiplicity).
// Split the inside of a <...> on TOP-LEVEL commas (depth-aware). Mirrors
// entity_rollup._split_template_args.
static std::vector<std::string> cpp_split_template_args(const std::string &inner) {
  std::vector<std::string> args;
  int depth = 0;
  std::string cur;
  auto flush = [&] {
    size_t a = cur.find_first_not_of(' ');
    if (a != std::string::npos) {
      size_t b = cur.find_last_not_of(' ');
      args.push_back(cur.substr(a, b - a + 1));
    }
    cur.clear();
  };
  for (char ch : inner) {
    if (ch == '<') { ++depth; cur.push_back(ch); }
    else if (ch == '>') { --depth; cur.push_back(ch); }
    else if (ch == ',' && depth == 0) { flush();
    } else {
      {
        cur.push_back(ch);
      }
    }
  }
  flush();
  return args;
}

// For `s` starting with a `...<` wrapper `prefix`, return the VALUE type: the
// LAST top-level template arg (map<K,V> -> V). Mirrors _wrapper_value_type.
static std::string cpp_wrapper_value_type(const std::string &s,
                                          const char *prefix) {
  std::string inner = s.substr(strlen(prefix));
  while (!inner.empty() && inner.back() == ' ') {
    inner.pop_back();
  }
  if (!inner.empty() && inner.back() == '>') {
    inner.pop_back();
  }
  while (!inner.empty() && inner.back() == ' ') {
    inner.pop_back();
  }
  auto args = cpp_split_template_args(inner);
  return args.empty() ? inner : args.back();
}

static std::pair<int64_t,int64_t> cpp_classify_field_type(
    const std::string &type_info) {
  const std::string s = [&] {
    std::string r = type_info;
    // Strip const/volatile
    for (const auto *q : {"const ", "volatile "}) {
      std::string::size_type p;
      while ((p = r.find(q)) != std::string::npos) {
        r.erase(p, strlen(q));
      }
    }
    while (!r.empty() && r.front() == ' ') {
      r.erase(r.begin());
    }
    while (!r.empty() && r.back() == ' ') {
      r.pop_back();
    }
    return r;
  }();
  // Array
  if (!s.empty() && s.back() == ']') {
    return {4, 4};
  }
  // Containers
  static const char *containers[] = {
    "std::vector<", "vector<", "std::list<", "list<",
    "std::deque<", "deque<", "std::set<", "set<",
    "std::unordered_set<", "unordered_set<",
    "std::map<", "std::unordered_map<", nullptr
  };
  for (const char **c = containers; (*c) != nullptr; ++c) {
    if (s.substr(0, strlen(*c)) == *c) {
      // Classify the VALUE type (last template arg, so map<K,V> uses V).
      auto [ik, _] = cpp_classify_field_type(cpp_wrapper_value_type(s, *c));
      return {ik, 3};
    }
  }
  // unique_ptr / optional -> composes (EXCLUSIVE ownership: destroyed with the
  // owner, cannot outlive it -- same lifetime as a value member), 0..1.
  static const char *excl[] = {"std::unique_ptr<", "unique_ptr<",
                                "std::optional<", "optional<", nullptr};
  for (const char **u = excl; (*u) != nullptr; ++u) {
    if (s.substr(0, strlen(*u)) == *u) {
      return {4, 2}; // composes=4
    }
  }
  // shared_ptr -> aggregates (SHARED ownership: the pointee can outlive the
  // owner while other shared_ptrs keep it alive).
  static const char *shared[] = {"std::shared_ptr<", "shared_ptr<", nullptr};
  for (const char **u = shared; (*u) != nullptr; ++u) {
    if (s.substr(0, strlen(*u)) == *u) {
      return {5, 2}; // aggregates=5
    }
  }
  static const char *weak_raw[] = {"std::weak_ptr<", "weak_ptr<", nullptr};
  for (const char **w = weak_raw; (*w) != nullptr; ++w) {
    if (s.substr(0, strlen(*w)) == *w) {
      return {6, 2}; // associates=6
    }
  }
  if (!s.empty() && s.back() == '*') {
    return {6, 2};
  }
  if (!s.empty() && s.back() == '&') {
    return {6, 2};
  }
  return {4, 1};  // composes=4, multiplicity=1 (value)
}

// Resolve entity from type spelling (strips wrappers, looks up by qual_name/spelling).
static std::optional<int64_t> cpp_resolve_entity_from_type(
    cidx::SqliteDb &db, std::string type_info) {
  // Strip qualifiers
  for (const auto *q : {"const ", "volatile "}) {
    std::string::size_type p;
    while ((p = type_info.find(q)) != std::string::npos) {
      type_info.erase(p, strlen(q));
    }
  }
  while (!type_info.empty() && type_info.front() == ' ') {
    type_info.erase(type_info.begin());
  }
  while (!type_info.empty() && type_info.back() == ' ') {
    type_info.pop_back();
  }
  // Strip trailing * & []
  bool stripped = true;
  while (stripped) {
    stripped = false;
    if (!type_info.empty() && type_info.back() == '*') {
      type_info.pop_back(); stripped = true;
    } else if (!type_info.empty() && type_info.back() == '&') {
      type_info.pop_back(); stripped = true;
    } else if (!type_info.empty() && type_info.back() == ']') {
      auto p = type_info.rfind('[');
      if (p != std::string::npos) { type_info = type_info.substr(0,p); stripped = true; }
    }
    while (!type_info.empty() && type_info.back() == ' ') {
      type_info.pop_back();
    }
  }
  // Strip smart-ptr / container wrappers
  static const char *wrappers[] = {
    "std::unique_ptr<", "unique_ptr<", "std::shared_ptr<", "shared_ptr<",
    "std::weak_ptr<",   "weak_ptr<",   "std::optional<",   "optional<",
    "std::vector<", "vector<", "std::list<", "list<",
    "std::deque<", "deque<", "std::set<", "set<",
    "std::unordered_set<", "unordered_set<",
    "std::map<", "std::unordered_map<", nullptr
  };
  for (const char **w = wrappers; (*w) != nullptr; ++w) {
    if (type_info.substr(0, strlen(*w)) == *w) {
      // Recurse on the VALUE type (last template arg) so map<K,V> -> V and
      // nested generics peel one level at a time.
      return cpp_resolve_entity_from_type(db, cpp_wrapper_value_type(type_info, *w));
    }
  }
  // Lookup by qual_name
  auto st1 = db.prepare(
      "SELECT id FROM symbol WHERE qual_name = ? AND kind IN (2,3,4,5) LIMIT 1");
  st1.bind(1, std::string_view(type_info));
  if (st1.step()) {
    return st1.col_int64(0);
  }
  // Lookup by spelling
  auto st2 = db.prepare(
      "SELECT id FROM symbol WHERE spelling = ? AND kind IN (2,3,4,5) LIMIT 1");
  st2.bind(1, std::string_view(type_info));
  if (st2.step()) {
    return st2.col_int64(0);
  }
  return std::nullopt;
}

// Phase 3: composes/aggregates/associates from field_of(8) edges.
static void cpp_materialise_field_relations(cidx::SqliteDb &db) {
  struct FieldRow {
    int64_t field_id, owner_id, field_kind_int;
    std::string type_info, field_access;
  };
  auto st = db.prepare(
      "SELECT e.src_id, e.dst_id, s.type_info, s.kind AS field_kind, "
      "       s.access AS field_access "
      "FROM edge e "
      "JOIN symbol s ON s.id = e.src_id "
      "JOIN symbol owner ON owner.id = e.dst_id "
      "WHERE e.kind = 8 "
      "  AND owner.kind IN (2,3,4,5) "
      "  AND s.kind IN (6, 21)");
  std::vector<FieldRow> rows;
  while (st.step()) {
    FieldRow r;
    r.field_id       = st.col_int64(0);
    r.owner_id       = st.col_int64(1);
    r.type_info      = st.col_text(2);
    r.field_kind_int = st.col_int64(3);
    r.field_access   = st.col_text(4);
    rows.push_back(r);
  }

  static const std::map<std::string,int64_t> acc_map = {
    {"public",0}, {"protected",1}, {"private",2}
  };

  for (const auto &r : rows) {
    if (r.field_kind_int != 6) {
      continue; // only data members
    }
    if (r.type_info.empty()) {
      continue;
    }

    // Stage 4: prefer a structural member -> NAMED-INSTANCE of_type(20) edge
    // (v34: was uses(7)). A `X<B> m_;` member mints the `X<B>` instance
    // (is_named_instance=1) and the
    // extractor records an of_type edge member -> instance keyed on the spec USR
    // (unambiguous across namespaces -- unlike a display_name match). The named
    // instance is its OWN design entity, so it is NOT collapsed onto the primary
    // -> we emit `A composes/associates X<B>`, completing A -> X<B> -> B. Reached
    // ONLY for minted named instances (non-system specializations); `std::vector
    // <Foo>` is never minted, so its peel-to-Foo resolution below is unchanged.
    std::optional<int64_t> ref_entity_id;
    bool skip_ref_collapse = false;
    auto nist = db.prepare(
        "SELECT e.dst_id FROM edge e "
        "JOIN symbol s ON s.id = e.dst_id "
        "WHERE e.src_id = ? AND e.kind = 20 AND s.is_named_instance = 1 "
        "ORDER BY e.dst_id LIMIT 1");
    nist.bind(1, r.field_id);
    if (nist.step()) {
      ref_entity_id = nist.col_int64(0);
      skip_ref_collapse = true;
    }

    if (!ref_entity_id) {
      // Try template_arg.ref_id first.  Use the LAST type arg (highest position)
      // so map<K,V> picks the VALUE V, not the key K; single-arg containers /
      // smart-ptrs are unaffected.
      auto tst = db.prepare(
          "SELECT ref_id FROM template_arg WHERE owner_id = ? "
          "AND arg_kind = 1 AND ref_id IS NOT NULL ORDER BY position DESC LIMIT 1");
      tst.bind(1, r.field_id);
      if (tst.step()) {
        ref_entity_id = tst.col_int64(0);
      }

      if (!ref_entity_id) {
        ref_entity_id = cpp_resolve_entity_from_type(db, r.type_info);
      }
    }
    if (!ref_entity_id) {
      continue;
    }

    // Confirm referent is entity
    auto ck = db.prepare("SELECT kind FROM symbol WHERE id = ?");
    ck.bind(1, *ref_entity_id);
    if (!ck.step()) {
      continue;
    }
    const int64_t ref_kind = ck.col_int64(0);
    if (ref_kind != 2 && ref_kind != 3 && ref_kind != 4 && ref_kind != 5) {
      continue;
    }

    auto [ek, mult] = cpp_classify_field_type(r.type_info);
    int64_t access_int = 0;
    if (acc_map.contains(r.field_access)) {
      access_int = acc_map.at(r.field_access);
    }

    // Collapse the owner onto its primary template.  The referent is collapsed
    // too UNLESS it is a named instance (kept un-collapsed so the edge points at
    // `X<B>`, not the primary `X`).
    int64_t owner_pid = cpp_collapse_to_primary(db, r.owner_id);
    int64_t ref_pid = skip_ref_collapse
                          ? *ref_entity_id
                          : cpp_collapse_to_primary(db, *ref_entity_id);
    if (owner_pid == ref_pid) {
      continue;
    }

    auto ins = db.prepare(
        "INSERT INTO entity_edge "
        "(src_id, dst_id, kind, count, via_member_id, multiplicity, "
        " access, is_virtual, create_form, partial) "
        "VALUES (?, ?, ?, 1, ?, ?, ?, 0, NULL, 0) "
        "ON CONFLICT(src_id, dst_id, kind, COALESCE(via_member_id, -1), COALESCE(create_form, -1)) DO UPDATE SET "
        "  count = entity_edge.count + 1");
    ins.bind(1, owner_pid);
    ins.bind(2, ref_pid);
    ins.bind(3, ek);
    ins.bind(4, r.field_id);
    ins.bind(5, mult);
    ins.bind(6, access_int);
    ins.step_done();
  }
}

// Strip wrappers/qualifiers/ptr-ref-array off a field type, returning the bare
// innermost token (e.g. 'std::vector<T>' -> 'T', 'const T *' -> 'T'). Mirrors
// entity_rollup._strip_to_param_core. Used to discover which template parameter
// a primary-template member binds.
static std::string cpp_strip_to_param_core(const std::string &type_spelling) {
  auto strip_quals = [](std::string s) {
    for (const auto *q : {"const ", "volatile "}) {
      std::string::size_type p;
      while ((p = s.find(q)) != std::string::npos) {
        s.erase(p, strlen(q));
      }
    }
    while (!s.empty() && s.front() == ' ') {
      s.erase(s.begin());
    }
    while (!s.empty() && s.back() == ' ') {
      s.pop_back();
    }
    return s;
  };
  std::string s = strip_quals(type_spelling);
  while (!s.empty() && (s.back() == '&' || s.back() == '*' || s.back() == ']')) {
    if (s.back() == ']') {
      auto p = s.rfind('[');
      s = (p == std::string::npos) ? std::string() : s.substr(0, p);
    } else {
      s.pop_back();
    }
    while (!s.empty() && s.front() == ' ') {
      s.erase(s.begin());
    }
    while (!s.empty() && s.back() == ' ') {
      s.pop_back();
    }
  }
  s = strip_quals(s);
  static const char *wrappers[] = {
    "std::unique_ptr<", "unique_ptr<", "std::shared_ptr<", "shared_ptr<",
    "std::weak_ptr<",   "weak_ptr<",   "std::optional<",   "optional<",
    "std::vector<", "vector<", "std::list<", "list<",
    "std::deque<", "deque<", "std::set<", "set<",
    "std::unordered_set<", "unordered_set<",
    "std::map<", "std::unordered_map<", nullptr
  };
  for (const char **w = wrappers; (*w) != nullptr; ++w) {
    if (s.substr(0, strlen(*w)) == *w) {
      return cpp_strip_to_param_core(cpp_wrapper_value_type(s, *w));
    }
  }
  return s;
}

// Phase 3b: composes/aggregates/associates for NAMED template instances.
// A `using Y = X<B>;` mints the X<B> instance (is_named_instance=1) but libclang
// materialises NO members for it, so Phase 3 cannot classify them. Instead read
// the PRIMARY's members and SUBSTITUTE the instance's bound type: for a member
// binding template param i (bare T, vector<T>, unique_ptr<T>, T*, ...), look up
// the instance's template_arg at position i (-> B) and emit X<B> <ownership> B.
// The instance is NOT collapsed onto the primary. Mirrors
// entity_rollup._materialise_instance_composition.
static void cpp_materialise_instance_composition(cidx::SqliteDb &db) {
  struct InstRow { int64_t inst_id, prim_id; };
  std::vector<InstRow> instances;
  {
    auto st = db.prepare(
        "SELECT e.src_id, e.dst_id "
        "FROM edge e "
        "JOIN symbol inst ON inst.id = e.src_id "
        "JOIN symbol prim ON prim.id = e.dst_id "
        "WHERE e.kind = 5 AND inst.is_named_instance = 1 AND prim.kind = 31 "
        "ORDER BY e.src_id, e.dst_id");
    while (st.step()) {
      instances.push_back(
          {.inst_id = st.col_int64(0), .prim_id = st.col_int64(1)});
    }
  }

  static const std::map<std::string,int64_t> acc_map = {
    {"public",0}, {"protected",1}, {"private",2}
  };

  for (const auto &inst : instances) {
    // primary template parameter NAME -> position (type params only)
    std::map<std::string,int64_t> param_pos;
    {
      auto st = db.prepare(
          "SELECT position, name FROM template_param WHERE owner_id = ? "
          "AND param_kind = 1 ORDER BY position");
      st.bind(1, inst.prim_id);
      while (st.step()) {
        const std::string nm = st.col_text(1);
        if (!nm.empty()) {
          param_pos.emplace(nm, st.col_int64(0));
        }
      }
    }

    // instance bound TYPE args: position -> ref_id (the entity B). NULL ref_id
    // (builtin arg) recorded as nullopt so it is skipped below.
    std::map<int64_t, std::optional<int64_t>> bound;
    {
      auto st = db.prepare(
          "SELECT position, ref_id FROM template_arg WHERE owner_id = ? "
          "AND arg_kind = 1 ORDER BY position");
      st.bind(1, inst.inst_id);
      while (st.step()) {
        std::optional<int64_t> ref;
        if (!st.col_is_null(1)) {
          ref = st.col_int64(1);
        }
        bound[st.col_int64(0)] = ref;
      }
    }

    // primary template's data members
    struct FieldRow { int64_t field_id; std::string type_info, access; };
    std::vector<FieldRow> fields;
    {
      auto st = db.prepare(
          "SELECT e.src_id, s.type_info, s.access "
          "FROM edge e "
          "JOIN symbol s ON s.id = e.src_id "
          "WHERE e.kind = 8 AND e.dst_id = ? AND s.kind = 6 "
          "ORDER BY e.src_id");
      st.bind(1, inst.prim_id);
      while (st.step()) {
        fields.push_back({.field_id = st.col_int64(0),
                          .type_info = st.col_text(1),
                          .access = st.col_text(2)});
      }
    }

    for (const auto &f : fields) {
      if (f.type_info.empty()) {
        continue;
      }
      const std::string core = cpp_strip_to_param_core(f.type_info);
      auto pit = param_pos.find(core);
      int64_t ref_entity_id;
      if (pit != param_pos.end()) {
        // Parameterised member (binds T): substitute the instance's bound type
        // -> X<B> <ownership> B.
        auto bit = bound.find(pit->second);
        if (bit == bound.end() || !bit->second) {
          continue; // builtin/unindexed
        }
        ref_entity_id = *bit->second;
      } else {
        // Stage 3: CONCRETE (non-parameterised) member, e.g. `Widget w;` on the
        // primary -> carry `X<B> <ownership> Widget` onto the instance too.
        // System / unindexed concrete types resolve to nullopt and are skipped,
        // so no std:: explosion.
        auto re = cpp_resolve_entity_from_type(db, f.type_info);
        if (!re) {
          continue;
        }
        ref_entity_id = *re;
      }

      auto ck = db.prepare("SELECT kind FROM symbol WHERE id = ?");
      ck.bind(1, ref_entity_id);
      if (!ck.step()) {
        continue;
      }
      const int64_t ref_kind = ck.col_int64(0);
      if (ref_kind != 2 && ref_kind != 3 && ref_kind != 4 && ref_kind != 5) {
        continue;
      }
      if (inst.inst_id == ref_entity_id) {
        continue;
      }

      auto [ek, mult] = cpp_classify_field_type(f.type_info);
      int64_t access_int = 0;
      if (acc_map.contains(f.access)) {
        access_int = acc_map.at(f.access);
      }

      auto ins = db.prepare(
          "INSERT INTO entity_edge "
          "(src_id, dst_id, kind, count, via_member_id, multiplicity, "
          " access, is_virtual, create_form, partial) "
          "VALUES (?, ?, ?, 1, ?, ?, ?, 0, NULL, 0) "
          "ON CONFLICT(src_id, dst_id, kind, COALESCE(via_member_id, -1), COALESCE(create_form, -1)) DO UPDATE SET "
          "  count = entity_edge.count + 1");
      ins.bind(1, inst.inst_id);
      ins.bind(2, ref_entity_id);
      ins.bind(3, ek);
      ins.bind(4, f.field_id);
      ins.bind(5, mult);
      ins.bind(6, access_int);
      ins.step_done();
    }
  }
}

// Phase 4: creates(7) / destroys(9) from PR1 construction/destruction edges.
static void cpp_materialise_creates_destroys(cidx::SqliteDb &db) {
  // Layer-0 construct/destroy edge.kind -> create_form
  static const std::map<int64_t,int64_t> form_map = {
    {10,3},{11,4},{12,5},{13,7},{14,8},{15,6}
  };
  constexpr int64_t destroy_kind = 16;

  struct SiteRow { int64_t src_fn, dst_sym, l0_kind; };
  auto st = db.prepare(
      "SELECT e.src_id, e.dst_id, e.kind "
      "FROM edge e "
      "JOIN symbol src ON src.id = e.src_id "
      "JOIN symbol dst ON dst.id = e.dst_id "
      "WHERE e.kind IN (10,11,12,13,14,15,16)");
  std::vector<SiteRow> rows;
  while (st.step()) {
    rows.push_back({.src_fn = st.col_int64(0),
                    .dst_sym = st.col_int64(1),
                    .l0_kind = st.col_int64(2)});
  }

  for (const auto &r : rows) {
    // Enclosing entity (method_of=9, owner must be entity)
    auto own_st = db.prepare(
        "SELECT e.dst_id FROM edge e "
        "JOIN symbol owner ON owner.id = e.dst_id "
        "WHERE e.src_id = ? AND e.kind = 9 "
        "  AND owner.kind IN (2,3,4,5) LIMIT 1");
    own_st.bind(1, r.src_fn);
    if (!own_st.step()) {
      continue; // free fn: no entity src
    }
    const int64_t owner_entity = own_st.col_int64(0);

    // Target entity: ctor/dtor parent → record
    std::optional<int64_t> target;
    auto par_st = db.prepare(
        "SELECT id FROM symbol "
        "WHERE usr = (SELECT parent_usr FROM symbol WHERE id = ?) "
        "  AND kind IN (2,3,4,5) LIMIT 1");
    par_st.bind(1, r.dst_sym);
    if (par_st.step()) {
      target = par_st.col_int64(0);
    } else {
      // dst itself might be entity (rare)
      auto dk = db.prepare("SELECT kind FROM symbol WHERE id = ?");
      dk.bind(1, r.dst_sym);
      if (dk.step()) {
        int64_t k = dk.col_int64(0);
        if (k == 2 || k == 3 || k == 4 || k == 5) {
          target = r.dst_sym;
        }
      }
    }
    if (!target) {
      continue;
    }

    // Collapse both endpoints onto their primary template.
    int64_t owner_pid = cpp_collapse_to_primary(db, owner_entity);
    int64_t target_pid = cpp_collapse_to_primary(db, *target);
    if (owner_pid == target_pid) {
      continue;
    }

    if (r.l0_kind == destroy_kind) {
      auto ins = db.prepare(
          "INSERT INTO entity_edge "
          "(src_id, dst_id, kind, count, via_member_id, multiplicity, "
          " access, is_virtual, create_form, partial) "
          "VALUES (?, ?, 9, 1, NULL, 1, 0, 0, NULL, 0) "
          "ON CONFLICT(src_id, dst_id, kind, COALESCE(via_member_id, -1), COALESCE(create_form, -1)) DO UPDATE SET "
          "  count = entity_edge.count + 1");
      ins.bind(1, owner_pid);
      ins.bind(2, target_pid);
      ins.step_done();
    } else {
      int64_t create_form = form_map.at(r.l0_kind);
      int64_t partial = (r.l0_kind == 15) ? 1 : 0;
      auto ins = db.prepare(
          "INSERT INTO entity_edge "
          "(src_id, dst_id, kind, count, via_member_id, multiplicity, "
          " access, is_virtual, create_form, partial) "
          "VALUES (?, ?, 7, 1, NULL, 1, 0, 0, ?, ?) "
          "ON CONFLICT(src_id, dst_id, kind, COALESCE(via_member_id, -1), COALESCE(create_form, -1)) DO UPDATE SET "
          "  count = entity_edge.count + 1, "
          "  create_form = COALESCE(excluded.create_form, entity_edge.create_form), "
          "  partial = excluded.partial");
      ins.bind(1, owner_pid);
      ins.bind(2, target_pid);
      ins.bind(3, create_form);
      ins.bind(4, partial);
      ins.step_done();
    }
  }

  // By-value return (create_form=2): method return type → creates(7, partial=1)
  struct RetRow { int64_t method_id, owner_id; std::string type_info; };
  auto rst = db.prepare(
      "SELECT s.id, s.type_info, e.dst_id AS owner_id "
      "FROM symbol s "
      "JOIN edge e ON e.src_id = s.id AND e.kind = 9 "
      "JOIN symbol owner ON owner.id = e.dst_id AND owner.kind IN (2,3,4,5) "
      "WHERE s.kind IN (21, 24) AND s.type_info IS NOT NULL");
  std::vector<RetRow> ret_rows;
  while (rst.step()) {
    ret_rows.push_back({.method_id = rst.col_int64(0),
                        .owner_id = rst.col_int64(2),
                        .type_info = rst.col_text(1)});
  }
  for (const auto &r : ret_rows) {
    const std::string &ti = r.type_info;
    std::string ret_type;
    auto paren = ti.find('(');
    if (paren != std::string::npos && paren > 0) {
      ret_type = ti.substr(0, paren);
      while (!ret_type.empty() && ret_type.back() == ' ') {
        ret_type.pop_back();
      }
    } else {
      ret_type = ti;
    }
    if (ret_type.empty() || ret_type == "void" || ret_type == "auto") {
      continue;
    }
    auto ret_eid = cpp_resolve_entity_from_type(db, ret_type);
    if (!ret_eid) {
      continue;
    }

    // Collapse both endpoints onto their primary template.
    int64_t owner_pid = cpp_collapse_to_primary(db, r.owner_id);
    int64_t ret_pid = cpp_collapse_to_primary(db, *ret_eid);
    if (ret_pid == owner_pid) {
      continue; // constructors return own type
    }

    auto ins = db.prepare(
        "INSERT INTO entity_edge "
        "(src_id, dst_id, kind, count, via_member_id, multiplicity, "
        " access, is_virtual, create_form, partial) "
        "VALUES (?, ?, 7, 1, NULL, 1, 0, 0, 2, 1) "
        "ON CONFLICT(src_id, dst_id, kind, COALESCE(via_member_id, -1), COALESCE(create_form, -1)) DO UPDATE SET "
        "  count = entity_edge.count + 1");
    ins.bind(1, owner_pid);
    ins.bind(2, ret_pid);
    ins.step_done();
  }
}

// Phase 5: uses(8) from method→method calls across entity boundaries.
static void cpp_materialise_uses(cidx::SqliteDb &db) {
  struct UseRow { int64_t caller, callee, is_pure; };
  auto st = db.prepare(
      "SELECT e.src_id, e.dst_id, dst.is_pure "
      "FROM edge e "
      "JOIN symbol src ON src.id = e.src_id "
      "JOIN symbol dst ON dst.id = e.dst_id "
      "WHERE e.kind IN (1, 7) "
      "  AND src.kind IN (21, 8, 24, 25, 30) "
      "  AND dst.kind IN (21, 8, 24, 25, 30)");
  std::vector<UseRow> rows;
  while (st.step()) {
    rows.push_back({.caller = st.col_int64(0),
                    .callee = st.col_int64(1),
                    .is_pure = st.col_int64(2)});
  }
  for (const auto &r : rows) {
    // Caller owner entity
    auto co = db.prepare(
        "SELECT e.dst_id FROM edge e "
        "JOIN symbol owner ON owner.id = e.dst_id "
        "WHERE e.src_id = ? AND e.kind = 9 "
        "  AND owner.kind IN (2,3,4,5) LIMIT 1");
    co.bind(1, r.caller);
    if (!co.step()) {
      continue;
    }
    int64_t src_eid = co.col_int64(0);

    // Callee owner entity
    auto coe = db.prepare(
        "SELECT e.dst_id FROM edge e "
        "JOIN symbol owner ON owner.id = e.dst_id "
        "WHERE e.src_id = ? AND e.kind = 9 "
        "  AND owner.kind IN (2,3,4,5) LIMIT 1");
    coe.bind(1, r.callee);
    if (!coe.step()) {
      continue;
    }
    int64_t dst_eid = coe.col_int64(0);

    // Collapse both endpoints onto their primary template.
    src_eid = cpp_collapse_to_primary(db, src_eid);
    dst_eid = cpp_collapse_to_primary(db, dst_eid);
    if (src_eid == dst_eid) {
      continue;
    }
    int64_t partial = (r.is_pure != 0) ? 1 : 0;

    auto ins = db.prepare(
        "INSERT INTO entity_edge "
        "(src_id, dst_id, kind, count, via_member_id, multiplicity, "
        " access, is_virtual, create_form, partial) "
        "VALUES (?, ?, 8, 1, ?, 1, 0, 0, NULL, ?) "
        "ON CONFLICT(src_id, dst_id, kind, COALESCE(via_member_id, -1), COALESCE(create_form, -1)) DO UPDATE SET "
        "  count = entity_edge.count + 1, "
        "  partial = MAX(entity_edge.partial, excluded.partial)");
    ins.bind(1, src_eid);
    ins.bind(2, dst_eid);
    ins.bind(3, r.callee);
    ins.bind(4, partial);
    ins.step_done();
  }
}

// Phase 6: befriends(10) from friend(17) edges between entity symbols.
static void cpp_materialise_befriends(cidx::SqliteDb &db) {
  auto st = db.prepare(
      "SELECT e.src_id, e.dst_id "
      "FROM edge e "
      "JOIN symbol src ON src.id = e.src_id "
      "JOIN symbol dst ON dst.id = e.dst_id "
      "WHERE e.kind = 17 "
      "  AND src.kind IN (2,3,4,5) "
      "  AND dst.kind IN (2,3,4,5)");
  std::vector<std::pair<int64_t,int64_t>> rows;
  while (st.step()) {
    rows.emplace_back(st.col_int64(0), st.col_int64(1));
  }
  for (const auto &[src0, dst0] : rows) {
    int64_t src = cpp_collapse_to_primary(db, src0);
    int64_t dst = cpp_collapse_to_primary(db, dst0);
    if (src == dst) {
      continue;
    }
    auto ins = db.prepare(
        "INSERT INTO entity_edge "
        "(src_id, dst_id, kind, count, via_member_id, multiplicity, "
        " access, is_virtual, create_form, partial) "
        "VALUES (?, ?, 10, 1, NULL, 1, 0, 0, NULL, 0) "
        "ON CONFLICT(src_id, dst_id, kind, COALESCE(via_member_id, -1), COALESCE(create_form, -1)) DO UPDATE SET "
        "  count = entity_edge.count + 1");
    ins.bind(1, src);
    ins.bind(2, dst);
    ins.step_done();
  }
}

// Phase 7: entity_node(id, kind) -- the materialized design type of every
// entity symbol. Mirrors entity_rollup._materialise_entity_nodes byte-identically.
// Abstractness (own pure-virtual methods + own data fields) decides
// class/abstract_class/interface (and the same split for class templates);
// union/enum keep their own type. The C++ keyword (class vs struct) is NOT
// distinguished here -- that lives at the low-level symbol layer.
void cpp_materialise_entity_nodes(cidx::SqliteDb &db) {
  // Usable standalone (the v21->v22 entity_node backfill in the Storage ctor
  // calls this directly), so it installs the per-pass RollupState itself when
  // one is not already active (i.e. when NOT called from materialise_entity_edges).
  CtxGuard guard(db);
  // entity_kind ids: class=1 abstract_class=2 interface=3 union=4 enum=5
  // class_template=6 abstract_class_template=7 interface_template=8.
  // is_interface / has_pure delegate to the precomputed owner-sets.
  const auto classify = [](int64_t sym_id, int64_t sym_kind) -> int64_t {
    if (sym_kind == 5) {
      return 5; // enum
    }
    if (sym_kind == 3) {
      return 4; // union
    }
    bool is_template = (sym_kind == 31);
    if (g_rollup_ctx->is_interface(sym_id)) {
      return is_template ? 8 : 3;
    }
    if (g_rollup_ctx->has_pure(sym_id)) {
      return is_template ? 7 : 2;
    }
    return is_template ? 6 : 1;
  };

  db.exec("DELETE FROM entity_node");
  std::vector<std::pair<int64_t, int64_t>> rows;  // (id, kind)
  {
    auto st = db.prepare("SELECT id, kind FROM symbol WHERE kind IN (2,3,4,5,31)");
    while (st.step()) {
      rows.emplace_back(st.col_int64(0), st.col_int64(1));
    }
  }
  for (const auto &[sym_id, sym_kind] : rows) {
    auto ins = db.prepare(
        "INSERT OR REPLACE INTO entity_node (id, kind) VALUES (?, ?)");
    ins.bind(1, sym_id);
    ins.bind(2, classify(sym_id, sym_kind));
    ins.step_done();
  }
  // v26: namespaces (kind 22) are first-class entity nodes too. A single
  // canonical node per namespace USR (already collapsed in `symbol`), so one
  // entity_node covers all its reopenings across files/components/repos.
  std::vector<int64_t> ns_ids;
  {
    auto st = db.prepare("SELECT id FROM symbol WHERE kind = 22");
    while (st.step()) {
      ns_ids.push_back(st.col_int64(0));
    }
  }
  for (const int64_t ns_id : ns_ids) {
    auto ins = db.prepare(
        "INSERT OR REPLACE INTO entity_node (id, kind) VALUES (?, ?)");
    ins.bind(1, ns_id);
    ins.bind(2, static_cast<int64_t>(9)); // entity_kind 9 = namespace
    ins.step_done();
  }
}

// v26: namespace --declares--> member entity node. A `declares` entity edge
// (kind 12) for every Layer-0 `contains`(3) edge whose SRC is a namespace
// (symbol kind 22) and whose DST is an entity node (record/enum/class-template/
// nested namespace -- present in entity_node). DIRECT only: `contains` is
// already the direct lexical link, so ABC never `declares` ABC::XXX's members
// -- ABC and ABC::XXX are distinct entities (content is not recursive). Members
// that are not entities (free functions, variables) have no entity_node and are
// intentionally skipped. Must run AFTER cpp_materialise_entity_nodes (reads
// entity_node). Mirrors entity_rollup._materialise_declares.
static void cpp_materialise_declares(cidx::SqliteDb &db) {
  auto st = db.prepare(
      "SELECT e.src_id, e.dst_id "
      "FROM edge e "
      "JOIN symbol src ON src.id = e.src_id "
      "JOIN entity_node en ON en.id = e.dst_id "
      "WHERE e.kind = 3 AND src.kind = 22");
  std::vector<std::pair<int64_t, int64_t>> rows;
  while (st.step()) {
    rows.emplace_back(st.col_int64(0), st.col_int64(1));
  }
  for (const auto &[src_id, dst_id] : rows) {
    if (src_id == dst_id) {
      continue;
    }
    auto ins = db.prepare(
        "INSERT INTO entity_edge "
        "(src_id, dst_id, kind, count, via_member_id, multiplicity, "
        " access, is_virtual, create_form, partial) "
        "VALUES (?, ?, 12, 1, NULL, 1, 0, 0, NULL, 0) "
        "ON CONFLICT(src_id, dst_id, kind, COALESCE(via_member_id, -1), "
        "COALESCE(create_form, -1)) DO NOTHING");
    ins.bind(1, src_id);
    ins.bind(2, dst_id);
    ins.step_done();
  }
}

void SqliteStorageService::materialise_entity_edges() {
  // Idempotent: full re-materialise each resolve. The DELETE runs INSIDE the
  // rebuild transaction so a failure in any phase rolls back to the previous
  // rows instead of leaving entity_edge empty (atomic resolve).
  //
  // RollupState precomputes the collapse next-hop map + interface owner-sets
  // ONCE for the whole pass (edge/symbol are read-only here), so every phase's
  // collapse / interface lookups are in-memory instead of per-row SQL.
  CtxGuard guard(db_);
  const auto rebuild = [&]() {
    db_.exec("DELETE FROM entity_edge");
    const auto run = [&]([[maybe_unused]] const char *name,
                         void (*fn)(cidx::SqliteDb &)) { fn(db_); };
    run("inheritance", cpp_materialise_inheritance);
    run("specializes", cpp_materialise_specializes);
    run("instantiates", cpp_materialise_instantiates);
    run("field_relations", cpp_materialise_field_relations);
    run("instance_composition", cpp_materialise_instance_composition);
    run("creates_destroys", cpp_materialise_creates_destroys);
    run("uses", cpp_materialise_uses);
    run("befriends", cpp_materialise_befriends);
    run("entity_nodes", cpp_materialise_entity_nodes);
    run("declares",
        cpp_materialise_declares); // v26: needs entity_node populated
  };
  if (in_txn_) {
    rebuild();
  } else {
    auto txn = transaction();
    rebuild();
    txn.commit();
  }
}

TransformReport SqliteStorageService::run_transform_pipeline() {
  const bool profiling = profile::active();
  const auto profile_started = profiling
                                   ? std::chrono::steady_clock::now()
                                   : std::chrono::steady_clock::time_point{};
  const auto record_profile = [profiling, profile_started] {
    if (profiling) {
      profile::add_timing(
          "transforms", std::chrono::duration<double>(
                            std::chrono::steady_clock::now() - profile_started)
                            .count());
    }
  };
  const TransformRegistry registry = make_transform_registry(&db_);
  const auto ordered = registry.execution_order();
  TransformReport report;
  std::unordered_map<std::string, std::string> dependency_outputs;
  std::uint64_t generation = 0;
  if (const auto prior = read_transform_meta(db_, "transform.generation")) {
    generation = std::stoull(*prior);
  }
  ++generation;
  std::unordered_map<std::string, std::string> current_keys;
  std::vector<std::string> current_changed;
  const TransformDescriptor *current_transform = nullptr;

  auto txn = transaction();
  try {
    for (const TransformDescriptor *transform : ordered) {
      current_transform = transform;
      const auto key_values = current_invalidation_values(db_, *transform);
      const auto own_changed = changed_inputs(db_, *transform, key_values);
      current_keys = key_values;
      current_changed = own_changed;
      TransformRun run;
      run.transform_id = transform->id;
      run.version = transform->version;
      run.generation = generation;
      run.applicability = transform->applicability;
      run.completeness = transform->completeness;
      run.input_identity = input_identity(db_, *transform, key_values,
                                           dependency_outputs);
      const auto previous = read_transform_run(db_, *transform);
      const auto started = std::chrono::steady_clock::now();
      const auto budget = effective_budget(db_, *transform);
      bool output_mutation = false;
      if (previous && previous->version == run.version &&
          previous->input_identity == run.input_identity &&
          (previous->status == TransformRunStatus::ran ||
           previous->status == TransformRunStatus::reused ||
           previous->status == TransformRunStatus::skipped)) {
        const auto current_output =
            query_identity(db_, transform->output_queries);
        const auto current_count =
            output_count(db_, transform->output_count_query);
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started)
                .count();
        if (budget.max_rows > 0 && current_count > budget.max_rows) {
          throw StorageError("transform " + transform->id +
                             " exceeded max_rows during reuse");
        }
        if (budget.max_milliseconds > 0 && elapsed > budget.max_milliseconds) {
          throw StorageError("transform " + transform->id +
                             " exceeded max_milliseconds during reuse");
        }
        run = *previous;
        run.status = TransformRunStatus::reused;
        run.generation = generation;
        run.published_generation = previous->generation;
        run.output_identity = current_output;
        run.output_count = current_count;
        if (current_output != previous->output_identity ||
            current_count != previous->output_count) {
          // A persisted identity is not proof of current contents. Rebuild
          // the transform under its declared publication rule and retain the
          // qualification evidence in the attempt diagnostic.
          run.diagnostic = "published output mutation detected; rebuilding";
          current_changed = {"output:" + transform->id};
          output_mutation = true;
        } else {
          run.diagnostic = "reused; published generation=" +
                           std::to_string(previous->generation);
          run.changed_inputs.clear();
          write_reused_attempt(db_, run, previous->generation);
          report.runs.push_back(run);
          dependency_outputs[transform->id] = dependency_token(run);
          continue;
        }
      }

      report.affected_transforms.push_back(transform->id);
      current_changed = own_changed;
      if (previous && previous->version != run.version) {
        current_changed.emplace_back("implementation-version");
      }
      if (output_mutation) {
        current_changed.push_back("output:" + transform->id);
      }
      if (previous && current_changed.empty() &&
          previous->input_identity != run.input_identity) {
        for (const auto &dependency : transform->dependencies) {
          current_changed.push_back("dependency:" + dependency);
        }
      }
      const bool readiness_only = readiness_transform(*transform);
      validate_implementation_provider(*transform);
      if (!readiness_only) {
        run_transform(*this, *transform);
      }
      if (transform_nondeterminism_for_testing_ &&
          *transform_nondeterminism_for_testing_ == transform->id) {
        transform_nondeterminism_for_testing_.reset();
        if (transform->id == "edge-site-count-rollup") {
          auto mutate = db_.prepare(
              "UPDATE edge SET count = count + 1 WHERE kind IN (1, 7)");
          mutate.step_done();
        }
      }
      const auto persisted_failure =
          read_transform_meta(db_, "transform.test.failure");
      if ((transform_failure_for_testing_ &&
           *transform_failure_for_testing_ == transform->id) ||
          (persisted_failure && *persisted_failure == transform->id)) {
        transform_failure_for_testing_.reset();
        write_transform_meta(db_, "transform.test.failure", "");
        throw StorageError("injected transform failure: " + transform->id);
      }
      // Readiness transforms still execute qualification and publication; a
      // successful no-op is a ran generation, not an unqualified skip.
      run.status = TransformRunStatus::ran;
      run.output_identity = query_identity(db_, transform->output_queries);
      run.output_count = output_count(db_, transform->output_count_query);
      if (output_mutation && previous &&
          (run.output_identity != previous->output_identity ||
           run.output_count != previous->output_count)) {
        throw StorageError("transform " + transform->id +
                           " failed output qualification after mutation");
      }
      const auto elapsed =
          std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now() - started);
      if (budget.max_rows > 0 && run.output_count > budget.max_rows) {
        throw StorageError("transform " + transform->id +
                           " exceeded max_rows");
      }
      if (budget.max_milliseconds > 0 && elapsed.count() > budget.max_milliseconds) {
        throw StorageError("transform " + transform->id +
                           " exceeded max_milliseconds");
      }
      run.diagnostic =
          std::string(output_mutation ? "published output mutation detected; "
                                      : "") +
          "duration_ms=" + std::to_string(elapsed.count());
      run.changed_inputs = current_changed;
      write_transform_run(db_, run);
      write_invalidation_values(db_, *transform, key_values);
      report.runs.push_back(run);
      dependency_outputs[transform->id] = dependency_token(run);
    }
    write_transform_meta(db_, "transform.generation", std::to_string(generation));
    write_transform_meta(db_, "transform.pipeline.state", "complete");
    write_transform_meta(db_, "transform.pipeline.stale_cause", "");
    report.complete = !report.runs.empty() &&
                      std::ranges::all_of(report.runs, qualified_ready);
    txn.commit();
  } catch (const std::exception &error) {
    txn.rollback();
    write_transform_meta(db_, "transform.test.failure", "");
    TransformRun failed;
    failed.transform_id = current_transform != nullptr
                              ? current_transform->id
                              : "pipeline";
    failed.version =
        current_transform != nullptr ? current_transform->version : 1;
    failed.generation = generation;
    failed.input_identity = current_transform == nullptr
                                ? ""
                                : input_identity(
                                      db_, *current_transform, current_keys,
                                      dependency_outputs);
    failed.status = TransformRunStatus::failed;
    failed.diagnostic = error.what();
    failed.changed_inputs = current_changed;
    if (current_transform != nullptr) {
      failed.applicability = current_transform->applicability;
      failed.completeness = current_transform->completeness;
    }
    const auto prior_runs = report.runs;
    std::unordered_map<std::string, TransformRun> final_runs;
    for (const auto &prior_run : prior_runs) {
      final_runs.emplace(prior_run.transform_id, prior_run);
    }
    if (failed.diagnostic.contains("failed output qualification") &&
        current_transform != nullptr &&
        current_transform->id == "edge-site-count-rollup" &&
        restore_edge_count_snapshot(db_)) {
      failed.diagnostic += "; restored published canonical rows";
    }
    // A failed generation invalidates every declared downstream consumer, but
    // never destroys its previously published rows. Their attempt metadata
    // names the failed dependency for status/explain clients.
    if (current_transform != nullptr) {
      // The transaction rolled back all writes from the failed generation,
      // including successful earlier transforms. Retain their published
      // rows, but record stale attempts so named readiness cannot report an
      // earlier fact as ready for the new input.
      for (const auto &prior_run : prior_runs) {
        if (prior_run.transform_id == current_transform->id ||
            (prior_run.status != TransformRunStatus::ran &&
             !std::ranges::contains(report.affected_transforms,
                                    prior_run.transform_id))) {
          continue;
        }
        const auto *candidate = registry.find(prior_run.transform_id);
        if (candidate == nullptr) {
          continue;
        }
        TransformRun stale;
        stale.transform_id = candidate->id;
        stale.version = candidate->version;
        stale.generation = generation;
        stale.status = TransformRunStatus::stale;
        stale.applicability = TransformApplicability::inapplicable;
        stale.completeness = TransformCompleteness::pending;
        stale.input_identity = prior_run.input_identity;
        stale.diagnostic = "generation rolled back by dependency " +
                           current_transform->id + ": " + error.what();
        stale.changed_inputs = prior_run.changed_inputs;
        stale.changed_inputs.emplace_back("generation-rollback");
        if (const auto previous = read_transform_run(db_, *candidate)) {
          stale.output_identity = previous->output_identity;
          stale.output_count = previous->output_count;
          stale.published_generation = previous->generation;
        }
        write_failed_attempt(db_, stale);
        write_attempt_invalidation_values(
            db_, *candidate, current_invalidation_values(db_, *candidate));
        final_runs[stale.transform_id] = stale;
      }
      std::unordered_set<std::string> stale_ids;
      stale_ids.insert(current_transform->id);
      for (const auto *candidate : ordered) {
        if (candidate->id == current_transform->id) {
          continue;
        }
        std::function<bool(const TransformDescriptor &)> depends_on_failure =
            [&](const TransformDescriptor &descriptor) {
              if (std::ranges::find(descriptor.dependencies,
                                    current_transform->id) !=
                  descriptor.dependencies.end()) {
                return true;
              }
              return std::ranges::any_of(
                  descriptor.dependencies, [&](const std::string &dependency) {
                    const auto *parent = registry.find(dependency);
                    return parent != nullptr && stale_ids.contains(parent->id) &&
                           parent->id != current_transform->id;
                  });
            };
        if (!depends_on_failure(*candidate)) {
          continue;
        }
        stale_ids.insert(candidate->id);
        TransformRun stale;
        stale.transform_id = candidate->id;
        stale.version = candidate->version;
        stale.generation = generation;
        stale.status = TransformRunStatus::stale;
        stale.applicability = TransformApplicability::inapplicable;
        stale.completeness = TransformCompleteness::pending;
        stale.diagnostic = "dependency " + current_transform->id +
                           " failed: " + error.what();
        stale.changed_inputs = {"dependency:" + current_transform->id};
        if (const auto previous = read_transform_run(db_, *candidate)) {
          stale.output_identity = previous->output_identity;
          stale.output_count = previous->output_count;
          stale.published_generation = previous->generation;
          stale.input_identity = previous->input_identity;
        }
        write_failed_attempt(db_, stale);
        final_runs[stale.transform_id] = stale;
      }
    }
    if (current_transform != nullptr) {
      if (const auto previous = read_transform_run(db_, *current_transform)) {
        failed.output_identity = previous->output_identity;
        failed.output_count = previous->output_count;
        failed.published_generation = previous->generation;
      }
    }
    if (current_transform != nullptr) {
      final_runs[current_transform->id] = failed;
    }
    write_failed_attempt(db_, failed);
    if (current_transform != nullptr) {
      write_attempt_invalidation_values(db_, *current_transform, current_keys);
    }
    write_transform_meta(db_, "transform.generation", std::to_string(generation));
    write_transform_meta(db_, "transform.pipeline.state", "failed");
    write_transform_meta(db_, "transform.pipeline.stale_cause", error.what());
    write_transform_meta(db_, "graph_resolved_at", "");
    report.runs.clear();
    for (const auto *descriptor : ordered) {
      if (!final_runs.contains(descriptor->id)) {
        if (const auto previous = read_transform_run(db_, *descriptor)) {
          final_runs.emplace(descriptor->id, *previous);
        } else {
          TransformRun unavailable;
          unavailable.transform_id = descriptor->id;
          unavailable.version = descriptor->version;
          unavailable.generation = generation;
          unavailable.status = TransformRunStatus::stale;
          unavailable.applicability = TransformApplicability::inapplicable;
          unavailable.completeness = TransformCompleteness::pending;
          unavailable.diagnostic = "not evaluated because the generation failed";
          final_runs.emplace(descriptor->id, std::move(unavailable));
        }
      }
      report.runs.push_back(final_runs.at(descriptor->id));
    }
    report.failed = true;
    last_transform_runs_ = report.runs;
    record_profile();
    return report;
  }

  report.still_stub_count = count_stubs(db_);
  report.complete =
      !report.runs.empty() && std::ranges::all_of(report.runs, qualified_ready);
  last_transform_runs_ = report.runs;
  record_profile();
  return report;
}

TransformReport SqliteStorageService::transform_status(
    const std::string &fact_set) {
  const TransformRegistry registry = make_transform_registry(&db_);
  TransformReport report;
  report.complete = true;
  bool saw_requested_fact_set = fact_set.empty();
  const auto pipeline_state =
      read_transform_meta(db_, "transform.pipeline.state");
  const bool pending = pipeline_state && *pipeline_state == "pending";
  for (const TransformDescriptor *descriptor : registry.execution_order()) {
    const bool owns_fact_set = fact_set.empty() ||
                               std::ranges::any_of(
                                   descriptor->fact_set_requirements,
                                   [&](const auto &requirement) {
                                     return requirement.name == fact_set;
                                   });
    if (!owns_fact_set) {
      continue;
    }
    saw_requested_fact_set = true;
    TransformRun fallback;
    fallback.transform_id = descriptor->id;
    fallback.version = descriptor->version;
    fallback.status = TransformRunStatus::stale;
    fallback.applicability = descriptor->applicability;
    fallback.completeness = descriptor->completeness;
    TransformRun run = read_transform_run(db_, *descriptor).value_or(fallback);
    if (pending) {
      run.status = TransformRunStatus::stale;
      run.completeness = TransformCompleteness::pending;
    }
    const auto attempt_status = read_transform_meta(
        db_, transform_meta_key(descriptor->id, "attempt.status"));
    if (attempt_status && (*attempt_status == "failed" ||
                           *attempt_status == "stale" ||
                           *attempt_status == "reused")) {
      if (*attempt_status == "failed") {
        run.status = TransformRunStatus::failed;
      } else if (*attempt_status == "stale") {
        run.status = TransformRunStatus::stale;
      } else {
        run.status = TransformRunStatus::reused;
      }
      if (*attempt_status != "reused") {
        run.completeness = TransformCompleteness::pending;
      }
      run.input_identity = read_transform_meta(
                               db_, transform_meta_key(descriptor->id,
                                                       "attempt.input"))
                               .value_or(run.input_identity);
      run.diagnostic = read_transform_meta(
                           db_, transform_meta_key(descriptor->id,
                                                   "attempt.diagnostic"))
                           .value_or("failed attempt");
      if (*attempt_status != "reused") {
        report.failed = true;
      }
    }
    if (pending) {
      run.status = TransformRunStatus::stale;
      run.completeness = TransformCompleteness::pending;
      run.diagnostic = read_transform_meta(
                           db_, transform_meta_key(descriptor->id,
                                                   "stale_cause"))
                           .value_or("transform pipeline pending");
    }
    for (const auto &requirement : descriptor->fact_set_requirements) {
      if (requirement.required && !qualified_ready(run)) {
        report.complete = false;
        report.missing_fact_sets.push_back(requirement.name);
      }
    }
    if (!qualified_ready(run)) {
      report.complete = false;
    }
    report.runs.push_back(std::move(run));
  }
  if (!saw_requested_fact_set || pending || report.failed) {
    report.complete = false;
  }
  return report;
}

TransformFactSetStatus SqliteStorageService::transform_fact_set_status(
    const std::string &fact_set) {
  const TransformRegistry registry = make_transform_registry(&db_);
  TransformFactSetStatus result;
  result.name = fact_set;
  for (const auto &descriptor : registry.descriptors()) {
    for (const auto &requirement : descriptor.fact_set_requirements) {
      if (requirement.name != fact_set) {
        continue;
      }
      result.known = true;
      result.schema_version = requirement.schema_version;
      result.catalog = requirement.catalog;
      const auto report = transform_status(fact_set);
      if (!report.runs.empty()) {
        result.status = report.runs.front().status;
        result.ready = report.complete;
        result.diagnostic = report.runs.front().diagnostic;
      } else {
        result.diagnostic = "fact set has no published producer run";
      }
      return result;
    }
  }
  result.diagnostic = "unknown fact set";
  return result;
}

std::string SqliteStorageService::transform_explain(
    const std::string &fact_set) {
  const TransformFactSetStatus fact_status =
      fact_set.empty() ? TransformFactSetStatus{}
                       : transform_fact_set_status(fact_set);
  if (!fact_set.empty() && !fact_status.known) {
    return "fact-set " + fact_set + ": unknown";
  }
  const TransformReport report = transform_status(fact_set);
  std::string explanation;
  for (const auto &run : report.runs) {
    if (!explanation.empty()) {
      explanation += '\n';
    }
    explanation += run.transform_id + ": " +
                   transform_run_status_name(run.status) + ", " +
                   transform_completeness_name(run.completeness);
    if (!run.diagnostic.empty()) {
      explanation += ", " + run.diagnostic;
    }
    if (const auto cause = read_transform_meta(
            db_, transform_meta_key(run.transform_id, "stale_cause"))) {
      explanation += ", cause=" + *cause;
    }
    if (const auto changed = read_transform_meta(
            db_, transform_meta_key(run.transform_id, "changed_inputs"));
        changed && !changed->empty()) {
      explanation += ", changed=" + *changed;
    }
  }
  if (!fact_set.empty() && explanation.empty()) {
    return "fact-set " + fact_set + ": unknown";
  }
  if (!report.missing_fact_sets.empty()) {
    explanation += "\nmissing-fact-sets: ";
    for (std::size_t i = 0; i < report.missing_fact_sets.size(); ++i) {
      if (i != 0) {
        explanation += ",";
      }
      explanation += report.missing_fact_sets[i];
    }
  }
  return explanation;
}

void SqliteStorageService::mark_transform_pipeline_pending(
    const std::string &reason) {
  write_transform_meta(db_, "transform.pipeline.state", "pending");
  write_transform_meta(db_, "transform.pipeline.stale_cause", reason);
  write_transform_meta(db_, "graph_resolved_at", "");
  const auto registry = make_transform_registry(&db_);
  for (const TransformDescriptor *descriptor :
       registry.execution_order()) {
    write_transform_meta(db_, transform_meta_key(descriptor->id, "status"),
                         "stale");
    write_transform_meta(db_, transform_meta_key(descriptor->id, "stale_cause"),
                         reason);
  }
}

void SqliteStorageService::inject_transform_failure_for_testing(
    std::string transform_id) {
  transform_failure_for_testing_ = std::move(transform_id);
  write_transform_meta(db_, "transform.test.failure",
                       *transform_failure_for_testing_);
}

void SqliteStorageService::inject_transform_nondeterminism_for_testing(
    std::string transform_id) {
  transform_nondeterminism_for_testing_ = std::move(transform_id);
}

void SqliteStorageService::set_transform_invalidation_for_testing(
    const std::string &key, const std::string &value) {
  const auto separator = key.find(':');
  if (separator == std::string::npos) {
    write_transform_meta(db_, "transform.input." + key, value);
  } else {
    write_transform_meta(db_, "transform.input." + key.substr(0, separator) +
                                "." + key.substr(separator + 1),
                         value);
  }
}

void SqliteStorageService::set_transform_implementation_provider_for_testing(
    const std::string &transform_id, int version) {
  auto registry = make_transform_registry();
  registry.set_implementation_version(transform_id, version);
  write_transform_meta(db_, "transform.implementation." + transform_id +
                                ".version",
                       std::to_string(version));
}

void SqliteStorageService::set_transform_budget_for_testing(
    const std::string &transform_id, std::int64_t max_rows,
    std::int64_t max_milliseconds) {
  if (max_rows < 0 || max_milliseconds < 0) {
    throw StorageError("transform budget must be non-negative");
  }
  write_transform_meta(db_, "transform.budget." + transform_id + ".max_rows",
                       std::to_string(max_rows));
  write_transform_meta(
      db_, "transform.budget." + transform_id + ".max_milliseconds",
      std::to_string(max_milliseconds));
}

int SqliteStorageService::resolve_pass() {
  const TransformReport report = run_transform_pipeline();
  if (report.failed) {
    const auto failed = std::ranges::find_if(
        report.runs, [](const TransformRun &run) {
          return run.status == TransformRunStatus::failed;
        });
    throw StorageError("resolve failed: " +
                       (failed == report.runs.end()
                            ? std::string("transform pipeline")
                            : failed->diagnostic));
  }
  return report.still_stub_count;
}

// -- fuzzy matching
// -----------------------------------------------------------------

} // namespace cidx
