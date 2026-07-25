#include "application/services.hpp"

#include <string>
#include <type_traits>
#include <utility>

namespace cidx::application {
namespace {

protocol::ResultEnvelope error_envelope(Operation operation, std::string code,
                                        std::string message) {
  protocol::ResultEnvelope result;
  const CommandMetadata *entry = metadata(operation);
  result.operation =
      entry == nullptr ? "application" : std::string(entry->group);
  result.status = code == "policy_refuted" ? protocol::Status::Refuted
                                           : protocol::Status::Error;
  result.completeness.state = "unknown";
  result.diagnostics.push_back(
      protocol::Diagnostic{.code = std::move(code),
                           .severity = "error",
                           .message = std::move(message)});
  return result;
}

} // namespace

protocol::ResultEnvelope ApplicationService::failure(Operation operation,
                                                     std::string code,
                                                     std::string message) {
  return error_envelope(operation, std::move(code), std::move(message));
}

protocol::ResultEnvelope
ApplicationService::execute(const CommandRequest &request,
                            ApplicationContext &context) const {
  const Operation operation = operation_of(request);
  const CommandMetadata *entry = metadata(operation);
  if (entry == nullptr) {
    return failure(operation, "backend_error", "operation is not registered");
  }
  if (context.cancellation().cancelled()) {
    return failure(operation, "timeout", "operation was cancelled");
  }
  if (context.policy().access == AccessMode::read_only &&
      entry->mutability == Mutability::mutating) {
    return failure(
        operation, "policy_refuted",
        "read-only application context rejects a mutating operation");
  }

  return std::visit(
      [this, &context](const auto &typed) -> protocol::ResultEnvelope {
        using T = std::decay_t<decltype(typed)>;
        if constexpr (std::is_same_v<T, IndexRequest>) {
          if (!handlers_.index) {
            return failure(operation_of(CommandRequest{typed}), "backend_error",
                           "index service is not installed");
          }
          return handlers_.index(typed, context);
        } else if constexpr (std::is_same_v<T, QueryRequest>) {
          if (!handlers_.query) {
            return failure(Operation::query, "backend_error",
                           "query service is not installed");
          }
          return handlers_.query(typed, context);
        } else if constexpr (std::is_same_v<T, AnalysisRequest>) {
          if (!handlers_.analysis) {
            return failure(operation_of(CommandRequest{typed}), "backend_error",
                           "analysis service is not installed");
          }
          return handlers_.analysis(typed, context);
        } else if constexpr (std::is_same_v<T, AstInspectionRequest>) {
          if (!handlers_.ast_inspection) {
            return failure(operation_of(CommandRequest{typed}), "backend_error",
                           "AST inspection service is not installed");
          }
          return handlers_.ast_inspection(typed, context);
        } else {
          if (!handlers_.diff) {
            return failure(operation_of(CommandRequest{typed}), "backend_error",
                           "diff service is not installed");
          }
          return handlers_.diff(typed, context);
        }
      },
      request);
}

} // namespace cidx::application
