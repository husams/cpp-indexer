// The cleanup-plan artifact (planning/cidx-include-hygiene M2/M4).
//
// A plan is an IMMUTABLE SNAPSHOT. It records exactly which bytes to remove,
// the evidence behind each decision, and the identity of everything it was
// derived from: source file hashes, compile-configuration digests, the index
// schema version. `apply` refuses a plan whose world has moved -- there is no
// force-through-staleness path. Regenerate instead.
//
// The artifact is data, never code: nothing in it is executed, and no field is
// ever interpreted as a command. `apply` reads it with the strict util/json_read
// parser precisely because a plan may arrive from anywhere.
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "include_hygiene/analysis.hpp"

namespace cidx::hygiene {

// The artifact's own version. Bump when a field's MEANING changes; `apply`
// refuses a version it does not know rather than guessing.
inline constexpr int kPlanFormatVersion = 1;

// One validation attempt: a compile of one TU under one configuration.
struct ValidationRecord {
  std::string stage;      // "before" | "candidate" | "combined"
  std::string tu_path;    // the translation unit compiled
  std::string config;     // configuration digest
  bool ok = false;
  std::string diagnostic; // first error when !ok; empty otherwise
};

// How far a candidate got. These three states are NEVER conflated: a finding
// can be true (zero references) and still not be safe to apply.
enum class PlanState {
  Accepted,     // validated_for_apply: proven under every affected TU/config
  Rejected,     // validation failed, or evidence says it is used
  ManualReview, // real finding, but no supported automatic proof exists
};

const char *plan_state_name(PlanState s);
std::optional<PlanState> plan_state_from(const std::string &name);

struct PlanItem {
  std::string id; // hygiene::candidate_id; stable across runs
  std::string file;     // repository-relative, the reviewable identity
  std::string abs_path; // resolved at apply time from repo_root + file
  int64_t line = 0;
  int64_t col = 0;
  int64_t begin_offset = 0;
  int64_t end_offset = 0;
  std::string directive_text;   // the exact bytes to remove
  std::string resolved_target;  // the header the directive reaches
  std::string classification;   // hygiene::classification_name
  PlanState state = PlanState::ManualReview;
  std::string reason; // why rejected / why manual

  // The proof, as recorded when the plan was made.
  std::vector<std::string> owners;
  std::vector<std::string> header_symbols;
  int64_t intersection_count = 0;
  std::vector<std::string> reference_kinds_searched;
  std::vector<std::string> macro_uses;
  bool guarded = false;
  std::vector<std::string> reverse_dependants;
  std::vector<std::string> affected_tus;
  std::vector<std::string> configs;
  std::vector<ValidationRecord> validations;
};

struct CleanupPlan {
  int format_version = kPlanFormatVersion;
  std::string repo_root;
  std::string db_path;
  int schema_version = 0;
  std::string resolved_at; // ISO-8601 UTC
  std::vector<std::string> config_digests;
  // Repo-relative path -> content hash at plan time. `apply` refuses when any
  // of these no longer matches.
  std::map<std::string, std::string> file_hashes;
  std::vector<PlanItem> items; // ordered: file, then begin_offset, then config
  std::vector<ValidationRecord> validations; // whole-plan stages
  // Plain-language statements of what this plan does NOT prove. Written into
  // the artifact so a reviewer reads them without consulting the docs.
  std::vector<std::string> limitations;
};

// Turn classified candidates into a plan, running the removal validation that
// decides which of them become executable (include_hygiene/planner.cpp -- it
// drives Clang, so it lives in the cidx_hygiene object library).
//
// Candidates are validated SEQUENTIALLY against an accumulating overlay, then
// the whole accepted set is proven together. That order is what makes "two
// individually redundant providers, but not both" come out right: once the
// first is accepted, the second is judged against a world where the first is
// already gone.
CleanupPlan build_plan(cidx::Storage &db, const struct AnalysisResult &res,
                       const std::string &db_path);

// Serialize to the canonical JSON form (json.dumps(indent=2) byte rules).
// Deterministic: same plan in, same bytes out.
std::string serialize(const CleanupPlan &p);

// Parse and structurally validate. Throws CidxError on malformed JSON, an
// unknown format_version, or a missing/ill-typed required field.
CleanupPlan deserialize(const std::string &text);

// Content hash of a file for the freshness check, or nullopt when unreadable.
std::optional<std::string> hash_file(const std::string &abs_path);

// Why a plan cannot be applied against the world as it stands now. Empty means
// fresh. Each entry names the exact drift so the user knows what changed.
std::vector<std::string> staleness_reasons(const CleanupPlan &p,
                                           int current_schema_version);

} // namespace cidx::hygiene
