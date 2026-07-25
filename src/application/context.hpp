// Application execution context. Product adapters provide one selected
// workspace snapshot and capability-scoped ports; services never receive
// terminal streams or a legacy CLI context.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "application/requests.hpp"
#include "query/exec.hpp"
#include "query/result_protocol.hpp"
#include "storage/ports.hpp"
#include "util/logger.hpp"
#include "workspace/context.hpp"

namespace cidx::application {

class ApplicationContext;

class IndexServicePort {
public:
  virtual ~IndexServicePort() = default;
  virtual protocol::ResultEnvelope execute(const IndexRequest &request,
                                           ApplicationContext &context) = 0;
};

class AnalysisServicePort {
public:
  virtual ~AnalysisServicePort() = default;
  virtual protocol::ResultEnvelope execute(const AnalysisRequest &request,
                                           ApplicationContext &context) = 0;
};

class AstServicePort {
public:
  virtual ~AstServicePort() = default;
  virtual protocol::ResultEnvelope execute(const AstInspectionRequest &request,
                                           ApplicationContext &context) = 0;
};

class DiffServicePort {
public:
  virtual ~DiffServicePort() = default;
  virtual protocol::ResultEnvelope execute(const DiffRequest &request,
                                           ApplicationContext &context) = 0;
};

class IncludeServicePort {
public:
  virtual ~IncludeServicePort() = default;
  virtual protocol::ResultEnvelope execute(const IncludeRequest &request,
                                           ApplicationContext &context) = 0;
};

struct ApplicationOperationPorts {
  IndexServicePort *index = nullptr;
  AnalysisServicePort *analysis = nullptr;
  AstServicePort *ast = nullptr;
  DiffServicePort *diff = nullptr;
  IncludeServicePort *include = nullptr;
};

enum class AccessMode : std::uint8_t { read_only, read_write };

enum class Capability : std::uint8_t {
  index_read,
  index_write,
  analysis,
  ast,
  diff,
  include_read,
  include_write,
  workspace_write,
  proof,
  artifacts,
  schema_migration,
};

using CapabilityMask = std::uint16_t;

[[nodiscard]] constexpr CapabilityMask capability_bit(Capability capability) {
  return static_cast<CapabilityMask>(1U << static_cast<unsigned>(capability));
}

[[nodiscard]] constexpr CapabilityMask all_capabilities() {
  return capability_bit(Capability::index_read) |
         capability_bit(Capability::index_write) |
         capability_bit(Capability::analysis) |
         capability_bit(Capability::ast) | capability_bit(Capability::diff) |
         capability_bit(Capability::include_read) |
         capability_bit(Capability::include_write) |
         capability_bit(Capability::workspace_write) |
         capability_bit(Capability::proof) |
         capability_bit(Capability::artifacts) |
         capability_bit(Capability::schema_migration);
}

struct ApplicationPolicy {
  AccessMode access = AccessMode::read_write;
  CapabilityMask capabilities = all_capabilities();
  bool allow_schema_migration = false;
  std::size_t max_work_items = 0;
  std::size_t max_diagnostics = 1000;
};

struct ApplicationReadPorts {
  storage::WorkspaceCatalogReadPort *workspace = nullptr;
  storage::SourceStoreReadPort *source = nullptr;
  storage::SymbolReadPort *symbols = nullptr;
  storage::TypeReadPort *types = nullptr;
  storage::FactReadPort *facts = nullptr;
  storage::DefinitionReadPort *definitions = nullptr;
  storage::IncludeReadPort *includes = nullptr;
  storage::SchemaCatalogReadPort *schema = nullptr;
  query::QueryReadPort *query = nullptr;
};

struct ApplicationWritePorts {
  storage::WorkspaceCatalogWritePort *workspace = nullptr;
  storage::SourceStoreWritePort *source = nullptr;
  storage::SymbolWritePort *symbols = nullptr;
  storage::TypeWritePort *types = nullptr;
  storage::FactWritePort *facts = nullptr;
  storage::DefinitionWritePort *definitions = nullptr;
  storage::IncludeWritePort *includes = nullptr;
  storage::UnitOfWorkFactory *unit_of_work = nullptr;
};

class CancellationToken final {
public:
  void cancel() noexcept { cancelled_.store(true, std::memory_order_relaxed); }
  [[nodiscard]] bool cancelled() const noexcept {
    return cancelled_.load(std::memory_order_relaxed);
  }

private:
  std::atomic_bool cancelled_{false};
};

class ProgressSink {
public:
  virtual ~ProgressSink() = default;
  virtual void publish(const protocol::ProgressEvent &event) = 0;
};

class ArtifactStore {
public:
  virtual ~ArtifactStore() = default;
  virtual protocol::ArtifactRef publish(std::string_view kind,
                                        std::string_view identity) = 0;
};

class ApplicationContext final {
public:
  // Kept for hermetic service tests that do not need a persistence adapter.
  explicit ApplicationContext(ApplicationPolicy policy = {})
      : policy_(policy) {}

  ApplicationContext(ApplicationPolicy policy,
                     ApplicationOperationPorts operations)
      : policy_(policy), operations_(operations) {}

  ApplicationContext(WorkspaceContext &workspace, ApplicationPolicy policy,
                     ApplicationReadPorts read_ports = {},
                     ApplicationWritePorts write_ports = {},
                     ApplicationOperationPorts operations = {},
                     Logger *logger = nullptr)
      : workspace_(&workspace), policy_(policy), read_ports_(read_ports),
        write_ports_(write_ports), logger_(logger), operations_(operations) {}

  [[nodiscard]] const ApplicationPolicy &policy() const noexcept {
    return policy_;
  }
  [[nodiscard]] const CancellationToken &cancellation() const noexcept {
    return cancellation_;
  }
  [[nodiscard]] CancellationToken &cancellation() noexcept {
    return cancellation_;
  }
  [[nodiscard]] WorkspaceContext *workspace() const noexcept {
    return workspace_;
  }
  [[nodiscard]] const ApplicationReadPorts &read_ports() const noexcept {
    return read_ports_;
  }
  [[nodiscard]] const ApplicationWritePorts &write_ports() const noexcept {
    return write_ports_;
  }
  [[nodiscard]] Logger *logger() const noexcept { return logger_; }
  [[nodiscard]] const ApplicationOperationPorts &operations() const noexcept {
    return operations_;
  }
  [[nodiscard]] ProgressSink *progress() const noexcept { return progress_; }
  [[nodiscard]] ArtifactStore *artifacts() const noexcept { return artifacts_; }

  [[nodiscard]] bool permits(CapabilityMask required) const noexcept {
    return (policy_.capabilities & required) == required;
  }

  void set_progress_sink(ProgressSink *sink) noexcept { progress_ = sink; }
  void set_artifact_store(ArtifactStore *store) noexcept { artifacts_ = store; }

  void publish(const protocol::ProgressEvent &event) const {
    if (progress_ != nullptr) {
      progress_->publish(event);
    }
  }

private:
  WorkspaceContext *workspace_ = nullptr;
  ApplicationPolicy policy_;
  CancellationToken cancellation_;
  ApplicationReadPorts read_ports_;
  ApplicationWritePorts write_ports_;
  Logger *logger_ = nullptr;
  ApplicationOperationPorts operations_;
  ProgressSink *progress_ = nullptr;
  ArtifactStore *artifacts_ = nullptr;
};

} // namespace cidx::application
