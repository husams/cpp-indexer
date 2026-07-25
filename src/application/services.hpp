// Application-service execution boundary. Concrete services receive typed
// requests and capability-scoped context ports; CLI adapters only translate
// argv and render the returned result envelope.
#pragma once

#include <optional>
#include <string>
#include <utility>

#include "application/context.hpp"
#include "application/registry.hpp"

namespace cidx {
class Storage;
namespace application {

class StorageApplicationOperations final : public IndexServicePort,
                                           public AnalysisServicePort,
                                           public AstServicePort,
                                           public DiffServicePort,
                                           public IncludeServicePort {
public:
  explicit StorageApplicationOperations(Storage &storage,
                                        std::string index_path = {})
      : storage_(storage), index_path_(std::move(index_path)) {}

  protocol::ResultEnvelope execute(const IndexRequest &request,
                                   ApplicationContext &context) override;
  protocol::ResultEnvelope execute(const AnalysisRequest &request,
                                   ApplicationContext &context) override;
  protocol::ResultEnvelope execute(const AstInspectionRequest &request,
                                   ApplicationContext &context) override;
  protocol::ResultEnvelope execute(const DiffRequest &request,
                                   ApplicationContext &context) override;
  protocol::ResultEnvelope execute(const IncludeRequest &request,
                                   ApplicationContext &context) override;

private:
  Storage &storage_;
  std::string index_path_;
};

class ApplicationServices {
public:
  virtual ~ApplicationServices() = default;
  virtual protocol::ResultEnvelope index(const IndexRequest &,
                                         ApplicationContext &) const = 0;
  virtual protocol::ResultEnvelope query(const QueryRequest &,
                                         ApplicationContext &) const = 0;
  virtual protocol::ResultEnvelope analysis(const AnalysisRequest &,
                                            ApplicationContext &) const = 0;
  virtual protocol::ResultEnvelope workspace(const WorkspaceRequest &,
                                             ApplicationContext &) const = 0;
  virtual protocol::ResultEnvelope ast(const AstInspectionRequest &,
                                       ApplicationContext &) const = 0;
  virtual protocol::ResultEnvelope diff(const DiffRequest &,
                                        ApplicationContext &) const = 0;
  virtual protocol::ResultEnvelope include(const IncludeRequest &,
                                           ApplicationContext &) const = 0;
  virtual protocol::ResultEnvelope refactor(const RefactoringRequest &,
                                            ApplicationContext &) const = 0;
  virtual protocol::ResultEnvelope proof(const ProofRequest &,
                                         ApplicationContext &) const = 0;
};

// The default product service set is deliberately independent of CLI state.
// It uses QueryReadPort and the HSE-61 workspace/persistence ports directly;
// product-specific adapters may replace individual services in tests or
// future surfaces without changing request dispatch or policy enforcement.
class DefaultApplicationServices final : public ApplicationServices {
public:
  protocol::ResultEnvelope index(const IndexRequest &request,
                                 ApplicationContext &context) const override;
  protocol::ResultEnvelope query(const QueryRequest &request,
                                 ApplicationContext &context) const override;
  protocol::ResultEnvelope analysis(const AnalysisRequest &request,
                                    ApplicationContext &context) const override;
  protocol::ResultEnvelope
  workspace(const WorkspaceRequest &request,
            ApplicationContext &context) const override;
  protocol::ResultEnvelope ast(const AstInspectionRequest &request,
                               ApplicationContext &context) const override;
  protocol::ResultEnvelope diff(const DiffRequest &request,
                                ApplicationContext &context) const override;
  protocol::ResultEnvelope include(const IncludeRequest &request,
                                   ApplicationContext &context) const override;
  protocol::ResultEnvelope refactor(const RefactoringRequest &request,
                                    ApplicationContext &context) const override;
  protocol::ResultEnvelope proof(const ProofRequest &request,
                                 ApplicationContext &context) const override;
};

class ApplicationService final {
public:
  explicit ApplicationService(const ApplicationServices &services)
      : services_(services) {}

  [[nodiscard]] protocol::ResultEnvelope
  execute(const CommandRequest &request, ApplicationContext &context) const;

private:
  [[nodiscard]] static protocol::ResultEnvelope
  failure(const std::optional<Operation> &operation, std::string code,
          std::string message);

  const ApplicationServices &services_;
};

} // namespace application
} // namespace cidx
