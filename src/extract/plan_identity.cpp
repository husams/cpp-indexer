#include "extract/plan_identity.hpp"

#include "extract/plan_json.hpp"
#include "util/hashing.hpp"

namespace cidx::extract {

std::string plan_hash(const ExtractionPlan &plan) {
  // cidx::sha256_hex() already returns an algorithm-tagged "sha256:<hex>"
  // digest (util/hashing.hpp); do not prepend a second "sha256:" prefix.
  return cidx::sha256_hex(canonical_json(plan));
}

std::string artifact_identity(const ExtractionPlan &plan,
                              const ExecutionIdentityInput &input) {
  // A record-separator byte between fields: none of the inputs can contain
  // it, so this cannot be confused with a different split of the same bytes
  // (e.g. workspace_identity="a"+tu_identity="b" vs "ab"+"").
  constexpr char kFieldSep = '\x1e';
  std::string combined;
  combined.reserve(plan_hash(plan).size() + input.workspace_identity.size() +
                   input.tu_identity.size() +
                   input.tu_content_fingerprint.size() + 4);
  combined += plan_hash(plan);
  combined += kFieldSep;
  combined += input.workspace_identity;
  combined += kFieldSep;
  combined += input.tu_identity;
  combined += kFieldSep;
  combined += input.tu_content_fingerprint;
  return cidx::sha256_hex(combined);
}

} // namespace cidx::extract
