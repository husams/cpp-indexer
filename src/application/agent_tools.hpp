// Versioned read-only agent tools over the typed application/query boundary.
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "application/context.hpp"
#include "application/requests.hpp"
#include "query/result_protocol.hpp"

namespace cidx::agent {

inline constexpr int kProtocolVersion = 1;
inline constexpr std::string_view kProtocol = "cidx.agent/v1";
inline constexpr int64_t kMaxResults = 10000;

enum class Tool : std::uint8_t { query, explain };

struct Budget {
  int64_t max_results = 1000;
};

struct Request {
  int version = kProtocolVersion;
  Tool tool = Tool::query;
  application::QueryRequest query;
  Budget budget;
};

[[nodiscard]] std::string_view tool_name(Tool tool) noexcept;
[[nodiscard]] std::span<const std::string_view> tool_catalog() noexcept;

class ToolService final {
public:
  [[nodiscard]] static protocol::ResultEnvelope
  invoke(const Request &request, application::ApplicationContext &context);

  // The process adapter is newline-delimited JSON: one request object in and
  // one response object out. Parsing is strict and never downgrades versions.
  [[nodiscard]] static Request decode_request(const std::string &json);
  [[nodiscard]] static json_out::Value
  encode_response(const Request &request,
                  const protocol::ResultEnvelope &response);
  [[nodiscard]] static json_out::Value
  invoke_json(const std::string &json,
              application::ApplicationContext &context);
};

// Descriptive aliases keep the public agent surface discoverable to
// repository tooling while preserving the concise internal names.
using AgentRequest = Request;
using AgentTools = ToolService;

} // namespace cidx::agent
