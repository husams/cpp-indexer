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

constexpr CapabilityMask kRead = capability_bit(Capability::index_read);
constexpr CapabilityMask kWrite = capability_bit(Capability::index_write);
constexpr CapabilityMask kAnalysis = capability_bit(Capability::analysis);
constexpr CapabilityMask kAst = capability_bit(Capability::ast);
constexpr CapabilityMask kDiff = capability_bit(Capability::diff);
constexpr CapabilityMask kIncludeRead =
    capability_bit(Capability::include_read);
constexpr CapabilityMask kIncludeWrite =
    capability_bit(Capability::include_write);
constexpr CapabilityMask kWorkspaceWrite =
    capability_bit(Capability::workspace_write);
constexpr CapabilityMask kProof = capability_bit(Capability::proof);
constexpr CapabilityMask kArtifacts = capability_bit(Capability::artifacts);

constexpr std::array<CommandMetadata, 31> kRegistry = {{
    {Operation::index_update, "index", "update", kNoAliases,
     "update indexed sources", Mutability::mutating, kWrite, kTextJson,
     protocol::ExitClass::Success, kCompletePartial},
    {Operation::index_rebuild, "index", "rebuild", kNoAliases,
     "rebuild indexed sources", Mutability::mutating, kWrite, kTextJson,
     protocol::ExitClass::Success, kCompletePartial},
    {Operation::index_status, "index", "status", kNoAliases,
     "report index status", Mutability::read_only, kRead, kTextJson,
     protocol::ExitClass::Success, kComplete},
    {Operation::index_explain, "index", "explain", kNoAliases,
     "explain index inputs", Mutability::read_only, kRead, kTextJson,
     protocol::ExitClass::Success, kComplete},
    {Operation::query, "query", "execute", kNoAliases,
     "execute a typed query plan", Mutability::read_only, kRead, kTextJson,
     protocol::ExitClass::Success, kCompletePartial},
    {Operation::analysis_list, "analysis", "list", kNoAliases,
     "list available analyses", Mutability::read_only, kAnalysis, kJson,
     protocol::ExitClass::Success, kComplete, false, false},
    {Operation::analysis_execute, "analysis", "execute", kNoAliases,
     "run an analysis", Mutability::read_only, kAnalysis, kTextJson,
     protocol::ExitClass::Success, kCompletePartial},
    {Operation::analysis_export, "analysis", "export", kNoAliases,
     "export analysis facts", Mutability::mutating, kAnalysis | kArtifacts,
     kTextJson, protocol::ExitClass::Success, kComplete},
    {Operation::workspace_list, "workspace", "list", kNoAliases,
     "list workspace identities", Mutability::read_only, kRead, kTextJson,
     protocol::ExitClass::Success, kComplete},
    {Operation::workspace_show, "workspace", "show", kNoAliases,
     "show the selected workspace", Mutability::read_only, kRead, kTextJson,
     protocol::ExitClass::Success, kComplete},
    {Operation::workspace_select, "workspace", "select", kNoAliases,
     "select a workspace", Mutability::mutating, kWorkspaceWrite, kTextJson,
     protocol::ExitClass::Success, kComplete},
    {Operation::workspace_refresh, "workspace", "refresh", kNoAliases,
     "refresh workspace inputs", Mutability::mutating, kWorkspaceWrite,
     kTextJson, protocol::ExitClass::Success, kComplete},
    {Operation::ast_dump, "ast", "dump", kNoAliases,
     "inspect a translation unit AST", Mutability::read_only, kAst, kTextJson,
     protocol::ExitClass::Success, kComplete},
    {Operation::ast_locals, "ast", "locals", kNoAliases,
     "inspect local bindings", Mutability::read_only, kAst, kTextJson,
     protocol::ExitClass::Success, kComplete},
    {Operation::ast_conditions, "ast", "conditions", kNoAliases,
     "inspect conditions", Mutability::read_only, kAst, kTextJson,
     protocol::ExitClass::Success, kComplete},
    {Operation::diff_file, "diff", "file", kNoAliases,
     "compare registered files", Mutability::read_only, kDiff, kTextJson,
     protocol::ExitClass::Success, kComplete},
    {Operation::diff_symbol, "diff", "symbol", kNoAliases,
     "compare registered symbols", Mutability::read_only, kDiff, kTextJson,
     protocol::ExitClass::Success, kComplete},
    {Operation::diff_source, "diff", "source", kNoAliases,
     "compare source revisions", Mutability::read_only, kDiff, kTextJson,
     protocol::ExitClass::Success, kComplete},
    {Operation::diff_configuration, "diff", "configuration", kNoAliases,
     "compare translation configurations", Mutability::read_only, kDiff,
     kTextJson, protocol::ExitClass::Success, kComplete},
    {Operation::diff_index, "diff", "index", kNoAliases,
     "compare index identities", Mutability::read_only, kDiff, kTextJson,
     protocol::ExitClass::Success, kComplete},
    {Operation::include_graph, "include", "graph", kNoAliases,
     "inspect include relationships", Mutability::read_only, kIncludeRead,
     kTextJson, protocol::ExitClass::Success, kComplete},
    {Operation::include_check, "include", "check", kNoAliases,
     "classify include findings", Mutability::read_only, kIncludeRead,
     kTextJson, protocol::ExitClass::Success, kComplete},
    {Operation::include_plan, "include", "plan", kNoAliases,
     "prepare an include cleanup plan", Mutability::read_only, kIncludeRead,
     kTextJson, protocol::ExitClass::Success, kComplete},
    {Operation::include_apply, "include", "apply", kNoAliases,
     "apply an approved include plan", Mutability::mutating, kIncludeWrite,
     kTextJson, protocol::ExitClass::Success, kComplete},
    {Operation::refactor_check, "refactor", "check", kNoAliases,
     "check a refactoring scope", Mutability::read_only, kIncludeRead,
     kTextJson, protocol::ExitClass::Success, kComplete},
    {Operation::refactor_plan, "refactor", "plan", kNoAliases,
     "plan a refactoring", Mutability::read_only, kIncludeRead, kTextJson,
     protocol::ExitClass::Success, kComplete},
    {Operation::refactor_apply, "refactor", "apply", kNoAliases,
     "apply a checked refactoring", Mutability::mutating, kIncludeWrite,
     kTextJson, protocol::ExitClass::Success, kComplete},
    {Operation::proof_prepare, "proof", "prepare", kNoAliases,
     "prepare proof inputs", Mutability::read_only, kProof, kTextJson,
     protocol::ExitClass::Success, kComplete},
    {Operation::proof_execute, "proof", "execute", kNoAliases,
     "execute proof orchestration", Mutability::read_only, kProof, kTextJson,
     protocol::ExitClass::Success, kCompletePartial},
    {Operation::proof_status, "proof", "status", kNoAliases,
     "report proof status", Mutability::read_only, kProof, kTextJson,
     protocol::ExitClass::Success, kComplete},
    {Operation::proof_explain, "proof", "explain", kNoAliases,
     "explain proof evidence", Mutability::read_only, kProof, kTextJson,
     protocol::ExitClass::Success, kComplete},
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

std::optional<Operation> operation_of(const CommandRequest &request) noexcept {
  return std::visit(
      [](const auto &typed) -> std::optional<Operation> {
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
          default:
            return std::nullopt;
          }
        } else if constexpr (std::is_same_v<T, QueryRequest>) {
          switch (typed.output) {
          case QueryOutput::human:
          case QueryOutput::json:
            return Operation::query;
          default:
            return std::nullopt;
          }
        } else if constexpr (std::is_same_v<T, AnalysisRequest>) {
          switch (typed.action) {
          case AnalysisAction::list:
            return Operation::analysis_list;
          case AnalysisAction::execute:
            return Operation::analysis_execute;
          case AnalysisAction::export_facts:
            return Operation::analysis_export;
          default:
            return std::nullopt;
          }
        } else if constexpr (std::is_same_v<T, WorkspaceRequest>) {
          switch (typed.action) {
          case WorkspaceAction::list:
            return Operation::workspace_list;
          case WorkspaceAction::show:
            return Operation::workspace_show;
          case WorkspaceAction::select:
            return Operation::workspace_select;
          case WorkspaceAction::refresh:
            return Operation::workspace_refresh;
          default:
            return std::nullopt;
          }
        } else if constexpr (std::is_same_v<T, AstInspectionRequest>) {
          switch (typed.action) {
          case AstInspectionAction::dump:
            return Operation::ast_dump;
          case AstInspectionAction::locals:
            return Operation::ast_locals;
          case AstInspectionAction::conditions:
            return Operation::ast_conditions;
          default:
            return std::nullopt;
          }
        } else if constexpr (std::is_same_v<T, DiffRequest>) {
          switch (typed.scope) {
          case DiffScope::file:
            return Operation::diff_file;
          case DiffScope::symbol:
            return Operation::diff_symbol;
          case DiffScope::source:
            return Operation::diff_source;
          case DiffScope::configuration:
            return Operation::diff_configuration;
          case DiffScope::index:
            return Operation::diff_index;
          default:
            return std::nullopt;
          }
        } else if constexpr (std::is_same_v<T, IncludeRequest>) {
          switch (typed.action) {
          case IncludeAction::graph:
            return Operation::include_graph;
          case IncludeAction::check:
            return Operation::include_check;
          case IncludeAction::plan:
            return Operation::include_plan;
          case IncludeAction::apply:
            return Operation::include_apply;
          default:
            return std::nullopt;
          }
        } else if constexpr (std::is_same_v<T, RefactoringRequest>) {
          switch (typed.action) {
          case RefactoringAction::check:
            return Operation::refactor_check;
          case RefactoringAction::plan:
            return Operation::refactor_plan;
          case RefactoringAction::apply:
            return Operation::refactor_apply;
          default:
            return std::nullopt;
          }
        } else {
          switch (typed.action) {
          case ProofAction::prepare:
            return Operation::proof_prepare;
          case ProofAction::execute:
            return Operation::proof_execute;
          case ProofAction::status:
            return Operation::proof_status;
          case ProofAction::explain:
            return Operation::proof_explain;
          default:
            return std::nullopt;
          }
        }
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
