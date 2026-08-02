// Opt-in production indexing telemetry. The disabled path is a null check;
// no files, SQL, or timers are created unless --profile-json is present.
#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

struct sqlite3;

namespace cidx::profile {

namespace detail {
extern std::atomic_bool active_flag;
}

struct TranslationUnitRecord {
  std::string path;
  std::uint64_t start_position = 0;
  std::int64_t database_cardinality_before = 0;
  std::int64_t fact_cardinality_before = 0;
  std::uint64_t source_bytes = 0;
  std::uint64_t preprocessed_bytes = 0;
  std::uint64_t include_count = 0;
  std::uint64_t new_headers = 0;
  std::uint64_t already_indexed_headers = 0;
  std::string configuration_state;
  double wall_seconds = 0.0;
  double in_process_cpu_seconds = 0.0;
  double child_process_wall_seconds = 0.0;
  std::uint64_t peak_rss_bytes = 0;
};

class Session {
public:
  struct Impl;

  Session(std::string output_path,
          std::optional<std::string> sqlite_configuration_path);
  ~Session();
  Session(const Session &) = delete;
  Session &operator=(const Session &) = delete;
  Session(Session &&) = delete;
  Session &operator=(Session &&) = delete;

  void finish();

private:
  std::unique_ptr<Impl> impl_;
};

[[nodiscard]] inline auto active() noexcept -> bool {
  return detail::active_flag.load(std::memory_order_relaxed);
}

// Adds one wall-clock span to a caller-owned accumulator, and only while
// profiling is active: on the disabled path the constructor is one relaxed
// atomic load and no clock is read. Accumulating locally keeps hot inner
// scopes off add_timing(), which takes the session lock and allocates a name.
//
// The accumulators these fill are the disjoint root_symbols subcomponents
// (see kRootSymbol*Timing below); every span belongs to exactly one of them.
class ScopedAccumulator {
public:
  using Clock = std::chrono::steady_clock;

  explicit ScopedAccumulator(double &sink) noexcept
      : sink_(active() ? &sink : nullptr),
        start_(sink_ != nullptr ? Clock::now() : Clock::time_point{}) {}
  ~ScopedAccumulator() {
    if (sink_ != nullptr) {
      *sink_ += std::chrono::duration<double>(Clock::now() - start_).count();
    }
  }
  ScopedAccumulator(const ScopedAccumulator &) = delete;
  ScopedAccumulator &operator=(const ScopedAccumulator &) = delete;
  ScopedAccumulator(ScopedAccumulator &&) = delete;
  ScopedAccumulator &operator=(ScopedAccumulator &&) = delete;

private:
  double *sink_;
  Clock::time_point start_;
};

// The routed symbol root pass (root_symbols) decomposed into disjoint spans.
// Their sum is at most root_symbols; the remainder is published as
// root_symbols.walk when the profile is written, so nothing is counted twice.
inline constexpr std::string_view kRootSymbolWalkTiming = "root_symbols.walk";
inline constexpr std::string_view kRootSymbolRoutingTiming =
    "root_symbols.routing";
inline constexpr std::string_view kRootSymbolIdentityTiming =
    "root_symbols.identity";
inline constexpr std::string_view kRootSymbolSinkTiming = "root_symbols.sink";
inline constexpr std::string_view kRootSymbolPersistenceTiming =
    "root_symbols.persistence";

// Duplicate suppression inside that pass: records persisted through storage
// versus records answered from an identity the pass had already written.
inline constexpr std::string_view kRootSymbolIdentityPersistCounter =
    "root_symbols_identity_persisted";
inline constexpr std::string_view kRootSymbolIdentityReuseCounter =
    "root_symbols_identity_reused";
[[nodiscard]] auto next_translation_unit_position() noexcept -> std::uint64_t;
void record_translation_unit(TranslationUnitRecord record) noexcept;
void add_timing(std::string_view name, double seconds) noexcept;
void add_counter(std::string_view name, std::uint64_t amount = 1) noexcept;
void add_fact_family(std::string_view name, std::uint64_t attempted,
                     std::uint64_t persisted,
                     std::uint64_t duplicates) noexcept;
void note_transaction_begin() noexcept;
void note_transaction_commit() noexcept;
void note_transaction_rollback() noexcept;

// Generic boundary intentionally consumed by HSE-114 after integration.
void note_reconciliation(std::uint64_t rows_changed) noexcept;
void note_driver_subprocess(double wall_seconds,
                            std::uint64_t peak_rss_bytes) noexcept;
[[nodiscard]] auto driver_subprocess_wall_seconds() noexcept -> double;
[[nodiscard]] auto process_peak_rss_bytes() noexcept -> std::uint64_t;
void note_toolchain_cache_lookup(bool hit) noexcept;

void note_sqlite_prepare(double seconds, std::uint64_t sql_text_bytes) noexcept;
void note_sqlite_step(double seconds, std::uint64_t virtual_machine_steps,
                      std::uint64_t fullscan_steps,
                      std::uint64_t reprepares) noexcept;
void apply_sqlite_experiment(sqlite3 *database);

} // namespace cidx::profile
