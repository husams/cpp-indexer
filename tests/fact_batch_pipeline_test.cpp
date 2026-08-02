#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "application/fact_batch_replay.hpp"
#include "ast/fact_extraction.hpp"
#include "storage/storage.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

using namespace cidx;

namespace {

auto partition(std::string file, std::string source) -> ast::FactPartitionKey {
  return {.file = {.component_path = "/repo",
                   .directory_path = "src",
                   .file_name = std::move(file)},
          .configuration = {.semantic_universe = "workspace",
                            .translation_unit = "src/main.cpp",
                            .normalized_configuration = "debug",
                            .identity_source = std::move(source)}};
}

auto descriptor() -> ast::ExtractionPassDescriptor {
  return {.id = "facts",
          .version = 1,
          .required_capabilities = {},
          .consumed_fact_families = {"route"},
          .produced_fact_families = {"symbols", "relations"},
          .catalog_versions = {1},
          .dependencies = {},
          .scope = ast::PassScope::translation_unit,
          .traversal = ast::TraversalMode::declaration,
          .completeness = ast::FactCompleteness::complete,
          .trust = ast::FactTrust::trusted,
          .budget = {.max_visited_constructs = 0,
                     .max_emitted_facts = 10,
                     .max_diagnostics = 0,
                     .max_whole_tu_traversals = 0,
                     .declared = true}};
}

class SnapshotDatabase {
public:
  SnapshotDatabase()
      : path_(
            std::filesystem::temp_directory_path() /
            ("cidx-s072-extraction-" +
             std::to_string(
                 std::chrono::steady_clock::now().time_since_epoch().count()) +
             ".db")) {
    Storage storage(path_.string());
    static_cast<void>(
        storage.add_component("snapshot", "/tmp/cidx-s072", "repo"));
  }

  ~SnapshotDatabase() {
    std::error_code ignored;
    static_cast<void>(std::filesystem::remove(path_, ignored));
  }

  SnapshotDatabase(const SnapshotDatabase &) = delete;
  SnapshotDatabase &operator=(const SnapshotDatabase &) = delete;

  [[nodiscard]] auto bytes() const -> std::vector<char> {
    std::ifstream stream(path_, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream),
            std::istreambuf_iterator<char>()};
  }

private:
  std::filesystem::path path_;
};

void emit_one_symbol(ast::PassExecutionContext &context) {
  if (context.session == nullptr) {
    throw std::logic_error("serial extraction session is missing");
  }
  ast::MintRequest symbol;
  symbol.usr = "snapshot-symbol";
  symbol.spelling = "snapshot_symbol";
  symbol.qual_name = "snapshot_symbol";
  symbol.display_name = "snapshot_symbol";
  symbol.kind_name = "function";
  context.session->declaration_ports->mint_symbol(symbol);
}

class RecordingReplayPort final : public application::FactBatchReplayPort {
public:
  explicit RecordingReplayPort(bool fail_commit = false)
      : fail_commit_(fail_commit) {}

  void begin_translation_unit() override { working_ = committed_; }

  auto apply_file(const ast::FactPartitionKey & /*key*/,
                  std::string_view /*natural_key*/) -> std::int64_t override {
    return next_id_++;
  }

  auto apply_symbol(const ast::FactPartitionKey &key,
                    const ast::SymbolRecord &record,
                    std::string_view /*natural_key*/) -> std::int64_t override {
    working_.push_back("symbol:" + key.file.portable_path() + ':' + record.usr);
    return next_id_++;
  }

  auto apply_relation(const ast::FactPartitionKey &key,
                      const ast::EdgeRecord &record) -> std::int64_t override {
    working_.push_back("relation:" + key.file.portable_path() + ':' +
                       std::to_string(record.src_id) + ':' +
                       std::to_string(record.dst_id) + ':' +
                       std::to_string(record.kind));
    return next_id_++;
  }

  auto apply_definition(const ast::FactPartitionKey &key,
                        const ast::DefinitionFactRecord &record)
      -> std::int64_t override {
    working_.push_back("definition:" + key.file.portable_path() + ':' +
                       std::to_string(record.symbol_id));
    return next_id_++;
  }

  void
  apply_other(const ast::FactPartitionKey &key, ast::FactFamily family,
              std::size_t /*record_index*/,
              const ast::FactRecords & /*records*/,
              const application::TransientFactApplyMap & /*ids*/) override {
    working_.push_back("family:" + key.file.portable_path() + ':' +
                       std::to_string(static_cast<unsigned>(family)));
  }

  void commit_translation_unit() override {
    if (fail_commit_) {
      throw std::runtime_error("commit failed");
    }
    committed_ = working_;
  }
  void rollback_translation_unit() noexcept override { working_.clear(); }

  [[nodiscard]] auto committed() const -> const std::vector<std::string> & {
    return committed_;
  }

private:
  std::int64_t next_id_ = 1;
  bool fail_commit_ = false;
  std::vector<std::string> committed_;
  std::vector<std::string> working_;
};

auto replay_fixture(bool reverse) -> ast::FactBatch {
  const auto first = partition("a.cpp", "/repo/src/a.cpp");
  const auto second = partition("b.cpp", "/repo/src/b.cpp");
  ast::FactBatchRecorder recorder("replay-fixture");
  const auto emit = [&recorder](const ast::FactPartitionKey &key,
                                std::string usr) {
    recorder.set_partition(key);
    ast::SymbolRecord symbol;
    symbol.file = key.file.portable_path();
    symbol.usr = std::move(usr);
    symbol.spelling = "symbol";
    symbol.kind = 8;
    symbol.kind_name = "function";
    symbol.identity_source = key.configuration.identity_source;
    recorder.emit(symbol);
  };
  if (reverse) {
    emit(second, "usr-b");
    emit(first, "usr-a");
  } else {
    emit(first, "usr-a");
    emit(second, "usr-b");
  }
  recorder.set_partition(first);
  const auto source = recorder.lookup_symbol_id("usr-a", "/repo/src/a.cpp");
  const auto destination =
      recorder.lookup_symbol_id("usr-b", "/repo/src/b.cpp");
  if (!source || !destination) {
    throw std::logic_error("replay fixture symbols were not indexed");
  }
  recorder.add_edge({.src_id = source.value(),
                     .dst_id = destination.value(),
                     .kind = 1,
                     .count = 1,
                     .base_access = std::nullopt,
                     .is_virtual = std::nullopt});
  return recorder.canonical_batch();
}

} // namespace

TEST_CASE("serial extraction publishes one immutable batch") {
  ast::ExtractionPassRegistry registry;
  registry.register_pass(descriptor(), [](ast::PassExecutionContext &context) {
    if (context.session == nullptr) {
      throw std::logic_error("serial extraction session is missing");
    }
    auto &ports = *context.session->declaration_ports;
    ast::MintRequest main_symbol;
    main_symbol.usr = "usr-main";
    main_symbol.spelling = "main";
    main_symbol.qual_name = "main";
    main_symbol.display_name = "main";
    main_symbol.kind_name = "function";
    main_symbol.decl_path = "/repo/src/main.cpp";
    main_symbol.identity_source = "/repo/src/main.cpp";
    const std::int64_t first = ports.mint_symbol(main_symbol);
    ast::MintRequest dependency_symbol;
    dependency_symbol.usr = "usr-dep";
    dependency_symbol.spelling = "dep";
    dependency_symbol.qual_name = "dep";
    dependency_symbol.display_name = "dep";
    dependency_symbol.kind_name = "function";
    dependency_symbol.decl_path = "/repo/src/dep.cpp";
    dependency_symbol.identity_source = "/repo/src/dep.cpp";
    const std::int64_t second = ports.mint_symbol(dependency_symbol);
    ports.add_edge({.src_id = first,
                    .dst_id = second,
                    .kind = 1,
                    .count = 1,
                    .base_access = std::nullopt,
                    .is_virtual = std::nullopt});
  });
  ast::IndexingPlan plan;
  plan.add("facts");
  const ast::SerialFactRoute route{
      .partitions = {{.partition = partition("main.cpp", "/repo/src/main.cpp"),
                      .transient_file_handle = 1},
                     {.partition = partition("dep.cpp", "/repo/src/dep.cpp"),
                      .transient_file_handle = 2}}};

  const auto result = ast::extract_serial_fact_batch({}, registry, plan, route);
  REQUIRE(result.ok());
  if (!result.batch) {
    FAIL("successful extraction did not publish a batch");
    return;
  }
  const ast::FactBatch &batch = result.batch.value();
  CHECK(batch.partitions().size() == 2);
  CHECK(batch.records().symbols.size() == 2);
  CHECK(batch.records().relations.size() == 1);
  CHECK(result.report.passes.size() == 1);
}

TEST_CASE("prepublication failure exposes no partial batch") {
  ast::ExtractionPassRegistry registry;
  registry.register_pass(descriptor(), [](ast::PassExecutionContext &context) {
    if (context.session == nullptr) {
      throw std::logic_error("serial extraction session is missing");
    }
    ast::MintRequest main_symbol;
    main_symbol.usr = "usr-main";
    main_symbol.spelling = "main";
    main_symbol.qual_name = "main";
    main_symbol.display_name = "main";
    main_symbol.kind_name = "function";
    main_symbol.identity_source = "/repo/src/main.cpp";
    context.session->declaration_ports->mint_symbol(main_symbol);
  });
  ast::IndexingPlan plan;
  plan.add("facts");
  const ast::SerialFactRoute route{
      .partitions = {{.partition = partition("main.cpp", "/repo/src/main.cpp"),
                      .transient_file_handle = std::nullopt}}};
  const auto result = ast::extract_serial_fact_batch(
      {}, registry, plan, route, [](const ast::FactBatchRecorder &) {
        throw std::runtime_error("before publication");
      });

  CHECK(!result.ok());
  CHECK(!result.batch);
  if (!result.failure) {
    FAIL("failed extraction did not publish a typed failure");
    return;
  }
  CHECK(result.failure.value().kind ==
        ast::FactExtractionFailureKind::prepublication_failed);
}

TEST_CASE("pass and budget failures expose no partial batch") {
  ast::IndexingPlan plan;
  plan.add("facts");
  const ast::SerialFactRoute route{
      .partitions = {{.partition = partition("main.cpp", "/repo/src/main.cpp"),
                      .transient_file_handle = std::nullopt}}};

  ast::ExtractionPassRegistry failing_registry;
  failing_registry.register_pass(
      descriptor(), [](ast::PassExecutionContext & /*context*/) {
        throw std::runtime_error("parse or pass failed");
      });
  const auto failed =
      ast::extract_serial_fact_batch({}, failing_registry, plan, route);
  CHECK(!failed.ok());
  CHECK(!failed.batch);
  if (!failed.failure) {
    FAIL("pass failure did not publish a typed failure");
    return;
  }
  CHECK(failed.failure.value().kind ==
        ast::FactExtractionFailureKind::pass_failed);

  ast::ExtractionPassDescriptor bounded = descriptor();
  bounded.budget.max_emitted_facts = 1;
  ast::ExtractionPassRegistry bounded_registry;
  bounded_registry.register_pass(
      bounded, [](ast::PassExecutionContext &context) {
        if (context.session == nullptr) {
          throw std::logic_error("serial extraction session is missing");
        }
        ast::MintRequest symbol;
        symbol.usr = "budgeted";
        symbol.spelling = "budgeted";
        symbol.qual_name = "budgeted";
        symbol.display_name = "budgeted";
        symbol.kind_name = "function";
        context.session->declaration_ports->mint_symbol(symbol);
        symbol.usr = "over-budget";
        context.session->declaration_ports->mint_symbol(symbol);
      });
  const auto exceeded =
      ast::extract_serial_fact_batch({}, bounded_registry, plan, route);
  CHECK(!exceeded.ok());
  CHECK(!exceeded.batch);
  if (!exceeded.failure) {
    FAIL("budget failure did not publish a typed failure");
    return;
  }
  CHECK(exceeded.failure.value().kind ==
        ast::FactExtractionFailureKind::budget_exceeded);
}

TEST_CASE("unpublished extraction preserves a byte-identical database") {
  SnapshotDatabase database;
  const std::vector<char> before = database.bytes();
  const ast::SerialFactRoute route{
      .partitions = {{.partition = partition("main.cpp", "/repo/src/main.cpp"),
                      .transient_file_handle = std::nullopt}}};
  ast::IndexingPlan plan;
  plan.add("facts");

  ast::ExtractionPassRegistry success_registry;
  success_registry.register_pass(descriptor(), emit_one_symbol);
  CHECK(ast::extract_serial_fact_batch({}, success_registry, plan, route).ok());
  CHECK(database.bytes() == before);

  ast::ExtractionPassRegistry failure_registry;
  failure_registry.register_pass(
      descriptor(), [](ast::PassExecutionContext & /*context*/) {
        throw std::runtime_error("injected parse failure");
      });
  CHECK(
      !ast::extract_serial_fact_batch({}, failure_registry, plan, route).ok());
  CHECK(database.bytes() == before);

  ast::ExtractionPassDescriptor bounded = descriptor();
  bounded.budget.max_emitted_facts = 1;
  ast::ExtractionPassRegistry budget_registry;
  budget_registry.register_pass(bounded,
                                [](ast::PassExecutionContext &context) {
                                  emit_one_symbol(context);
                                  emit_one_symbol(context);
                                });
  CHECK(!ast::extract_serial_fact_batch({}, budget_registry, plan, route).ok());
  CHECK(database.bytes() == before);

  const auto prepublication = ast::extract_serial_fact_batch(
      {}, success_registry, plan, route,
      [](const ast::FactBatchRecorder & /*recorder*/) {
        throw std::runtime_error("injected prepublication failure");
      });
  CHECK(!prepublication.ok());
  CHECK(database.bytes() == before);
}

TEST_CASE("transactional replay is deterministic and rolls back failures") {
  RecordingReplayPort forward_port;
  const auto forward =
      application::replay_fact_batch(replay_fixture(false), forward_port);
  INFO(forward.error.value_or(""));
  REQUIRE(forward.committed);

  RecordingReplayPort reverse_port;
  const auto reverse =
      application::replay_fact_batch(replay_fixture(true), reverse_port);
  REQUIRE(reverse.committed);
  CHECK(forward_port.committed() == reverse_port.committed());
  REQUIRE(forward_port.committed().size() == 3);
  CHECK(forward_port.committed()[0].starts_with("symbol:/repo/src/a.cpp"));
  CHECK(forward_port.committed()[1].starts_with("symbol:/repo/src/b.cpp"));

  const std::vector<std::string> before = forward_port.committed();
  const auto failed = application::replay_fact_batch(
      replay_fixture(false), forward_port,
      application::FactReplayFailurePoint::after_symbols);
  CHECK(!failed.committed);
  CHECK(failed.error.has_value());
  CHECK(forward_port.committed() == before);

  for (const auto failure :
       {application::FactReplayFailurePoint::before_apply,
        application::FactReplayFailurePoint::before_commit}) {
    const auto injected = application::replay_fact_batch(replay_fixture(false),
                                                         forward_port, failure);
    CHECK(!injected.committed);
    CHECK(injected.error.has_value());
    CHECK(forward_port.committed() == before);
  }

  RecordingReplayPort commit_failure_port(true);
  const auto commit_failure = application::replay_fact_batch(
      replay_fixture(false), commit_failure_port);
  CHECK(!commit_failure.committed);
  CHECK(commit_failure.error.has_value());
  CHECK(commit_failure_port.committed().empty());
}
