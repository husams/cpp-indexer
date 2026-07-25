#include "storage/transforms.hpp"

#include <algorithm>
#include <functional>
#include <set>
#include <stdexcept>
#include <unordered_map>
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

void TransformRegistry::register_transform(TransformDescriptor descriptor) {
  if (descriptor.id.empty()) {
    throw std::invalid_argument("transform id must not be empty");
  }
  if (find(descriptor.id) != nullptr) {
    throw std::invalid_argument("duplicate transform id: " + descriptor.id);
  }
  descriptors_.push_back(std::move(descriptor));
}

const TransformDescriptor *
TransformRegistry::find(const std::string &id) const {
  const auto it = std::ranges::find(descriptors_, id, &TransformDescriptor::id);
  return it == descriptors_.end() ? nullptr : &*it;
}

void TransformRegistry::validate() const {
  for (const auto &descriptor : descriptors_) {
    if (descriptor.version < 1) {
      throw std::invalid_argument("transform version must be positive: " +
                                  descriptor.id);
    }
    if (descriptor.input_facts.empty() || descriptor.produced_facts.empty()) {
      throw std::invalid_argument("transform facts must be declared: " +
                                  descriptor.id);
    }
    if (descriptor.input_queries.empty() || descriptor.output_queries.empty() ||
        descriptor.output_count_query.empty()) {
      throw std::invalid_argument("transform identities must be declared: " +
                                  descriptor.id);
    }
    std::set<std::string> invalidation_keys(
        descriptor.invalidation_keys.begin(),
        descriptor.invalidation_keys.end());
    if (invalidation_keys.size() != descriptor.invalidation_keys.size()) {
      throw std::invalid_argument("duplicate invalidation key: " +
                                  descriptor.id);
    }
    for (const auto &dependency : descriptor.dependencies) {
      if (dependency == descriptor.id || find(dependency) == nullptr) {
        throw std::invalid_argument("undeclared transform dependency: " +
                                    descriptor.id + " -> " + dependency);
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
