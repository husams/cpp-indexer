// Registry metadata for application operations.  Adapters use this table for
// help/catalog generation; dispatch itself uses the typed request variant.
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include "application/context.hpp"
#include "application/requests.hpp"
#include "query/result_protocol.hpp"

namespace cidx::application {

enum class Operation : std::uint8_t {
  index_update,
  index_rebuild,
  index_status,
  index_explain,
  query,
  analysis_list,
  analysis_execute,
  analysis_export,
  ast_dump,
  ast_locals,
  ast_conditions,
  diff_file,
  diff_symbol,
  diff_source,
  diff_configuration,
  diff_index,
  workspace_list,
  workspace_show,
  workspace_select,
  workspace_refresh,
  include_graph,
  include_check,
  include_plan,
  include_apply,
  refactor_check,
  refactor_plan,
  refactor_apply,
  proof_prepare,
  proof_execute,
  proof_status,
  proof_explain,
};

enum class Mutability : std::uint8_t { read_only, mutating };
enum class OutputFormat : std::uint8_t { text, json };

struct CommandMetadata {
  Operation operation;
  std::string_view group;
  std::string_view name;
  std::span<const std::string_view> aliases;
  std::string_view summary;
  Mutability mutability;
  CapabilityMask required_capabilities;
  std::span<const OutputFormat> formats;
  protocol::ExitClass exit_class;
  std::span<const protocol::Status> result_statuses;
  bool deprecated = false;
};

[[nodiscard]] std::span<const CommandMetadata> command_registry() noexcept;
[[nodiscard]] const CommandMetadata *metadata(Operation operation) noexcept;
[[nodiscard]] std::optional<Operation>
operation_of(const CommandRequest &request) noexcept;
[[nodiscard]] bool registry_is_valid() noexcept;

} // namespace cidx::application
