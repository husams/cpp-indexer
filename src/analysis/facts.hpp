// Frontend-neutral versioned fact snapshots for semantic and raw-AST inputs.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace cidx::analysis {

enum class FactType : std::uint8_t { integer, boolean, string };

struct FactColumn {
  std::string name;
  FactType type = FactType::string;

  friend bool operator==(const FactColumn &, const FactColumn &) = default;
};

using FactValue = std::variant<std::int64_t, bool, std::string>;
using FactRow = std::vector<FactValue>;

struct RelationDescriptor {
  std::string name;
  int version = 1;
  int catalog_version = 1;
  std::vector<FactColumn> columns;
};

struct FactRelation {
  RelationDescriptor descriptor;
  std::vector<FactRow> rows;
};

enum class FactCompleteness : std::uint8_t {
  complete,
  partial,
  unknown,
  stale
};

enum class FactFreshness : std::uint8_t { current, stale, unknown };

class FactProviderError : public std::runtime_error {
public:
  FactProviderError(std::string code, const std::string &message);

  [[nodiscard]] const std::string &code() const noexcept { return code_; }

private:
  std::string code_;
};

struct FactRequest {
  std::vector<std::string> relations;
  std::optional<std::string> workspace_identity;
  std::optional<std::string> tu_identity;
};

struct FactSnapshot {
  std::string provider;
  std::string workspace_identity = "unknown";
  std::optional<std::string> tu_identity;
  std::string applicability = "workspace";
  int schema_version = 1;
  int catalog_version = 1;
  std::string catalog_hash;
  FactCompleteness completeness = FactCompleteness::complete;
  FactFreshness freshness = FactFreshness::unknown;
  bool truncated = false;
  std::optional<std::string> source_revision;
  std::optional<std::string> source_fingerprint;
  std::vector<std::string> evidence_references;
  std::vector<std::string> input_hashes;
  std::optional<std::string> artifact_path;
  std::map<std::string, FactRelation> relations;

  void add_relation(FactRelation relation);
  [[nodiscard]] const FactRelation *find_relation(std::string_view name) const;
  [[nodiscard]] const FactRelation &
  require_relation(std::string_view name) const;
  void validate() const;
  [[nodiscard]] std::string canonical() const;
  [[nodiscard]] std::string stable_hash() const;
};

class FactProvider {
public:
  virtual ~FactProvider() = default;
  [[nodiscard]] virtual FactSnapshot
  snapshot(const FactRequest &request) const = 0;
};

class StaticFactProvider final : public FactProvider {
public:
  explicit StaticFactProvider(FactSnapshot snapshot);

  [[nodiscard]] FactSnapshot
  snapshot(const FactRequest &request) const override;

private:
  FactSnapshot snapshot_;
};

class SqliteFactProvider final : public FactProvider {
public:
  explicit SqliteFactProvider(std::string path);

  [[nodiscard]] FactSnapshot
  snapshot(const FactRequest &request) const override;

private:
  std::string path_;
};

class AstgraphFactProvider final : public FactProvider {
public:
  explicit AstgraphFactProvider(std::string path);

  [[nodiscard]] FactSnapshot
  snapshot(const FactRequest &request) const override;

private:
  std::string path_;
};

class ExtensionFactProvider final : public FactProvider {
public:
  explicit ExtensionFactProvider(std::string path);

  [[nodiscard]] FactSnapshot
  snapshot(const FactRequest &request) const override;

private:
  std::string path_;
};

struct JoinSpec {
  std::string left_relation;
  std::string right_relation;
  std::string left_key;
  std::string right_key;
  std::string output_relation;
};

class ComposedFactProvider final : public FactProvider {
public:
  ComposedFactProvider(std::unique_ptr<FactProvider> left,
                       std::unique_ptr<FactProvider> right,
                       std::vector<JoinSpec> joins);

  [[nodiscard]] FactSnapshot
  snapshot(const FactRequest &request) const override;

private:
  std::unique_ptr<FactProvider> left_;
  std::unique_ptr<FactProvider> right_;
  std::vector<JoinSpec> joins_;
};

[[nodiscard]] FactSnapshot
compose_snapshots(const FactSnapshot &left, const FactSnapshot &right,
                  const std::vector<JoinSpec> &joins);

struct FactExportStats {
  int files = 0;
  std::int64_t rows = 0;
};

[[nodiscard]] FactExportStats write_fact_files(const FactSnapshot &snapshot,
                                               const std::string &out_dir,
                                               std::string_view prelude);

[[nodiscard]] std::string fact_file_name(std::string_view logical_relation);

[[nodiscard]] std::string fact_type_name(FactType type);
[[nodiscard]] std::string fact_value_text(const FactValue &value);

} // namespace cidx::analysis
