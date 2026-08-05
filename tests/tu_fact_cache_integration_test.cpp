// End-to-end coverage for the production TU fact cache: a real Clang parse
// publishes an entry, a second run replays it without invoking the parser, and
// every mutation class or damaged-cache class falls back to extraction while
// index.db stays authoritative and readable.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "application/tu_fact_cache_service.hpp"
#include "application/tu_replay_context.hpp"
#include "ast/index_engine.hpp"
#include "profile/index_profile.hpp"
#include "storage/storage.hpp"
#include "util/files.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

namespace app = cidx::application;
namespace ast = cidx::ast;

struct RunResult {
  app::TuCacheDecision decision;
  int stored = 0;
  cidx::HeaderStats headers;
  bool parse_failed = false;
  bool file_current = false;
  std::vector<std::string> facts;
  std::string profile;
};

class Workspace {
public:
  explicit Workspace(std::string_view name)
      : root_(std::filesystem::temp_directory_path() /
              ("cidx_tu_cache_" + std::string(name) + "_" +
               std::to_string(::getpid()))) {
    std::error_code error;
    std::filesystem::remove_all(root_, error);
    std::filesystem::create_directories(component());
    std::filesystem::create_directories(generated_root());
    write(main_source(), "#include \"api.h\"\n"
                         "int main_entry() { return api_value(); }\n");
    write(api_header(), "#pragma once\n"
                        "#include \"values.inc\"\n"
                        "inline int api_value() { return kApiValue; }\n");
    write(generated_header(), "static const int kApiValue = 1;\n");
    cidx::Storage db(database().string());
    db.add_component("app", component().string());
    static_cast<void>(
        db.add_file_path(main_source().string(), std::nullopt, std::nullopt,
                         std::vector<std::string>{
                             "-std=c++23", "-I" + generated_root().string()},
                         std::string("c++")));
  }

  ~Workspace() {
    std::error_code error;
    std::filesystem::remove_all(root_, error);
  }

  Workspace(const Workspace &) = delete;
  auto operator=(const Workspace &) -> Workspace & = delete;
  Workspace(Workspace &&) = delete;
  auto operator=(Workspace &&) -> Workspace & = delete;

  [[nodiscard]] auto root() const -> const std::filesystem::path & {
    return root_;
  }
  [[nodiscard]] auto component() const -> std::filesystem::path {
    return root_ / "app";
  }
  // Deliberately outside every component root: an input opened by the
  // preprocessor that never gets a core `file` row (AC #1371/#1372).
  [[nodiscard]] auto generated_root() const -> std::filesystem::path {
    return root_ / "generated";
  }
  [[nodiscard]] auto main_source() const -> std::filesystem::path {
    return component() / "main.cpp";
  }
  [[nodiscard]] auto api_header() const -> std::filesystem::path {
    return component() / "api.h";
  }
  [[nodiscard]] auto generated_header() const -> std::filesystem::path {
    return generated_root() / "values.inc";
  }
  [[nodiscard]] auto database() const -> std::filesystem::path {
    return root_ / "index.db";
  }
  [[nodiscard]] auto artifacts() const -> std::filesystem::path {
    return root_ / "cache";
  }

  static void write(const std::filesystem::path &path,
                    std::string_view content) {
    std::ofstream output(path, std::ios::trunc);
    output << content;
  }

  // One indexing run, as production performs it: a fresh storage handle,
  // session, and cache orchestrator per invocation.
  auto
  index(bool no_front_end_reuse = false,
        app::TuCacheFaultInjection fault = app::TuCacheFaultInjection::none,
        bool profile_run = false, bool cache_enabled = true) -> RunResult {
    RunResult result;
    const std::filesystem::path profile_path =
        root_ / ("profile_" + std::to_string(++runs_) + ".json");
    {
      cidx::Storage db(database().string());
      const std::optional<cidx::File> file =
          db.get_file(main_source().string());
      REQUIRE(file.has_value());
      ast::IndexSession session(db);
      app::TuFactCacheIndexer indexer(db, session,
                                      {.enabled = cache_enabled,
                                       .artifact_root = artifacts().string(),
                                       .fault = fault});
      std::optional<cidx::profile::Session> profile;
      if (profile_run) {
        profile.emplace(profile_path.string(), std::nullopt);
      }
      const ast::IndexOneOutcome outcome =
          indexer.index_one(*file, main_source().string(), true,
                            ast::IndexFailurePoint::none, no_front_end_reuse);
      if (profile) {
        profile->finish();
      }
      result.decision = indexer.last_decision();
      result.stored = outcome.stored;
      result.headers = outcome.headers;
      result.parse_failed = outcome.parse_failed;
      const std::optional<cidx::File> after =
          db.get_file(main_source().string());
      result.file_current =
          after.has_value() &&
          cidx::files::index_status(*after, main_source().string()) ==
              cidx::files::IndexStatus::kOk;
      result.facts = facts(db);
    }
    if (profile_run) {
      std::ifstream input(profile_path);
      result.profile = std::string(std::istreambuf_iterator<char>(input),
                                   std::istreambuf_iterator<char>());
    }
    return result;
  }

  // Every symbol the run published, in a stable, database-id-free form.
  static auto facts(cidx::Storage &db) -> std::vector<std::string> {
    std::vector<std::string> rows;
    for (const auto &[file, path] : db.list_files()) {
      for (const cidx::Symbol &symbol : db.symbols_in_file(file.id)) {
        rows.push_back(std::filesystem::path(path).filename().string() + '|' +
                       symbol.usr + '|' + symbol.spelling + '|' + symbol.kind +
                       '|' + std::to_string(symbol.line.value_or(-1)));
      }
    }
    std::ranges::sort(rows);
    return rows;
  }

private:
  std::filesystem::path root_;
  int runs_ = 0;
};

// Matches the whole JSON key, so "tu_fact_cache.miss" cannot be answered by
// the "tu_fact_cache.missing" field that contains it as a prefix.
auto field(const std::string &profile, std::string_view name)
    -> std::optional<std::string> {
  const std::string key = '"' + std::string(name) + "\": ";
  const std::size_t at = profile.find(key);
  if (at == std::string::npos) {
    return std::nullopt;
  }
  const std::size_t start = at + key.size();
  return profile.substr(start, profile.find_first_of(",\n", start) - start);
}

auto counter(const std::string &profile, std::string_view name)
    -> std::optional<std::int64_t> {
  const std::optional<std::string> value = field(profile, name);
  if (!value) {
    return std::nullopt;
  }
  return std::stoll(*value);
}

auto timing(const std::string &profile, std::string_view name)
    -> std::optional<double> {
  const std::optional<std::string> value = field(profile, name);
  if (!value) {
    return std::nullopt;
  }
  return std::stod(*value);
}

} // namespace

TEST_CASE("a validated cache entry replays without invoking the parser") {
  Workspace workspace("replay");
  const RunResult first =
      workspace.index(false, app::TuCacheFaultInjection::none, true);
  REQUIRE_FALSE(first.parse_failed);
  CHECK(first.decision.action == app::TuCacheAction::extracted_and_published);
  CHECK(first.decision.parser_invoked);
  CHECK_FALSE(first.facts.empty());
  // A cold run reports its decision classes at zero rather than omitting
  // them, so "no hits" is readable from the profile (AC #1382).
  CHECK(counter(first.profile, "tu_fact_cache.missing") == 1);
  // The decision taxonomy has exactly one spelling. A second, hand-maintained
  // copy of these names once spelled this one "miss", which no code ever
  // incremented, so every published profile carried a permanently dead field.
  CHECK_FALSE(field(first.profile, "tu_fact_cache.miss").has_value());
  CHECK(counter(first.profile, "tu_fact_cache.hit") == 0);
  CHECK(counter(first.profile, "tu_fact_cache.parser_calls_avoided") == 0);
  CHECK(counter(first.profile, "tu_fact_cache.evictions") == 0);
  CHECK(counter(first.profile, "tu_dependency.visited_edges") == 0);
  CHECK(counter(first.profile, "root_traverse_decl_calls") > 0);

  const RunResult second =
      workspace.index(false, app::TuCacheFaultInjection::none, true);
  CHECK(second.decision.action == app::TuCacheAction::replayed);
  CHECK(second.decision.status == cidx::storage::TuFactCacheStatus::hit);
  CHECK_FALSE(second.decision.parser_invoked);
  // AC #1374: the parser is not merely faster on a hit, it is not called. Root
  // traversal is the first thing a real parse does, and the replay never
  // reaches it.
  CHECK(counter(second.profile, "root_traverse_decl_calls") == 0);
  CHECK(counter(second.profile, "tu_fact_cache.hit") == 1);
  CHECK(counter(second.profile, "tu_fact_cache.parser_calls_avoided") == 1);
  // Rebuild cost is only comparable if both sides are actually measured.
  CHECK(timing(second.profile, "tu_fact_cache.replay") > 0.0);
  CHECK(timing(second.profile, "tu_fact_cache.extraction_rebuild") == 0.0);
  CHECK(timing(first.profile, "tu_fact_cache.extraction_rebuild") > 0.0);
  CHECK(timing(first.profile, "tu_fact_cache.replay") == 0.0);
  // AC #1370/#1377: the replay republishes the same facts, and the observable
  // per-file counters the product surfaces print are preserved.
  CHECK(second.facts == first.facts);
  CHECK(first.file_current);
  CHECK(second.file_current);
  CHECK(second.stored == first.stored);
  CHECK(second.headers.indexed == first.headers.indexed);
  CHECK(second.headers.symbols == first.headers.symbols);
  CHECK(second.headers.already == first.headers.already);
  CHECK(second.headers.system == first.headers.system);
  CHECK(second.headers.unowned == first.headers.unowned);
}

TEST_CASE(
    "editing a transitively included owned header invalidates the entry") {
  Workspace workspace("owned_header");
  const RunResult first = workspace.index();
  REQUIRE(first.decision.action == app::TuCacheAction::extracted_and_published);
  REQUIRE(workspace.index().decision.action == app::TuCacheAction::replayed);

  Workspace::write(workspace.api_header(),
                   "#pragma once\n"
                   "#include \"values.inc\"\n"
                   "inline int api_value() { return kApiValue; }\n"
                   "inline int api_extra() { return kApiValue + 1; }\n");
  const RunResult edited = workspace.index();
  CHECK(edited.decision.action == app::TuCacheAction::extracted_and_published);
  CHECK(edited.decision.status == cidx::storage::TuFactCacheStatus::stale);
  CHECK(edited.decision.parser_invoked);
  CHECK(edited.facts != first.facts);
  // The re-extracted state is itself cacheable again.
  CHECK(workspace.index().decision.action == app::TuCacheAction::replayed);
}

TEST_CASE("editing an unowned generated input with no file row invalidates") {
  Workspace workspace("generated");
  REQUIRE(workspace.index().decision.action ==
          app::TuCacheAction::extracted_and_published);
  REQUIRE(workspace.index().decision.action == app::TuCacheAction::replayed);

  {
    // AC #1372: the intermediate has no core `file` row, so a reverse walk
    // over owned files alone would stop before reaching the translation unit.
    cidx::Storage db(workspace.database().string());
    CHECK_FALSE(db.get_file(workspace.generated_header().string()).has_value());
  }

  Workspace::write(workspace.generated_header(),
                   "static const int kApiValue = 7;\n");
  const RunResult edited = workspace.index();
  CHECK(edited.decision.action == app::TuCacheAction::extracted_and_published);
  CHECK(edited.decision.status == cidx::storage::TuFactCacheStatus::stale);
  CHECK(edited.decision.parser_invoked);
}

TEST_CASE("editing the main source invalidates the entry") {
  Workspace workspace("main_source");
  const RunResult first = workspace.index();
  REQUIRE(first.decision.action == app::TuCacheAction::extracted_and_published);
  REQUIRE(workspace.index().decision.action == app::TuCacheAction::replayed);

  Workspace::write(workspace.main_source(),
                   "#include \"api.h\"\n"
                   "int main_entry() { return api_value(); }\n"
                   "int second_entry() { return api_value() + 1; }\n");
  const RunResult edited = workspace.index();
  CHECK(edited.decision.action == app::TuCacheAction::extracted_and_published);
  CHECK(edited.decision.status == cidx::storage::TuFactCacheStatus::stale);
  CHECK(edited.facts != first.facts);
}

TEST_CASE("a different configuration identity is a different cache slot") {
  // The slot is workspace + canonical source + normalized configuration, so
  // two configurations of one translation unit can never share an entry.
  // (Re-registering a changed compile command is `cidx import`'s job; what the
  // cache owns is that the two identities do not collide, and that a path
  // spelling difference alone does not create a second slot.)
  const std::string release = cidx::storage::TuFactCache::logical_id(
      "workspace:test", "/repo/app/main.cpp", "config-release");
  const std::string debug = cidx::storage::TuFactCache::logical_id(
      "workspace:test", "/repo/app/main.cpp", "config-debug");
  const std::string other_workspace = cidx::storage::TuFactCache::logical_id(
      "workspace:other", "/repo/app/main.cpp", "config-release");
  CHECK(release != debug);
  CHECK(release != other_workspace);
  CHECK(release ==
        cidx::storage::TuFactCache::logical_id(
            "workspace:test", "/repo/app/./main.cpp", "config-release"));
}

TEST_CASE("a symlinked path is a different identity, never a silent hit") {
  Workspace workspace("symlink");
  REQUIRE(workspace.index().decision.action ==
          app::TuCacheAction::extracted_and_published);

  const std::filesystem::path link = workspace.root() / "app_link";
  std::error_code error;
  std::filesystem::create_directory_symlink(workspace.component(), link, error);
  if (error) {
    MESSAGE("symlinks unavailable on this filesystem; skipping");
    return;
  }
  const std::filesystem::path linked_source = link / "main.cpp";
  {
    cidx::Storage db(workspace.database().string());
    db.add_component("app_link", link.string());
    static_cast<void>(db.add_file_path(
        linked_source.string(), std::nullopt, std::nullopt,
        std::vector<std::string>{"-std=c++23",
                                 "-I" + workspace.generated_root().string()},
        std::string("c++")));
    const std::optional<cidx::File> file = db.get_file(linked_source.string());
    REQUIRE(file.has_value());
    ast::IndexSession session(db);
    app::TuFactCacheIndexer indexer(
        db, session,
        {.enabled = true,
         .artifact_root = workspace.artifacts().string(),
         .fault = app::TuCacheFaultInjection::none});
    const ast::IndexOneOutcome outcome =
        indexer.index_one(*file, linked_source.string(), true,
                          ast::IndexFailurePoint::none, false);
    CHECK_FALSE(outcome.parse_failed);
    // Paths are canonicalized without resolving symlinks, exactly as the rest
    // of the engine treats "as opened" paths, so the linked spelling gets its
    // own slot instead of silently reusing another path's facts.
    CHECK(indexer.last_decision().action != app::TuCacheAction::replayed);
  }
  {
    // ...and that own slot is a real one: the second run through the link
    // replays, so the miss above was isolation, not a permanently dead entry.
    cidx::Storage db(workspace.database().string());
    const std::optional<cidx::File> file = db.get_file(linked_source.string());
    REQUIRE(file.has_value());
    ast::IndexSession session(db);
    app::TuFactCacheIndexer indexer(
        db, session,
        {.enabled = true,
         .artifact_root = workspace.artifacts().string(),
         .fault = app::TuCacheFaultInjection::none});
    static_cast<void>(indexer.index_one(*file, linked_source.string(), true,
                                        ast::IndexFailurePoint::none, false));
    CHECK(indexer.last_decision().action == app::TuCacheAction::replayed);
  }
}

TEST_CASE("deleting the sidecar forces re-extraction and keeps index.db "
          "readable") {
  Workspace workspace("deleted_sidecar");
  const RunResult first = workspace.index();
  REQUIRE(first.decision.action == app::TuCacheAction::extracted_and_published);

  std::error_code error;
  std::filesystem::remove_all(workspace.artifacts(), error);
  REQUIRE_FALSE(error);

  const RunResult after = workspace.index();
  CHECK(after.decision.action == app::TuCacheAction::extracted_and_published);
  CHECK(after.decision.parser_invoked);
  CHECK_FALSE(after.parse_failed);
  // AC #1381: the core database never became unreadable and never claimed to
  // be current on the strength of an absent optional artifact — it is current
  // because this run really re-extracted the translation unit.
  CHECK(after.facts == first.facts);
  CHECK(after.file_current);
}

TEST_CASE("a corrupt sidecar object is a miss, not a hit") {
  Workspace workspace("corrupt_sidecar");
  const RunResult first = workspace.index();
  REQUIRE(first.decision.action == app::TuCacheAction::extracted_and_published);

  // Every published object is damaged; there is nothing to skip, because the
  // manifest is a table inside index.db (ADR-016), not a file in this tree.
  std::size_t damaged = 0;
  for (const auto &entry :
       std::filesystem::recursive_directory_iterator(workspace.artifacts())) {
    if (!entry.is_regular_file() || entry.path().extension() != ".db") {
      continue;
    }
    std::ofstream output(entry.path(), std::ios::binary | std::ios::trunc);
    output << "not a sqlite database";
    ++damaged;
  }
  REQUIRE(damaged > 0);

  const RunResult after = workspace.index();
  CHECK(after.decision.action != app::TuCacheAction::replayed);
  CHECK(after.decision.parser_invoked);
  CHECK(after.facts == first.facts);
}

TEST_CASE("a failing replay rolls back and falls back to extraction") {
  Workspace workspace("replay_failure");
  const RunResult first = workspace.index();
  REQUIRE(first.decision.action == app::TuCacheAction::extracted_and_published);

  const RunResult injected =
      workspace.index(false, app::TuCacheFaultInjection::replay_failure);
  CHECK(injected.decision.action != app::TuCacheAction::replayed);
  CHECK(injected.decision.parser_invoked);
  CHECK(injected.facts == first.facts);

  const RunResult decode_failure =
      workspace.index(false, app::TuCacheFaultInjection::decode_failure);
  CHECK(decode_failure.decision.action != app::TuCacheAction::replayed);
  CHECK(decode_failure.decision.parser_invoked);
  CHECK(decode_failure.facts == first.facts);
}

TEST_CASE("front-end reuse enabled and disabled use distinct cache slots") {
  Workspace workspace("reuse_identity");
  const RunResult enabled = workspace.index(false);
  REQUIRE(enabled.decision.action ==
          app::TuCacheAction::extracted_and_published);
  // AC #1373: the published compatibility identity is part of the key, so a
  // run with reuse disabled must not consume the enabled run's entry.
  const RunResult disabled = workspace.index(true);
  CHECK(disabled.decision.action != app::TuCacheAction::replayed);
  CHECK(disabled.decision.status == cidx::storage::TuFactCacheStatus::stale);
  CHECK(workspace.index(true).decision.action == app::TuCacheAction::replayed);
  // Switching back misses again rather than reusing the disabled entry: the
  // slot holds one current object and the reuse identity is part of the
  // content identity that object is published under.
  const RunResult back = workspace.index(false);
  CHECK(back.decision.action != app::TuCacheAction::replayed);
  CHECK(back.decision.status == cidx::storage::TuFactCacheStatus::stale);
}

TEST_CASE("a --no-graph run does not consume a graph-enabled entry") {
  Workspace workspace("graph_policy");
  REQUIRE(workspace.index().decision.action ==
          app::TuCacheAction::extracted_and_published);
  cidx::Storage db(workspace.database().string());
  const std::optional<cidx::File> file =
      db.get_file(workspace.main_source().string());
  REQUIRE(file.has_value());
  ast::IndexSession session(db);
  app::TuFactCacheIndexer indexer(
      db, session,
      {.enabled = true,
       .artifact_root = workspace.artifacts().string(),
       .fault = app::TuCacheFaultInjection::none});
  const ast::IndexOneOutcome outcome =
      indexer.index_one(*file, workspace.main_source().string(), false,
                        ast::IndexFailurePoint::none, false);
  CHECK_FALSE(outcome.parse_failed);
  // The cached batch carries graph facts this run must not publish, so the
  // extraction policy belongs to the identity.
  CHECK(indexer.last_decision().action != app::TuCacheAction::replayed);
  // The --no-graph run gets its own entry rather than being permanently
  // uncacheable.
  static_cast<void>(indexer.index_one(*file, workspace.main_source().string(),
                                      false, ast::IndexFailurePoint::none,
                                      false));
  CHECK(indexer.last_decision().action == app::TuCacheAction::replayed);
}

TEST_CASE("a conditionally included header participates in invalidation") {
  Workspace workspace("conditional");
  const std::filesystem::path extra = workspace.component() / "extra.h";
  Workspace::write(extra,
                   "#pragma once\ninline int extra_value() { return 5; }\n");
  Workspace::write(workspace.main_source(),
                   "#include \"api.h\"\n"
                   "#if FEATURE_LEVEL >= 2\n"
                   "#include \"extra.h\"\n"
                   "#endif\n"
                   "int main_entry() { return api_value(); }\n");
  Workspace::write(workspace.generated_header(),
                   "static const int kApiValue = 1;\n"
                   "#define FEATURE_LEVEL 1\n");
  REQUIRE(workspace.index().decision.action ==
          app::TuCacheAction::extracted_and_published);
  REQUIRE(workspace.index().decision.action == app::TuCacheAction::replayed);

  // Turning the condition on opens a header the previous entry never saw.
  Workspace::write(workspace.generated_header(),
                   "static const int kApiValue = 1;\n"
                   "#define FEATURE_LEVEL 2\n");
  CHECK(workspace.index().decision.action ==
        app::TuCacheAction::extracted_and_published);
  REQUIRE(workspace.index().decision.action == app::TuCacheAction::replayed);
  {
    cidx::Storage db(workspace.database().string());
    cidx::storage::TuFactCache cache(db, workspace.artifacts().string());
    bool recorded = false;
    bool conditional = false;
    for (const auto &evidence : cache.dependency_evidence()) {
      for (const auto &edge : evidence.edges) {
        if (edge.destination != extra.string()) {
          continue;
        }
        recorded = true;
        conditional = conditional || !edge.conditional_context.empty();
      }
    }
    // The edge is recorded together with the conditional region that opened
    // it, which is what lets the reverse planner reach this TU from the
    // header without re-parsing.
    CHECK(recorded);
    CHECK(conditional);
  }

  // Editing the conditionally included header now invalidates the entry: the
  // dependency set the entry was built over really does include it.
  Workspace::write(extra,
                   "#pragma once\ninline int extra_value() { return 6; }\n");
  const RunResult edited = workspace.index();
  CHECK(edited.decision.action != app::TuCacheAction::replayed);
  CHECK(edited.decision.status == cidx::storage::TuFactCacheStatus::stale);
}

TEST_CASE("an object that no longer matches its manifest is a miss") {
  Workspace workspace("manifest_divergence");
  const RunResult first = workspace.index();
  REQUIRE(first.decision.action == app::TuCacheAction::extracted_and_published);

  // The manifest is a table in index.db, not a file here, so the way to make
  // the two disagree is to change the object behind its recorded size/digest
  // while leaving both the manifest row and the file present.
  std::size_t altered = 0;
  for (const auto &entry :
       std::filesystem::recursive_directory_iterator(workspace.artifacts())) {
    if (!entry.is_regular_file() || entry.path().extension() != ".db") {
      continue;
    }
    std::ofstream output(entry.path(), std::ios::binary | std::ios::app);
    output << "trailing bytes the manifest never recorded";
    ++altered;
  }
  REQUIRE(altered > 0);

  const RunResult after = workspace.index();
  CHECK(after.decision.action != app::TuCacheAction::replayed);
  CHECK(after.decision.parser_invoked);
  CHECK(after.facts == first.facts);
  CHECK(after.file_current);
}

TEST_CASE("evidence published as incomplete is never replayed") {
  Workspace workspace("incomplete_evidence");
  const RunResult published =
      workspace.index(false, app::TuCacheFaultInjection::incomplete_evidence);
  REQUIRE(published.decision.action ==
          app::TuCacheAction::extracted_and_published);
  // The entry exists and its payload is intact; only its dependency evidence
  // is marked incomplete, and that alone must keep it from ever being used.
  const RunResult after = workspace.index();
  CHECK(after.decision.action != app::TuCacheAction::replayed);
  CHECK(after.decision.parser_invoked);
  CHECK(after.facts == published.facts);
}

TEST_CASE("touching a source without changing it still replays") {
  Workspace workspace("mtime_only");
  const RunResult first = workspace.index();
  REQUIRE(first.decision.action == app::TuCacheAction::extracted_and_published);

  // AC #1375: main-file timestamp tracking is not the validity contract, so a
  // rebuild, a checkout, or a restored backup that leaves content identical
  // must still be served from the cache.
  const auto later = std::filesystem::last_write_time(workspace.main_source()) +
                     std::chrono::hours(1);
  std::filesystem::last_write_time(workspace.main_source(), later);
  std::filesystem::last_write_time(workspace.api_header(), later);
  std::filesystem::last_write_time(workspace.generated_header(), later);

  const RunResult touched = workspace.index();
  CHECK(touched.decision.action == app::TuCacheAction::replayed);
  CHECK_FALSE(touched.decision.parser_invoked);
  CHECK(touched.facts == first.facts);
}

TEST_CASE("the cache can be turned off and indexing is unchanged") {
  Workspace workspace("disabled");
  const RunResult first = workspace.index();
  REQUIRE(first.decision.action == app::TuCacheAction::extracted_and_published);
  const RunResult off =
      workspace.index(false, app::TuCacheFaultInjection::none, false, false);
  CHECK(off.decision.action == app::TuCacheAction::disabled);
  CHECK(off.decision.parser_invoked);
  CHECK(off.facts == first.facts);
}

TEST_CASE("reverse planning reaches the translation unit through an unowned "
          "intermediate") {
  Workspace workspace("planner");
  REQUIRE(workspace.index().decision.action ==
          app::TuCacheAction::extracted_and_published);
  cidx::Storage db(workspace.database().string());
  const std::optional<cidx::File> file =
      db.get_file(workspace.main_source().string());
  REQUIRE(file.has_value());
  ast::IndexSession session(db);
  app::TuFactCacheIndexer indexer(
      db, session,
      {.enabled = true,
       .artifact_root = workspace.artifacts().string(),
       .fault = app::TuCacheFaultInjection::none});
  const cidx::storage::TuDependencyPlan plan =
      indexer.plan_affected({workspace.generated_header().string()});
  CHECK(plan.complete);
  CHECK(plan.affected_count == 1);
  REQUIRE(plan.affected.size() == 1);
  CHECK(plan.affected.front().source == workspace.main_source().string());

  const cidx::storage::TuDependencyPlan unrelated =
      indexer.plan_affected({(workspace.root() / "absent.h").string()});
  CHECK(unrelated.affected_count == 0);
  CHECK(unrelated.proven_unaffected_count == 1);
}

TEST_CASE("the replay context round trips and rejects damaged payloads") {
  app::TuReplayContext context;
  context.translation_unit = "config:abc\x1fsource:def";
  context.generation = "1:config:abc";
  context.stored = 3;
  context.headers.indexed = 2;
  context.diagnostics.push_back({.id = -1,
                                 .file_id = -1,
                                 .severity = 2,
                                 .spelling = "unused variable",
                                 .file_path = "/tmp/main.cpp",
                                 .line = 7,
                                 .col = 3});
  app::TuReplayRoute route;
  route.role = ast::PlannedFileRole::translation_unit;
  route.path = "/tmp/main.cpp";
  route.is_translation_unit_row = true;
  route.md5 = "0123456789abcdef0123456789abcdef";
  route.compile_options = std::vector<std::string>{"-std=c++23"};
  route.partition.file.file_name = "main.cpp";
  route.partition.configuration.translation_unit = context.translation_unit;
  context.routes.push_back(route);

  const std::vector<std::byte> encoded = app::encode_tu_replay_context(context);
  const app::TuReplayContext decoded = app::decode_tu_replay_context(encoded);
  CHECK(decoded.translation_unit == context.translation_unit);
  CHECK(decoded.generation == context.generation);
  CHECK(decoded.stored == context.stored);
  CHECK(decoded.headers.indexed == context.headers.indexed);
  REQUIRE(decoded.diagnostics.size() == 1);
  CHECK(decoded.diagnostics.front().spelling == "unused variable");
  CHECK(decoded.diagnostics.front().line == 7);
  REQUIRE(decoded.routes.size() == 1);
  CHECK(decoded.routes.front().path == route.path);
  CHECK(decoded.routes.front().compile_options == route.compile_options);
  CHECK(app::encode_tu_replay_context(decoded) == encoded);

  std::vector<std::byte> truncated(
      encoded.begin(),
      encoded.begin() + static_cast<std::ptrdiff_t>(encoded.size() / 2U));
  CHECK_THROWS_AS(static_cast<void>(app::decode_tu_replay_context(truncated)),
                  app::TuReplayContextError);
  std::vector<std::byte> trailing = encoded;
  trailing.push_back(std::byte{0});
  CHECK_THROWS_AS(static_cast<void>(app::decode_tu_replay_context(trailing)),
                  app::TuReplayContextError);
}
