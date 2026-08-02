#include "ast/fact_batch.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

auto symbols_from_args(int argc, char **argv) -> std::size_t {
  std::size_t result = 2'000;
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument(argv[index]);
    if (argument == "--symbols" && index + 1 < argc) {
      result = std::stoull(argv[++index]);
    }
  }
  if (result < 2) {
    throw std::invalid_argument("--symbols must be at least 2");
  }
  return result;
}

auto partition() -> cidx::ast::FactPartitionKey {
  return {.file = {.component_path = "/benchmark",
                   .directory_path = "src",
                   .file_name = "scale.cpp"},
          .configuration = {.semantic_universe = "benchmark",
                            .translation_unit = "src/scale.cpp",
                            .normalized_configuration = "release",
                            .identity_source = "/benchmark/src/scale.cpp"}};
}

auto counter(const cidx::ast::FactBatchOperationCounters &counters,
             std::string_view operation, bool touched) -> std::uint64_t {
  const auto &values = touched ? counters.records_touched : counters.calls;
  const auto found = values.find(std::string(operation));
  return found == values.end() ? 0 : found->second;
}

auto canonical_fingerprint(const cidx::ast::FactBatch &batch) -> std::uint64_t {
  std::string material;
  for (const auto &[handle, key] : batch.symbol_keys()) {
    material += std::to_string(handle) + ':' + key + '\n';
  }
  for (const cidx::ast::SymbolRecord &record : batch.records().symbols) {
    material += cidx::ast::stable_symbol_record_key(record) + '\n';
  }
  for (const cidx::ast::FileFactPartition &owner : batch.partitions()) {
    material += owner.key.stable_string() + '\n';
  }
  return cidx::ast::stable_fact_hash(material);
}

} // namespace

auto main(int argc, char **argv) -> int {
  try {
    const std::size_t symbol_count = symbols_from_args(argc, argv);
    cidx::ast::FactBatchRecorder recorder("complexity-benchmark");
    const auto emission_started = std::chrono::steady_clock::now();
    std::int64_t first_id = 0;
    for (std::size_t index = 0; index < symbol_count; ++index) {
      const std::string suffix = std::to_string(index);
      const std::size_t identity_index = index % 8 == 7 ? index - 1 : index;
      const std::string identity_suffix = std::to_string(identity_index / 2);
      const std::string usr = "usr-" + identity_suffix;
      const std::string qualified = "scale::symbol_" + identity_suffix;
      cidx::ast::FactPartitionKey current_partition = partition();
      current_partition.configuration.semantic_universe =
          identity_index % 2 == 0 ? "benchmark-primary" : "benchmark-secondary";
      current_partition.configuration.normalized_configuration =
          identity_index % 4 < 2 ? "release" : "release-with-debug-info";
      recorder.set_partition(current_partition);
      cidx::ast::SymbolRecord symbol;
      symbol.file = "/benchmark/src/scale.cpp";
      symbol.usr = usr;
      symbol.spelling = "symbol_" + suffix;
      symbol.kind = 8;
      symbol.kind_name = "function";
      symbol.qual_name = qualified;
      symbol.identity_source = "/benchmark/src/scale.cpp";
      recorder.emit(symbol);
      const std::int64_t id = recorder.lookup_symbol_id(usr).value_or(0);
      if (index == 0) {
        first_id = id;
      } else {
        recorder.add_edge({.src_id = first_id,
                           .dst_id = id,
                           .kind = 1,
                           .count = 1,
                           .base_access = std::nullopt,
                           .is_virtual = std::nullopt});
      }
      static_cast<void>(recorder.type_arg_candidates(qualified, true));
      static_cast<void>(
          recorder.symbol_ids_by_qual_name_kind(qualified, "function"));
      recorder.update_display_name(id, "symbol(" + suffix + ")");
    }
    const auto emission_finished = std::chrono::steady_clock::now();
    const auto canonical_started = std::chrono::steady_clock::now();
    const cidx::ast::FactBatch batch = recorder.canonical_batch();
    const cidx::ast::FactBatch repeated_batch = recorder.canonical_batch();
    const auto canonical_finished = std::chrono::steady_clock::now();
    const std::uint64_t fingerprint = canonical_fingerprint(batch);

    const auto &counters = recorder.counters();
    const std::size_t repeated_declarations = symbol_count / 8;
    const std::size_t candidate_records_touched =
        ((3 * symbol_count) / 2) - repeated_declarations;
    const bool bounded =
        counter(counters, "lookup_symbol_sourceless", false) == symbol_count &&
        counter(counters, "lookup_symbol_sourceless", true) == symbol_count &&
        counter(counters, "type_arg_candidates", true) ==
            candidate_records_touched &&
        counter(counters, "symbol_ids_by_qual_name_kind", true) ==
            candidate_records_touched &&
        counter(counters, "update_display_name", true) ==
            symbol_count + repeated_declarations &&
        batch.records().symbols.size() == symbol_count &&
        batch.partitions().size() == 4 &&
        batch.symbol_keys().size() == symbol_count - repeated_declarations &&
        canonical_fingerprint(repeated_batch) == fingerprint;
    if (!bounded) {
      std::cerr << "operation-count contract failed: candidates="
                << counter(counters, "type_arg_candidates", true)
                << " qualified_kind="
                << counter(counters, "symbol_ids_by_qual_name_kind", true)
                << " display=" << counter(counters, "update_display_name", true)
                << " symbols=" << batch.records().symbols.size()
                << " partitions=" << batch.partitions().size()
                << " keys=" << batch.symbol_keys().size() << '\n';
      return EXIT_FAILURE;
    }

    const auto emission_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(emission_finished -
                                                             emission_started)
            .count();
    const auto canonical_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            canonical_finished - canonical_started)
            .count();
    std::cout << "{\"symbols\":" << symbol_count
              << ",\"emission_ns\":" << emission_ns
              << ",\"canonicalization_ns\":" << canonical_ns
              << ",\"sourceless_touched\":"
              << counter(counters, "lookup_symbol_sourceless", true)
              << ",\"candidate_touched\":"
              << counter(counters, "type_arg_candidates", true)
              << ",\"qualified_kind_touched\":"
              << counter(counters, "symbol_ids_by_qual_name_kind", true)
              << ",\"display_touched\":"
              << counter(counters, "update_display_name", true)
              << ",\"canonical_fingerprint\":" << fingerprint << "}\n";
    return EXIT_SUCCESS;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
