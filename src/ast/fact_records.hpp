// Typed extraction evidence records shared by passes and recording backends.
#pragma once

#include "ast/edge_records.hpp"
#include "ast/fact_identity.hpp"

#include <cstdint>
#include <optional>
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

struct SymbolEmissionMetadata {
  SymbolNaturalKey symbol;
  LegacyApplyOrderKey apply_order;
  std::uint64_t first_seen = 0;
  std::uint64_t last_seen = 0;
};

struct DeclarationSiteRecord {
  SymbolNaturalKey symbol;
  FactPartitionKey partition;
  std::int64_t line = 0;
  std::int64_t col = 0;
  std::int64_t end_line = 0;
  std::int64_t end_col = 0;
  bool is_definition = false;
  LegacyApplyOrderKey apply_order;
};

enum class IncludeDirectiveKind : std::uint8_t {
  include,
  include_next,
  import,
  include_macros,
  unknown,
};

struct IncludeDirectiveRecord {
  FactPartitionKey partition;
  PortableFileIdentity source;
  std::optional<PortableFileIdentity> destination;
  std::string destination_path;
  std::string spelling;
  IncludeDirectiveKind directive = IncludeDirectiveKind::include;
  std::int64_t line = 0;
  std::int64_t col = 0;
  std::int64_t begin_offset = 0;
  std::int64_t end_offset = 0;
  std::string conditional_fingerprint;
  bool is_angled = false;
  bool resolved = true;
  bool is_system = false;
  bool guarded = false;
};

struct MacroUseRecord {
  FactPartitionKey partition;
  PortableFileIdentity source;
  std::string definition_path;
  std::string name;
  std::int64_t count = 1;
};

enum class DiagnosticSeverity : std::uint8_t {
  remark,
  warning,
  error,
  fatal,
};

struct DiagnosticFactRecord {
  FactPartitionKey partition;
  DiagnosticSeverity severity = DiagnosticSeverity::warning;
  std::string spelling;
  std::optional<PortableFileIdentity> location_file;
  std::optional<std::int64_t> line;
  std::optional<std::int64_t> col;
};

enum class LifecycleCleanupKind : std::uint8_t {
  relations,
  definitions,
  diagnostics,
  includes,
  applicability,
};

struct LifecycleCleanupIntent {
  FactPartitionKey partition;
  LifecycleCleanupKind kind = LifecycleCleanupKind::relations;
  PortableFileIdentity target;
  std::string prior_generation;
};

enum class ApplicabilityRole : std::uint8_t {
  translation_unit,
  header,
};

enum class ApplicabilityState : std::uint8_t {
  registered,
  unregistered,
  ambiguous,
  stale,
  unavailable,
};

struct ApplicabilityOwnershipRecord {
  FactPartitionKey partition;
  PortableFileIdentity file;
  ApplicabilityRole role = ApplicabilityRole::header;
  ApplicabilityState state = ApplicabilityState::registered;
  std::optional<std::string> reason;
  std::string generation;
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
