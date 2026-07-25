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

enum class TransformInputKind : std::uint8_t {
  source,
  catalog,
  schema,
  applicability,
  configuration,
  pass,
  package,
  model,
  implementation
};

enum class TransformApplicability : std::uint8_t { applicable, inapplicable };
enum class TransformCompleteness : std::uint8_t { complete, partial, pending };
enum class TransformPublicationRule : std::uint8_t {
  atomic_generation,
  preserve_previous_on_failure
};

struct TransformInvalidationInput {
  std::string name;
  TransformInputKind kind = TransformInputKind::source;
  // Stable provider identity; labels alone are not valid invalidation input.
  std::string provider_id;
  std::string value_query;
  std::string static_value;
};

struct TransformBudget {
  std::int64_t max_rows = 0;
  std::int64_t max_milliseconds = 0;
};

struct TransformFactSetRequirement {
  std::string name;
  std::vector<std::string> facts;
  int schema_version = 1;
  std::string catalog = "cidx-core";
  bool required = true;
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
  std::vector<TransformInvalidationInput> invalidation_inputs;
  TransformApplicability applicability = TransformApplicability::applicable;
  TransformCompleteness completeness = TransformCompleteness::complete;
  TransformPublicationRule publication_rule =
      TransformPublicationRule::preserve_previous_on_failure;
  TransformBudget budget;
  std::vector<TransformFactSetRequirement> fact_set_requirements;
  int input_schema_version = 1;
  int output_schema_version = 1;
  std::string input_catalog = "cidx-core";
  std::string output_catalog = "cidx-core";
};

struct TransformRun {
  std::string transform_id;
  int version = 0;
  TransformRunStatus status = TransformRunStatus::stale;
  std::string input_identity;
  std::string output_identity;
  std::int64_t output_count = 0;
  std::string diagnostic;
  std::uint64_t generation = 0;
  std::uint64_t published_generation = 0;
  TransformApplicability applicability = TransformApplicability::applicable;
  TransformCompleteness completeness = TransformCompleteness::complete;
  std::vector<std::string> changed_inputs;
};

struct TransformReport {
  std::vector<TransformRun> runs;
  int still_stub_count = 0;
  bool failed = false;
  bool complete = false;
  std::vector<std::string> affected_transforms;
  std::vector<std::string> missing_fact_sets;
};

struct TransformFactSetStatus {
  std::string name;
  int schema_version = 0;
  std::string catalog;
  bool known = false;
  bool ready = false;
  TransformRunStatus status = TransformRunStatus::stale;
  std::string diagnostic;
};

[[nodiscard]] const char *transform_run_status_name(TransformRunStatus status);
[[nodiscard]] const char *transform_input_kind_name(TransformInputKind kind);
[[nodiscard]] const char *transform_applicability_name(
    TransformApplicability applicability);
[[nodiscard]] const char *transform_completeness_name(
    TransformCompleteness completeness);

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
