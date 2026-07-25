// Application-service execution boundary.  Concrete product surfaces install
// handlers; the service owns typed dispatch, policy checks, cancellation, and
// the result/event contract.
#pragma once

#include <functional>
#include <string>
#include <utility>

#include "application/context.hpp"
#include "application/registry.hpp"

namespace cidx::application {

using IndexHandler = std::function<protocol::ResultEnvelope(
    const IndexRequest &, ApplicationContext &)>;
using QueryHandler = std::function<protocol::ResultEnvelope(
    const QueryRequest &, ApplicationContext &)>;
using AnalysisHandler = std::function<protocol::ResultEnvelope(
    const AnalysisRequest &, ApplicationContext &)>;
using AstInspectionHandler = std::function<protocol::ResultEnvelope(
    const AstInspectionRequest &, ApplicationContext &)>;
using DiffHandler = std::function<protocol::ResultEnvelope(
    const DiffRequest &, ApplicationContext &)>;

struct ApplicationHandlers {
  IndexHandler index;
  QueryHandler query;
  AnalysisHandler analysis;
  AstInspectionHandler ast_inspection;
  DiffHandler diff;
};

class ApplicationService final {
public:
  explicit ApplicationService(ApplicationHandlers handlers)
      : handlers_(std::move(handlers)) {}

  [[nodiscard]] protocol::ResultEnvelope
  execute(const CommandRequest &request, ApplicationContext &context) const;

private:
  [[nodiscard]] static protocol::ResultEnvelope
  failure(Operation operation, std::string code, std::string message);

  ApplicationHandlers handlers_;
};

} // namespace cidx::application
