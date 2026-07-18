#include "include_hygiene/executor.hpp"

#include "include_hygiene/analysis.hpp"
#include "include_hygiene/validator.hpp"
#include "storage/storage.hpp"
#include "util/pathutil.hpp"

#include "clang/Format/Format.h"
#include "clang/Tooling/Core/Replacement.h"
#include "llvm/Support/Error.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace cidx::hygiene {

namespace {

std::string read_file(const std::string &path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return {};
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

// A plan item's `file` is repo-relative and joined onto repo_root before use.
// The plan is untrusted, so an absolute path or any `..` component -- which
// would let a tampered plan point the edit anywhere on disk -- is rejected
// before the join.
bool within_repo(const std::string &rel) {
  if (rel.empty() || rel.front() == '/') {
    return false;
  }
  std::size_t i = 0;
  while (i < rel.size()) {
    std::size_t j = rel.find('/', i);
    if (j == std::string::npos) {
      j = rel.size();
    }
    if (rel.compare(i, j - i, "..") == 0) {
      return false;
    }
    i = j + 1;
  }
  return true;
}

bool write_all(int fd, const std::string &content, std::string &err) {
  std::size_t off = 0;
  while (off < content.size()) {
    const ssize_t n =
        ::write(fd, content.data() + off, content.size() - off);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      err = "write failed";
      return false;
    }
    off += static_cast<std::size_t>(n);
  }
  return true;
}

// Write through a uniquely-named temporary sibling then rename. rename(2) is
// atomic within a filesystem, so a reader either sees the whole old file or the
// whole new one -- never a truncated source file, even if the process dies
// here. A sibling (not /tmp) keeps it on the same filesystem, which rename
// requires.
//
// The original file's identity and metadata are preserved:
//   * a symlinked source is REFUSED -- replacing the inode would silently
//     turn the link into a regular file (and could point outside the repo);
//   * the temp inode inherits the original's mode and, best-effort, its owner,
//     so an executable or group-shared source keeps its bits after the swap;
//   * mkstemp gives a unique name, so two concurrent runs never collide on a
//     fixed ".cidx-tmp" and a stale temp is never mistaken for a valid one.
bool write_atomic(const std::string &path, const std::string &content,
                  std::string &err) {
  struct stat lst{};
  if (::lstat(path.c_str(), &lst) != 0) {
    err = "cannot stat " + path;
    return false;
  }
  if (S_ISLNK(lst.st_mode)) {
    err = "refusing to edit a symlink: " + path +
          " (rename would replace the link with a regular file)";
    return false;
  }

  std::string tmpl = pathutil::dirname(path) + "/.cidx-apply-XXXXXX";
  std::vector<char> name(tmpl.begin(), tmpl.end());
  name.push_back('\0');
  const int fd = ::mkstemp(name.data());
  if (fd < 0) {
    err = "cannot create a temporary file next to " + path;
    return false;
  }
  const std::string tmp(name.data());

  const auto fail = [&](const std::string &m) {
    ::close(fd);
    ::unlink(tmp.c_str());
    err = m;
    return false;
  };

  if (!write_all(fd, content, err)) {
    return fail("write failed: " + tmp);
  }
  if (::fsync(fd) != 0) {
    return fail("fsync failed: " + tmp);
  }
  // Match the original's permissions before it becomes the source file. Owner
  // preservation is best-effort: it needs privilege the tool usually lacks, and
  // the common case (same owner) needs no change, so a chown failure is not
  // fatal.
  if (::fchmod(fd, lst.st_mode & 07777) != 0) {
    return fail("cannot set permissions on " + tmp);
  }
  ::fchown(fd, lst.st_uid, lst.st_gid); // best-effort; ignore failure
  if (::close(fd) != 0) {
    ::unlink(tmp.c_str());
    err = "close failed: " + tmp;
    return false;
  }
  if (std::rename(tmp.c_str(), path.c_str()) != 0) {
    ::unlink(tmp.c_str());
    err = "rename failed: " + tmp + " -> " + path;
    return false;
  }
  return true;
}

// Tidy up after the deletions using the repository's own style.
//
// Only cleanupAroundReplacements runs -- deliberately NOT sortIncludes and NOT
// a general reformat. Deleting whole directive lines cannot unsort an already
// sorted include block or disturb the formatting of any line it leaves behind,
// so the style is already honored; running the sorter over a file that was
// never sorted would rewrite the entire block and bury the one-line edit under
// an unrelated diff. cleanupAroundReplacements is the narrow tool for exactly
// this: it fixes what the deletion itself left behind (stranded blank lines),
// using the discovered .clang-format, and touches nothing else.
std::string clang_format_cleanup(const std::string &path,
                                 const std::string &original,
                                 const std::vector<Removal> &removals,
                                 std::string &err) {
  clang::tooling::Replacements reps;
  for (const Removal &r : removals) {
    if (llvm::Error e = reps.add(clang::tooling::Replacement(
            path, static_cast<unsigned>(r.begin_offset),
            static_cast<unsigned>(r.end_offset - r.begin_offset), ""))) {
      err = "conflicting replacements in " + path + ": " +
            llvm::toString(std::move(e));
      return {};
    }
  }

  // getStyle("file", ...) walks up from the file to find the nearest
  // .clang-format, which is what "the repository's rules" means.
  llvm::Expected<clang::format::FormatStyle> style =
      clang::format::getStyle("file", path, "LLVM", original);
  if (!style) {
    llvm::consumeError(style.takeError());
    err = "cannot resolve a clang-format style for " + path;
    return {};
  }

  llvm::Expected<clang::tooling::Replacements> cleaned =
      clang::format::cleanupAroundReplacements(original, reps, *style);
  if (!cleaned) {
    llvm::consumeError(cleaned.takeError());
    err = "clang-format cleanup failed for " + path;
    return {};
  }
  llvm::Expected<std::string> out =
      clang::tooling::applyAllReplacements(original, *cleaned);
  if (!out) {
    llvm::consumeError(out.takeError());
    err = "cannot apply replacements to " + path;
    return {};
  }
  return *out;
}

} // namespace

ExecuteResult execute(cidx::Storage &db, const CleanupPlan &plan,
                      const ExecuteOptions &opts) {
  ExecuteResult res;

  // -- 1. staleness: refuse a plan whose world has moved ---------------------
  for (const std::string &r : staleness_reasons(plan, cidx::kSchemaVersion)) {
    res.refusals.push_back(r);
  }
  if (!res.refusals.empty()) {
    res.refusals.push_back(
        "regenerate the plan with `cidx include plan` and review it again");
    return res;
  }

  // -- 2. select the items to apply -----------------------------------------
  const std::set<std::string> only(opts.only.begin(), opts.only.end());
  std::vector<const PlanItem *> chosen;
  for (const PlanItem &it : plan.items) {
    if (it.state != PlanState::Accepted) {
      ++res.skipped; // manual_review / rejected are never applied
      continue;
    }
    if (!only.empty() && only.count(it.id) == 0) {
      ++res.skipped;
      continue;
    }
    chosen.push_back(&it);
  }
  for (const std::string &id : opts.only) {
    const bool known = std::any_of(
        plan.items.begin(), plan.items.end(),
        [&](const PlanItem &it) { return it.id == id; });
    if (!known) {
      res.refusals.push_back("--only names an id that is not in this plan: " + id);
    }
  }
  if (!res.refusals.empty()) {
    return res;
  }
  if (chosen.empty()) {
    res.ok = true; // nothing to do is a success, not a failure
    return res;
  }

  std::vector<Removal> removals;
  std::set<std::string> touched;
  for (const PlanItem *it : chosen) {
    // A selected item that escapes the repo root, or whose file the plan never
    // committed a hash for, cannot be trusted: the staleness gate only proves
    // the files it was given, so a tampered plan could edit a file it deleted
    // the hash for and slip past the check entirely.
    if (!within_repo(it->file)) {
      res.refusals.push_back("item " + it->id + " names a path outside the "
                             "repository root: " + it->file);
      continue;
    }
    if (plan.file_hashes.count(it->file) == 0) {
      res.refusals.push_back("plan carries no content hash for " + it->file +
                             " (item " + it->id +
                             "): it cannot be proven current, so it is refused");
      continue;
    }
    const std::string abs = pathutil::join(plan.repo_root, it->file);
    // The plan records the exact bytes it intends to delete. If the file no
    // longer holds those bytes at that offset, the hash check should already
    // have fired -- this is the belt-and-braces check that we never delete
    // something other than the directive that was reviewed.
    const std::string content = read_file(abs);
    if (it->end_offset > static_cast<int64_t>(content.size()) ||
        content.substr(static_cast<std::size_t>(it->begin_offset),
                       static_cast<std::size_t>(it->end_offset -
                                                it->begin_offset)) !=
            it->directive_text) {
      res.refusals.push_back(
          "the bytes at " + it->file + ":" + std::to_string(it->line) +
          " are not the directive this plan reviewed (item " + it->id + ")");
      continue;
    }
    removals.push_back({abs, it->begin_offset, it->end_offset});
    touched.insert(abs);
  }
  if (!res.refusals.empty()) {
    return res;
  }

  // -- 2b. re-derive every finding from the DB, not the plan ----------------
  // The plan is an editable artifact: its `state`, `classification`, and
  // evidence are all attacker-controllable. Re-run the analysis over exactly
  // the touched files and require each selected item to STILL be a removable
  // finding (unused_by_reference or duplicate) at the same bytes. A tampered
  // plan that promoted a `used` or `manual_review` include to accepted is
  // rejected here, before the compile gate ever runs.
  {
    AnalysisOptions aopts;
    aopts.scope_paths.assign(touched.begin(), touched.end());
    aopts.want_unused = true;
    aopts.want_duplicates = true;
    const AnalysisResult fresh = analyze(db, aopts);
    std::map<std::string, const IncludeCandidate *> by_id;
    for (const IncludeCandidate &c : fresh.candidates) {
      by_id.emplace(c.id, &c);
    }
    for (const PlanItem *it : chosen) {
      const auto found = by_id.find(it->id);
      const IncludeCandidate *c =
          found == by_id.end() ? nullptr : found->second;
      const bool removable =
          c != nullptr && (c->cls == Classification::UnusedByReference ||
                           c->cls == Classification::Duplicate);
      if (!removable || c->begin_offset != it->begin_offset ||
          c->end_offset != it->end_offset ||
          c->directive_text != it->directive_text) {
        res.refusals.push_back(
            "the current index no longer classifies item " + it->id + " (" +
            it->file + ":" + std::to_string(it->line) +
            ") as a removable finding: re-run `cidx include plan`");
      }
    }
    if (!res.refusals.empty()) {
      return res;
    }
  }

  // -- 3. revalidate the COMBINED overlay -----------------------------------
  // Not per-item: two individually redundant providers must not both go.
  RemovalValidator validator(db);
  std::vector<TuTarget> targets;
  for (const std::string &f : touched) {
    const std::optional<std::vector<TuTarget>> t = validator.affected_tus(f);
    if (!t) {
      res.refusals.push_back(
          "no compile command reaches " + f +
          ": this edit cannot be proven, so it will not be applied");
      continue;
    }
    for (const TuTarget &x : *t) {
      targets.push_back(x);
    }
  }
  if (!res.refusals.empty()) {
    return res;
  }
  // De-duplicate: several edited files usually share TUs.
  std::sort(targets.begin(), targets.end(),
            [](const TuTarget &a, const TuTarget &b) {
              return std::tie(a.tu_path, a.config_digest) <
                     std::tie(b.tu_path, b.config_digest);
            });
  targets.erase(std::unique(targets.begin(), targets.end(),
                            [](const TuTarget &a, const TuTarget &b) {
                              return a.tu_path == b.tu_path &&
                                     a.config_digest == b.config_digest;
                            }),
                targets.end());

  // The build configurations reaching these files must be the ones the plan was
  // reviewed against. A compile command that changed since (a flag, an include
  // path, the language mode) is a different world -- the plan's validations no
  // longer describe it -- so a digest the plan never recorded is a staleness
  // refusal, not something to silently re-prove.
  {
    const std::set<std::string> proven(plan.config_digests.begin(),
                                       plan.config_digests.end());
    std::set<std::string> unseen;
    for (const TuTarget &t : targets) {
      if (proven.count(t.config_digest) == 0) {
        unseen.insert(t.config_digest);
      }
    }
    if (!unseen.empty()) {
      res.refusals.push_back(
          "a build configuration reaching these files is not one the plan was "
          "reviewed against: regenerate the plan with `cidx include plan`");
      return res;
    }
  }

  res.validations = validator.validate(
      targets, RemovalValidator::overlay_for(removals), "combined");
  if (!RemovalValidator::all_ok(res.validations)) {
    for (const ValidationRecord &v : res.validations) {
      if (!v.ok) {
        res.refusals.push_back("combined validation failed for " + v.tu_path +
                               ": " + v.diagnostic);
      }
    }
    return res;
  }

  // -- 4/5/6/7. build every final buffer, formatted, before writing anything -
  std::map<std::string, std::vector<Removal>> by_file;
  for (const Removal &r : removals) {
    by_file[r.abs_path].push_back(r);
  }
  std::map<std::string, std::string> finals;
  for (const auto &[path, rs] : by_file) {
    std::string err;
    const std::string original = read_file(path);
    std::string out = clang_format_cleanup(path, original, rs, err);
    if (!err.empty()) {
      res.refusals.push_back(err);
      return res;
    }
    finals[path] = std::move(out);
  }

  // -- 8. preflight: prove the EXACT bytes about to be written ---------------
  // Step 3 proved original-minus-directive. cleanupAroundReplacements may then
  // have dropped a stranded blank line, so the final buffers are not
  // byte-identical to what was proven. Validating the removal and then writing
  // something else would make the proof a lie -- so prove the real bytes.
  for (const ValidationRecord &v : validator.validate(targets, finals,
                                                      "preflight")) {
    res.validations.push_back(v);
    if (!v.ok) {
      res.refusals.push_back("preflight failed for " + v.tu_path + ": " +
                             v.diagnostic);
    }
  }
  if (!res.refusals.empty()) {
    return res;
  }

  if (opts.dry_run) {
    res.ok = true;
    for (const auto &[path, _] : finals) {
      res.edited_files.push_back(path);
    }
    res.removed = static_cast<int64_t>(chosen.size());
    return res;
  }

  // -- 9. staged writes ------------------------------------------------------
  for (const auto &[path, content] : finals) {
    std::string err;
    if (!write_atomic(path, content, err)) {
      res.refusals.push_back(err);
      return res; // earlier files stay written; each was individually atomic
    }
    res.edited_files.push_back(path);
  }
  res.removed = static_cast<int64_t>(chosen.size());

  // -- 10. post-write validation of the real tree ---------------------------
  for (const ValidationRecord &v :
       validator.validate(targets, {}, "post_write")) {
    res.validations.push_back(v);
    if (!v.ok) {
      res.refusals.push_back("POST-WRITE validation failed for " + v.tu_path +
                             ": " + v.diagnostic +
                             " -- the tree has been edited; review it");
    }
  }
  if (!res.refusals.empty()) {
    return res;
  }

  // -- 11. targeted reindex --------------------------------------------------
  // The edited files and everything that includes them now have stale symbol
  // and include rows. Mark them pending rather than reindexing inline: the
  // caller runs `cidx index`, which is the one code path that knows how.
  std::set<std::string> stale = touched;
  for (const std::string &f : touched) {
    if (const std::optional<std::vector<TuTarget>> t = validator.affected_tus(f)) {
      for (const TuTarget &x : *t) {
        stale.insert(x.tu_path);
      }
    }
  }
  for (const std::string &f : stale) {
    if (const std::optional<File> row = db.get_file(f)) {
      db.set_file_indexed(row->id, false);
      res.reindexed.push_back(f);
    }
  }

  res.ok = true;
  return res;
}

} // namespace cidx::hygiene
