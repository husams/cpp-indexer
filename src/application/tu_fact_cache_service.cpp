#include "application/tu_fact_cache_service.hpp"

#include "application/tu_replay_context.hpp"
#include "ast/fact_batch_artifact.hpp"
#include "profile/index_profile.hpp"
#include "storage/fact_batch_writer.hpp"
#include "storage/records.hpp"
#include "storage/storage.hpp"
#include "util/env.hpp"
#include "util/hashing.hpp"
#include "workspace/tu_fact_cache_identity.hpp"

#include <algorithm>
#include <cctype>
#include <exception>
#include <optional>
#include <span>
#include <sstream>
#include <unordered_map>
#include <utility>

namespace cidx::application {

namespace {

constexpr std::string_view kSystemDependencyPath = "<system-headers>";

auto counter_name(storage::TuFactCacheStatus status) -> std::string {
  return "tu_fact_cache." + std::string(storage::to_string(status));
}

auto lower(std::string value) -> std::string {
  std::ranges::transform(value, value.begin(), [](unsigned char character) {
    return static_cast<char>(std::tolower(character));
  });
  return value;
}

auto dependency_kind_name(bool system, bool generated) -> std::string {
  if (system) {
    return "toolchain";
  }
  return generated ? "generated" : "include";
}

// Content-only currentness for replay. The cached snapshot's mtime belongs to
// the run that produced the entry, so an untouched-content file with a fresh
// mtime (a checkout, a restored backup) must still replay; content equality is
// the stronger claim and the one the cache identity is built on.
auto content_is_current(const std::string &path,
                        const ast::PlannedSourceSnapshot &snapshot) -> bool {
  const std::optional<std::string> current = cidx::md5_of(path);
  return snapshot.md5.has_value() && current.has_value() &&
         *snapshot.md5 == *current;
}

} // namespace

auto tu_fact_cache_options_from_environment() -> TuFactCacheOptions {
  TuFactCacheOptions options;
  if (const std::optional<std::string> value = get_env("CIDX_TU_FACT_CACHE")) {
    const std::string normalized = lower(*value);
    options.enabled = !(normalized == "0" || normalized == "off" ||
                        normalized == "false" || normalized == "no");
  }
  if (const std::optional<std::string> root =
          get_env("CIDX_TU_FACT_CACHE_ROOT")) {
    options.artifact_root = *root;
  }
  return options;
}

class TuFactCacheIndexer::Impl {
public:
  Impl(cidx::Storage &db, ast::IndexSession &session,
       TuFactCacheOptions options)
      : db_(&db), session_(&session), options_(std::move(options)) {
    // Also registered here so a profiled run that ends up indexing nothing
    // still publishes the decision taxonomy at zero.
    register_counters();
    if (options_.enabled) {
      cache_.emplace(db, options_.artifact_root);
    }
  }

  auto index_one(const cidx::File &rec, const std::string &path,
                 bool graph_enabled, ast::IndexFailurePoint failure,
                 bool no_front_end_reuse) -> ast::IndexOneOutcome {
    decision_ = {};
    // Registered per translation unit rather than in the constructor: a
    // profiling session may start after this object is built, and a taxonomy
    // that exists only when the construction order happens to be right is not
    // a contract a report can be read against.
    register_counters();
    reclaim_once();
    // A deliberately injected engine failure exercises the extraction
    // pipeline; routing it through the cache would test the wrong thing.
    if (!cache_ || failure != ast::IndexFailurePoint::none) {
      decision_.action = TuCacheAction::disabled;
      decision_.status = storage::TuFactCacheStatus::unavailable;
      decision_.reason = cache_ ? "failure injection" : "cache disabled";
      return ast::run_index_one(*db_, *session_, rec, path, graph_enabled,
                                failure, no_front_end_reuse);
    }
    // The extraction policy is part of what the entry was built under: a
    // --no-graph run must not consume an entry whose batch carries graph
    // facts, and vice versa.
    graph_enabled_ = graph_enabled;
    storage::TuFactCache &cache = *cache_;
    const std::optional<ast::IndexOneOutcome> replayed =
        try_replay(cache, rec, path, no_front_end_reuse);
    if (replayed) {
      return *replayed;
    }
    // What the miss cost: the extraction the cache could not avoid. Reported
    // next to tu_fact_cache.replay so a benchmark can state replay-vs-rebuild
    // cost instead of inferring it.
    double rebuild_seconds = 0.0;
    ast::IndexOneOutcome outcome = [&] {
      const profile::ScopedAccumulator rebuild(rebuild_seconds);
      return ast::run_index_one(*db_, *session_, rec, path, graph_enabled,
                                failure, no_front_end_reuse);
    }();
    profile::add_timing("tu_fact_cache.extraction_rebuild", rebuild_seconds);
    publish(cache, path, outcome);
    return outcome;
  }

  [[nodiscard]] auto decision() const -> const TuCacheDecision & {
    return decision_;
  }

  auto plan_affected(const std::vector<std::string> &changed)
      -> storage::TuDependencyPlan {
    if (!cache_) {
      return {.affected = {},
              .affected_count = 0,
              .proven_unaffected_count = 0,
              .visited_dependency_nodes = 0,
              .visited_dependency_edges = 0,
              .complete = false,
              .fallback_reason =
                  storage::DependencyFallbackReason::unavailable_evidence};
    }
    std::vector<std::string> canonical;
    canonical.reserve(changed.size());
    for (const std::string &entry : changed) {
      canonical.push_back(canonical_tu_cache_path(entry));
    }
    const storage::TuDependencyPlan plan =
        storage::plan_affected_translation_units(canonical,
                                                 cache_->dependency_evidence());
    profile::add_counter("tu_dependency.affected_configurations",
                         plan.affected_count);
    profile::add_counter("tu_dependency.proven_unaffected_configurations",
                         plan.proven_unaffected_count);
    profile::add_counter("tu_dependency.visited_nodes",
                         plan.visited_dependency_nodes);
    profile::add_counter("tu_dependency.visited_edges",
                         plan.visited_dependency_edges);
    if (!plan.complete) {
      profile::add_counter("tu_dependency.fallbacks");
    }
    return plan;
  }

private:
  // Every decision counter exists from the start of the run, at zero. A
  // benchmark reading a profile must be able to tell "no hits" from "the
  // cache never reported anything", and a counter that only appears once it
  // fires cannot express the first case.
  static void register_counters() {
    for (const storage::TuFactCacheStatus status :
         {storage::TuFactCacheStatus::hit, storage::TuFactCacheStatus::missing,
          storage::TuFactCacheStatus::stale,
          storage::TuFactCacheStatus::corrupt,
          storage::TuFactCacheStatus::incompatible,
          storage::TuFactCacheStatus::partial,
          storage::TuFactCacheStatus::truncated,
          storage::TuFactCacheStatus::untrusted,
          storage::TuFactCacheStatus::unavailable}) {
      profile::add_counter(counter_name(status), 0);
    }
    for (const std::string_view name :
         {"tu_fact_cache.parser_calls_avoided", "tu_fact_cache.replay_bytes",
          "tu_fact_cache.cache_size_bytes", "tu_fact_cache.evictions",
          "tu_fact_cache.fallbacks", "tu_fact_cache.incomplete_evidence",
          "tu_dependency.affected_configurations",
          "tu_dependency.proven_unaffected_configurations",
          "tu_dependency.visited_nodes", "tu_dependency.visited_edges",
          "tu_dependency.fallbacks"}) {
      profile::add_counter(name, 0);
    }
  }

  // One reclamation pass per indexer: replaced, unleased, unpinned objects
  // from earlier runs are the cache's only growth source, and reporting the
  // count is what makes retention observable in a benchmark profile.
  void reclaim_once() {
    if (reclaimed_ || !cache_) {
      return;
    }
    reclaimed_ = true;
    profile::add_counter("tu_fact_cache.evictions", cache_->recover());
  }

  struct ValidatedIdentity {
    std::string sha256;
    storage::DependencyFallbackReason fallback =
        storage::DependencyFallbackReason::none;
  };

  auto content_digest(const std::string &path) -> std::optional<std::string> {
    const std::string key = canonical_tu_cache_path(path);
    if (const auto found = digests_.find(key); found != digests_.end()) {
      return found->second;
    }
    // One run observes one content state per path: the engine already refuses
    // a translation unit whose source changes mid-run, so memoizing here can
    // only reuse a digest the extraction path would also have trusted.
    const std::optional<std::string> digest = sha256_of(key);
    digests_.emplace(key, digest);
    return digest;
  }

  // Rebuilds the identity from what is true on disk right now. `dependencies`
  // describes which files the entry was built over; their current content
  // decides whether that entry is still valid.
  auto compute_identity(const ast::TranslationUnitCacheInputs &inputs,
                        const std::string &source_digest,
                        std::vector<TuFactDependencyIdentity> dependencies)
      -> std::string {
    TuFactCacheIdentityInput input;
    input.main_source_path = inputs.source_identity;
    input.main_source_sha256 = source_digest;
    input.translation_unit_descriptor =
        inputs.configuration_identity + '\x1f' + inputs.configuration_hash +
        "\x1fgraph:" + (graph_enabled_ ? "1" : "0");
    input.clang_identity = inputs.clang_identity;
    input.front_end_reuse_identity = inputs.front_end_reuse_identity;
    input.dependencies = std::move(dependencies);
    for (const std::string &entry : inputs.environment) {
      const std::size_t separator = entry.find('=');
      input.environment.push_back(
          {.name = entry.substr(0, separator),
           .value_sha256 = sha256_hex(separator == std::string::npos
                                          ? std::string()
                                          : entry.substr(separator + 1))});
    }
    for (const std::string &generated : inputs.generated_inputs) {
      input.dependencies.push_back(
          {.path = generated,
           .content_sha256 = content_digest(generated).value_or("<missing>"),
           .kind = "generated",
           .conditional_context = {}});
    }
    return make_tu_fact_cache_identity(std::move(input)).sha256;
  }

  // The stored edges name the dependency set; their content is re-read here so
  // a hit means "every recorded input still has the content it was extracted
  // from". Anything unverifiable is a fallback, never a hit.
  auto identity_from_evidence(
      const ast::TranslationUnitCacheInputs &inputs,
      const std::string &source_digest,
      const storage::TranslationUnitDependencyEvidence &evidence)
      -> ValidatedIdentity {
    std::vector<TuFactDependencyIdentity> dependencies;
    std::vector<std::string> system_paths;
    dependencies.reserve(evidence.edges.size());
    for (const storage::TuDependencyEdge &edge : evidence.edges) {
      if (!edge.resolved) {
        return {.sha256 = {},
                .fallback =
                    storage::DependencyFallbackReason::unresolved_dependency};
      }
      if (edge.system) {
        system_paths.push_back(edge.destination);
        continue;
      }
      const std::optional<std::string> digest =
          content_digest(edge.destination);
      if (!digest) {
        return {.sha256 = {},
                .fallback =
                    storage::DependencyFallbackReason::incomplete_evidence};
      }
      dependencies.push_back(
          {.path = edge.destination,
           .content_sha256 = *digest,
           .kind = std::string(storage::to_string(edge.kind)),
           .conditional_context = edge.conditional_context});
    }
    append_system_dependency(dependencies, std::move(system_paths));
    return {.sha256 = compute_identity(inputs, source_digest,
                                       std::move(dependencies)),
            .fallback = storage::DependencyFallbackReason::none};
  }

  // System and toolchain headers are covered by the normalized toolchain and
  // front-end reuse identities (ADR-016), not by hashing thousands of files
  // per translation unit. Their *set* still participates: adding or dropping
  // a system include changes this digest and invalidates the entry.
  static void
  append_system_dependency(std::vector<TuFactDependencyIdentity> &dependencies,
                           std::vector<std::string> system_paths) {
    if (system_paths.empty()) {
      return;
    }
    std::ranges::sort(system_paths);
    const auto duplicates = std::ranges::unique(system_paths);
    system_paths.erase(duplicates.begin(), duplicates.end());
    std::string material;
    for (const std::string &path : system_paths) {
      material += path;
      material += '\0';
    }
    dependencies.push_back({.path = std::string(kSystemDependencyPath),
                            .content_sha256 = sha256_hex(material),
                            .kind = "toolchain",
                            .conditional_context = {}});
  }

  auto try_replay(storage::TuFactCache &cache, const cidx::File &rec,
                  const std::string &path, bool no_front_end_reuse)
      -> std::optional<ast::IndexOneOutcome> {
    const ast::TranslationUnitCacheInputs inputs =
        session_->translation_unit_cache_inputs(
            path, rec.id, cidx::md5_of(path), no_front_end_reuse);
    inputs_ = inputs;
    const std::optional<std::string> source_digest = content_digest(path);
    if (!source_digest) {
      return note_miss(storage::TuFactCacheStatus::unavailable,
                       storage::DependencyFallbackReason::unavailable_evidence,
                       "main source is unreadable");
    }
    const std::optional<storage::TranslationUnitDependencyEvidence> evidence =
        cache.dependency_evidence_for(inputs.workspace_identity,
                                      inputs.source_identity,
                                      inputs.configuration_identity);
    if (!evidence) {
      return note_miss(storage::TuFactCacheStatus::missing,
                       storage::DependencyFallbackReason::missing_generation,
                       "no cache entry for this translation unit");
    }
    if (evidence->state != storage::DependencyEvidenceState::complete) {
      return note_miss(
          evidence->state == storage::DependencyEvidenceState::corrupt
              ? storage::TuFactCacheStatus::corrupt
              : storage::TuFactCacheStatus::stale,
          fallback_for(evidence->state), "dependency evidence is not usable");
    }
    const ValidatedIdentity identity =
        identity_from_evidence(inputs, *source_digest, *evidence);
    if (identity.fallback != storage::DependencyFallbackReason::none) {
      return note_miss(storage::TuFactCacheStatus::stale, identity.fallback,
                       "dependency evidence cannot be revalidated");
    }
    if (identity.sha256 != evidence->generation) {
      return note_miss(storage::TuFactCacheStatus::stale,
                       storage::DependencyFallbackReason::stale_evidence,
                       "inputs changed since the entry was published");
    }
    const storage::TuFactCacheLookup lookup =
        cache.lookup(inputs.workspace_identity, inputs.source_identity,
                     inputs.configuration_identity, identity.sha256);
    if (!lookup.usable()) {
      return note_miss(lookup.status,
                       storage::DependencyFallbackReason::incomplete_evidence,
                       "cache object did not validate");
    }
    double replay_seconds = 0.0;
    std::optional<ast::IndexOneOutcome> outcome;
    {
      const profile::ScopedAccumulator elapsed(replay_seconds);
      outcome = replay(rec, path, inputs, lookup);
    }
    // Timed whether or not it succeeded: a replay that rolls back still spent
    // the time, and hiding that would flatter the cache.
    profile::add_timing("tu_fact_cache.replay", replay_seconds);
    if (!outcome) {
      // The replay transaction rolled back; the translation unit is untouched
      // and extraction below is what makes it current.
      return note_miss(storage::TuFactCacheStatus::corrupt,
                       storage::DependencyFallbackReason::corrupt_evidence,
                       replay_error_);
    }
    decision_.action = TuCacheAction::replayed;
    decision_.status = storage::TuFactCacheStatus::hit;
    decision_.cache_identity = identity.sha256;
    decision_.parser_invoked = false;
    decision_.reason = "replayed from cache";
    profile::add_counter(counter_name(storage::TuFactCacheStatus::hit));
    profile::add_counter("tu_fact_cache.parser_calls_avoided");
    profile::add_counter("tu_fact_cache.replay_bytes",
                         lookup.fact_batch_artifact.size() +
                             lookup.replay_context.size());
    return outcome;
  }

  auto replay(const cidx::File &rec, const std::string &path,
              const ast::TranslationUnitCacheInputs &inputs,
              const storage::TuFactCacheLookup &lookup)
      -> std::optional<ast::IndexOneOutcome> {
    replay_error_.clear();
    try {
      if (options_.fault == TuCacheFaultInjection::decode_failure) {
        throw TuReplayContextError("injected cache decode failure");
      }
      const TuReplayContext context =
          decode_tu_replay_context(lookup.replay_context);
      ast::FactBatchArtifactCompatibility compatibility;
      compatibility.dependency_identities.reset();
      const ast::FactBatchArtifactDecodeResult decoded =
          decode_fact_batch_artifact(
              ast::FactBatchArtifact::from_bytes(lookup.fact_batch_artifact),
              compatibility);
      if (!decoded.batch.has_value() || !decoded.usable()) {
        replay_error_ = decoded.diagnostics.empty()
                            ? "cached FactBatch did not decode"
                            : decoded.diagnostics.front().message;
        return std::nullopt;
      }
      const ast::OwnedHeaderRoutePlan plan =
          rebuild_route_plan(context, rec.id);
      storage::FactBatchWriter writer(*db_);
      const storage::FactBatchPublicationContext publication{
          .route_plan = plan,
          .translation_unit = context.translation_unit,
          .expected_generation = plan.generation(),
          .source_is_current = content_is_current,
          .configuration_id = inputs.configuration_id,
          .configuration = inputs.configuration,
          .failure = options_.fault == TuCacheFaultInjection::replay_failure
                         ? storage::FactBatchWriterFailurePoint::before_commit
                         : storage::FactBatchWriterFailurePoint::none,
          .measure_statements = false};
      const storage::FactBatchWriterResult result =
          writer.apply(*decoded.batch, publication);
      if (!result.ok()) {
        replay_error_ =
            result.error.value_or("cached FactBatch publication failed");
        return std::nullopt;
      }
      ast::IndexOneOutcome outcome;
      outcome.stored = context.stored;
      outcome.headers = context.headers;
      outcome.diagnostics = context.diagnostics;
      outcome.source_mtime = ast::SourceSnapshot::capture(path).mtime;
      outcome.source_md5 = cidx::md5_of(path);
      outcome.session_metrics = session_->metrics();
      return outcome;
    } catch (const std::exception &error) {
      replay_error_ = error.what();
      return std::nullopt;
    }
  }

  void publish(storage::TuFactCache &cache, const std::string &path,
               const ast::IndexOneOutcome &outcome) {
    if (!outcome.publication || outcome.parse_failed ||
        outcome.source_changed || !inputs_) {
      return;
    }
    const ast::TranslationUnitCacheInputs &inputs = *inputs_;
    const std::optional<std::string> source_digest = content_digest(path);
    if (!source_digest) {
      return;
    }
    std::vector<storage::TuDependencyEdge> edges;
    std::vector<TuFactDependencyIdentity> dependencies;
    std::vector<std::string> system_paths;
    if (!collect_dependencies(outcome, edges, dependencies, system_paths)) {
      profile::add_counter("tu_fact_cache.incomplete_evidence");
      decision_.action = TuCacheAction::extracted;
      decision_.fallback =
          storage::DependencyFallbackReason::unresolved_dependency;
      decision_.reason = "dependency evidence is incomplete; nothing published";
      return;
    }
    append_system_dependency(dependencies, std::move(system_paths));
    const std::string identity =
        compute_identity(inputs, *source_digest, std::move(dependencies));
    try {
      storage::TuFactCachePublication publication{
          .workspace_identity = inputs.workspace_identity,
          .source_identity = inputs.source_identity,
          .configuration_identity = inputs.configuration_identity,
          .cache_identity = identity,
          .fact_batch_artifact = encode_batch(*outcome.publication),
          .replay_context =
              encode_tu_replay_context(build_tu_replay_context(outcome)),
          .dependency_evidence =
              storage::capture_translation_unit_dependency_evidence(
                  inputs.source_identity, inputs.configuration_identity,
                  identity, std::move(edges),
                  options_.fault !=
                      TuCacheFaultInjection::incomplete_evidence)};
      const cidx::ArtifactRecord record = cache.publish(publication);
      profile::add_counter("tu_fact_cache.cache_size_bytes",
                           static_cast<std::uint64_t>(record.byte_size));
      decision_.action = TuCacheAction::extracted_and_published;
      decision_.cache_identity = identity;
      decision_.reason = "published a new cache entry";
    } catch (const std::exception &error) {
      // Publication is best effort: a cache we could not write never blocks an
      // index run that already committed its facts.
      profile::add_counter("tu_fact_cache.fallbacks");
      decision_.action = TuCacheAction::extracted;
      decision_.reason =
          std::string("cache publication failed: ") + error.what();
    }
  }

  auto collect_dependencies(const ast::IndexOneOutcome &outcome,
                            std::vector<storage::TuDependencyEdge> &edges,
                            std::vector<TuFactDependencyIdentity> &dependencies,
                            std::vector<std::string> &system_paths) -> bool {
    for (const ast::IncludeFact &fact : outcome.dependency_facts.includes) {
      if (!fact.resolved || fact.dst_path.empty()) {
        // An unresolved directive can start resolving without any recorded
        // file changing, so an entry built over one could never be revalidated.
        return false;
      }
      const bool generated = is_generated_input(fact.dst_path);
      const std::string kind = dependency_kind_name(fact.is_system, generated);
      if (fact.is_system) {
        system_paths.push_back(canonical_tu_cache_path(fact.dst_path));
        continue;
      }
      const std::optional<std::string> digest = content_digest(fact.dst_path);
      if (!digest) {
        return false;
      }
      edges.push_back({.source = fact.src_path,
                       .destination = fact.dst_path,
                       .destination_content_sha256 = *digest,
                       .conditional_context = fact.cond_fingerprint,
                       .provenance = fact.spelling,
                       .kind = generated ? storage::TuDependencyKind::generated
                                         : storage::TuDependencyKind::include,
                       .resolved = true,
                       .system = false});
      dependencies.push_back({.path = fact.dst_path,
                              .content_sha256 = *digest,
                              .kind = kind,
                              .conditional_context = fact.cond_fingerprint});
    }
    for (const ast::MacroUseFact &fact : outcome.dependency_facts.macro_uses) {
      if (fact.def_path.empty()) {
        continue;
      }
      const std::optional<std::string> digest = content_digest(fact.def_path);
      if (!digest) {
        return false;
      }
      edges.push_back({.source = fact.src_path,
                       .destination = fact.def_path,
                       .destination_content_sha256 = *digest,
                       .conditional_context = {},
                       .provenance = fact.name,
                       .kind = storage::TuDependencyKind::macro,
                       .resolved = true,
                       .system = false});
      dependencies.push_back({.path = fact.def_path,
                              .content_sha256 = *digest,
                              .kind = "macro",
                              .conditional_context = {}});
    }
    return true;
  }

  // A dependency with no core file row is not thereby a build product, but an
  // .inc/.def/.gen input is the case the reverse graph used to lose, so it is
  // labeled for the planner and the identity.
  [[nodiscard]] auto is_generated_input(const std::string &path) const -> bool {
    const std::string canonical = canonical_tu_cache_path(path);
    if (inputs_) {
      for (const std::string &generated : inputs_->generated_inputs) {
        if (canonical_tu_cache_path(generated) == canonical) {
          return true;
        }
      }
    }
    return !db_->get_file(canonical).has_value() &&
           (canonical.ends_with(".inc") || canonical.ends_with(".def") ||
            canonical.ends_with(".gen"));
  }

  static auto encode_batch(const ast::ExtractedFactPublication &publication)
      -> std::vector<std::byte> {
    const ast::FactBatchArtifact artifact =
        ast::encode_fact_batch_artifact(publication.batch);
    std::ostringstream output(std::ios::binary);
    artifact.write_to(output);
    const std::string text = output.str();
    const std::span<const std::byte> raw =
        std::as_bytes(std::span(text.data(), text.size()));
    return {raw.begin(), raw.end()};
  }

  static auto fallback_for(storage::DependencyEvidenceState state)
      -> storage::DependencyFallbackReason {
    switch (state) {
    case storage::DependencyEvidenceState::complete:
      return storage::DependencyFallbackReason::none;
    case storage::DependencyEvidenceState::incomplete:
      return storage::DependencyFallbackReason::incomplete_evidence;
    case storage::DependencyEvidenceState::stale:
      return storage::DependencyFallbackReason::stale_evidence;
    case storage::DependencyEvidenceState::corrupt:
      return storage::DependencyFallbackReason::corrupt_evidence;
    case storage::DependencyEvidenceState::unavailable:
      return storage::DependencyFallbackReason::unavailable_evidence;
    }
    return storage::DependencyFallbackReason::unavailable_evidence;
  }

  auto note_miss(storage::TuFactCacheStatus status,
                 storage::DependencyFallbackReason fallback, std::string reason)
      -> std::optional<ast::IndexOneOutcome> {
    decision_.action = TuCacheAction::extracted;
    decision_.status = status;
    decision_.fallback = fallback;
    decision_.reason = std::move(reason);
    decision_.parser_invoked = true;
    profile::add_counter(counter_name(status));
    // A slot that has never been published is not a fallback: nothing was
    // reused conservatively, there was simply nothing there. Counting it as
    // one would make every cold run look like a cache in trouble.
    if (fallback != storage::DependencyFallbackReason::none &&
        fallback != storage::DependencyFallbackReason::missing_generation) {
      profile::add_counter("tu_fact_cache.fallbacks");
    }
    if (fallback == storage::DependencyFallbackReason::incomplete_evidence ||
        fallback == storage::DependencyFallbackReason::unresolved_dependency) {
      profile::add_counter("tu_fact_cache.incomplete_evidence");
    }
    return std::nullopt;
  }

  cidx::Storage *db_;
  ast::IndexSession *session_;
  TuFactCacheOptions options_;
  std::optional<storage::TuFactCache> cache_;
  std::optional<ast::TranslationUnitCacheInputs> inputs_;
  std::unordered_map<std::string, std::optional<std::string>> digests_;
  bool graph_enabled_ = true;
  bool reclaimed_ = false;
  TuCacheDecision decision_;
  std::string replay_error_;
};

TuFactCacheIndexer::TuFactCacheIndexer(cidx::Storage &db,
                                       ast::IndexSession &session,
                                       TuFactCacheOptions options)
    : impl_(std::make_unique<Impl>(db, session, std::move(options))) {}
TuFactCacheIndexer::~TuFactCacheIndexer() = default;
TuFactCacheIndexer::TuFactCacheIndexer(TuFactCacheIndexer &&) noexcept =
    default;
auto TuFactCacheIndexer::operator=(TuFactCacheIndexer &&) noexcept
    -> TuFactCacheIndexer & = default;

auto TuFactCacheIndexer::index_one(const cidx::File &rec,
                                   const std::string &path, bool graph_enabled,
                                   ast::IndexFailurePoint failure,
                                   bool no_front_end_reuse)
    -> ast::IndexOneOutcome {
  return impl_->index_one(rec, path, graph_enabled, failure,
                          no_front_end_reuse);
}

auto TuFactCacheIndexer::last_decision() const -> const TuCacheDecision & {
  return impl_->decision();
}

auto TuFactCacheIndexer::plan_affected(
    const std::vector<std::string> &changed_dependencies)
    -> storage::TuDependencyPlan {
  return impl_->plan_affected(changed_dependencies);
}

} // namespace cidx::application
