#include "extract/extension_facts.hpp"

#include <algorithm>
#include <sstream>
#include <tuple>

namespace cidx::extract {

namespace {

auto node_key(const ExtensionNodeFact &fact) {
  return std::tie(fact.provenance.plan_hash, fact.provenance.artifact_identity,
                  fact.provenance.rule_id, fact.namespace_name, fact.node_kind,
                  fact.identity, fact.evidence.file, fact.evidence.line,
                  fact.evidence.col);
}

auto relation_key(const ExtensionRelationFact &fact) {
  return std::tie(fact.provenance.plan_hash, fact.provenance.artifact_identity,
                  fact.provenance.rule_id, fact.namespace_name,
                  fact.relation_kind, fact.from_identity, fact.to_identity,
                  fact.evidence.file, fact.evidence.line, fact.evidence.col);
}

auto attribute_key(const ExtensionAttributeFact &fact) {
  return std::tie(fact.provenance.plan_hash, fact.provenance.artifact_identity,
                  fact.provenance.rule_id, fact.namespace_name,
                  fact.attribute_name, fact.identity, fact.value,
                  fact.evidence.file, fact.evidence.line, fact.evidence.col);
}

auto unknown_key(const ExtensionUnknownFact &fact) {
  return std::tie(fact.provenance.plan_hash, fact.provenance.artifact_identity,
                  fact.provenance.rule_id, fact.namespace_name,
                  fact.reason_code, fact.identity, fact.evidence.file,
                  fact.evidence.line, fact.evidence.col);
}

template <typename T, typename KeyFn>
void sort_and_dedup(std::vector<T> &items, KeyFn key_fn) {
  std::ranges::sort(
      items, [&](const T &a, const T &b) { return key_fn(a) < key_fn(b); });
  items.erase(std::unique(items.begin(), items.end(),
                          [&](const T &a, const T &b) {
                            return key_fn(a) == key_fn(b);
                          }),
              items.end());
}

std::string completeness_tag(DeclaredCompleteness completeness) {
  return to_string(completeness);
}

} // namespace

void InMemoryExtensionFactSink::emit(const ExtensionNodeFact &fact) {
  nodes_.push_back(fact);
}
void InMemoryExtensionFactSink::emit(const ExtensionRelationFact &fact) {
  relations_.push_back(fact);
}
void InMemoryExtensionFactSink::emit(const ExtensionAttributeFact &fact) {
  attributes_.push_back(fact);
}
void InMemoryExtensionFactSink::emit(const ExtensionUnknownFact &fact) {
  unknowns_.push_back(fact);
}

void InMemoryExtensionFactSink::canonicalize() {
  sort_and_dedup(nodes_, node_key);
  sort_and_dedup(relations_, relation_key);
  sort_and_dedup(attributes_, attribute_key);
  sort_and_dedup(unknowns_, unknown_key);
}

std::string InMemoryExtensionFactSink::canonical_text() const {
  std::ostringstream out;
  for (const auto &fact : nodes_) {
    out << "node\t" << fact.provenance.plan_hash << '\t'
        << fact.provenance.artifact_identity << '\t' << fact.provenance.rule_id
        << '\t' << fact.provenance.producer_package << '\t'
        << fact.provenance.producer_version << '\t'
        << completeness_tag(fact.provenance.completeness) << '\t'
        << fact.namespace_name << '\t' << fact.node_kind << '\t'
        << fact.identity << '\t' << fact.evidence.file << ':'
        << fact.evidence.line << ':' << fact.evidence.col << '\n';
  }
  for (const auto &fact : relations_) {
    out << "relation\t" << fact.provenance.plan_hash << '\t'
        << fact.provenance.artifact_identity << '\t' << fact.provenance.rule_id
        << '\t' << fact.provenance.producer_package << '\t'
        << fact.provenance.producer_version << '\t'
        << completeness_tag(fact.provenance.completeness) << '\t'
        << fact.namespace_name << '\t' << fact.relation_kind << '\t'
        << fact.from_identity << '\t' << fact.to_identity << '\t'
        << fact.evidence.file << ':' << fact.evidence.line << ':'
        << fact.evidence.col << '\n';
  }
  for (const auto &fact : attributes_) {
    out << "attribute\t" << fact.provenance.plan_hash << '\t'
        << fact.provenance.artifact_identity << '\t' << fact.provenance.rule_id
        << '\t' << fact.provenance.producer_package << '\t'
        << fact.provenance.producer_version << '\t'
        << completeness_tag(fact.provenance.completeness) << '\t'
        << fact.namespace_name << '\t' << fact.attribute_name << '\t'
        << fact.identity << '\t' << fact.value << '\t' << fact.evidence.file
        << ':' << fact.evidence.line << ':' << fact.evidence.col << '\n';
  }
  for (const auto &fact : unknowns_) {
    out << "unknown\t" << fact.provenance.plan_hash << '\t'
        << fact.provenance.artifact_identity << '\t' << fact.provenance.rule_id
        << '\t' << fact.provenance.producer_package << '\t'
        << fact.provenance.producer_version << '\t'
        << completeness_tag(fact.provenance.completeness) << '\t'
        << fact.namespace_name << '\t' << fact.reason_code << '\t'
        << fact.identity << '\t' << fact.evidence.file << ':'
        << fact.evidence.line << ':' << fact.evidence.col << '\n';
  }
  return out.str();
}

} // namespace cidx::extract
