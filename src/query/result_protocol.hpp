// Versioned result and event protocol shared by QueryPlan and product adapters.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "cli/json_out.hpp"
#include "query/generated_result_protocol.hpp"

namespace cidx::protocol {

using Status = generated::Status;
using ExitClass = generated::ExitClass;
inline constexpr int kProtocolVersion = generated::kProtocolVersion;
inline constexpr std::string_view kProtocol = generated::kProtocol;
inline constexpr std::string_view kEventProtocol = generated::kEventProtocol;
inline constexpr std::size_t kMaxEvidenceDepth = generated::kMaxEvidenceDepth;
inline constexpr std::size_t kMaxEvidenceNodes = generated::kMaxEvidenceNodes;
inline constexpr std::size_t kMaxTextBytes = generated::kMaxTextBytes;

[[nodiscard]] std::string_view status_name(Status status);
[[nodiscard]] std::string_view exit_class_name(ExitClass exit_class);
[[nodiscard]] int exit_code(ExitClass exit_class);

struct Identity {
  std::string workspace = "unknown";
  std::string index = "unknown";
  std::vector<std::string> fact_sets;
  std::string freshness = "unverifiable";
  std::optional<std::string> source_revision;
  std::optional<std::string> source_fingerprint;
};

struct Producer {
  std::string package = "cidx";
  std::string version = "unknown";
  std::string backend = "unknown";
  int schema_version = kProtocolVersion;
};

struct Completeness {
  std::string state = "complete";
  bool truncated = false;
  bool stale = false;
  std::optional<int64_t> budget;
};

struct Diagnostic {
  std::string code;
  std::string severity = "error";
  std::string message;
  std::optional<std::string> next_action;
};

struct EvidenceNode {
  std::string id;
  std::string evidence_class = "derived";
  std::string trust = "unverified";
  std::string summary;
  std::optional<std::string> source;
  std::vector<EvidenceNode> children;
};

struct ArtifactRef {
  std::string kind;
  std::string id;
  int schema_version = 1;
  int catalog_version = 1;
  std::string catalog_hash;
  // Shared provenance token for the core index generation this artifact
  // publishes or derives from. Artifact IDs identify different artifacts and
  // therefore must not be compared as if they were generation identities.
  std::optional<std::string> generation;
};

struct ReplayInput {
  std::string command;
  std::vector<std::string> arguments;
};

struct ResourceMetadata {
  std::optional<int64_t> elapsed_ms;
  std::optional<int64_t> peak_bytes;
};

struct ResultEnvelope {
  std::string operation;
  Status status = Status::Complete;
  Identity identity;
  Producer producer;
  Completeness completeness;
  json_out::Value result = json_out::Value::obj({});
  std::vector<Diagnostic> diagnostics;
  std::vector<EvidenceNode> evidence;
  std::vector<ArtifactRef> artifacts;
  std::optional<ReplayInput> replay;
  std::optional<ResourceMetadata> resources;

  [[nodiscard]] ExitClass exit_class() const;
  [[nodiscard]] int exit_code() const;
  [[nodiscard]] bool valid() const;
  [[nodiscard]] json_out::Value to_json() const;
  [[nodiscard]] json_out::Value error_status_json() const;
  [[nodiscard]] std::string human_text() const;
};

struct ProgressEvent {
  int64_t sequence = 0;
  std::string operation;
  std::string event = "progress";
  std::string message;
  std::optional<int64_t> completed;
  std::optional<int64_t> total;

  [[nodiscard]] json_out::Value to_json() const;
};

[[nodiscard]] std::string redact_text(std::string_view value,
                                      std::size_t max_bytes = kMaxTextBytes);
[[nodiscard]] std::vector<std::string>
redact_arguments(const std::vector<std::string> &arguments,
                 std::size_t max_bytes = kMaxTextBytes);

} // namespace cidx::protocol
