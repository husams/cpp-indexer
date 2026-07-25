#include "extract/plan_identity.hpp"

#include "extract/plan_json.hpp"
#include "util/hashing.hpp"

namespace cidx::extract {

std::string plan_hash(const ExtractionPlan &plan) {
  return "sha256:" + cidx::sha256_hex(canonical_json(plan));
}

} // namespace cidx::extract
