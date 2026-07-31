#define DOCTEST_CONFIG_IMPLEMENT
#include "doctest/doctest.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "application/agent_tools.hpp"
#include "application/registry.hpp"
#include "application/services.hpp"
#include "cli/application_adapter.hpp"
#include "query/exec.hpp"
#include "storage/storage.hpp"
#include "util/files.hpp"

namespace {

cidx::protocol::ResultEnvelope complete(std::string operation) {
  cidx::protocol::ResultEnvelope result;
  result.operation = std::move(operation);
  result.identity.workspace = "test";
  result.identity.index = "test";
  result.identity.fact_sets = {"application"};
  result.identity.freshness = "current";
  result.producer.backend = "test";
  return result;
}

struct RecordingServices final : cidx::application::ApplicationServices {
  cidx::protocol::ResultEnvelope
  index(const cidx::application::IndexRequest &,
        cidx::application::ApplicationContext &) const override {
    calls.push_back("index");
    return complete("index");
  }
  cidx::protocol::ResultEnvelope
  query(const cidx::application::QueryRequest &,
        cidx::application::ApplicationContext &) const override {
    calls.push_back("query");
    return complete("query");
  }
  cidx::protocol::ResultEnvelope
  analysis(const cidx::application::AnalysisRequest &,
           cidx::application::ApplicationContext &) const override {
    calls.push_back("analysis");
    return complete("analysis");
  }
  cidx::protocol::ResultEnvelope
  workspace(const cidx::application::WorkspaceRequest &,
            cidx::application::ApplicationContext &) const override {
    calls.push_back("workspace");
    return complete("workspace");
  }
  cidx::protocol::ResultEnvelope
  ast(const cidx::application::AstInspectionRequest &,
      cidx::application::ApplicationContext &) const override {
    calls.push_back("ast");
    return complete("ast");
  }
  cidx::protocol::ResultEnvelope
  diff(const cidx::application::DiffRequest &,
       cidx::application::ApplicationContext &) const override {
    calls.push_back("diff");
    return complete("diff");
  }
  cidx::protocol::ResultEnvelope
  include(const cidx::application::IncludeRequest &,
          cidx::application::ApplicationContext &) const override {
    calls.push_back("include");
    return complete("include");
  }
  cidx::protocol::ResultEnvelope
  refactor(const cidx::application::RefactoringRequest &,
           cidx::application::ApplicationContext &) const override {
    calls.push_back("refactor");
    return complete("refactor");
  }
  cidx::protocol::ResultEnvelope
  proof(const cidx::application::ProofRequest &,
        cidx::application::ApplicationContext &) const override {
    calls.push_back("proof");
    return complete("proof");
  }

  mutable std::vector<std::string> calls;
};

struct RecordingProgress final : cidx::application::ProgressSink {
  void publish(const cidx::protocol::ProgressEvent &event) override {
    events.push_back(event);
  }

  std::vector<cidx::protocol::ProgressEvent> events;
};

struct CancellingProgress final : cidx::application::ProgressSink {
  explicit CancellingProgress(cidx::application::CancellationToken &token)
      : token(token) {}

  void publish(const cidx::protocol::ProgressEvent &) override {
    token.cancel();
  }

  cidx::application::CancellationToken &token;
};

std::size_t count_occurrences(std::string_view value, std::string_view needle) {
  std::size_t count = 0;
  std::size_t offset = 0;
  while ((offset = value.find(needle, offset)) != std::string_view::npos) {
    ++count;
    offset += needle.size();
  }
  return count;
}

void check_canonical_truncation(const cidx::protocol::ResultEnvelope &result) {
  const std::string serialized =
      cidx::json_out::dumps_indent2(result.to_json());
  CHECK(count_occurrences(serialized, "\"truncated\": true") == 1);
  CHECK(serialized.find("\"completeness\"") != std::string::npos);
  CHECK(serialized.find("\"budget\": 1") != std::string::npos);
}

} // namespace

TEST_CASE("application registry is complete and validated") {
  CHECK(cidx::application::registry_is_valid());
  CHECK(cidx::application::command_registry().size() == 31);
  CHECK(cidx::application::metadata(cidx::application::Operation::query) !=
        nullptr);
  CHECK(cidx::application::metadata(cidx::application::Operation::query)
            ->mutability == cidx::application::Mutability::read_only);
}

TEST_CASE(
    "production parser returns typed requests and isolates compatibility") {
  const auto query = cidx::cli::parse_request({"query", "nodes()", "--json"});
  REQUIRE(query.has_value());
  REQUIRE(std::holds_alternative<cidx::application::QueryRequest>(*query));
  const auto &typed = std::get<cidx::application::QueryRequest>(*query);
  CHECK(typed.expression == "nodes()");
  CHECK(typed.output == cidx::application::QueryOutput::json);

  const auto bare_index = cidx::cli::parse_application_request({"index"});
  REQUIRE(std::holds_alternative<cidx::application::CommandRequest>(
      bare_index.value));
  const auto &bare_request =
      std::get<cidx::application::CommandRequest>(bare_index.value);
  REQUIRE(
      std::holds_alternative<cidx::application::IndexRequest>(bare_request));
  CHECK(std::get<cidx::application::IndexRequest>(bare_request).action ==
        cidx::application::IndexAction::update);

  const auto profiled_index = cidx::cli::parse_application_request(
      {"index", "fixture.cpp", "--profile-json", "/tmp/cidx-profile.json",
       "--profile-sqlite-config", "/tmp/cidx-sqlite.json"});
  const auto &profiled_command =
      std::get<cidx::application::CommandRequest>(profiled_index.value);
  const auto &profiled_request =
      std::get<cidx::application::IndexRequest>(profiled_command);
  CHECK(profiled_request.profile_json ==
        std::optional<std::string>{"/tmp/cidx-profile.json"});
  CHECK(profiled_request.profile_sqlite_configuration ==
        std::optional<std::string>{"/tmp/cidx-sqlite.json"});
  const auto throws_usage_error =
      [](const std::vector<std::string> &arguments) {
        try {
          static_cast<void>(cidx::cli::parse_application_request(arguments));
          return false;
        } catch (const cidx::UsageError &) {
          return true;
        }
      };
  CHECK(throws_usage_error(
      {"index", "--profile-sqlite-config", "/tmp/sqlite.json"}));
  CHECK(throws_usage_error({"index", "--profile-json"}));

  const auto compatibility =
      cidx::cli::parse_application_request({"search", "nodes"});
  REQUIRE(std::holds_alternative<cidx::cli::CompatibilityRequest>(
      compatibility.value));
  CHECK(std::get<cidx::cli::CompatibilityRequest>(compatibility.value).argv ==
        std::vector<std::string>{"search", "nodes"});

  const auto ast = cidx::cli::parse_application_request(
      {"ast", "dump", "fixture.cpp", "--json"});
  REQUIRE(std::holds_alternative<cidx::application::CommandRequest>(ast.value));
  const auto &ast_request =
      std::get<cidx::application::CommandRequest>(ast.value);
  REQUIRE(std::holds_alternative<cidx::application::AstInspectionRequest>(
      ast_request));
  CHECK(std::get<cidx::application::AstInspectionRequest>(ast_request).json);

  const auto diff = cidx::cli::parse_application_request(
      {"diff", "source", "left", "right", "--left-source-revision", "a",
       "--right-source-revision", "b"});
  REQUIRE(
      std::holds_alternative<cidx::application::CommandRequest>(diff.value));
  const auto &diff_request =
      std::get<cidx::application::CommandRequest>(diff.value);
  REQUIRE(std::holds_alternative<cidx::application::DiffRequest>(diff_request));
  CHECK(std::get<cidx::application::DiffRequest>(diff_request)
            .left_source_revision == std::optional<std::string>{"a"});

  const auto include = cidx::cli::parse_application_request(
      {"include", "graph", "--reverse", "--transitive"});
  REQUIRE(
      std::holds_alternative<cidx::cli::CompatibilityRequest>(include.value));

  for (const std::vector<std::string> &command :
       {std::vector<std::string>{"workspace", "show"},
        std::vector<std::string>{"refactor", "check"},
        std::vector<std::string>{"proof", "status"}}) {
    const auto unsupported = cidx::cli::parse_application_request(command);
    CHECK(std::holds_alternative<cidx::cli::CompatibilityRequest>(
        unsupported.value));
  }
}

TEST_CASE("typed services dispatch every HSE-68 request family") {
  RecordingServices services;
  cidx::application::ApplicationService service(services);
  cidx::application::ApplicationContext context;
  const std::vector<cidx::application::CommandRequest> requests = {
      cidx::application::IndexRequest{},
      cidx::application::QueryRequest{.expression = "nodes()"},
      cidx::application::AnalysisRequest{},
      cidx::application::WorkspaceRequest{},
      cidx::application::AstInspectionRequest{.source = "fixture.cpp"},
      cidx::application::DiffRequest{.left = "left.cpp", .right = "right.cpp"},
      cidx::application::IncludeRequest{},
      cidx::application::RefactoringRequest{},
      cidx::application::ProofRequest{},
  };
  for (const auto &request : requests) {
    CHECK(service.execute(request, context).status ==
          cidx::protocol::Status::Complete);
  }
  CHECK(services.calls == std::vector<std::string>{
                              "index", "query", "analysis", "workspace", "ast",
                              "diff", "include", "refactor", "proof"});
}

TEST_CASE("unknown actions and scopes fail explicitly") {
  RecordingServices services;
  cidx::application::ApplicationService service(services);
  cidx::application::ApplicationContext context;

  const auto invalid_index = service.execute(
      cidx::application::IndexRequest{
          .action = static_cast<cidx::application::IndexAction>(255)},
      context);
  REQUIRE(invalid_index.status == cidx::protocol::Status::Error);
  CHECK(invalid_index.diagnostics.front().code == "invalid_input");

  const auto invalid_scope = service.execute(
      cidx::application::DiffRequest{
          .scope = static_cast<cidx::application::DiffScope>(255)},
      context);
  REQUIRE(invalid_scope.status == cidx::protocol::Status::Error);
  CHECK(invalid_scope.diagnostics.front().code == "invalid_input");
  CHECK(services.calls.empty());
}

TEST_CASE("capability policy rejects writes before the service") {
  RecordingServices services;
  cidx::application::ApplicationService service(services);
  cidx::application::ApplicationContext context(
      {.access = cidx::application::AccessMode::read_only,
       .capabilities = cidx::application::capability_bit(
           cidx::application::Capability::index_read)});

  const auto result = service.execute(
      cidx::application::IncludeRequest{
          .action = cidx::application::IncludeAction::apply},
      context);
  REQUIRE(result.status == cidx::protocol::Status::Refuted);
  CHECK(result.diagnostics.front().code == "policy_refuted");
  CHECK(services.calls.empty());
}

TEST_CASE("cancellation is enforced at the application boundary") {
  RecordingServices services;
  cidx::application::ApplicationService service(services);
  cidx::application::ApplicationContext context;
  context.cancellation().cancel();
  const auto result = service.execute(
      cidx::application::QueryRequest{.expression = "nodes()"}, context);
  CHECK(result.status == cidx::protocol::Status::Error);
  CHECK(result.diagnostics.front().code == "timeout");
}

TEST_CASE("metadata derives capabilities and policy protects artifacts") {
  const auto *analysis = cidx::application::metadata(
      cidx::application::Operation::analysis_execute);
  REQUIRE(analysis != nullptr);
  CHECK(analysis->required_capabilities ==
        cidx::application::capability_bit(
            cidx::application::Capability::analysis));

  RecordingServices services;
  cidx::application::ApplicationService service(services);
  cidx::application::ApplicationContext context(
      {.access = cidx::application::AccessMode::read_only,
       .capabilities = cidx::application::capability_bit(
           cidx::application::Capability::include_read)});
  const auto result = service.execute(
      cidx::application::IncludeRequest{
          .action = cidx::application::IncludeAction::plan,
          .output = "/tmp/forbidden-plan"},
      context);
  CHECK(result.status == cidx::protocol::Status::Refuted);
  CHECK(services.calls.empty());
}

TEST_CASE("direct source and index diff compares effective identities") {
  cidx::Storage db(":memory:");
  cidx::application::StorageApplicationOperations operations(db);
  cidx::application::ApplicationOperationPorts operation_ports{.diff =
                                                                   &operations};
  cidx::application::ApplicationContext context(
      cidx::application::ApplicationPolicy{
          .capabilities = cidx::application::capability_bit(
              cidx::application::Capability::diff)},
      operation_ports);
  const cidx::application::DefaultApplicationServices services;
  const cidx::application::ApplicationService service(services);
  const auto source = service.execute(
      cidx::application::DiffRequest{.scope =
                                         cidx::application::DiffScope::source,
                                     .left = "rev-a",
                                     .right = "rev-b"},
      context);
  CHECK(cidx::json_out::dumps_indent2(source.result).find("false") !=
        std::string::npos);
  const auto index = service.execute(
      cidx::application::DiffRequest{.scope =
                                         cidx::application::DiffScope::index,
                                     .left = "index-a",
                                     .right = "index-b"},
      context);
  CHECK(cidx::json_out::dumps_indent2(index.result).find("false") !=
        std::string::npos);
}

TEST_CASE("configuration diff consumes real and overridden configurations") {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "cidx-application-config-diff";
  std::error_code ec;
  std::filesystem::remove_all(root, ec);
  std::filesystem::create_directories(root, ec);
  REQUIRE_FALSE(ec);
  const std::filesystem::path left = root / "left.cpp";
  const std::filesystem::path right = root / "right.cpp";
  std::ofstream(left) << "int left() { return 1; }\n";
  std::ofstream(right) << "int right() { return 2; }\n";
  const std::string index = (root / "config.sqlite").string();
  cidx::Storage db(index);
  db.add_component("app", root.string());
  for (const auto &source : {left, right}) {
    db.add_file_path(source.string(), std::nullopt, std::nullopt,
                     std::vector<std::string>{"-std=c++17"},
                     std::string("c++"));
  }
  cidx::application::StorageApplicationOperations operations(db, index);
  cidx::application::ApplicationOperationPorts operation_ports{.diff =
                                                                   &operations};
  cidx::application::ApplicationContext context(
      cidx::application::ApplicationPolicy{
          .capabilities = cidx::application::capability_bit(
              cidx::application::Capability::diff)},
      operation_ports);
  const cidx::application::DefaultApplicationServices services;
  const cidx::application::ApplicationService service(services);

  const auto equal = service.execute(
      cidx::application::DiffRequest{
          .scope = cidx::application::DiffScope::configuration,
          .left = left.string(),
          .right = right.string(),
          .left_index = index},
      context);
  CHECK(
      cidx::json_out::dumps_indent2(equal.result).find("\"identical\": true") !=
      std::string::npos);

  const auto unequal = service.execute(
      cidx::application::DiffRequest{
          .scope = cidx::application::DiffScope::configuration,
          .left = left.string(),
          .right = right.string(),
          .left_index = index,
          .left_configuration = std::string("-std=c++17"),
          .right_configuration = std::string("-std=c++20")},
      context);
  CHECK(cidx::json_out::dumps_indent2(unequal.result)
            .find("\"identical\": false") != std::string::npos);

  const auto mixed = service.execute(
      cidx::application::DiffRequest{
          .scope = cidx::application::DiffScope::configuration,
          .left = left.string(),
          .right = right.string(),
          .left_index = index,
          .left_configuration = std::string("-DLEFT=1")},
      context);
  CHECK(cidx::json_out::dumps_indent2(mixed.result)
            .find("\"identical\": false") != std::string::npos);

  std::filesystem::remove_all(root, ec);
}

TEST_CASE("direct AST service parses a registered translation unit") {
  const std::filesystem::path source =
      std::filesystem::temp_directory_path() / "cidx-application-ast.cpp";
  {
    std::ofstream output(source);
    REQUIRE(output.good());
    output << "int answer() { return 42; }\n";
  }
  cidx::Storage db(":memory:");
  db.add_component("app", source.parent_path().string());
  db.add_file_path(source.string(), std::nullopt, std::nullopt,
                   std::vector<std::string>{"-std=c++17"}, std::string("c++"));
  cidx::application::StorageApplicationOperations operations(db);
  cidx::application::ApplicationOperationPorts operation_ports{.ast =
                                                                   &operations};
  cidx::application::ApplicationContext context(
      cidx::application::ApplicationPolicy{
          .capabilities = cidx::application::capability_bit(
              cidx::application::Capability::ast)},
      operation_ports);
  const cidx::application::DefaultApplicationServices services;
  const cidx::application::ApplicationService service(services);
  const auto result = service.execute(
      cidx::application::AstInspectionRequest{
          .action = cidx::application::AstInspectionAction::dump,
          .source = source.string()},
      context);
  CHECK(result.status == cidx::protocol::Status::Complete);
  CHECK(cidx::json_out::dumps_indent2(result.result).find("cursor_nodes") !=
        std::string::npos);
  for (const auto action :
       {cidx::application::AstInspectionAction::locals,
        cidx::application::AstInspectionAction::conditions}) {
    const auto unsupported = service.execute(
        cidx::application::AstInspectionRequest{.action = action,
                                                .source = source.string()},
        context);
    CHECK(unsupported.status == cidx::protocol::Status::Error);
    CHECK(unsupported.diagnostics.front().code == "invalid_input");
  }
  std::error_code ec;
  std::filesystem::remove(source, ec);
}

TEST_CASE("index service enforces work and diagnostic budgets stably") {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "cidx-application-budget";
  std::error_code ec;
  std::filesystem::remove_all(root, ec);
  std::filesystem::create_directories(root, ec);
  REQUIRE_FALSE(ec);
  const std::filesystem::path first = root / "first.cpp";
  const std::filesystem::path second = root / "second.cpp";
  const std::filesystem::path third = root / "third.cpp";
  {
    std::ofstream(first) << "int first() { return 1; }\n";
    std::ofstream(second) << "int second() { return 2; }\n";
    std::ofstream(third) << "int third() { return 3; }\n";
  }

  cidx::Storage db(":memory:");
  db.add_component("app", root.string());
  for (const auto &source : {first, second, third}) {
    db.add_file_path(source.string(), std::nullopt, std::nullopt,
                     std::vector<std::string>{"-std=c++17"},
                     std::string("c++"));
  }
  cidx::StorageWorkspaceAdapter workspace_data(db);
  cidx::WorkspaceContext workspace =
      cidx::WorkspaceContext::borrow(workspace_data);
  cidx::application::StorageApplicationOperations operations(db);
  cidx::application::ApplicationOperationPorts operation_ports{.index =
                                                                   &operations};
  cidx::application::ApplicationContext full_context(
      workspace, cidx::application::ApplicationPolicy{.max_diagnostics = 0}, {},
      {}, operation_ports);
  const cidx::application::DefaultApplicationServices services;
  const cidx::application::ApplicationService service(services);
  REQUIRE(
      service.execute(cidx::application::IndexRequest{}, full_context).status ==
      cidx::protocol::Status::Complete);

  std::ofstream(first, std::ios::app) << "int changed_first() { return 3; }\n";
  std::ofstream(second, std::ios::app)
      << "int changed_second() { return 4; }\n";
  std::ofstream(third, std::ios::app) << "int changed_third() { return 5; }\n";

  cidx::application::ApplicationContext context(
      workspace,
      cidx::application::ApplicationPolicy{.max_work_items = 1,
                                           .max_diagnostics = 0},
      {}, {}, operation_ports);
  RecordingProgress progress;
  context.set_progress_sink(&progress);
  const auto result = service.execute(
      cidx::application::IndexRequest{
          .files = {first.string(), second.string()}},
      context);

  CHECK(result.status == cidx::protocol::Status::Partial);
  CHECK(result.completeness.truncated);
  CHECK(result.completeness.state == "partial");
  CHECK(result.completeness.budget == 1);
  REQUIRE(result.diagnostics.size() == 1);
  CHECK(result.diagnostics.front().code == "truncated_budget");
  CHECK(result.identity.index == "stale");
  CHECK(result.identity.freshness == "unverifiable");
  CHECK(result.valid());
  CHECK(cidx::json_out::dumps_indent2(result.result).find("\"indexed\": 1") !=
        std::string::npos);
  REQUIRE(progress.events.size() == 1);
  CHECK(progress.events.front().sequence == 0);
  CHECK(progress.events.front().completed == 0);
  CHECK(progress.events.front().total == 1);

  cidx::application::ApplicationContext cancelled_context(
      workspace,
      cidx::application::ApplicationPolicy{.max_work_items = 1,
                                           .max_diagnostics = 0},
      {}, {}, operation_ports);
  CancellingProgress cancelling(cancelled_context.cancellation());
  cancelled_context.set_progress_sink(&cancelling);
  const auto cancelled =
      service.execute(cidx::application::IndexRequest{}, cancelled_context);
  CHECK(cancelled.status == cidx::protocol::Status::Error);
  CHECK(cancelled.identity.index == "stale");
  CHECK(cancelled.identity.freshness == "unverifiable");
  CHECK(cancelled.completeness.budget == 1);
  check_canonical_truncation(cancelled);
  CHECK(cancelled.valid());
  REQUIRE_FALSE(cancelled.diagnostics.empty());
  CHECK(cancelled.diagnostics.front().code == "timeout");
  REQUIRE(db.get_file(second).has_value());
  CHECK(cidx::files::index_status(*db.get_file(second), second.string()) !=
        cidx::files::IndexStatus::kOk);
  REQUIRE(db.get_file(third).has_value());
  CHECK(cidx::files::index_status(*db.get_file(third), third.string()) !=
        cidx::files::IndexStatus::kOk);

  const auto unknown = service.execute(
      cidx::application::IndexRequest{.files = {"missing.cpp"}}, context);
  CHECK(unknown.status == cidx::protocol::Status::Error);
  CHECK(unknown.diagnostics.front().code == "invalid_input");

  std::filesystem::remove_all(root, ec);
}

TEST_CASE("index failure preserves fallback diagnostics, logs, and budget") {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "cidx-application-failure";
  std::error_code ec;
  std::filesystem::remove_all(root, ec);
  std::filesystem::create_directories(root, ec);
  REQUIRE_FALSE(ec);
  const std::filesystem::path missing = root / "a-missing.cpp";
  const std::filesystem::path pending = root / "b-pending.cpp";
  const std::string log_path = (root / "cidx.log").string();

  cidx::Storage db(":memory:");
  db.add_component("app", root.string());
  const int64_t missing_id = db.add_file_path(
      missing.string(), std::nullopt, std::nullopt,
      std::vector<std::string>{"-std=c++17"}, std::string("c++"));
  db.add_file_path(pending.string(), std::nullopt, std::nullopt,
                   std::vector<std::string>{"-std=c++17"}, std::string("c++"));
  cidx::StorageWorkspaceAdapter workspace_data(db);
  cidx::WorkspaceContext workspace =
      cidx::WorkspaceContext::borrow(workspace_data);
  cidx::Logger logger;
  logger.set_file(log_path);
  cidx::application::StorageApplicationOperations operations(db);
  cidx::application::ApplicationOperationPorts operation_ports{.index =
                                                                   &operations};
  cidx::application::ApplicationContext context(
      workspace, cidx::application::ApplicationPolicy{.max_work_items = 1}, {},
      {}, operation_ports, &logger);
  const cidx::application::DefaultApplicationServices services;
  const cidx::application::ApplicationService service(services);

  const auto result =
      service.execute(cidx::application::IndexRequest{}, context);
  CHECK(result.status == cidx::protocol::Status::Error);
  CHECK(result.completeness.budget == 1);
  check_canonical_truncation(result);
  CHECK(result.identity.index == "unverifiable");
  CHECK(result.identity.freshness == "unverifiable");
  CHECK(result.valid());
  REQUIRE_FALSE(result.diagnostics.empty());
  CHECK(result.diagnostics.front().code == "backend_error");
  const auto persisted = db.get_diagnostics(missing_id);
  REQUIRE_FALSE(persisted.empty());
  CHECK(std::ranges::any_of(persisted, [](const cidx::Diagnostic &diagnostic) {
    return diagnostic.severity >= 3;
  }));
  REQUIRE(db.get_file(missing.string()).has_value());
  CHECK_FALSE(db.get_file(missing.string())->indexed);

  const std::filesystem::path invalid = root / "c-invalid.cpp";
  std::ofstream(invalid) << "#include \"definitely-missing.h\"\n"
                            "int broken() { return 0; }\n";
  const int64_t invalid_id = db.add_file_path(
      invalid.string(), std::nullopt, std::nullopt,
      std::vector<std::string>{"-std=c++17"}, std::string("c++"));
  const auto invalid_result = service.execute(
      cidx::application::IndexRequest{.files = {invalid.string()}}, context);
  CHECK(invalid_result.status == cidx::protocol::Status::Error);
  CHECK_FALSE(db.get_diagnostics(invalid_id).empty());
  CHECK(std::filesystem::exists(log_path));
  std::ifstream log(log_path);
  const std::string logged((std::istreambuf_iterator<char>(log)),
                           std::istreambuf_iterator<char>());
  CHECK(logged.find("failed parse flags:") != std::string::npos);
  CHECK(logged.find("; clang: ") != std::string::npos);

  std::filesystem::remove_all(root, ec);
}

TEST_CASE("direct analysis service exports real fact relations") {
  const std::filesystem::path directory =
      std::filesystem::temp_directory_path() / "cidx-application-analysis";
  std::error_code ec;
  std::filesystem::remove_all(directory, ec);
  std::filesystem::create_directories(directory, ec);
  REQUIRE_FALSE(ec);
  const std::string index = (directory / "analysis.sqlite").string();
  cidx::Storage db(index);
  cidx::application::StorageApplicationOperations operations(db, index);
  cidx::application::ApplicationOperationPorts operation_ports{.analysis =
                                                                   &operations};
  cidx::application::ApplicationContext context(
      cidx::application::ApplicationPolicy{
          .capabilities = cidx::application::capability_bit(
                              cidx::application::Capability::analysis) |
                          cidx::application::capability_bit(
                              cidx::application::Capability::artifacts)},
      operation_ports);
  const cidx::application::DefaultApplicationServices services;
  const cidx::application::ApplicationService service(services);
  const auto result = service.execute(
      cidx::application::AnalysisRequest{
          .action = cidx::application::AnalysisAction::export_facts,
          .export_directory = (directory / "facts").string()},
      context);
  CHECK(result.status == cidx::protocol::Status::Complete);
  CHECK(std::filesystem::exists(directory / "facts" / "symbol.facts"));
  std::filesystem::remove_all(directory, ec);
}

TEST_CASE("default services execute real storage-backed operations") {
  cidx::Storage db(":memory:");
  cidx::StorageWorkspaceAdapter workspace_data(db);
  cidx::WorkspaceContext workspace =
      cidx::WorkspaceContext::borrow(workspace_data);
  cidx::query::SqliteQueryReadAdapter query_read(db);
  cidx::application::ApplicationReadPorts read_ports{
      .workspace = &db.workspace_catalog_read(),
      .source = &db.source_read(),
      .symbols = &db.symbol_read(),
      .types = &db.type_read(),
      .facts = &db.fact_read(),
      .definitions = &db.definition_read(),
      .includes = &db.include_read(),
      .schema = &db.schema_read(),
      .query = &query_read,
  };
  cidx::application::ApplicationWritePorts write_ports{
      .workspace = &db.workspace_catalog_write(),
      .source = &db.source_write(),
      .symbols = &db.symbol_write(),
      .types = &db.type_write(),
      .facts = &db.fact_write(),
      .definitions = &db.definition_write(),
      .includes = &db.include_write(),
      .unit_of_work = &db.unit_of_work(),
  };
  cidx::application::StorageApplicationOperations operations(db);
  cidx::application::ApplicationOperationPorts operation_ports{
      .index = &operations,
      .analysis = &operations,
      .ast = &operations,
      .diff = &operations,
      .include = &operations,
  };
  cidx::application::ApplicationContext context(
      workspace, {}, read_ports, write_ports, operation_ports, nullptr);
  const cidx::application::DefaultApplicationServices services;
  const cidx::application::ApplicationService service(services);

  CHECK(service.execute(cidx::application::IndexRequest{}, context).status ==
        cidx::protocol::Status::Complete);
  CHECK(service.execute(cidx::application::IncludeRequest{}, context).status ==
        cidx::protocol::Status::Complete);
  CHECK(
      service
          .execute(cidx::application::QueryRequest{.expression =
                                                       "codebase() | nodes()"},
                   context)
          .status == cidx::protocol::Status::Complete);

  for (const cidx::application::CommandRequest &request :
       {cidx::application::CommandRequest{
            cidx::application::WorkspaceRequest{}},
        cidx::application::CommandRequest{
            cidx::application::RefactoringRequest{}},
        cidx::application::CommandRequest{cidx::application::ProofRequest{}}}) {
    const auto unsupported = service.execute(request, context);
    CHECK(unsupported.status == cidx::protocol::Status::Error);
    CHECK(unsupported.diagnostics.front().code == "invalid_input");
  }

  const auto analysis = service.execute(
      cidx::application::AnalysisRequest{
          .action = cidx::application::AnalysisAction::list},
      context);
  CHECK(cidx::json_out::dumps_indent2(analysis.result).find("callgraph") !=
        std::string::npos);

  const auto unknown_source = service.execute(
      cidx::application::AstInspectionRequest{.source = "missing.cpp"},
      context);
  CHECK(unknown_source.status == cidx::protocol::Status::Error);
  CHECK(unknown_source.diagnostics.front().code == "unknown_source");
}

TEST_CASE("agent catalog and versioned read-only budget contract") {
  const auto catalog = cidx::agent::tool_catalog();
  CHECK(catalog.size() == 2);
  CHECK(catalog[0] == "query");
  CHECK(catalog[1] == "explain");

  cidx::Storage db(":memory:");
  cidx::Symbol first;
  first.usr = "USR::agent-first";
  first.spelling = "first";
  first.kind = "function";
  first.is_definition = true;
  first.resolved = true;
  cidx::Symbol second = first;
  second.usr = "USR::agent-second";
  second.spelling = "second";
  db.add_symbol(first);
  db.add_symbol(second);

  cidx::StorageWorkspaceAdapter workspace_data(db);
  cidx::WorkspaceContext workspace =
      cidx::WorkspaceContext::borrow(workspace_data);
  cidx::query::SqliteQueryReadAdapter query_read(db);
  cidx::application::ApplicationReadPorts read_ports{.query = &query_read};
  cidx::application::ApplicationContext context(
      workspace,
      cidx::application::ApplicationPolicy{
          .access = cidx::application::AccessMode::read_only,
          .capabilities = cidx::application::capability_bit(
              cidx::application::Capability::index_read)},
      read_ports);

  cidx::agent::Request request;
  request.tool = cidx::agent::Tool::query;
  request.query.expression = "codebase() | nodes() | select(name, file, line)";
  request.budget.max_results = 1;
  const cidx::agent::ToolService tools;
  const auto response = tools.invoke(request, context);
  CHECK(response.completeness.truncated);
  CHECK(response.completeness.budget == std::optional<int64_t>{1});
  CHECK(response.status == cidx::protocol::Status::Partial);
  CHECK(cidx::json_out::dumps_indent2(response.result).find("\"index\"") !=
        std::string::npos);
  const auto encoded = tools.encode_response(request, response);
  const auto encoded_text = cidx::json_out::dumps_indent2(encoded);
  CHECK(encoded_text.find("cidx.agent/v1") != std::string::npos);
  CHECK(encoded_text.find("\"exhausted_at\": 1") != std::string::npos);

  request.budget.max_results = 10;
  const auto evidence_response = tools.invoke(request, context);
  CHECK(evidence_response.status == cidx::protocol::Status::Unknown);
  CHECK(!evidence_response.completeness.truncated);
  CHECK(std::any_of(
      evidence_response.diagnostics.begin(), evidence_response.diagnostics.end(),
      [](const auto &diagnostic) { return diagnostic.code == "missing_evidence"; }));

  const auto decoded = tools.decode_request(
      R"json({"version":1,"tool":"explain","cxq":"codebase()","budget":{"max_results":2}})json");
  CHECK(decoded.tool == cidx::agent::Tool::explain);
  CHECK(decoded.query.explain);
  CHECK(decoded.budget.max_results == 2);
  const auto decode_invalid_version = [&tools] {
    (void)tools.decode_request(
        R"json({"version":99,"tool":"query","cxq":"codebase()"})json");
  };
  CHECK_THROWS_WITH(
      decode_invalid_version(),
      "E_PROTOCOL_VERSION: unsupported agent protocol version 99");
}

int main(int argc, char **argv) {
  doctest::Context context(argc, argv);
  return context.run();
}
