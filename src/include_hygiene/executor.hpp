// Cleanup-plan execution (planning/cidx-include-hygiene M4).
//
// This is the ONLY code in cidx that edits source files. Its contract:
//
//   1. Refuse a stale plan outright. If any source hash, configuration digest,
//      or the index schema version has moved, the plan describes a world that
//      no longer exists. There is no force-through-staleness path.
//   2. Revalidate everything. The plan's own recorded validations are evidence
//      of what WAS true, never a substitute for proving it again now.
//   3. Prove the COMBINED overlay, not just each edit alone. Two individually
//      redundant providers must not both be removed.
//   4. Nothing is written until every final buffer has passed. A write is then
//      staged through a temporary sibling and renamed, so a crash mid-run
//      cannot leave a half-written source file.
//   5. Nothing in the plan is ever executed. It is data: paths, offsets, and
//      evidence. No field is a command.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "include_hygiene/plan.hpp"

namespace cidx {
class Storage;
}

namespace cidx::hygiene {

struct ExecuteOptions {
  bool dry_run = false;
  // Apply only these candidate ids. Empty = every accepted item. An allowlist
  // narrows WHAT is applied; it never weakens the final combined validation,
  // which always covers exactly the set being written.
  std::vector<std::string> only;
};

struct ExecuteResult {
  bool ok = false;
  std::vector<std::string> refusals; // non-empty => nothing was written
  std::vector<std::string> edited_files;
  int64_t removed = 0;
  int64_t skipped = 0;
  std::vector<ValidationRecord> validations;
  // Files whose index entries are now stale and were re-marked for indexing.
  std::vector<std::string> reindexed;
};

// Execute `plan` against `db`. Never throws for an expected refusal; those
// come back in ExecuteResult::refusals with ok == false.
ExecuteResult execute(cidx::Storage &db, const CleanupPlan &plan,
                      const ExecuteOptions &opts);

} // namespace cidx::hygiene
