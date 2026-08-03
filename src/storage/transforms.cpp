#include "storage/transforms.hpp"

#include <algorithm>
#include <functional>
#include <set>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace cidx {

const char *transform_run_status_name(TransformRunStatus status) {
  switch (status) {
  case TransformRunStatus::reused:
    return "reused";
  case TransformRunStatus::ran:
    return "ran";
  case TransformRunStatus::skipped:
    return "skipped";
  case TransformRunStatus::failed:
    return "failed";
  case TransformRunStatus::stale:
    return "stale";
  }
  return "stale";
}

const char *transform_execution_mode_name(TransformExecutionMode mode) {
  return mode == TransformExecutionMode::incremental ? "incremental" : "full";
}

const char *transform_input_kind_name(TransformInputKind kind) {
  switch (kind) {
  case TransformInputKind::source:
    return "source";
  case TransformInputKind::catalog:
    return "catalog";
  case TransformInputKind::schema:
    return "schema";
  case TransformInputKind::applicability:
    return "applicability";
  case TransformInputKind::configuration:
    return "configuration";
  case TransformInputKind::pass:
    return "pass";
  case TransformInputKind::package:
    return "package";
  case TransformInputKind::model:
    return "model";
  case TransformInputKind::implementation:
    return "implementation";
  }
  return "implementation";
}

const char *transform_applicability_name(TransformApplicability applicability) {
  return applicability == TransformApplicability::applicable ? "applicable"
                                                             : "inapplicable";
}

const char *transform_completeness_name(TransformCompleteness completeness) {
  switch (completeness) {
  case TransformCompleteness::complete:
    return "complete";
  case TransformCompleteness::partial:
    return "partial";
  case TransformCompleteness::pending:
    return "pending";
  }
  return "pending";
}

void TransformRegistry::register_source_fact(TransformSourceFact source_fact) {
  if (source_fact.name.empty() || source_fact.schema_version < 1 ||
      source_fact.catalog.empty() ||
      std::ranges::any_of(
          source_facts_,
          [&](const auto &candidate) {
            return candidate.name == source_fact.name;
          })) {
    throw std::invalid_argument("invalid or duplicate source fact: " +
                                source_fact.name);
  }
  source_facts_.push_back(std::move(source_fact));
}

void TransformRegistry::register_transform(TransformDescriptor descriptor) {
  if (descriptor.id.empty()) {
    throw std::invalid_argument("transform id must not be empty");
  }
  if (find(descriptor.id) != nullptr) {
    throw std::invalid_argument("duplicate transform id: " + descriptor.id);
  }
  descriptors_.push_back(std::move(descriptor));
}

void TransformRegistry::set_implementation_version(const std::string &id,
                                                   int version) {
  if (version < 1) {
    throw std::invalid_argument(
        "implementation provider version must be positive: " + id);
  }
  const auto it = std::ranges::find(descriptors_, id, &TransformDescriptor::id);
  if (it == descriptors_.end()) {
    throw std::invalid_argument("unknown transform implementation provider: " +
                                id);
  }
  it->implementation_provider.version = version;
  for (auto &input : it->invalidation_inputs) {
    if (input.kind != TransformInputKind::implementation) {
      continue;
    }
    input.provider_id = it->implementation_provider.provider_id;
    input.static_value = it->implementation_provider.provider_id + "|" +
                         std::to_string(version) + "|" +
                         it->implementation_provider.content;
  }
}

const TransformDescriptor *
TransformRegistry::find(const std::string &id) const {
  const auto it = std::ranges::find(descriptors_, id, &TransformDescriptor::id);
  return it == descriptors_.end() ? nullptr : &*it;
}

void TransformRegistry::validate() const {
  const auto source_fact =
      [&](const std::string &name) -> const TransformSourceFact * {
    const auto it =
        std::ranges::find(source_facts_, name, &TransformSourceFact::name);
    return it == source_facts_.end() ? nullptr : &*it;
  };
  std::unordered_map<std::string, std::string> producers;
  for (const auto &descriptor : descriptors_) {
    if (descriptor.version < 1) {
      throw std::invalid_argument("transform version must be positive: " +
                                  descriptor.id);
    }
    if (descriptor.input_facts.empty() || descriptor.produced_facts.empty()) {
      throw std::invalid_argument("transform facts must be declared: " +
                                  descriptor.id);
    }
    if (descriptor.options.empty() ||
        std::ranges::find(descriptor.options, "deterministic-sql-v1") ==
            descriptor.options.end()) {
      throw std::invalid_argument(
          "transform must declare deterministic options: " + descriptor.id);
    }
    if (descriptor.budget.max_rows < 0 ||
        descriptor.budget.max_milliseconds < 0) {
      throw std::invalid_argument("transform budget must be non-negative: " +
                                  descriptor.id);
    }
    if (descriptor.input_queries.empty() || descriptor.output_queries.empty() ||
        descriptor.output_count_query.empty()) {
      throw std::invalid_argument("transform identities must be declared: " +
                                  descriptor.id);
    }
    if (descriptor.input_schema_version < 1 ||
        descriptor.output_schema_version < 1 ||
        descriptor.input_catalog.empty() || descriptor.output_catalog.empty()) {
      throw std::invalid_argument("transform fact schema/catalog is invalid: " +
                                  descriptor.id);
    }
    if (descriptor.implementation_provider.provider_id.empty() ||
        descriptor.implementation_provider.version < 1 ||
        descriptor.implementation_provider.content.empty()) {
      throw std::invalid_argument(
          "transform implementation provider is invalid: " + descriptor.id);
    }
    std::set<std::string> declared_requirements;
    std::set<std::string> declared_inputs;
    for (const auto &fact : descriptor.input_facts) {
      if (fact.empty() || !declared_inputs.insert(fact).second) {
        throw std::invalid_argument("duplicate input fact: " + descriptor.id);
      }
    }
    for (const auto &requirement : descriptor.fact_set_requirements) {
      if (requirement.name.empty() || requirement.facts.empty() ||
          requirement.schema_version < 1 || requirement.catalog.empty() ||
          !declared_requirements.insert(requirement.name).second) {
        throw std::invalid_argument("invalid produced fact-set declaration: " +
                                    descriptor.id);
      }
      std::set<std::string> members;
      for (const auto &fact : requirement.facts) {
        if (fact.empty() || !members.insert(fact).second) {
          throw std::invalid_argument(
              "duplicate fact-set member: " + descriptor.id + " -> " +
              requirement.name);
        }
      }
    }
    for (const auto &fact : descriptor.produced_facts) {
      if (fact.empty() || producers.contains(fact)) {
        throw std::invalid_argument("duplicate produced fact: " + fact);
      }
      producers.emplace(fact, descriptor.id);
      if (!declared_requirements.contains(fact)) {
        throw std::invalid_argument("produced fact-set is not registered: " +
                                    descriptor.id + " -> " + fact);
      }
    }
    for (const auto &requirement : descriptor.fact_set_requirements) {
      for (const auto &fact : requirement.facts) {
        const auto producer = producers.find(fact);
        if (producer == producers.end() || producer->second != descriptor.id) {
          throw std::invalid_argument("fact-set member is not produced by its "
                                      "declared provider: " +
                                      descriptor.id + " -> " + fact);
        }
        if (requirement.schema_version != descriptor.output_schema_version ||
            requirement.catalog != descriptor.output_catalog) {
          throw std::invalid_argument(
              "produced fact-set schema mismatch: " + descriptor.id + " -> " +
              requirement.name);
        }
      }
    }
    std::set<std::string> typed_keys;
    for (const auto &input : descriptor.invalidation_inputs) {
      if (input.name.empty() || !typed_keys.insert(input.name).second) {
        throw std::invalid_argument("invalid typed invalidation input: " +
                                    descriptor.id);
      }
      if (input.provider_id.empty() ||
          (input.value_query.empty() && input.static_value.empty())) {
        throw std::invalid_argument("invalidation input has no provider: " +
                                    descriptor.id + " -> " + input.name);
      }
    }
    if (descriptor.publication_rule !=
            TransformPublicationRule::preserve_previous_on_failure &&
        descriptor.publication_rule !=
            TransformPublicationRule::atomic_generation) {
      throw std::invalid_argument("invalid publication rule: " + descriptor.id);
    }
    std::set<std::string> invalidation_keys(
        descriptor.invalidation_keys.begin(),
        descriptor.invalidation_keys.end());
    if (invalidation_keys.size() != descriptor.invalidation_keys.size()) {
      throw std::invalid_argument("duplicate invalidation key: " +
                                  descriptor.id);
    }
    if (invalidation_keys.size() != descriptor.invalidation_inputs.size()) {
      throw std::invalid_argument(
          "typed invalidation inputs do not match keys: " + descriptor.id);
    }
    for (const auto &key : invalidation_keys) {
      if (!typed_keys.contains(key)) {
        throw std::invalid_argument("invalidation key has no typed provider: " +
                                    descriptor.id + " -> " + key);
      }
    }
    for (const auto &dependency : descriptor.dependencies) {
      if (dependency == descriptor.id || find(dependency) == nullptr) {
        throw std::invalid_argument("undeclared transform dependency: " +
                                    descriptor.id + " -> " + dependency);
      }
    }
  }

  for (const auto &descriptor : descriptors_) {
    for (const auto &fact : descriptor.input_facts) {
      const auto producer = producers.find(fact);
      if (producer == producers.end()) {
        const auto *source = source_fact(fact);
        if (source == nullptr) {
          throw std::invalid_argument(
              "undeclared input fact: " + descriptor.id + " -> " + fact);
        }
        if (source->schema_version != descriptor.input_schema_version ||
            source->catalog != descriptor.input_catalog) {
          throw std::invalid_argument("incompatible source fact schema: " +
                                      descriptor.id + " -> " + fact);
        }
        continue;
      }
      if (std::ranges::find(descriptor.dependencies, producer->second) ==
          descriptor.dependencies.end()) {
        throw std::invalid_argument("missing fact dependency: " +
                                    descriptor.id + " -> " + producer->second);
      }
      const auto *producer_descriptor = find(producer->second);
      const auto requirement = std::ranges::find_if(
          producer_descriptor->fact_set_requirements,
          [&](const auto &candidate) { return candidate.name == fact; });
      if (requirement == producer_descriptor->fact_set_requirements.end() ||
          requirement->schema_version != descriptor.input_schema_version ||
          requirement->catalog != descriptor.input_catalog) {
        throw std::invalid_argument(
            "incompatible fact-set schema: " + descriptor.id + " -> " + fact);
      }
    }
  }

  std::unordered_map<std::string, int> marks;
  const std::function<void(const TransformDescriptor &)> visit =
      [&](const TransformDescriptor &descriptor) {
        const int mark = marks[descriptor.id];
        if (mark == 1) {
          throw std::invalid_argument("transform dependency cycle at: " +
                                      descriptor.id);
        }
        if (mark == 2) {
          return;
        }
        marks[descriptor.id] = 1;
        for (const auto &dependency : descriptor.dependencies) {
          visit(*find(dependency));
        }
        marks[descriptor.id] = 2;
      };
  for (const auto &descriptor : descriptors_) {
    visit(descriptor);
  }
}

std::vector<const TransformDescriptor *>
TransformRegistry::execution_order() const {
  validate();
  std::vector<const TransformDescriptor *> ordered;
  std::set<std::string> visited;
  const std::function<void(const TransformDescriptor &)> visit =
      [&](const TransformDescriptor &descriptor) {
        if (!visited.insert(descriptor.id).second) {
          return;
        }
        for (const auto &dependency : descriptor.dependencies) {
          visit(*find(dependency));
        }
        ordered.push_back(&descriptor);
      };
  for (const auto &descriptor : descriptors_) {
    visit(descriptor);
  }
  return ordered;
}

} // namespace cidx
