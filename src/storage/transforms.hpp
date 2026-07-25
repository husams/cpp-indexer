// Named, dependency-ordered derived-fact transforms.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace cidx {

enum class TransformRunStatus : std::uint8_t {
  reused,
  ran,
  skipped,
  failed,
  stale
};

struct TransformDescriptor {
  std::string id;
  int version = 0;
  std::vector<std::string> input_facts;
  std::vector<std::string> produced_facts;
  std::vector<std::string> dependencies;
  std::vector<std::string> invalidation_keys;
  std::vector<std::string> options;
  std::vector<std::string> input_queries;
  std::vector<std::string> output_queries;
  std::string output_count_query;
};

struct TransformRun {
  std::string transform_id;
  int version = 0;
  TransformRunStatus status = TransformRunStatus::stale;
  std::string input_identity;
  std::string output_identity;
  std::int64_t output_count = 0;
  std::string diagnostic;
};

struct TransformReport {
  std::vector<TransformRun> runs;
  int still_stub_count = 0;
};

[[nodiscard]] const char *transform_run_status_name(TransformRunStatus status);

class TransformRegistry {
public:
  void register_transform(TransformDescriptor descriptor);

  [[nodiscard]] const TransformDescriptor *find(const std::string &id) const;
  [[nodiscard]] const std::vector<TransformDescriptor> &descriptors() const {
    return descriptors_;
  }
  [[nodiscard]] std::vector<const TransformDescriptor *>
  execution_order() const;
  void validate() const;

private:
  std::vector<TransformDescriptor> descriptors_;
};

} // namespace cidx
