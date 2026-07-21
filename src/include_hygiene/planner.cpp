// build_plan: turn classified candidates into a reviewable, executable plan
// (planning/cidx-include-hygiene M2/M3). Lives in the cidx_hygiene object
// library because it drives the Clang-based RemovalValidator.
#include "include_hygiene/plan.hpp"

#include "include_hygiene/analysis.hpp"
#include "include_hygiene/validator.hpp"
#include "storage/storage.hpp"
#include "util/pathutil.hpp"

#include <algorithm>
#include <ctime>
#include <set>

namespace cidx::hygiene {

namespace {

// UTC, second resolution. Provenance for a human reading the artifact; nothing
// keys off it, so a coarse stamp is right.
std::string iso_now() {
  const std::time_t t = std::time(nullptr);
  std::tm tm{};
#ifdef _WIN32
  ::gmtime_s(&tm, &t);
#else
  ::gmtime_r(&t, &tm);
#endif
  char buf[32];
  std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
  return buf;
}

std::vector<std::string> split_components(const std::string &path) {
  std::vector<std::string> out;
  std::string cur;
  for (const char c : path) {
    if (c == '/') {
      if (!cur.empty()) {
        out.push_back(cur);
        cur.clear();
      }
      continue;
    }
    cur += c;
  }
  if (!cur.empty()) {
    out.push_back(cur);
  }
  return out;
}

// Longest common DIRECTORY of every touched file: the plan's paths are stored
// relative to this, so the artifact stays reviewable in a diff and portable
// between clones of the same tree.
//
// Compared component-wise, not byte-wise. A byte-wise common prefix can stop
// mid-name ("/a/foo" vs "/a/foobar" share "/a/foo", which is not a shared
// directory), and backing off to the previous '/' to compensate over-corrects
// when the prefix already IS one of the paths -- "/w" and "/w/src" would
// collapse to "/", making every path in the plan absolute-minus-slash.
std::string common_root(const std::vector<std::string> &paths) {
  if (paths.empty()) {
    return "/";
  }
  std::vector<std::string> root = split_components(pathutil::dirname(paths.front()));
  for (const std::string &p : paths) {
    const std::vector<std::string> dir = split_components(pathutil::dirname(p));
    std::size_t i = 0;
    while (i < root.size() && i < dir.size() && root[i] == dir[i]) {
      ++i;
    }
    root.resize(i);
    if (root.empty()) {
      return "/"; // nothing in common beyond the filesystem root
    }
  }
  std::string out;
  for (const std::string &c : root) {
    out += "/";
    out += c;
  }
  return out;
}

std::string relative_to(const std::string &root, const std::string &abs) {
  if (root != "/" && abs.starts_with(root + "/")) {
    return abs.substr(root.size() + 1);
  }
  if (root == "/" && abs.starts_with('/')) {
    return abs.substr(1);
  }
  return abs;
}

// Every edge kind and type relation the analyzer consults. Recorded in the
// artifact so a reviewer knows the exact search space behind a zero-reference
// claim, and so a future kind that was NOT searched is visible in old plans.
const std::vector<std::string> &searched_kinds() {
  static const std::vector<std::string> k = {
      "calls",           "inherits",        "contains",
      "specializes",     "instantiates",    "overrides",
      "uses",            "field_of",        "method_of",
      "construct-value", "construct-temp",  "construct-heap",
      "construct-copy",  "construct-move",  "factory-construct",
      "destroy",         "friend",          "dispatch_calls",
      "returns",         "of_type",         "underlying_type",
      "parameter_type",  "template_argument_type"};
  return k;
}

} // namespace

CleanupPlan build_plan(cidx::Storage &db, const AnalysisResult &res,
                       const std::string &db_path) {
  CleanupPlan plan;
  plan.format_version = kPlanFormatVersion;
  plan.db_path = db_path;
  plan.schema_version = cidx::kSchemaVersion;
  plan.resolved_at = iso_now();

  // What this plan does NOT prove, stated in the artifact itself so a reviewer
  // reads it without going to the docs.
  plan.limitations = {
      "Compile success is not proof of behavioral equivalence: a header removed "
      "for a static registration, a configuration-changing macro, or a pragma "
      "can still compile and change the program.",
      "Only configurations recorded in the index were validated. A build "
      "configuration cidx has never indexed is not covered.",
      "A zero-reference result is only as complete as the indexed files and the "
      "semantic edge kinds listed in reference_kinds_searched.",
      "Items marked manual_review are real findings with no automatic proof; "
      "they are never applied.",
  };

  std::vector<std::string> files;
  files.reserve(res.candidates.size());
  for (const IncludeCandidate &c : res.candidates) {
    files.push_back(c.src_path);
  }
  std::ranges::sort(files);
  files.erase(std::ranges::unique(files).begin(), files.end());
  plan.repo_root = common_root(files);

  for (const std::string &f : files) {
    if (const std::optional<std::string> h = hash_file(f)) {
      plan.file_hashes[relative_to(plan.repo_root, f)] = *h;
    }
  }

  std::set<std::string> digests;
  RemovalValidator validator(db);
  // Accepted removals accumulate here: each later candidate is validated in a
  // world where every earlier accepted one is already gone.
  std::vector<Removal> accepted;

  for (const IncludeCandidate &c : res.candidates) {
    if (c.cls == Classification::Used) {
      continue; // not a finding; nothing to plan
    }
    PlanItem it;
    it.id = c.id;
    it.file = relative_to(plan.repo_root, c.src_path);
    it.abs_path = c.src_path;
    it.line = c.line;
    it.col = c.col;
    it.begin_offset = c.begin_offset;
    it.end_offset = c.end_offset;
    it.directive_text = c.directive_text;
    it.resolved_target = c.dst_path;
    it.classification = classification_name(c.cls);
    it.owners = c.owners;
    it.header_symbols = c.header_symbols;
    it.intersection_count = c.intersection_count;
    it.reference_kinds_searched = searched_kinds();
    it.macro_uses = c.macro_uses;
    it.guarded = c.guarded;
    it.reverse_dependants = c.reverse_dependants;
    it.configs = c.configs;
    for (const std::string &d : c.configs) {
      digests.insert(d);
    }

    if (c.cls == Classification::ManualReview) {
      it.state = PlanState::ManualReview;
      it.reason = c.caveats.empty() ? "no automatic proof is available"
                                    : c.caveats.front();
      plan.items.push_back(std::move(it));
      continue;
    }

    // Every TU/configuration that must still build. No compile command
    // reaching the file means the removal cannot be proven -- which downgrades
    // it to manual_review rather than letting it pass unproven.
    const std::optional<std::vector<TuTarget>> targets =
        validator.affected_tus(c.src_path);
    if (!targets) {
      it.state = PlanState::ManualReview;
      it.reason = "no recorded compile command reaches this file, so removal "
                  "cannot be validated";
      plan.items.push_back(std::move(it));
      continue;
    }
    for (const TuTarget &t : *targets) {
      it.affected_tus.push_back(t.tu_path);
    }
    std::ranges::sort(it.affected_tus);
    it.affected_tus.erase(std::ranges::unique(it.affected_tus).begin(),
                          it.affected_tus.end());

    std::vector<Removal> trial = accepted;
    trial.push_back({.abs_path = c.src_path,
                     .begin_offset = c.begin_offset,
                     .end_offset = c.end_offset});
    it.validations = validator.validate(
        *targets, RemovalValidator::overlay_for(trial), "candidate");

    if (RemovalValidator::all_ok(it.validations)) {
      it.state = PlanState::Accepted;
      accepted = std::move(trial); // this removal is now part of the world
    } else {
      it.state = PlanState::Rejected;
      for (const ValidationRecord &v : it.validations) {
        if (!v.ok) {
          it.reason = "removal breaks " + v.tu_path + ": " + v.diagnostic;
          break;
        }
      }
    }
    plan.items.push_back(std::move(it));
  }

  plan.config_digests.assign(digests.begin(), digests.end());

  // The final combined proof: everything accepted, removed together. An item
  // is only executable if this passes too.
  if (!accepted.empty()) {
    std::vector<TuTarget> all;
    std::set<std::pair<std::string, std::string>> seen;
    for (const Removal &r : accepted) {
      if (const std::optional<std::vector<TuTarget>> t =
              validator.affected_tus(r.abs_path)) {
        for (const TuTarget &x : *t) {
          if (seen.insert({x.tu_path, x.config_digest}).second) {
            all.push_back(x);
          }
        }
      }
    }
    plan.validations = validator.validate(
        all, RemovalValidator::overlay_for(accepted), "combined");
    if (!RemovalValidator::all_ok(plan.validations)) {
      // The set is not provable together even though each member passed
      // against the prefix before it. Demote every accepted item rather than
      // ship a plan whose combined proof failed.
      for (PlanItem &it : plan.items) {
        if (it.state == PlanState::Accepted) {
          it.state = PlanState::Rejected;
          it.reason = "the combined removal set does not compile together";
        }
      }
    }
  }

  // Deterministic: file, then source offset, then configuration digest.
  std::ranges::sort(plan.items, [](const PlanItem &a, const PlanItem &b) {
    return std::tie(a.file, a.begin_offset, a.configs) <
           std::tie(b.file, b.begin_offset, b.configs);
  });
  return plan;
}

} // namespace cidx::hygiene
