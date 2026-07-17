// Include-candidate classification (planning/cidx-include-hygiene M2).
//
// The authoritative rule, for a direct include edge S -> H:
//
//   unused(S, H) := Refs(Owners(S)) INTERSECT Symbols(H) = {}
//
// where Owners(S) is every symbol declared or defined in S, Symbols(H) is every
// symbol declared or defined DIRECTLY in H (never in a header H itself
// includes), and Refs is the target set of every persisted semantic edge and
// type relation out of those owners.
//
// Two things this layer deliberately does NOT do:
//
//   * It does not compile anything. Compilation is a separate APPLY-SAFETY
//     GATE (validator.hpp), not the definition of unused. A symbol-unused
//     header may still be required -- as a transitive provider, for a macro,
//     for a pragma, for a registration side effect -- so a finding here is a
//     claim about references, and nothing more.
//   * It does not decide that removing anything is safe. It emits
//     `unused_by_reference` and, separately, every reason the edit might still
//     be wrong. Only the validator can promote a candidate to executable.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "storage/records.hpp"

namespace cidx {
class Storage;
}

namespace cidx::hygiene {

enum class Classification {
  // A repeated directive for a guarded target in the same file, configuration,
  // and conditional region as an earlier one.
  Duplicate,
  // Refs(Owners(S)) INTERSECT Symbols(H) is empty.
  UnusedByReference,
  // Concrete references exist: the include is doing work.
  Used,
  // Zero references, but something makes an automatic verdict unsound.
  ManualReview,
};

const char *classification_name(Classification c);

// One reference that makes an include used, rendered for a human.
struct Evidence {
  std::string owner;    // qualified name of the source-owned referrer
  std::string target;   // qualified name of the header-owned symbol
  std::string relation; // "calls", "uses", "inherits", "type", ...
};

// One directive, classified, with the evidence behind the verdict.
struct IncludeCandidate {
  // Stable across runs and machines: sha1(repo-relative src, begin_offset).
  // Plans reference candidates by this id (`apply --only`).
  std::string id;
  std::string src_path;
  std::string dst_path;
  std::string spelling;
  bool is_angled = false;
  int64_t line = 0;
  int64_t col = 0;
  int64_t begin_offset = 0;
  int64_t end_offset = 0;
  std::string directive_text; // the exact bytes to be removed
  std::string cond_fingerprint;
  bool guarded = false;
  int64_t directive_kind = kIncludeDirectiveInclude;

  Classification cls = Classification::ManualReview;
  // Why this is manual_review, or why it is not executable. Empty for a clean
  // duplicate/unused finding. Ordered and deterministic.
  std::vector<std::string> caveats;

  // The proof. owners/header_symbols are qualified names, sorted.
  std::vector<std::string> owners;
  std::vector<std::string> header_symbols;
  int64_t intersection_count = 0;
  std::vector<Evidence> evidence;    // non-empty exactly when cls == Used
  std::vector<std::string> macro_uses;         // macros H supplies to S
  std::vector<std::string> reverse_dependants; // files reaching H (headers only)
  std::vector<std::string> configs;            // digests this edge appears under
};

// What `check` and `plan` both compute. `scope_paths` empty = every source file
// with recorded includes.
struct AnalysisOptions {
  std::vector<std::string> scope_paths; // absolute; empty = whole index
  bool want_duplicates = true;
  bool want_unused = true;
};

struct AnalysisResult {
  std::vector<IncludeCandidate> candidates; // deterministic order
  // Set when the index has no include facts at all: every "unused" verdict
  // would be vacuous, so callers must refuse rather than report zero findings.
  bool include_graph_empty = false;
};

// Classify every direct include in scope. Read-only.
AnalysisResult analyze(cidx::Storage &db, const AnalysisOptions &opts);

// Stable candidate id: sha1(src_path + "\0" + begin_offset), first 12 hex.
std::string candidate_id(const std::string &src_path, int64_t begin_offset);

} // namespace cidx::hygiene
