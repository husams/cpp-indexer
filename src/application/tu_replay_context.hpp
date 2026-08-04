// Versioned, application-owned routing context for controlled cache replay.
//
// A cached FactBatch is only replayable together with the publication plan it
// was extracted under: the controlled writer resolves every batch partition
// through a route, and a route carries the owned-header identity, snapshot,
// and compile options the writer needs to mint or reuse a file row. The plan
// itself is not serializable (it owns minted transient handles), so the cache
// stores its inputs and rebuilds an identical plan at replay time. Handles are
// content-derived, so a rebuild from the same routes reproduces them exactly.
#pragma once

#include "ast/index_engine.hpp"
#include "ast/owned_header_plan.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace cidx::application {

inline constexpr std::string_view kTuReplayContextVersion =
    "cidx-tu-replay-context/v1";

// Every decode failure is a cache miss, never a hard error: the caller falls
// back to extraction.
class TuReplayContextError final : public std::runtime_error {
public:
  explicit TuReplayContextError(const std::string &message);
};

struct TuReplayRoute {
  ast::PlannedFileRole role = ast::PlannedFileRole::owned_header;
  std::string path;
  std::size_t discovery_ordinal = 0;
  // The translation unit's own row is re-resolved from the live database at
  // replay time; owned headers are minted by the writer. A database row id is
  // therefore never serialized.
  bool is_translation_unit_row = false;
  std::optional<double> mtime;
  std::optional<std::string> md5;
  std::optional<std::vector<std::string>> compile_options;
  std::optional<std::string> driver;
  bool cleanup_symbols = false;
  ast::FactPartitionKey partition;
};

struct TuReplayContext {
  std::string translation_unit;
  std::string generation;
  std::vector<TuReplayRoute> routes;
  // The observable outcome of the extraction this entry was built from. A
  // replay must reproduce it exactly: the same stored/header counters the
  // product surfaces print and the same diagnostics the extraction reported
  // (AC #1377), neither of which is recoverable from the FactBatch alone.
  int stored = 0;
  cidx::HeaderStats headers;
  std::vector<cidx::Diagnostic> diagnostics;
};

[[nodiscard]] auto build_tu_replay_context(const ast::IndexOneOutcome &outcome)
    -> TuReplayContext;

[[nodiscard]] auto encode_tu_replay_context(const TuReplayContext &context)
    -> std::vector<std::byte>;

// Throws TuReplayContextError on a truncated, corrupt, or version-incompatible
// payload.
[[nodiscard]] auto decode_tu_replay_context(std::span<const std::byte> bytes)
    -> TuReplayContext;

// Rebuilds the publication plan. `translation_unit_file_id` is the live row id
// of the main source, never a value carried by the cache.
[[nodiscard]] auto rebuild_route_plan(const TuReplayContext &context,
                                      std::int64_t translation_unit_file_id)
    -> ast::OwnedHeaderRoutePlan;

} // namespace cidx::application
