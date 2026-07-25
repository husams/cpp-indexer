#include "application/registry.hpp"

#include <array>
#include <type_traits>

namespace cidx::application {
namespace {

constexpr std::array<OutputFormat, 2> kTextJson = {OutputFormat::text,
                                                   OutputFormat::json};
constexpr std::array<OutputFormat, 1> kJson = {OutputFormat::json};
constexpr std::array<std::string_view, 0> kNoAliases = {};
constexpr std::array<protocol::Status, 1> kComplete = {
    protocol::Status::Complete};
constexpr std::array<protocol::Status, 2> kCompletePartial = {
    protocol::Status::Complete, protocol::Status::Partial};

constexpr CapabilityMask kReadCapability = static_cast<CapabilityMask>(
    1U << static_cast<unsigned>(Capability::index_read));
constexpr CapabilityMask kWriteCapability = static_cast<CapabilityMask>(
    1U << static_cast<unsigned>(Capability::index_write));
constexpr CapabilityMask kArtifactCapability = static_cast<CapabilityMask>(
    1U << static_cast<unsigned>(Capability::artifacts));

constexpr std::array<CommandMetadata, 13> kRegistry = {{
    {Operation::index_update, "index", "update", kNoAliases,
     "update indexed sources", Mutability::mutating, kWriteCapability,
     kTextJson, protocol::ExitClass::Success, kCompletePartial},
    {Operation::index_rebuild, "index", "rebuild", kNoAliases,
     "rebuild indexed sources", Mutability::mutating, kWriteCapability,
     kTextJson, protocol::ExitClass::Success, kCompletePartial},
    {Operation::index_status, "index", "status", kNoAliases,
     "report index status", Mutability::read_only, kReadCapability, kTextJson,
     protocol::ExitClass::Success, kComplete},
    {Operation::index_explain, "index", "explain", kNoAliases,
     "explain index inputs", Mutability::read_only, kReadCapability, kTextJson,
     protocol::ExitClass::Success, kComplete},
    {Operation::query, "query", "execute", kNoAliases,
     "execute a typed query plan", Mutability::read_only, kReadCapability,
     kTextJson, protocol::ExitClass::Success, kCompletePartial},
    {Operation::analysis_list, "analysis", "list", kNoAliases,
     "list available analyses", Mutability::read_only, kReadCapability, kJson,
     protocol::ExitClass::Success, kComplete},
    {Operation::analysis_execute, "analysis", "execute", kNoAliases,
     "run an analysis", Mutability::read_only, kReadCapability, kTextJson,
     protocol::ExitClass::Success, kCompletePartial},
    {Operation::analysis_export, "analysis", "export", kNoAliases,
     "export analysis facts", Mutability::mutating, kArtifactCapability,
     kTextJson, protocol::ExitClass::Success, kComplete},
    {Operation::ast_dump, "ast", "dump", kNoAliases,
     "inspect a translation unit AST", Mutability::read_only, kReadCapability,
     kTextJson, protocol::ExitClass::Success, kComplete},
    {Operation::ast_locals, "ast", "locals", kNoAliases,
     "inspect local bindings", Mutability::read_only, kReadCapability,
     kTextJson, protocol::ExitClass::Success, kComplete},
    {Operation::ast_conditions, "ast", "conditions", kNoAliases,
     "inspect conditions", Mutability::read_only, kReadCapability, kTextJson,
     protocol::ExitClass::Success, kComplete},
    {Operation::diff_file, "diff", "file", kNoAliases,
     "compare registered files", Mutability::read_only, kReadCapability,
     kTextJson, protocol::ExitClass::Success, kComplete},
    {Operation::diff_symbol, "diff", "symbol", kNoAliases,
     "compare registered symbols", Mutability::read_only, kReadCapability,
     kTextJson, protocol::ExitClass::Success, kComplete},
}};

} // namespace

std::span<const CommandMetadata> command_registry() noexcept {
  return kRegistry;
}

const CommandMetadata *metadata(Operation operation) noexcept {
  for (const CommandMetadata &entry : kRegistry) {
    if (entry.operation == operation) {
      return &entry;
    }
  }
  return nullptr;
}

Operation operation_of(const CommandRequest &request) noexcept {
  return std::visit(
      [](const auto &typed) -> Operation {
        using T = std::decay_t<decltype(typed)>;
        if constexpr (std::is_same_v<T, IndexRequest>) {
          switch (typed.action) {
          case IndexAction::update:
            return Operation::index_update;
          case IndexAction::rebuild:
            return Operation::index_rebuild;
          case IndexAction::status:
            return Operation::index_status;
          case IndexAction::explain:
            return Operation::index_explain;
          }
        } else if constexpr (std::is_same_v<T, QueryRequest>) {
          return Operation::query;
        } else if constexpr (std::is_same_v<T, AnalysisRequest>) {
          switch (typed.action) {
          case AnalysisAction::list:
            return Operation::analysis_list;
          case AnalysisAction::execute:
            return Operation::analysis_execute;
          case AnalysisAction::export_facts:
            return Operation::analysis_export;
          }
        } else if constexpr (std::is_same_v<T, AstInspectionRequest>) {
          switch (typed.action) {
          case AstInspectionAction::dump:
            return Operation::ast_dump;
          case AstInspectionAction::locals:
            return Operation::ast_locals;
          case AstInspectionAction::conditions:
            return Operation::ast_conditions;
          }
        } else {
          return typed.scope == DiffScope::file ? Operation::diff_file
                                                : Operation::diff_symbol;
        }
        return Operation::query;
      },
      request);
}

bool registry_is_valid() noexcept {
  for (const CommandMetadata &entry : kRegistry) {
    if (entry.group.empty() || entry.name.empty() || entry.summary.empty() ||
        entry.formats.empty() || entry.result_statuses.empty() ||
        entry.required_capabilities == 0 ||
        metadata(entry.operation) != &entry) {
      return false;
    }
  }
  return true;
}

} // namespace cidx::application
