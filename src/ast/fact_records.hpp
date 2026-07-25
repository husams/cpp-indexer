// Typed extraction evidence records shared by passes and recording backends.
#pragma once

#include "ast/edge_records.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace cidx::ast {

enum class FactCompleteness : std::uint8_t {
  complete,
  partial,
  unknown,
  error,
};

enum class FactTrust : std::uint8_t {
  trusted,
  provisional,
  inferred,
};

struct EvidenceRecord {
  std::string producer;
  std::string construct;
  std::string file;
  std::int64_t line = 0;
  std::int64_t col = 0;
  FactCompleteness completeness = FactCompleteness::complete;
  FactTrust trust = FactTrust::trusted;
  std::string detail;
};

struct PresentationIntent {
  std::int64_t symbol_id = 0;
  std::vector<std::string> display_args;
};

struct TypeEdgeRecord {
  std::int64_t src_id = 0;
  std::int64_t kind = 0;
  std::int64_t position = 0;
  std::int64_t dst_id = 0;
};

struct SymbolTypeRecord {
  std::int64_t symbol_id = 0;
  std::int64_t kind = 0;
  std::int64_t type_id = 0;
};

struct ParameterFactRecord {
  std::int64_t owner_id = 0;
  ParameterRecord parameter;
};

struct DefinitionFactRecord {
  std::int64_t id = 0;
  std::int64_t symbol_id = 0;
  std::int64_t file_id = 0;
  std::int64_t line = 0;
  std::int64_t col = 0;
  std::int64_t end_line = 0;
  std::int64_t end_col = 0;
  std::optional<std::string> init_text;
};

struct DefinitionEdgeRecord {
  std::int64_t definition_id = 0;
  std::int64_t destination_id = 0;
  std::int64_t kind = 0;
};

} // namespace cidx::ast
