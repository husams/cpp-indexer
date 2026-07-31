#include "profile/index_profile.hpp"

#include "util/errors.hpp"
#include "util/json_read.hpp"

#include <sqlite3.h>
#include <sys/resource.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <map>
#include <mutex>
#include <sstream>
#include <utility>
#include <vector>

namespace cidx::profile {

std::atomic_bool detail::active_flag = false;

namespace {

constexpr auto kTimingNames = std::to_array<std::string_view>({
    "source_validation_hashing",
    "workspace_snapshot_configuration",
    "driver_subprocesses",
    "clang_tool_inclusive",
    "clang_front_end",
    "root_symbols",
    "root_declarations",
    "root_definitions",
    "root_namespaces",
    "body_extraction",
    "fact_persistence",
    "include_persistence",
    "applicability_association",
    "commit",
    "transforms",
    "verification",
    "metrics_only_sql",
    "identity_reconciliation",
    "sqlite_prepare",
    "sqlite_vdbe",
});

auto json_string(std::string_view value) -> std::string {
  std::ostringstream output;
  output << '"';
  for (const unsigned char character : value) {
    switch (character) {
    case '"':
      output << "\\\"";
      break;
    case '\\':
      output << "\\\\";
      break;
    case '\b':
      output << "\\b";
      break;
    case '\f':
      output << "\\f";
      break;
    case '\n':
      output << "\\n";
      break;
    case '\r':
      output << "\\r";
      break;
    case '\t':
      output << "\\t";
      break;
    default:
      if (character < 0x20) {
        output << "\\u00" << std::hex << std::setw(2) << std::setfill('0')
               << static_cast<unsigned>(character) << std::dec
               << std::setfill(' ');
      } else {
        output << static_cast<char>(character);
      }
      break;
    }
  }
  output << '"';
  return output.str();
}

auto seconds_json(double value) -> std::string {
  std::ostringstream output;
  output << std::setprecision(17) << std::max(0.0, value);
  return output.str();
}

auto read_text(const std::string &path) -> std::string {
  std::ifstream input(path);
  if (!input) {
    throw CidxError("cannot read profiling SQLite configuration " + path);
  }
  return {std::istreambuf_iterator<char>(input),
          std::istreambuf_iterator<char>()};
}

auto pragma_from_member(const json_out::Member &member) -> std::string {
  const auto &[name, value] = member;
  if (name == "cache_size" || name == "mmap_size") {
    if (value.t != json_out::Value::T::Int) {
      throw CidxError("profiling SQLite option " + name +
                      " must be an integer");
    }
    return "PRAGMA " + name + " = " + std::to_string(value.i);
  }
  if (name != "temp_store" && name != "journal_mode" && name != "synchronous") {
    throw CidxError("unknown profiling SQLite option " + name);
  }
  if (value.t != json_out::Value::T::Str) {
    throw CidxError("profiling SQLite option " + name + " must be a string");
  }
  std::string setting = value.s;
  std::ranges::transform(setting, setting.begin(), [](unsigned char character) {
    return static_cast<char>(std::toupper(character));
  });
  const bool allowed =
      (name == "temp_store" &&
       (setting == "DEFAULT" || setting == "FILE" || setting == "MEMORY")) ||
      (name == "journal_mode" &&
       (setting == "DELETE" || setting == "WAL" || setting == "TRUNCATE" ||
        setting == "PERSIST" || setting == "MEMORY" || setting == "OFF")) ||
      (name == "synchronous" && (setting == "OFF" || setting == "NORMAL" ||
                                 setting == "FULL" || setting == "EXTRA"));
  if (!allowed) {
    throw CidxError("unsupported profiling SQLite value for " + name);
  }
  return "PRAGMA " + name + " = " + setting;
}

} // namespace

struct Session::Impl {
  struct FactFamilyCounts {
    std::uint64_t attempted = 0;
    std::uint64_t persisted = 0;
    std::uint64_t duplicates = 0;
  };

  explicit Impl(std::string path) : output_path(std::move(path)) {
    for (const std::string_view name : kTimingNames) {
      timings.emplace(name, 0.0);
    }
  }

  std::string output_path;
  std::vector<std::string> sqlite_pragmas;
  std::map<std::string, double, std::less<>> timings;
  std::map<std::string, std::uint64_t, std::less<>> counters;
  std::map<std::string, FactFamilyCounts, std::less<>> fact_families;
  std::vector<TranslationUnitRecord> translation_units;
  std::uint64_t sqlite_prepare_calls = 0;
  std::uint64_t sqlite_step_calls = 0;
  std::uint64_t sqlite_virtual_machine_steps = 0;
  std::uint64_t sqlite_fullscan_steps = 0;
  std::uint64_t sqlite_reprepares = 0;
  std::uint64_t transaction_begins = 0;
  std::uint64_t transaction_commits = 0;
  std::uint64_t transaction_rollbacks = 0;
  std::uint64_t driver_subprocess_count = 0;
  std::uint64_t driver_child_peak_rss_bytes = 0;
  std::uint64_t toolchain_cache_lookups = 0;
  std::uint64_t toolchain_cache_hits = 0;
  double driver_subprocess_wall_seconds = 0.0;
  bool finished = false;
  std::mutex mutex;
};

namespace {

Session::Impl *current = nullptr;
std::atomic_uint64_t telemetry_failures = 0;

template <typename Function> void best_effort(Function &&function) noexcept {
  try {
    std::forward<Function>(function)();
  } catch (...) {
    telemetry_failures.fetch_add(1, std::memory_order_relaxed);
  }
}

void write_fact_family_map(
    std::ostream &output,
    const std::map<std::string, Session::Impl::FactFamilyCounts, std::less<>>
        &values,
    int indentation) {
  output << "{";
  bool first = true;
  for (const auto &[name, counts] : values) {
    output << (first ? "\n" : ",\n") << std::string(indentation + 2, ' ')
           << json_string(name) << ": {\"attempted\": " << counts.attempted
           << ", \"persisted\": " << counts.persisted
           << ", \"duplicates\": " << counts.duplicates << "}";
    first = false;
  }
  if (!values.empty()) {
    output << "\n" << std::string(indentation, ' ');
  }
  output << "}";
}

void write_translation_unit(std::ostream &output,
                            const TranslationUnitRecord &record) {
  output << "    {\n"
         << "      \"path\": " << json_string(record.path) << ",\n"
         << "      \"start_position\": " << record.start_position << ",\n"
         << "      \"database_cardinality_before\": "
         << record.database_cardinality_before << ",\n"
         << "      \"fact_cardinality_before\": "
         << record.fact_cardinality_before << ",\n"
         << "      \"source_bytes\": " << record.source_bytes << ",\n"
         << "      \"preprocessed_bytes\": " << record.preprocessed_bytes
         << ",\n"
         << "      \"include_count\": " << record.include_count << ",\n"
         << "      \"new_headers\": " << record.new_headers << ",\n"
         << "      \"already_indexed_headers\": "
         << record.already_indexed_headers << ",\n"
         << "      \"configuration_state\": "
         << json_string(record.configuration_state) << ",\n"
         << "      \"wall_seconds\": " << seconds_json(record.wall_seconds)
         << ",\n"
         << "      \"in_process_cpu_seconds\": "
         << seconds_json(record.in_process_cpu_seconds) << ",\n"
         << "      \"child_process_wall_seconds\": "
         << seconds_json(record.child_process_wall_seconds) << ",\n"
         << "      \"peak_rss_bytes\": " << record.peak_rss_bytes << "\n"
         << "    }";
}

void write_profile(Session::Impl &impl) {
  std::ofstream output(impl.output_path, std::ios::trunc);
  if (!output) {
    throw CidxError("cannot write profiling output " + impl.output_path);
  }
  output << "{\n  \"schema_version\": 1,\n  \"summary\": {\n"
         << "    \"timings\": {";
  bool first = true;
  for (const auto &[name, value] : impl.timings) {
    output << (first ? "\n" : ",\n") << "      " << json_string(name) << ": "
           << seconds_json(value);
    first = false;
  }
  output << "\n    },\n    \"counters\": {\n"
         << "      \"association_fact_count\": "
         << impl.counters["association_fact_count"] << ",\n"
         << "      \"include_fact_count\": "
         << impl.counters["include_fact_count"] << ",\n"
         << "      \"root_traverse_decl_calls\": "
         << impl.counters["root_traverse_decl_calls"] << ",\n"
         << "      \"registered_root_traversal_budget\": "
         << impl.counters["registered_root_traversal_budget"] << ",\n"
         << "      \"observed_root_traversals\": "
         << impl.counters["observed_root_traversals"] << ",\n"
         << "      \"facts_by_family\": ";
  write_fact_family_map(output, impl.fact_families, 6);
  output << ",\n      \"sqlite\": {\n"
         << "        \"prepare_calls\": " << impl.sqlite_prepare_calls << ",\n"
         << "        \"step_calls\": " << impl.sqlite_step_calls << ",\n"
         << "        \"virtual_machine_steps\": "
         << impl.sqlite_virtual_machine_steps << ",\n"
         << "        \"fullscan_steps\": " << impl.sqlite_fullscan_steps
         << ",\n"
         << "        \"reprepares\": " << impl.sqlite_reprepares << "\n"
         << "      },\n"
         << "      \"transactions\": {\n"
         << "        \"begins\": " << impl.transaction_begins << ",\n"
         << "        \"commits\": " << impl.transaction_commits << ",\n"
         << "        \"rollbacks\": " << impl.transaction_rollbacks << "\n"
         << "      },\n"
         << "      \"toolchain_cache\": {\n"
         << "        \"lookups\": " << impl.toolchain_cache_lookups << ",\n"
         << "        \"hits\": " << impl.toolchain_cache_hits << ",\n"
         << "        \"misses\": "
         << impl.toolchain_cache_lookups - impl.toolchain_cache_hits << ",\n"
         << "        \"driver_subprocesses\": " << impl.driver_subprocess_count
         << ",\n"
         << "        \"driver_child_peak_rss_bytes\": "
         << impl.driver_child_peak_rss_bytes << "\n      },\n"
         << "      \"reconciliation_calls\": "
         << impl.counters["reconciliation_calls"] << ",\n"
         << "      \"reconciliation_rows_changed\": "
         << impl.counters["reconciliation_rows_changed"] << ",\n"
         << "      \"include_path_resolution_queries\": "
         << impl.counters["include_path_resolution_queries"] << ",\n"
         << "      \"telemetry_failures\": "
         << telemetry_failures.load(std::memory_order_relaxed) << "\n"
         << "    },\n    \"sqlite\": {\n"
         << "      \"prepare_calls\": " << impl.sqlite_prepare_calls << ",\n"
         << "      \"step_calls\": " << impl.sqlite_step_calls << "\n"
         << "    }\n  },\n  \"translation_units\": [";
  for (std::size_t index = 0; index < impl.translation_units.size(); ++index) {
    output << (index == 0 ? "\n" : ",\n");
    write_translation_unit(output, impl.translation_units[index]);
  }
  if (!impl.translation_units.empty()) {
    output << "\n";
  }
  output << "  ]\n}\n";
  if (!output) {
    throw CidxError("cannot finish profiling output " + impl.output_path);
  }
}

} // namespace

Session::Session(std::string output_path,
                 std::optional<std::string> sqlite_configuration_path)
    : impl_(std::make_unique<Impl>(std::move(output_path))) {
  if (current != nullptr) {
    throw CidxError("an indexing profiling session is already active");
  }
  const std::filesystem::path output(impl_->output_path);
  if (impl_->output_path.empty() ||
      (!output.parent_path().empty() &&
       !std::filesystem::is_directory(output.parent_path()))) {
    throw CidxError("profiling output parent directory does not exist");
  }
  if (sqlite_configuration_path) {
    const json_out::Value config =
        json_read::parse(read_text(*sqlite_configuration_path));
    if (config.t != json_out::Value::T::Obj) {
      throw CidxError("profiling SQLite configuration must be a JSON object");
    }
    for (const json_out::Member &member : config.o) {
      impl_->sqlite_pragmas.push_back(pragma_from_member(member));
    }
  }
  current = impl_.get();
  telemetry_failures.store(0, std::memory_order_relaxed);
  detail::active_flag.store(true, std::memory_order_relaxed);
}

Session::~Session() {
  if (impl_ != nullptr && !impl_->finished) {
    try {
      finish();
    } catch (...) {
      telemetry_failures.fetch_add(1, std::memory_order_relaxed);
    }
  }
  if (current == impl_.get()) {
    current = nullptr;
    detail::active_flag.store(false, std::memory_order_relaxed);
  }
}

void Session::finish() {
  std::scoped_lock lock(impl_->mutex);
  if (!impl_->finished) {
    write_profile(*impl_);
    impl_->finished = true;
  }
}

auto next_translation_unit_position() noexcept -> std::uint64_t {
  return current == nullptr ? 0 : current->translation_units.size();
}

void record_translation_unit(TranslationUnitRecord record) noexcept {
  best_effort([&record] {
    if (current != nullptr) {
      std::scoped_lock lock(current->mutex);
      current->translation_units.push_back(std::move(record));
    }
  });
}

void add_timing(std::string_view name, double seconds) noexcept {
  best_effort([name, seconds] {
    if (current != nullptr) {
      std::scoped_lock lock(current->mutex);
      current->timings[std::string(name)] += std::max(0.0, seconds);
    }
  });
}

void add_counter(std::string_view name, std::uint64_t amount) noexcept {
  best_effort([name, amount] {
    if (current != nullptr) {
      std::scoped_lock lock(current->mutex);
      current->counters[std::string(name)] += amount;
    }
  });
}

void add_fact_family(std::string_view name, std::uint64_t attempted,
                     std::uint64_t persisted,
                     std::uint64_t duplicates) noexcept {
  best_effort([name, attempted, persisted, duplicates] {
    if (current != nullptr) {
      std::scoped_lock lock(current->mutex);
      auto &counts = current->fact_families[std::string(name)];
      counts.attempted += attempted;
      counts.persisted += persisted;
      counts.duplicates += duplicates;
    }
  });
}

void note_transaction_begin() noexcept {
  if (current != nullptr) {
    std::scoped_lock lock(current->mutex);
    ++current->transaction_begins;
  }
}

void note_transaction_commit() noexcept {
  if (current != nullptr) {
    std::scoped_lock lock(current->mutex);
    ++current->transaction_commits;
  }
}

void note_transaction_rollback() noexcept {
  if (current != nullptr) {
    std::scoped_lock lock(current->mutex);
    ++current->transaction_rollbacks;
  }
}

void note_reconciliation(std::uint64_t rows_changed) noexcept {
  best_effort([rows_changed] {
    if (current != nullptr) {
      std::scoped_lock lock(current->mutex);
      ++current->counters["reconciliation_calls"];
      current->counters["reconciliation_rows_changed"] += rows_changed;
    }
  });
}

void note_driver_subprocess(double wall_seconds,
                            std::uint64_t peak_rss_bytes) noexcept {
  best_effort([wall_seconds, peak_rss_bytes] {
    if (current != nullptr) {
      std::scoped_lock lock(current->mutex);
      ++current->driver_subprocess_count;
      current->driver_subprocess_wall_seconds += std::max(0.0, wall_seconds);
      current->driver_child_peak_rss_bytes =
          std::max(current->driver_child_peak_rss_bytes, peak_rss_bytes);
      current->timings["driver_subprocesses"] += std::max(0.0, wall_seconds);
    }
  });
}

auto driver_subprocess_wall_seconds() noexcept -> double {
  if (current == nullptr) {
    return 0.0;
  }
  std::scoped_lock lock(current->mutex);
  return current->driver_subprocess_wall_seconds;
}

auto process_peak_rss_bytes() noexcept -> std::uint64_t {
  struct rusage usage{};
  if (::getrusage(RUSAGE_SELF, &usage) != 0) {
    return 0;
  }
#ifdef __APPLE__
  return static_cast<std::uint64_t>(usage.ru_maxrss);
#else
  return static_cast<std::uint64_t>(usage.ru_maxrss) * 1024U;
#endif
}

void note_toolchain_cache_lookup(bool hit) noexcept {
  if (current != nullptr) {
    std::scoped_lock lock(current->mutex);
    ++current->toolchain_cache_lookups;
    if (hit) {
      ++current->toolchain_cache_hits;
    }
  }
}

void note_sqlite_prepare(double seconds) noexcept {
  best_effort([seconds] {
    if (current != nullptr) {
      std::scoped_lock lock(current->mutex);
      ++current->sqlite_prepare_calls;
      current->timings["sqlite_prepare"] += std::max(0.0, seconds);
    }
  });
}

void note_sqlite_step(double seconds, std::uint64_t virtual_machine_steps,
                      std::uint64_t fullscan_steps,
                      std::uint64_t reprepares) noexcept {
  best_effort([seconds, virtual_machine_steps, fullscan_steps, reprepares] {
    if (current != nullptr) {
      std::scoped_lock lock(current->mutex);
      ++current->sqlite_step_calls;
      current->sqlite_virtual_machine_steps += virtual_machine_steps;
      current->sqlite_fullscan_steps += fullscan_steps;
      current->sqlite_reprepares += reprepares;
      current->timings["sqlite_vdbe"] += std::max(0.0, seconds);
    }
  });
}

void apply_sqlite_experiment(sqlite3 *database) {
  if (current == nullptr) {
    return;
  }
  for (const std::string &pragma : current->sqlite_pragmas) {
    char *error = nullptr;
    const int result =
        sqlite3_exec(database, pragma.c_str(), nullptr, nullptr, &error);
    if (result != SQLITE_OK) {
      const std::string message =
          error == nullptr ? "unknown SQLite error" : error;
      sqlite3_free(error);
      throw CidxError("cannot apply profiling SQLite option: " + message);
    }
  }
}

} // namespace cidx::profile
