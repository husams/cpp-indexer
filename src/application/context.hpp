// Application execution context.  Product adapters provide sinks and
// policies; services never need terminal streams or a particular CLI.
#pragma once

#include <atomic>
#include <cstdint>
#include <string_view>

#include "query/result_protocol.hpp"

namespace cidx::application {

enum class AccessMode : std::uint8_t { read_only, read_write };

struct ApplicationPolicy {
  AccessMode access = AccessMode::read_write;
  bool allow_schema_migration = false;
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
  explicit ApplicationContext(ApplicationPolicy policy = {})
      : policy_(policy) {}

  [[nodiscard]] const ApplicationPolicy &policy() const noexcept {
    return policy_;
  }
  [[nodiscard]] const CancellationToken &cancellation() const noexcept {
    return cancellation_;
  }
  [[nodiscard]] CancellationToken &cancellation() noexcept {
    return cancellation_;
  }
  [[nodiscard]] ProgressSink *progress() const noexcept { return progress_; }
  [[nodiscard]] ArtifactStore *artifacts() const noexcept { return artifacts_; }

  void set_progress_sink(ProgressSink *sink) noexcept { progress_ = sink; }
  void set_artifact_store(ArtifactStore *store) noexcept { artifacts_ = store; }

  void publish(protocol::ProgressEvent event) const {
    if (progress_ != nullptr) {
      progress_->publish(event);
    }
  }

private:
  ApplicationPolicy policy_;
  CancellationToken cancellation_;
  ProgressSink *progress_ = nullptr;
  ArtifactStore *artifacts_ = nullptr;
};

} // namespace cidx::application
