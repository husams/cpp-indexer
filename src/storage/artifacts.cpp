#include "storage/artifacts.hpp"

#include "storage/storage.hpp"
#include "util/errors.hpp"
#include "util/hashing.hpp"

extern "C" {
#include "sha1/sha1.h"
}

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <exception>
#include <fcntl.h>
#include <fstream>
#include <set>
#include <sqlite3.h>
#include <sys/file.h>
#include <system_error>
#include <unistd.h>
#include <utility>

namespace cidx {
namespace {

constexpr std::string_view kEnvelopeVersion = "cidx-artifact/v1";

void bind_text(SqliteStmt &statement, int index, std::string_view value) {
  statement.bind(index, value);
}

std::string to_string(ArtifactCompleteness value) {
  switch (value) {
  case ArtifactCompleteness::complete:
    return "complete";
  case ArtifactCompleteness::partial:
    return "partial";
  case ArtifactCompleteness::unknown:
    return "unknown";
  }
  return "unknown";
}

std::string to_string(ArtifactTruncation value) {
  switch (value) {
  case ArtifactTruncation::none:
    return "none";
  case ArtifactTruncation::truncated:
    return "truncated";
  case ArtifactTruncation::unknown:
    return "unknown";
  }
  return "unknown";
}

std::string to_string(ArtifactTrust value) {
  switch (value) {
  case ArtifactTrust::unverified:
    return "unverified";
  case ArtifactTrust::producer_verified:
    return "producer-verified";
  case ArtifactTrust::reader_verified:
    return "reader-verified";
  }
  return "unknown";
}

ArtifactCompleteness completeness_from_string(std::string_view value) {
  if (value == "complete") {
    return ArtifactCompleteness::complete;
  }
  if (value == "partial") {
    return ArtifactCompleteness::partial;
  }
  return ArtifactCompleteness::unknown;
}

ArtifactTruncation truncation_from_string(std::string_view value) {
  if (value == "none") {
    return ArtifactTruncation::none;
  }
  if (value == "truncated") {
    return ArtifactTruncation::truncated;
  }
  return ArtifactTruncation::unknown;
}

ArtifactTrust trust_from_string(std::string_view value) {
  if (value == "producer-verified") {
    return ArtifactTrust::producer_verified;
  }
  if (value == "reader-verified") {
    return ArtifactTrust::reader_verified;
  }
  return ArtifactTrust::unverified;
}

class ArtifactPublicationLock {
public:
  explicit ArtifactPublicationLock(const std::filesystem::path &root) {
    std::filesystem::create_directories(root);
    fd_ = ::open((root / ".artifact-publication.lock").c_str(),
                 O_CREAT | O_RDWR, 0600);
    if (fd_ < 0 || ::flock(fd_, LOCK_EX) != 0) {
      if (fd_ >= 0) {
        (void)::close(fd_);
      }
      throw StorageError("cannot acquire artifact publication lock");
    }
  }
  ArtifactPublicationLock(const ArtifactPublicationLock &) = delete;
  ArtifactPublicationLock &operator=(const ArtifactPublicationLock &) = delete;
  ~ArtifactPublicationLock() {
    if (fd_ >= 0) {
      (void)::flock(fd_, LOCK_UN);
      (void)::close(fd_);
    }
  }

private:
  int fd_ = -1;
};

bool valid_attachment_name(std::string_view value) {
  if (value.empty()) {
    return false;
  }
  const bool first_valid =
      std::isalpha(static_cast<unsigned char>(value.front())) != 0 ||
      value.front() == '_';
  if (!first_valid) {
    return false;
  }
  return std::ranges::all_of(value, [](const char ch) {
    return std::isalnum(static_cast<unsigned char>(ch)) || ch == '_';
  });
}

bool safe_relative_path(const std::filesystem::path &path) {
  if (path.empty() || path.is_absolute()) {
    return false;
  }
  return std::ranges::none_of(path,
                              [](const auto &part) { return part == ".."; });
}

std::string digest_file(const std::filesystem::path &path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw StorageError("cannot read artifact " + path.string());
  }
  SHA1_CTX context;
  SHA1_Init(&context);
  std::array<char, static_cast<std::size_t>(64) * 1024> buffer{};
  while (input) {
    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const auto count = input.gcount();
    if (count > 0) {
      SHA1_Update(&context, buffer.data(), static_cast<unsigned long>(count));
    }
  }
  if (!input.eof()) {
    throw StorageError("cannot finish reading artifact " + path.string());
  }
  std::array<unsigned char, SHA1_DIGEST_SIZE> digest{};
  SHA1_Final(digest.data(), &context);
  static constexpr std::string_view hex = "0123456789abcdef";
  std::string result;
  result.reserve(static_cast<std::size_t>(SHA1_DIGEST_SIZE) * 2);
  for (const auto byte : digest) {
    result.push_back(hex[byte >> 4]);
    result.push_back(hex[byte & 0x0f]);
  }
  return result;
}

void fsync_path(const std::filesystem::path &path) {
  const int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) {
    throw StorageError("cannot open artifact for durability sync " +
                       path.string());
  }
  const int sync_result = ::fsync(fd);
  const int close_result = ::close(fd);
  if (sync_result != 0 || close_result != 0) {
    throw StorageError("cannot fsync artifact " + path.string());
  }
}

void fsync_directory(const std::filesystem::path &path) {
  const int fd = ::open(path.c_str(), O_RDONLY);
  if (fd >= 0) {
    (void)::fsync(fd);
    (void)::close(fd);
  }
}

std::string envelope_value(SqliteDb &db, std::string_view key) {
  auto statement = db.prepare("SELECT value FROM artifact_meta WHERE key = ?");
  statement.bind(1, key);
  return statement.step() ? statement.col_text(0) : std::string{};
}

void add_diagnostic(ArtifactValidation &validation, std::string code,
                    std::string message) {
  validation.diagnostics.push_back(ArtifactDiagnostic{
      .code = std::move(code), .message = std::move(message)});
}

std::string diagnostics_message(const ArtifactValidation &validation) {
  std::string message;
  for (const auto &diagnostic : validation.diagnostics) {
    if (!message.empty()) {
      message += "; ";
    }
    message += diagnostic.code + ": " + diagnostic.message;
  }
  return message;
}

std::string relation_list(const std::vector<std::string> &relations) {
  std::string value;
  for (const auto &relation : relations) {
    if (!value.empty()) {
      value += ',';
    }
    value += relation;
  }
  return value;
}

void write_envelope(SqliteDb &sidecar, const ArtifactSpec &spec) {
  sidecar.exec("CREATE TABLE IF NOT EXISTS artifact_meta ("
               "key TEXT PRIMARY KEY, value TEXT NOT NULL)");
  const auto insert = [&sidecar](std::string_view key, std::string_view value) {
    auto statement = sidecar.prepare(
        "INSERT OR REPLACE INTO artifact_meta(key, value) VALUES (?, ?)");
    statement.bind(1, key);
    bind_text(statement, 2, value);
    statement.step_done();
  };
  insert("manifest_version", kEnvelopeVersion);
  insert("logical_id", spec.logical_id);
  insert("kind", spec.kind);
  insert("artifact_schema", spec.artifact_schema);
  insert("catalog_version", std::to_string(spec.catalog_version));
  insert("catalog_hash", spec.catalog_hash);
  insert("producer_version", spec.producer_version);
  insert("engine_version", spec.engine_version);
  insert("workspace_identity", spec.workspace_identity);
  insert("tu_identity", spec.tu_identity);
  insert("configuration_identity", spec.configuration_identity);
  insert("input_fact_set_identity", spec.input_fact_set_identity);
  insert("completeness", to_string(spec.completeness));
  insert("truncation", to_string(spec.truncation));
  insert("trust", to_string(spec.trust));
  insert("evidence", spec.evidence);
  insert("attachment_name", spec.attachment_name);
  insert("exposed_relations", relation_list(spec.exposed_relations));
}

} // namespace

ArtifactAttachment::ArtifactAttachment(ArtifactStore *owner, Storage *storage,
                                       std::string name,
                                       bool previous_query_only)
    : owner_(owner), storage_(storage), name_(std::move(name)),
      previous_query_only_(previous_query_only) {}

ArtifactAttachment::~ArtifactAttachment() noexcept { reset(); }

ArtifactAttachment::ArtifactAttachment(ArtifactAttachment &&other) noexcept
    : owner_(std::exchange(other.owner_, nullptr)),
      storage_(std::exchange(other.storage_, nullptr)),
      name_(std::move(other.name_)),
      previous_query_only_(other.previous_query_only_) {}

ArtifactAttachment &
ArtifactAttachment::operator=(ArtifactAttachment &&other) noexcept {
  if (this != &other) {
    reset();
    owner_ = std::exchange(other.owner_, nullptr);
    storage_ = std::exchange(other.storage_, nullptr);
    name_ = std::move(other.name_);
    previous_query_only_ = other.previous_query_only_;
  }
  return *this;
}

void ArtifactAttachment::reset() noexcept {
  if (owner_ == nullptr || storage_ == nullptr) {
    return;
  }
  owner_->release_attachment(name_, previous_query_only_);
  owner_ = nullptr;
  storage_ = nullptr;
}

ArtifactStore::ArtifactStore(Storage &storage, std::filesystem::path root,
                             std::size_t max_attached)
    : storage_(storage), root_(std::move(root)), max_attached_(max_attached) {
  if (root_.empty()) {
    root_ = std::filesystem::current_path() / ".cidx-artifacts";
  }
}

ArtifactRecord ArtifactStore::read_record(SqliteStmt &statement) const {
  ArtifactRecord record;
  record.id = statement.col_int64(0);
  record.spec.logical_id = statement.col_text(1);
  record.spec.kind = statement.col_text(2);
  record.spec.artifact_schema = statement.col_text(3);
  record.spec.catalog_version = statement.col_int64(4);
  record.spec.catalog_hash = statement.col_text(5);
  record.spec.producer_version = statement.col_text(6);
  record.spec.engine_version = statement.col_text(7);
  record.spec.workspace_identity = statement.col_text(8);
  record.spec.tu_identity = statement.col_text(9);
  record.spec.configuration_identity = statement.col_text(10);
  record.spec.input_fact_set_identity = statement.col_text(11);
  record.spec.completeness = completeness_from_string(statement.col_text(12));
  record.spec.truncation = truncation_from_string(statement.col_text(13));
  record.spec.trust = trust_from_string(statement.col_text(14));
  record.spec.evidence = statement.col_text(15);
  record.spec.attachment_name = statement.col_text(16);
  record.spec.retention_policy = statement.col_text(17);
  record.relative_path = statement.col_text(18);
  record.content_hash = statement.col_text(19);
  record.byte_size = statement.col_int64(20);
  record.state = statement.col_text(21);
  record.created_at = statement.col_text(22);
  record.published_at = statement.col_text(23);

  auto relations = storage_.raw_db().prepare(
      "SELECT relation_name FROM artifact_relation WHERE artifact_id = ? "
      "ORDER BY relation_name");
  relations.bind(1, record.id);
  while (relations.step()) {
    record.spec.exposed_relations.push_back(relations.col_text(0));
  }
  return record;
}

std::optional<ArtifactRecord>
ArtifactStore::current(std::string_view logical_id) const {
  auto statement = storage_.raw_db().prepare(
      "SELECT id, logical_id, kind, artifact_schema, catalog_version, "
      "catalog_hash, producer_version, engine_version, workspace_identity, "
      "tu_identity, "
      "configuration_identity, input_fact_set_identity, completeness, "
      "truncation, trust, evidence, attachment_name, retention_policy, "
      "relative_path, "
      "content_hash, byte_size, state, created_at, published_at FROM artifact "
      "WHERE logical_id = ? AND state = 'current' ORDER BY id DESC LIMIT 1");
  statement.bind(1, logical_id);
  return statement.step() ? std::optional(read_record(statement))
                          : std::nullopt;
}

ArtifactValidation
ArtifactStore::validate_record(const ArtifactRecord &record) const {
  ArtifactValidation validation{.manifest = record, .diagnostics = {}};
  const auto relative = std::filesystem::path(record.relative_path);
  if (!safe_relative_path(relative)) {
    add_diagnostic(validation, "invalid_location",
                   "manifest location must be a safe relative path");
    return validation;
  }
  const auto path = root_ / relative;
  std::error_code ec;
  if (!std::filesystem::is_regular_file(path, ec)) {
    add_diagnostic(validation, "missing", "artifact file is missing");
    return validation;
  }
  const auto size =
      static_cast<std::int64_t>(std::filesystem::file_size(path, ec));
  if (ec || size != record.byte_size) {
    add_diagnostic(validation, "corrupt",
                   "artifact byte size differs from the manifest");
  }
  if (!ec && digest_file(path) != record.content_hash) {
    add_diagnostic(validation, "corrupt",
                   "artifact content hash differs from the manifest");
  }

  try {
    SqliteDb sidecar(path.string(), true);
    auto check = sidecar.prepare("PRAGMA integrity_check");
    if (!check.step() || check.col_text(0) != "ok") {
      add_diagnostic(validation, "corrupt", "sidecar integrity_check failed");
    }
    if (envelope_value(sidecar, "manifest_version") != kEnvelopeVersion ||
        envelope_value(sidecar, "logical_id") != record.spec.logical_id ||
        envelope_value(sidecar, "kind") != record.spec.kind ||
        envelope_value(sidecar, "artifact_schema") !=
            record.spec.artifact_schema ||
        envelope_value(sidecar, "catalog_version") !=
            std::to_string(record.spec.catalog_version) ||
        envelope_value(sidecar, "catalog_hash") != record.spec.catalog_hash ||
        envelope_value(sidecar, "producer_version") !=
            record.spec.producer_version ||
        envelope_value(sidecar, "engine_version") !=
            record.spec.engine_version ||
        envelope_value(sidecar, "workspace_identity") !=
            record.spec.workspace_identity ||
        envelope_value(sidecar, "tu_identity") != record.spec.tu_identity ||
        envelope_value(sidecar, "configuration_identity") !=
            record.spec.configuration_identity ||
        envelope_value(sidecar, "input_fact_set_identity") !=
            record.spec.input_fact_set_identity ||
        envelope_value(sidecar, "completeness") !=
            to_string(record.spec.completeness) ||
        envelope_value(sidecar, "truncation") !=
            to_string(record.spec.truncation) ||
        envelope_value(sidecar, "trust") != to_string(record.spec.trust) ||
        envelope_value(sidecar, "evidence") != record.spec.evidence ||
        envelope_value(sidecar, "attachment_name") !=
            record.spec.attachment_name ||
        envelope_value(sidecar, "exposed_relations") !=
            relation_list(record.spec.exposed_relations)) {
      add_diagnostic(validation, "incompatible",
                     "sidecar envelope does not match the manifest");
    }
  } catch (const StorageError &error) {
    add_diagnostic(validation, "corrupt", error.what());
  }
  if (record.spec.completeness != ArtifactCompleteness::complete) {
    add_diagnostic(
        validation, "partial",
        "artifact is not complete and cannot answer a complete query");
  }
  if (record.spec.truncation != ArtifactTruncation::none) {
    add_diagnostic(validation, "truncated",
                   "artifact is truncated and cannot answer a complete query");
  }
  if (record.spec.trust == ArtifactTrust::unverified) {
    add_diagnostic(validation, "untrusted",
                   "artifact trust policy does not permit attachment");
  }
  if (record.spec.artifact_schema.empty() ||
      record.spec.artifact_schema == "unknown" ||
      record.spec.catalog_hash.empty() ||
      record.spec.producer_version.empty() ||
      record.spec.producer_version == "unknown" ||
      record.spec.engine_version.empty() ||
      record.spec.engine_version == "unknown") {
    add_diagnostic(validation, "incompatible",
                   "artifact compatibility fields are incomplete");
  }
  if (record.spec.catalog_version != catalog::kCatalogVersion ||
      record.spec.catalog_hash != catalog::kCatalogHash) {
    add_diagnostic(
        validation, "incompatible",
        "artifact semantic catalog contract does not match the reader");
  }
  if (record.spec.evidence != "source" && record.spec.evidence != "derived" &&
      record.spec.evidence != "inferred" && record.spec.evidence != "runtime" &&
      record.spec.evidence != "assumption" && record.spec.evidence != "proof") {
    add_diagnostic(validation, "incompatible",
                   "artifact evidence class is invalid");
  }
  return validation;
}

ArtifactValidation ArtifactStore::validate(std::string_view logical_id) const {
  const auto record = current(logical_id);
  if (!record) {
    ArtifactValidation result;
    add_diagnostic(result, "missing", "no current manifest entry exists");
    return result;
  }
  return validate_record(*record);
}

ArtifactRecord ArtifactStore::publish(const ArtifactSpec &input_spec,
                                      const SidecarWriter &writer) {
  if (input_spec.logical_id.empty() || input_spec.kind.empty() ||
      input_spec.workspace_identity.empty() ||
      !valid_attachment_name(input_spec.attachment_name)) {
    throw StorageError(
        "artifact manifest has an incomplete or invalid identity");
  }
  if (!writer) {
    throw StorageError("artifact publication requires a sidecar writer");
  }
  ArtifactSpec spec = input_spec;
  ArtifactPublicationLock publication_lock(root_);
  std::ranges::sort(spec.exposed_relations);
  spec.exposed_relations.erase(
      std::ranges::unique(spec.exposed_relations).begin(),
      spec.exposed_relations.end());

  const auto staging = root_ / ".staging";
  std::filesystem::create_directories(staging);
  const auto nonce =
      std::chrono::steady_clock::now().time_since_epoch().count();
  const auto temporary =
      staging / (sha1_hex(spec.logical_id + std::to_string(nonce)) + ".db");
  try {
    {
      SqliteDb sidecar(temporary.string());
      writer(sidecar);
      write_envelope(sidecar, spec);
    }
    return publish_staged(spec, temporary);
  } catch (...) {
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    throw;
  }
}

ArtifactRecord
ArtifactStore::publish_existing(const ArtifactSpec &input_spec,
                                const std::filesystem::path &source_path) {
  if (input_spec.logical_id.empty() || input_spec.kind.empty() ||
      input_spec.workspace_identity.empty() ||
      !valid_attachment_name(input_spec.attachment_name)) {
    throw StorageError(
        "artifact manifest has an incomplete or invalid identity");
  }
  std::error_code ec;
  if (!std::filesystem::is_regular_file(source_path, ec)) {
    throw StorageError("cannot adopt missing artifact " + source_path.string());
  }
  ArtifactSpec spec = input_spec;
  ArtifactPublicationLock publication_lock(root_);
  std::ranges::sort(spec.exposed_relations);
  spec.exposed_relations.erase(
      std::ranges::unique(spec.exposed_relations).begin(),
      spec.exposed_relations.end());
  const auto staging = root_ / ".staging";
  std::filesystem::create_directories(staging);
  const auto nonce =
      std::chrono::steady_clock::now().time_since_epoch().count();
  const auto temporary =
      staging / (sha1_hex(spec.logical_id + std::to_string(nonce)) + ".db");
  try {
    std::filesystem::copy_file(
        source_path, temporary,
        std::filesystem::copy_options::overwrite_existing);
    {
      SqliteDb sidecar(temporary.string());
      auto check = sidecar.prepare("PRAGMA integrity_check");
      if (!check.step() || check.col_text(0) != "ok") {
        throw StorageError("cannot adopt corrupt artifact " +
                           source_path.string());
      }
      write_envelope(sidecar, spec);
    }
    const auto record = publish_staged(spec, temporary);
    if (std::filesystem::absolute(source_path) !=
        std::filesystem::absolute(root_ / record.relative_path)) {
      std::filesystem::remove(source_path, ec);
    }
    return record;
  } catch (...) {
    std::filesystem::remove(temporary, ec);
    throw;
  }
}

ArtifactRecord
ArtifactStore::publish_staged(const ArtifactSpec &spec,
                              const std::filesystem::path &staged_path) {
  try {
    const auto hash = digest_file(staged_path);
    const auto relative = std::filesystem::path("artifacts") /
                          sha1_hex(spec.kind) / (hash + ".db");
    const auto final_path = root_ / relative;
    std::filesystem::create_directories(final_path.parent_path());
    if (std::filesystem::exists(final_path)) {
      if (digest_file(final_path) != hash) {
        throw StorageError("content-addressed artifact path already contains a "
                           "different file");
      }
      std::filesystem::remove(staged_path);
    } else {
      fsync_path(staged_path);
      std::filesystem::rename(staged_path, final_path);
      fsync_directory(final_path.parent_path());
    }

    ArtifactRecord record;
    record.spec = spec;
    record.relative_path = relative.generic_string();
    record.content_hash = hash;
    record.byte_size =
        static_cast<std::int64_t>(std::filesystem::file_size(final_path));
    record.state = "current";
    auto existing = storage_.raw_db().prepare(
        "SELECT id, logical_id, kind, artifact_schema, catalog_version, "
        "catalog_hash, producer_version, engine_version, workspace_identity, "
        "tu_identity, configuration_identity, input_fact_set_identity, "
        "completeness, truncation, trust, evidence, attachment_name, "
        "retention_policy, relative_path, content_hash, byte_size, state, "
        "created_at, published_at FROM artifact WHERE logical_id = ? AND "
        "content_hash = ? ORDER BY id DESC LIMIT 1");
    bind_text(existing, 1, spec.logical_id);
    bind_text(existing, 2, hash);
    if (existing.step()) {
      const auto existing_id = existing.col_int64(0);
      auto txn = storage_.transaction();
      auto supersede = storage_.raw_db().prepare(
          "UPDATE artifact SET state = 'stale' WHERE logical_id = ?");
      bind_text(supersede, 1, spec.logical_id);
      supersede.step_done();
      auto restore =
          storage_.raw_db().prepare("UPDATE artifact SET state = 'current', "
                                    "published_at = CURRENT_TIMESTAMP "
                                    "WHERE id = ?");
      restore.bind(1, existing_id);
      restore.step_done();
      txn.commit();
      std::filesystem::remove(staged_path);
      const auto current_record = current(spec.logical_id);
      if (!current_record) {
        throw StorageError("idempotent artifact publication lost its manifest");
      }
      return *current_record;
    }
    auto txn = storage_.transaction();
    auto supersede =
        storage_.raw_db().prepare("UPDATE artifact SET state = 'stale' WHERE "
                                  "logical_id = ? AND state = 'current'");
    bind_text(supersede, 1, spec.logical_id);
    supersede.step_done();
    auto insert = storage_.raw_db().prepare(
        "INSERT INTO artifact(logical_id, kind, artifact_schema, "
        "catalog_version, catalog_hash, "
        "producer_version, engine_version, workspace_identity, tu_identity, "
        "configuration_identity, input_fact_set_identity, completeness, "
        "truncation, "
        "trust, evidence, attachment_name, retention_policy, relative_path, "
        "content_hash, "
        "byte_size, state, published_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, "
        "?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, CURRENT_TIMESTAMP)");
    bind_text(insert, 1, spec.logical_id);
    bind_text(insert, 2, spec.kind);
    bind_text(insert, 3, spec.artifact_schema);
    bind_text(insert, 4, std::to_string(spec.catalog_version));
    bind_text(insert, 5, spec.catalog_hash);
    bind_text(insert, 6, spec.producer_version);
    bind_text(insert, 7, spec.engine_version);
    bind_text(insert, 8, spec.workspace_identity);
    bind_text(insert, 9, spec.tu_identity);
    bind_text(insert, 10, spec.configuration_identity);
    bind_text(insert, 11, spec.input_fact_set_identity);
    bind_text(insert, 12, to_string(spec.completeness));
    bind_text(insert, 13, to_string(spec.truncation));
    bind_text(insert, 14, to_string(spec.trust));
    bind_text(insert, 15, spec.evidence);
    bind_text(insert, 16, spec.attachment_name);
    bind_text(insert, 17, spec.retention_policy);
    bind_text(insert, 18, record.relative_path);
    bind_text(insert, 19, record.content_hash);
    insert.bind(20, record.byte_size);
    bind_text(insert, 21, record.state);
    insert.step_done();
    record.id = sqlite3_last_insert_rowid(storage_.raw_db().raw());
    for (const auto &relation : spec.exposed_relations) {
      auto relation_insert = storage_.raw_db().prepare(
          "INSERT INTO artifact_relation(artifact_id, relation_name) VALUES "
          "(?, ?)");
      relation_insert.bind(1, record.id);
      bind_text(relation_insert, 2, relation);
      relation_insert.step_done();
    }
    txn.commit();
    return record;
  } catch (...) {
    std::error_code ignored;
    std::filesystem::remove(staged_path, ignored);
    throw;
  }
}

std::unique_ptr<ArtifactAttachment>
ArtifactStore::attach_current(std::string_view logical_id) {
  if (attached_names_.size() >= max_attached_ ||
      storage_.attached_artifact_names_.size() >= 8) {
    throw StorageError("artifact attachment limit exceeded");
  }
  const auto validation = validate(logical_id);
  if (!validation.usable()) {
    throw StorageError("cannot attach artifact: " +
                       diagnostics_message(validation));
  }
  if (!validation.manifest) {
    throw StorageError("cannot attach artifact without a manifest");
  }
  const auto &record = *validation.manifest;
  const auto name = record.spec.attachment_name;
  if (storage_.attached_artifact_names_.contains(name)) {
    throw StorageError("artifact attachment name is already in use");
  }
  auto query_only = storage_.raw_db().prepare("PRAGMA query_only");
  const bool previous_query_only =
      query_only.step() && query_only.col_int64(0) != 0;
  const auto absolute = (root_ / record.relative_path).lexically_normal();
  auto attach =
      storage_.raw_db().prepare("ATTACH DATABASE ? AS \"" + name + "\"");
  const auto uri = "file:" + absolute.string() + "?mode=ro";
  bind_text(attach, 1, uri);
  attach.step_done();
  try {
    storage_.raw_db().exec("PRAGMA query_only = 1");
  } catch (...) {
    const auto ignored = std::current_exception();
    (void)ignored;
    storage_.raw_db().exec("DETACH DATABASE \"" + name + "\"");
    throw;
  }
  if (storage_.attached_artifact_names_.empty()) {
    storage_.artifact_query_only_before_attach_ = previous_query_only;
    query_only_before_attach_ = previous_query_only;
  }
  attached_names_.push_back(name);
  storage_.attached_artifact_names_.insert(name);
  return std::unique_ptr<ArtifactAttachment>(
      new ArtifactAttachment(this, &storage_, name, previous_query_only));
}

void ArtifactStore::release_attachment(std::string_view name,
                                       bool previous_query_only) noexcept {
  try {
    storage_.raw_db().exec("DETACH DATABASE \"" + std::string(name) + "\"");
    const auto it = std::ranges::find(attached_names_, name);
    if (it != attached_names_.end()) {
      attached_names_.erase(it);
    }
    storage_.attached_artifact_names_.erase(std::string(name));
  } catch (...) {
    const auto ignored = std::current_exception();
    (void)ignored;
  }
  if (storage_.attached_artifact_names_.empty()) {
    reset_query_only(storage_.artifact_query_only_before_attach_.value_or(
        previous_query_only));
    storage_.artifact_query_only_before_attach_.reset();
    query_only_before_attach_.reset();
  }
}

void ArtifactStore::reset_query_only(bool previous) noexcept {
  try {
    storage_.raw_db().exec(std::string("PRAGMA query_only = ") +
                           (previous ? "1" : "0"));
  } catch (...) {
    const auto ignored = std::current_exception();
    (void)ignored;
  }
}

void ArtifactStore::record_identity_mapping(
    std::string_view logical_id, std::string_view local_identity,
    std::string_view identity_kind, std::string_view stable_identity,
    std::string_view resolution_state,
    std::optional<std::int64_t> core_symbol_id, std::string_view diagnostic) {
  const auto record = current(logical_id);
  if (!record) {
    throw StorageError("cannot record mapping for a missing current artifact");
  }
  if (local_identity.empty() || identity_kind.empty() ||
      stable_identity.empty()) {
    throw StorageError(
        "artifact identity mapping requires stable identity fields");
  }
  auto statement = storage_.raw_db().prepare(
      "INSERT OR REPLACE INTO artifact_identity_map(artifact_id, "
      "local_identity, "
      "identity_kind, stable_identity, resolution_state, core_symbol_id, "
      "diagnostic) "
      "VALUES (?, ?, ?, ?, ?, ?, ?)");
  statement.bind(1, record->id);
  bind_text(statement, 2, local_identity);
  bind_text(statement, 3, identity_kind);
  bind_text(statement, 4, stable_identity);
  bind_text(statement, 5, resolution_state);
  if (core_symbol_id) {
    statement.bind(6, *core_symbol_id);
  } else {
    statement.bind_null(6);
  }
  bind_text(statement, 7, diagnostic);
  statement.step_done();
}

void ArtifactStore::lease(std::string_view logical_id,
                          std::string_view lease_id, std::string_view purpose) {
  const auto record = current(logical_id);
  if (!record) {
    throw StorageError("cannot lease a missing current artifact");
  }
  auto statement = storage_.raw_db().prepare(
      "INSERT OR REPLACE INTO artifact_lease(artifact_id, lease_id, purpose) "
      "VALUES (?, ?, ?)");
  statement.bind(1, record->id);
  bind_text(statement, 2, lease_id);
  bind_text(statement, 3, purpose);
  statement.step_done();
}

void ArtifactStore::unlease(std::string_view logical_id,
                            std::string_view lease_id) {
  auto statement = storage_.raw_db().prepare(
      "DELETE FROM artifact_lease WHERE lease_id = ? AND artifact_id IN "
      "(SELECT id FROM artifact WHERE logical_id = ?)");
  bind_text(statement, 1, lease_id);
  bind_text(statement, 2, logical_id);
  statement.step_done();
}

void ArtifactStore::pin(std::string_view logical_id, std::string_view pin_id,
                        std::string_view reason) {
  const auto record = current(logical_id);
  if (!record) {
    throw StorageError("cannot pin a missing current artifact");
  }
  auto statement = storage_.raw_db().prepare(
      "INSERT OR REPLACE INTO artifact_pin(artifact_id, pin_id, reason) VALUES "
      "(?, ?, ?)");
  statement.bind(1, record->id);
  bind_text(statement, 2, pin_id);
  bind_text(statement, 3, reason);
  statement.step_done();
}

void ArtifactStore::unpin(std::string_view logical_id,
                          std::string_view pin_id) {
  auto statement = storage_.raw_db().prepare(
      "DELETE FROM artifact_pin WHERE pin_id = ? AND artifact_id IN "
      "(SELECT id FROM artifact WHERE logical_id = ?)");
  bind_text(statement, 1, pin_id);
  bind_text(statement, 2, logical_id);
  statement.step_done();
}

std::pair<std::vector<ArtifactRecord>, std::vector<ArtifactDiagnostic>>
ArtifactStore::export_plan(bool include_optional) const {
  std::vector<ArtifactRecord> records;
  std::vector<ArtifactDiagnostic> diagnostics;
  auto statement = storage_.raw_db().prepare(
      "SELECT id, logical_id, kind, artifact_schema, catalog_version, "
      "catalog_hash, producer_version, engine_version, workspace_identity, "
      "tu_identity, "
      "configuration_identity, input_fact_set_identity, completeness, "
      "truncation, trust, evidence, attachment_name, retention_policy, "
      "relative_path, content_hash, "
      "byte_size, state, created_at, published_at FROM artifact WHERE state = "
      "'current' "
      "ORDER BY logical_id");
  while (statement.step()) {
    auto record = read_record(statement);
    if (!include_optional && record.spec.retention_policy == "optional") {
      continue;
    }
    records.push_back(record);
    const auto validation = validate_record(record);
    diagnostics.insert(diagnostics.end(), validation.diagnostics.begin(),
                       validation.diagnostics.end());
  }
  return {std::move(records), std::move(diagnostics)};
}

std::size_t ArtifactStore::recover() {
  ArtifactPublicationLock publication_lock(root_);
  std::set<std::string> referenced;
  auto statement = storage_.raw_db().prepare(
      "SELECT relative_path FROM artifact WHERE EXISTS (SELECT 1 FROM "
      "artifact_lease "
      "WHERE artifact_lease.artifact_id = artifact.id) OR EXISTS (SELECT 1 "
      "FROM artifact_pin "
      "WHERE artifact_pin.artifact_id = artifact.id) OR state = 'current'");
  while (statement.step()) {
    referenced.insert(statement.col_text(0));
  }
  std::size_t removed = 0;
  auto stale = storage_.raw_db().prepare(
      "SELECT id, relative_path FROM artifact WHERE state IN ('stale', "
      "'retired') "
      "AND NOT EXISTS (SELECT 1 FROM artifact_lease WHERE "
      "artifact_lease.artifact_id = artifact.id) "
      "AND NOT EXISTS (SELECT 1 FROM artifact_pin WHERE "
      "artifact_pin.artifact_id = artifact.id)");
  while (stale.step()) {
    const auto id = stale.col_int64(0);
    const auto relative = stale.col_text(1);
    std::error_code ec;
    std::filesystem::remove(root_ / relative, ec);
    if (!ec || !std::filesystem::exists(root_ / relative)) {
      auto erase =
          storage_.raw_db().prepare("DELETE FROM artifact WHERE id = ?");
      erase.bind(1, id);
      erase.step_done();
      ++removed;
    }
  }
  const auto artifacts_root = root_ / "artifacts";
  if (std::filesystem::exists(artifacts_root)) {
    for (const auto &entry :
         std::filesystem::recursive_directory_iterator(artifacts_root)) {
      if (!entry.is_regular_file()) {
        continue;
      }
      const auto relative =
          std::filesystem::relative(entry.path(), root_).generic_string();
      if (!referenced.contains(relative)) {
        std::error_code ec;
        std::filesystem::remove(entry.path(), ec);
        if (!ec) {
          ++removed;
        }
      }
    }
  }
  const auto staging = root_ / ".staging";
  if (std::filesystem::exists(staging)) {
    for (const auto &entry : std::filesystem::directory_iterator(staging)) {
      if (!entry.is_regular_file()) {
        continue;
      }
      std::error_code ec;
      std::filesystem::remove(entry.path(), ec);
      if (!ec) {
        ++removed;
      }
    }
  }
  return removed;
}

} // namespace cidx
