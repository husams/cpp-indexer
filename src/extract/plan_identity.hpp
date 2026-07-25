// Deterministic ExtractionPlan and artifact identity.
//
// `plan_hash` is sha256("sha256:<hex>") over the plan's canonical JSON
// (plan_json.hpp) alone: the identity of the RULE/PLAN itself, independent of
// any particular execution. Any change to a rule's matcher expression,
// bindings, emits, scope, traversal mode, budgets, catalog versions, or
// producer/package identity changes the canonical JSON and therefore this
// hash.
//
// `artifact_identity` is the identity of one EXECUTION of a plan: it folds in
// the pinned workspace/TU identity and a fingerprint of the actual source
// content/target/language configuration the plan ran against, in addition to
// the plan hash. Two runs of the identical plan against different source (or
// a different workspace snapshot/TU descriptor) always produce different
// artifact identities, even when the matched facts happen to look
// content-identical -- this is the identity stamped onto every emitted
// ExtensionProvenance and used when publishing/registering the extension
// artifact (src/extract/artifact.hpp), satisfying HSE-64's "changing
// source/configuration ... changes artifact identity".
#pragma once

#include "extract/plan_ir.hpp"

#include <string>

namespace cidx::extract {

[[nodiscard]] std::string plan_hash(const ExtractionPlan &plan);

// Opaque identity strings for the pinned execution input. `workspace_identity`
// and `tu_identity` are typically WorkspaceSnapshot::identity and
// TranslationUnitDescriptor::semantic_hash (src/workspace/context.hpp) when a
// full HSE-61 resolution is available; both may be empty for ad hoc/test
// execution, in which case `tu_content_fingerprint` alone still guarantees
// source changes are visible in the artifact identity.
struct ExecutionIdentityInput {
  std::string workspace_identity;
  std::string tu_identity;
  // A fingerprint of the actual Clang input: source buffer content, target
  // triple, and language configuration (engine.cpp computes this from the
  // ASTContext). Never empty for a real execution.
  std::string tu_content_fingerprint;
};

[[nodiscard]] std::string
artifact_identity(const ExtractionPlan &plan,
                  const ExecutionIdentityInput &input);

} // namespace cidx::extract
