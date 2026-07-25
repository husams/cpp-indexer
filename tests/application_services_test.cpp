#define DOCTEST_CONFIG_IMPLEMENT
#include "doctest/doctest.h"

#include <string>
#include <utility>
#include <vector>

#include "application/registry.hpp"
#include "application/services.hpp"
#include "cli/application_adapter.hpp"

namespace {

cidx::protocol::ResultEnvelope complete(std::string operation) {
  cidx::protocol::ResultEnvelope result;
  result.operation = std::move(operation);
  return result;
}

struct RecordingProgress final : cidx::application::ProgressSink {
  void publish(const cidx::protocol::ProgressEvent &event) override {
    events.push_back(event);
  }
  std::vector<cidx::protocol::ProgressEvent> events;
};

} // namespace

TEST_CASE("application registry is complete and validated") {
  CHECK(cidx::application::registry_is_valid());
  CHECK(cidx::application::command_registry().size() == 13);
  CHECK(cidx::application::metadata(cidx::application::Operation::query) !=
        nullptr);
  CHECK(cidx::application::metadata(cidx::application::Operation::query)
            ->mutability == cidx::application::Mutability::read_only);
}

TEST_CASE("migrated CLI operations become typed requests") {
  const auto query = cidx::cli::parse_request({"query", "nodes()", "--json"});
  REQUIRE(query.has_value());
  REQUIRE(std::holds_alternative<cidx::application::QueryRequest>(*query));
  const auto &typed = std::get<cidx::application::QueryRequest>(*query);
  CHECK(typed.expression == "nodes()");
  CHECK(typed.output == cidx::application::QueryOutput::json);

  CHECK_FALSE(cidx::cli::parse_request({"search", "nodes"}).has_value());
}

TEST_CASE("typed services can be invoked without CLI state") {
  std::vector<std::string> calls;
  cidx::application::ApplicationHandlers handlers;
  handlers.index = [&calls](const cidx::application::IndexRequest &,
                            cidx::application::ApplicationContext &) {
    calls.emplace_back("index");
    return complete("index");
  };
  handlers.query = [&calls](const cidx::application::QueryRequest &,
                            cidx::application::ApplicationContext &) {
    calls.emplace_back("query");
    return complete("query");
  };
  handlers.analysis = [&calls](const cidx::application::AnalysisRequest &,
                               cidx::application::ApplicationContext &) {
    calls.emplace_back("analysis");
    return complete("analysis");
  };
  handlers.ast_inspection =
      [&calls](const cidx::application::AstInspectionRequest &,
               cidx::application::ApplicationContext &) {
        calls.emplace_back("ast");
        return complete("ast");
      };
  handlers.diff = [&calls](const cidx::application::DiffRequest &,
                           cidx::application::ApplicationContext &) {
    calls.emplace_back("diff");
    return complete("diff");
  };

  cidx::application::ApplicationService service(std::move(handlers));
  cidx::application::ApplicationContext context;
  const std::vector<cidx::application::CommandRequest> requests = {
      cidx::application::IndexRequest{},
      cidx::application::QueryRequest{.expression = "nodes()"},
      cidx::application::AnalysisRequest{},
      cidx::application::AstInspectionRequest{.source = "fixture.cpp"},
      cidx::application::DiffRequest{.left = "left.cpp", .right = "right.cpp"},
  };
  for (const auto &request : requests) {
    CHECK(service.execute(request, context).status ==
          cidx::protocol::Status::Complete);
  }
  CHECK(calls ==
        std::vector<std::string>{"index", "query", "analysis", "ast", "diff"});
}

TEST_CASE("read-only context rejects mutating requests before handlers") {
  bool called = false;
  cidx::application::ApplicationHandlers handlers;
  handlers.index = [&called](const cidx::application::IndexRequest &,
                             cidx::application::ApplicationContext &) {
    called = true;
    return complete("index");
  };
  cidx::application::ApplicationService service(std::move(handlers));
  cidx::application::ApplicationContext context(
      {.access = cidx::application::AccessMode::read_only});

  const auto result = service.execute(
      cidx::application::IndexRequest{
          .action = cidx::application::IndexAction::rebuild},
      context);
  REQUIRE(result.status == cidx::protocol::Status::Refuted);
  REQUIRE(result.diagnostics.size() == 1);
  CHECK(result.diagnostics.front().code == "policy_refuted");
  CHECK_FALSE(called);
}

TEST_CASE("progress is a separate channel and cancellation is typed") {
  cidx::application::ApplicationHandlers handlers;
  handlers.query = [](const cidx::application::QueryRequest &,
                      cidx::application::ApplicationContext &context) {
    context.publish(cidx::protocol::ProgressEvent{
        .sequence = 1, .operation = "query", .message = "scanning"});
    return complete("query");
  };
  cidx::application::ApplicationService service(std::move(handlers));
  cidx::application::ApplicationContext context;
  RecordingProgress progress;
  context.set_progress_sink(&progress);

  const auto result = service.execute(
      cidx::application::QueryRequest{.expression = "nodes()"}, context);
  CHECK(result.status == cidx::protocol::Status::Complete);
  REQUIRE(progress.events.size() == 1);
  CHECK(progress.events.front().message == "scanning");

  context.cancellation().cancel();
  const auto cancelled = service.execute(
      cidx::application::QueryRequest{.expression = "nodes()"}, context);
  REQUIRE(cancelled.status == cidx::protocol::Status::Error);
  CHECK(cancelled.diagnostics.front().code == "timeout");
}

int main(int argc, char **argv) {
  doctest::Context context(argc, argv);
  return context.run();
}
