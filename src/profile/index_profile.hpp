// Opt-in production indexing telemetry. The disabled path is a null check;
// no files, SQL, or timers are created unless --profile-json is present.
#pragma once

#include <atomic>
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
void note_toolchain_cache_lookup(bool hit) noexcept;

void note_sqlite_prepare(double seconds) noexcept;
void note_sqlite_step(double seconds, std::uint64_t virtual_machine_steps,
                      std::uint64_t fullscan_steps,
                      std::uint64_t reprepares) noexcept;
void apply_sqlite_experiment(sqlite3 *database);

} // namespace cidx::profile
