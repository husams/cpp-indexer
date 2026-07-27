// Canonical JSON codec for the ExtractionPlan IR (docs/extraction-plan.md).
//
// Deliberately self-contained: it does not reuse cli::json_out::Value /
// json_read::parse (those are product.cli-owned per ADR-011, and this module
// sits below product-surface in the dependency direction) and does not
// introduce a new cross-layer manifest exception. The plan's JSON shape is
// small and fixed, so a focused encoder/parser scoped to ExtractionPlan is
// both architecturally clean and a more precise conformance check than a
// generic value tree would be.
//
// The parser is intentionally hostile-input-safe: a plan is untrusted content
// (HSE-64 fuzzing requirement). It never has undefined behavior on malformed
// text, always fails closed with a PlanParseError, and caps nesting so a
// pathological document cannot exhaust the stack.
#pragma once

#include "extract/plan_ir.hpp"

#include <stdexcept>
#include <string>
#include <string_view>

namespace cidx::extract {

class PlanParseError final : public std::runtime_error {
public:
  PlanParseError(const std::string &message, std::size_t offset);

  [[nodiscard]] std::size_t offset() const noexcept { return offset_; }

private:
  std::size_t offset_;
};

// Deterministic: identical plans always produce byte-identical text (stable
// field order, arrays kept in declaration order, no floating point, no
// insertion-order-dependent maps).
[[nodiscard]] std::string canonical_json(const ExtractionPlan &plan);

// Throws PlanParseError on malformed JSON, an unknown/missing required field,
// a field of the wrong shape, or nesting beyond kExtractionPlanMaxJsonDepth.
// Never throws anything else and never has undefined behavior on arbitrary
// input -- this is the entry point plan/package fuzzing drives.
[[nodiscard]] ExtractionPlan parse_plan_json(std::string_view text);

inline constexpr int kExtractionPlanMaxJsonDepth = 64;

} // namespace cidx::extract
