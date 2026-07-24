#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include <fstream>
#include <sstream>
#include <string>

#include "query/result_protocol.hpp"

namespace {

std::string read_file(const std::string &path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream out;
  out << in.rdbuf();
  std::string value = out.str();
  if (!value.empty() && value.back() == '\n') {
    value.pop_back();
  }
  return value;
}

cidx::protocol::ResultEnvelope golden_envelope() {
  using namespace cidx::protocol;
  ResultEnvelope envelope;
  envelope.operation = "query";
  envelope.identity = {"workspace://demo", "semantic-index://demo",
                       {"symbols", "files"}, "current", "git:abc123",
                       "sha256:source"};
  envelope.producer = {"cidx", "0.53.0", "cpp", 1};
  envelope.result = cidx::json_out::Value::obj({
      {"shape", cidx::json_out::Value::of(std::string("rows"))},
      {"view", cidx::json_out::Value::of(std::string("symbol"))},
      {"count", cidx::json_out::Value::of(1)},
      {"truncated", cidx::json_out::Value::of(false)},
      {"rows", cidx::json_out::Value::arr({cidx::json_out::Value::obj({
          {"id", cidx::json_out::Value::of(42)},
          {"name", cidx::json_out::Value::of(std::string("main"))},
          {"kind", cidx::json_out::Value::of(std::string("function"))}})})}});
  envelope.evidence.push_back({"queryplan", "derived", "producer-verified",
                               "bounded QueryPlan execution",
                               "query://demo", {}});
  envelope.artifacts.push_back(
      {"query-result", "query-result://demo", 1, 1, "sha256:catalog"});
  return envelope;
}

} // namespace

TEST_CASE("result protocol matches the shared golden envelope") {
  const auto envelope = golden_envelope();
  CHECK(envelope.valid());
  CHECK(envelope.exit_code() == 0);
  CHECK(cidx::json_out::dumps_indent2(envelope.to_json()) ==
        read_file(CIDX_RESULT_PROTOCOL_GOLDEN));
}

TEST_CASE("result protocol keeps status, truncation, stale input, and exit class distinct") {
  using namespace cidx::protocol;
  ResultEnvelope envelope = golden_envelope();
  envelope.status = Status::Partial;
  envelope.completeness = {"partial", true, false, 1000};
  envelope.diagnostics.push_back({"truncated_budget", "warning", "bounded", "narrow the query"});
  CHECK(envelope.exit_class() == ExitClass::Success);
  CHECK(envelope.human_text().find("truncated") != std::string::npos);

  envelope.status = Status::Unknown;
  envelope.identity.freshness = "stale";
  envelope.completeness = {"unknown", false, true, std::nullopt};
  envelope.diagnostics.push_back({"stale_input", "error", "stale", "re-index"});
  CHECK(envelope.exit_class() == ExitClass::InvalidOrStaleInput);
  CHECK(envelope.exit_code() == 3);
  CHECK(envelope.human_text().find("stale index") != std::string::npos);

  envelope.status = Status::Refuted;
  envelope.diagnostics = {{"policy_refuted", "error", "claim refuted", "review evidence"}};
  CHECK(envelope.exit_class() == ExitClass::PolicyFailure);
  CHECK(envelope.exit_code() == 4);

  envelope.status = Status::Error;
  envelope.diagnostics = {{"timeout", "error", "backend timed out", "retry"}};
  CHECK(envelope.exit_class() == ExitClass::InfrastructureFailure);
  CHECK(envelope.exit_code() == 6);
  envelope.diagnostics = {{"invalid_input", "error", "bad input", "fix it"}};
  CHECK(envelope.exit_class() == ExitClass::InvalidOrStaleInput);
  CHECK(envelope.exit_code() == 3);
  envelope.diagnostics = {{"usage", "error", "bad usage", "read help"}};
  CHECK(envelope.exit_class() == ExitClass::Usage);
  CHECK(envelope.exit_code() == 2);
}

TEST_CASE("result protocol rejects contradictory and weakly identified envelopes") {
  using namespace cidx::protocol;
  ResultEnvelope envelope = golden_envelope();
  envelope.completeness.state = "unknown";
  CHECK_FALSE(envelope.valid());
  envelope = golden_envelope();
  envelope.identity.workspace = "unknown";
  CHECK_FALSE(envelope.valid());
  envelope = golden_envelope();
  envelope.status = Status::Error;
  envelope.completeness = {"unknown", false, false, std::nullopt};
  envelope.diagnostics = {{"backend_error", "error", "TOKEN=secret", "PASSWORD=next"}};
  CHECK(envelope.human_text().find("TOKEN=secret") == std::string::npos);
  CHECK(envelope.human_text().find("PASSWORD=next") == std::string::npos);
  CHECK(envelope.human_text().find("<redacted:secret>") != std::string::npos);
  CHECK(envelope.error_status_json().t == cidx::json_out::Value::T::Obj);
}

TEST_CASE("progress events and redaction stay separate from final results") {
  using namespace cidx::protocol;
  const ProgressEvent event{3, "index", "progress", "TOKEN=secret", 2, 4};
  const std::string json = cidx::json_out::dumps_indent2(event.to_json());
  CHECK(json.find("cidx.event/v1") != std::string::npos);
  CHECK(json.find("<redacted:secret>") != std::string::npos);
  CHECK(redact_text(std::string(5000, 'x')).find("<redacted:size-limit>") !=
        std::string::npos);
  const ProgressEvent golden_event{3, "index", "progress", "indexed 2 of 4 files", 2, 4};
  CHECK(cidx::json_out::dumps_indent2(golden_event.to_json()) ==
        read_file(CIDX_EVENT_PROTOCOL_GOLDEN));
}

TEST_CASE("cross-language acceptance vectors and error reduction are executable") {
  using namespace cidx::protocol;
  for (const auto &vector : generated::kAcceptanceVectors) {
    ResultEnvelope envelope = golden_envelope();
    envelope.operation = std::string(vector.operation);
    envelope.status = vector.status;
    envelope.identity.freshness = std::string(vector.freshness);
    envelope.completeness.state = std::string(vector.completeness_state);
    envelope.completeness.stale = vector.freshness == "stale";
    envelope.completeness.truncated = vector.status == Status::Partial &&
                                     vector.diagnostic == "truncated_budget";
    envelope.diagnostics.clear();
    if (!vector.diagnostic.empty()) {
      envelope.diagnostics.push_back({std::string(vector.diagnostic), "error",
                                      "vector diagnostic", "next"});
    }
    CHECK(envelope.valid());
    CHECK(envelope.exit_class() == vector.exit_class);
    CHECK(envelope.exit_code() == vector.exit_code);
    CHECK(envelope.to_json().t == cidx::json_out::Value::T::Obj);
  }

  ResultEnvelope error = golden_envelope();
  error.status = Status::Error;
  error.completeness = {"unknown", false, false, std::nullopt};
  error.diagnostics = {{"backend_error", "error", "backend unavailable", std::nullopt}};
  CHECK(cidx::json_out::dumps_indent2(error.error_status_json()) ==
        read_file(CIDX_ERROR_STATUS_GOLDEN));
}
