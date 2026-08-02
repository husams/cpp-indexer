// Correctness-first transactional replay of immutable fact batches.
#pragma once

#include "ast/fact_batch.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <string>

namespace cidx::application {

struct TransientFactApplyMap {
  std::map<std::int64_t, std::int64_t> symbols;
  std::map<std::int64_t, std::int64_t> relations;
  std::map<std::int64_t, std::int64_t> definitions;
};

class FactBatchReplayPort {
public:
  virtual ~FactBatchReplayPort() = default;

  virtual void begin_translation_unit() = 0;
  virtual auto apply_symbol(const ast::FactPartitionKey &partition,
                            const ast::SymbolRecord &record,
                            std::string_view natural_key) -> std::int64_t = 0;
  virtual auto apply_relation(const ast::FactPartitionKey &partition,
                              const ast::EdgeRecord &record)
      -> std::int64_t = 0;
  virtual auto apply_definition(const ast::FactPartitionKey &partition,
                                const ast::DefinitionFactRecord &record)
      -> std::int64_t = 0;
  virtual void apply_other(const ast::FactPartitionKey &partition,
                           ast::FactFamily family, std::size_t record_index,
                           const ast::FactRecords &records,
                           const TransientFactApplyMap &ids) = 0;
  virtual void commit_translation_unit() = 0;
  virtual void rollback_translation_unit() noexcept = 0;
};

enum class FactReplayFailurePoint : std::uint8_t {
  none,
  before_apply,
  after_symbols,
  before_commit,
};

struct FactReplayResult {
  bool committed = false;
  TransientFactApplyMap ids;
  std::optional<std::string> error;
};

[[nodiscard]] auto
replay_fact_batch(const ast::FactBatch &batch, FactBatchReplayPort &port,
                  FactReplayFailurePoint failure = FactReplayFailurePoint::none)
    -> FactReplayResult;

} // namespace cidx::application
