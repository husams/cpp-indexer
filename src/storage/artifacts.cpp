#include "storage/artifacts.hpp"

#include "storage/storage.hpp"
#include "util/errors.hpp"
#include "util/hashing.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <exception>
#include <fcntl.h>
#include <fstream>
#include <iterator>
#include <limits>
#include <set>
#include <sqlite3.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

namespace cidx {
struct ArtifactAttachmentLifetime {
  ArtifactStore *owner = nullptr;
};

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

class ScopedFd {
public:
  explicit ScopedFd(int fd = -1) : fd_(fd) {}
  ~ScopedFd() {
    if (fd_ >= 0) {
      (void)::close(fd_);
    }
  }
  ScopedFd(const ScopedFd &) = delete;
  ScopedFd &operator=(const ScopedFd &) = delete;
  ScopedFd(ScopedFd &&other) noexcept : fd_(other.fd_) { other.fd_ = -1; }
  ScopedFd &operator=(ScopedFd &&other) noexcept {
    if (this != &other) {
      if (fd_ >= 0) {
        (void)::close(fd_);
      }
      fd_ = other.fd_;
      other.fd_ = -1;
    }
    return *this;
  }

  [[nodiscard]] int get() const { return fd_; }
  [[nodiscard]] int release() {
    const int fd = fd_;
    fd_ = -1;
    return fd;
  }

private:
  int fd_;
};

void fsync_fd(int fd, std::string_view description);
int open_trusted_root(const std::filesystem::path &path);
int open_child_directory(int parent_fd, std::string_view name, bool create);
void validate_directory_descriptor(int parent_fd, std::string_view name,
                                   int directory_fd);
int open_relative_file(int root_fd, const std::filesystem::path &relative,
                       int flags);
std::string create_staged_name(const ArtifactSpec &spec);
int create_staged_file(int staging_fd, std::string_view name);
void remove_staged_file(int staging_fd, std::string_view name) noexcept;

class ArtifactPublicationLock {
public:
  explicit ArtifactPublicationLock(int root_fd) {
    fd_ = ::openat(root_fd, ".artifact-publication.lock",
                   O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
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

void fsync_fd(const int fd, const std::string_view description) {
  if (fd < 0 || ::fsync(fd) != 0) {
    throw StorageError("cannot fsync " + std::string(description));
  }
}

int open_trusted_root(const std::filesystem::path &path) {
  const auto absolute = std::filesystem::absolute(path).lexically_normal();
  ScopedFd current(::open(absolute.root_path().c_str(),
                          O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
  if (current.get() < 0) {
    throw StorageError("cannot open artifact root " + absolute.string());
  }
  for (const auto &part : absolute.relative_path()) {
    if (part == ".") {
      continue;
    }
    if (part == ".." || part.empty()) {
      throw StorageError("artifact root contains an unsafe path component");
    }
    int next = ::openat(current.get(), part.c_str(),
                        O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    bool created = false;
    if (next < 0 && errno == ENOENT) {
      if (::mkdirat(current.get(), part.c_str(), 0700) != 0 &&
          errno != EEXIST) {
        throw StorageError("cannot create artifact root component " +
                           part.string());
      }
      created = true;
      next = ::openat(current.get(), part.c_str(),
                      O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    }
    if (next < 0) {
      throw StorageError("cannot open artifact root component " +
                         part.string());
    }
    if (created) {
      fsync_fd(current.get(), "artifact root parent");
    }
    current = ScopedFd(next);
  }
  return current.release();
}

int open_child_directory(const int parent_fd, const std::string_view name,
                         const bool create) {
  const std::string component(name);
  bool created = false;
  if (create && ::mkdirat(parent_fd, component.c_str(), 0700) == 0) {
    created = true;
  } else if (create && errno != EEXIST) {
    throw StorageError("cannot create artifact directory " + component);
  }
  const int fd = ::openat(parent_fd, component.c_str(),
                          O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) {
    throw StorageError("cannot open artifact directory " + component);
  }
  if (created) {
    fsync_fd(parent_fd, "artifact directory parent");
  }
  return fd;
}

void validate_directory_descriptor(const int parent_fd,
                                   const std::string_view name,
                                   const int directory_fd) {
  const std::string component(name);
  struct stat descriptor_stat{};
  struct stat entry_stat{};
  if (::fstat(directory_fd, &descriptor_stat) != 0 ||
      !S_ISDIR(descriptor_stat.st_mode) ||
      ::fstatat(parent_fd, component.c_str(), &entry_stat,
                AT_SYMLINK_NOFOLLOW) != 0 ||
      !S_ISDIR(entry_stat.st_mode) ||
      descriptor_stat.st_dev != entry_stat.st_dev ||
      descriptor_stat.st_ino != entry_stat.st_ino) {
    throw StorageError("artifact publication directory was replaced");
  }
}

int open_relative_file(const int root_fd, const std::filesystem::path &relative,
                       const int flags) {
  if (!safe_relative_path(relative)) {
    throw StorageError("artifact path is not safely relative to its root");
  }
  ScopedFd current(::dup(root_fd));
  if (current.get() < 0) {
    throw StorageError("cannot duplicate artifact root descriptor");
  }
  auto part = relative.begin();
  const auto end = relative.end();
  if (part == end) {
    throw StorageError("artifact file path is empty");
  }
  for (; part != end; ++part) {
    if (*part == "." || part->empty()) {
      continue;
    }
    const bool final = std::next(part) == end;
    const int open_flags =
        final ? flags | O_CLOEXEC | O_NOFOLLOW
              : O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW;
    const int next = ::openat(current.get(), part->c_str(), open_flags);
    if (next < 0) {
      throw StorageError("cannot open artifact path component " +
                         part->string());
    }
    if (final) {
      return next;
    }
    current = ScopedFd(next);
  }
  throw StorageError("artifact file path has no final component");
}

void remove_relative_file(const int root_fd,
                          const std::filesystem::path &relative) noexcept {
  try {
    if (!safe_relative_path(relative) || relative.filename().empty()) {
      return;
    }
    const auto parent = relative.parent_path();
    ScopedFd parent_fd =
        parent.empty() ? ScopedFd(::dup(root_fd))
                       : ScopedFd(open_relative_file(root_fd, parent,
                                                     O_RDONLY | O_DIRECTORY));
    if (parent_fd.get() >= 0) {
      (void)::unlinkat(parent_fd.get(), relative.filename().c_str(), 0);
    }
  } catch (...) {
    return;
  }
}

std::string create_staged_name(const ArtifactSpec &spec) {
  const auto nonce =
      std::chrono::steady_clock::now().time_since_epoch().count();
  return sha256_hex(spec.logical_id + std::to_string(nonce)) + ".db";
}

int create_staged_file(const int staging_fd, const std::string_view name) {
  const std::string filename(name);
  const int fd =
      ::openat(staging_fd, filename.c_str(),
               O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (fd < 0) {
    throw StorageError("cannot create artifact staging file safely");
  }
  return fd;
}

void remove_staged_file(const int staging_fd,
                        const std::string_view name) noexcept {
  try {
    const std::string filename(name);
    (void)::unlinkat(staging_fd, filename.c_str(), 0);
  } catch (...) {
    return;
  }
}

std::string digest_file(const std::filesystem::path &path) {
  const auto digest = sha256_of(path.string());
  if (!digest) {
    throw StorageError("cannot read artifact " + path.string());
  }
  return *digest;
}

std::string digest_descriptor(const int fd) {
  const auto digest = sha256_of_fd(fd);
  if (!digest) {
    throw StorageError("cannot read descriptor-backed artifact");
  }
  return *digest;
}

std::filesystem::path relative_to_root(const std::filesystem::path &root,
                                       const std::filesystem::path &candidate) {
  std::error_code ec;
  const auto relative = std::filesystem::relative(
      std::filesystem::absolute(candidate).lexically_normal(),
      std::filesystem::absolute(root).lexically_normal(), ec);
  if (ec || !safe_relative_path(relative)) {
    throw StorageError("artifact path is outside the trusted root");
  }
  return relative;
}

bool has_symlink_component(const std::filesystem::path &path) {
  auto current =
      path.has_root_path() ? path.root_path() : std::filesystem::path{};
  for (const auto &part : path.relative_path()) {
    current /= part;
    std::error_code ec;
    const auto status = std::filesystem::symlink_status(current, ec);
    if (ec == std::errc::no_such_file_or_directory) {
      return false;
    }
    if (ec) {
      throw StorageError("cannot inspect artifact path " + current.string() +
                         ": " + ec.message());
    }
    if (std::filesystem::is_symlink(status)) {
      return true;
    }
  }
  return false;
}

bool is_within_root(const std::filesystem::path &root,
                    const std::filesystem::path &candidate) {
  std::error_code ec;
  const auto canonical_root = std::filesystem::weakly_canonical(root, ec);
  if (ec) {
    throw StorageError("cannot resolve artifact root " + root.string() + ": " +
                       ec.message());
  }
  const auto canonical_candidate =
      std::filesystem::weakly_canonical(candidate, ec);
  if (ec) {
    throw StorageError("cannot resolve artifact path " + candidate.string() +
                       ": " + ec.message());
  }
  const auto relative =
      std::filesystem::relative(canonical_candidate, canonical_root, ec);
  return !ec && !relative.empty() && relative != "." &&
         !std::ranges::any_of(relative,
                              [](const auto &part) { return part == ".."; });
}

bool supported_contract(const ArtifactSpec &spec) {
  if (spec.kind == "astgraph") {
    return spec.artifact_schema == "cidx-astgraph/v3" &&
           spec.producer_version.starts_with("cidx-astgraph ") &&
           spec.engine_version.starts_with("cidx ");
  }
  if (spec.kind == "analysis-result") {
    return spec.artifact_schema == "cidx-analysis/v1" &&
           spec.producer_version.starts_with("cidx-analysis ") &&
           spec.engine_version.starts_with("cidx ");
  }
  if (spec.kind == "query-result") {
    return spec.artifact_schema == "cidx-query/v1" &&
           spec.producer_version.starts_with("cidx-query ") &&
           spec.engine_version.starts_with("cidx ");
  }
  if (spec.kind == "proof") {
    return spec.artifact_schema == "cidx-proof/v1" &&
           spec.producer_version.starts_with("cidx-proof ") &&
           spec.engine_version.starts_with("cidx ");
  }
  if (spec.kind == "tu-fact-cache") {
    return spec.artifact_schema == "cidx-tu-fact-cache/v1" &&
           spec.producer_version.starts_with("cidx-tu-fact-cache ") &&
           spec.engine_version.starts_with("cidx ");
  }
  if (spec.kind.starts_with("extension:")) {
    return spec.artifact_schema == "cidx-extension/v1" &&
           spec.producer_version.starts_with("cidx-extension ") &&
           spec.engine_version.starts_with("cidx ");
  }
  return false;
}

std::vector<std::string_view> required_relations(const ArtifactSpec &spec) {
  if (spec.artifact_schema == "cidx-astgraph/v3") {
    return {"node", "edge", "symbol", "meta"};
  }
  if (spec.artifact_schema == "cidx-analysis/v1" ||
      spec.artifact_schema == "cidx-query/v1") {
    return {"result"};
  }
  if (spec.artifact_schema == "cidx-proof/v1") {
    return {"proof"};
  }
  if (spec.artifact_schema == "cidx-tu-fact-cache/v1") {
    return {"tu_dependency", "tu_fact_cache", "tu_replay_context"};
  }
  if (spec.artifact_schema == "cidx-extension/v1") {
    return {"extension"};
  }
  return {};
}

bool valid_relation_name(std::string_view value) {
  return valid_attachment_name(value);
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

ArtifactAttachment::ArtifactAttachment(
    std::shared_ptr<ArtifactAttachmentLifetime> lifetime, std::string name,
    bool previous_query_only)
    : lifetime_(std::move(lifetime)), name_(std::move(name)),
      previous_query_only_(previous_query_only) {}

ArtifactAttachment::~ArtifactAttachment() noexcept { reset(); }

ArtifactAttachment::ArtifactAttachment(ArtifactAttachment &&other) noexcept
    : lifetime_(std::move(other.lifetime_)), name_(std::move(other.name_)),
      previous_query_only_(other.previous_query_only_) {}

ArtifactAttachment &
ArtifactAttachment::operator=(ArtifactAttachment &&other) noexcept {
  if (this != &other) {
    reset();
    lifetime_ = std::move(other.lifetime_);
    name_ = std::move(other.name_);
    previous_query_only_ = other.previous_query_only_;
  }
  return *this;
}

void ArtifactAttachment::reset() noexcept {
  if (!lifetime_) {
    return;
  }
  if (lifetime_->owner != nullptr) {
    lifetime_->owner->release_attachment(name_, previous_query_only_);
  }
  lifetime_.reset();
}

ArtifactStore::ArtifactStore(Storage &storage, std::filesystem::path root,
                             std::size_t max_attached)
    : storage_(storage), root_(std::move(root)), max_attached_(max_attached) {
  if (root_.empty()) {
    root_ = std::filesystem::current_path() / ".cidx-artifacts";
  }
  root_ = std::filesystem::absolute(root_).lexically_normal();
  std::error_code root_ec;
  if (std::filesystem::is_symlink(
          std::filesystem::symlink_status(root_, root_ec))) {
    throw StorageError("artifact root must not be a symlink");
  }
  const auto canonical_root = std::filesystem::weakly_canonical(root_, root_ec);
  if (root_ec) {
    throw StorageError("cannot resolve artifact root " + root_.string() + ": " +
                       root_ec.message());
  }
  root_ = canonical_root;
  root_fd_ = open_trusted_root(root_);
  attachment_lifetime_ = std::make_shared<ArtifactAttachmentLifetime>();
  attachment_lifetime_->owner = this;
}

ArtifactStore::~ArtifactStore() {
  while (!attached_names_.empty()) {
    release_attachment(attached_names_.back(),
                       query_only_before_attach_.value_or(false));
  }
  if (attachment_lifetime_) {
    attachment_lifetime_->owner = nullptr;
  }
  if (root_fd_ >= 0) {
    (void)::close(root_fd_);
    root_fd_ = -1;
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
  if (has_symlink_component(path) || !is_within_root(root_, path)) {
    add_diagnostic(validation, "invalid_location",
                   "artifact path escapes the trusted artifact root");
    return validation;
  }
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
    SqliteDb sidecar(path.string(), true, SqliteProfile::read_only_replay);
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
    if (!supported_contract(record.spec)) {
      add_diagnostic(
          validation, "unsupported_contract",
          "artifact kind, schema, producer, or engine is not supported by "
          "this reader");
    }
    for (const auto required : required_relations(record.spec)) {
      if (!std::ranges::contains(record.spec.exposed_relations, required)) {
        add_diagnostic(validation, "missing_required_relation",
                       "supported artifact schema requires relation: " +
                           std::string(required));
      }
    }
    for (const auto &relation : record.spec.exposed_relations) {
      if (!valid_relation_name(relation)) {
        add_diagnostic(validation, "invalid_relation",
                       "artifact relation is not a safe SQLite identifier");
        continue;
      }
      auto relation_check = sidecar.prepare(
          "SELECT 1 FROM sqlite_master WHERE type IN ('table','view') AND "
          "name = ? LIMIT 1");
      bind_text(relation_check, 1, relation);
      if (!relation_check.step()) {
        add_diagnostic(validation, "missing_relation",
                       "declared artifact relation does not exist: " +
                           relation);
      }
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
  ArtifactPublicationLock publication_lock(root_fd_);
  std::ranges::sort(spec.exposed_relations);
  spec.exposed_relations.erase(
      std::ranges::unique(spec.exposed_relations).begin(),
      spec.exposed_relations.end());

  ScopedFd staging_fd(open_child_directory(root_fd_, ".staging", true));
  ScopedFd artifacts_fd(open_child_directory(root_fd_, "artifacts", true));
  const auto kind_name = sha256_hex(spec.kind);
  ScopedFd kind_fd(open_child_directory(artifacts_fd.get(), kind_name, true));
  const auto temporary_name = create_staged_name(spec);
  ScopedFd temporary_fd(create_staged_file(staging_fd.get(), temporary_name));
  try {
    {
      SqliteDb sidecar(":memory:");
      writer(sidecar);
      write_envelope(sidecar, spec);
      sidecar.backup_to_fd(temporary_fd.get());
    }
    fsync_fd(temporary_fd.get(), "artifact staging file");
    return publish_staged(spec, staging_fd.get(), temporary_fd.get(),
                          temporary_name, artifacts_fd.get(), kind_fd.get(),
                          kind_name);
  } catch (...) {
    remove_staged_file(staging_fd.get(), temporary_name);
    throw;
  }
}

ArtifactRecord
ArtifactStore::publish_existing(const ArtifactSpec &input_spec,
                                const std::filesystem::path &source_path) {
  return publish_existing(input_spec, source_path, {});
}

ArtifactRecord
ArtifactStore::publish_existing(const ArtifactSpec &input_spec,
                                const std::filesystem::path &source_path,
                                const IdentityMappingWriter &mapping_writer) {
  if (input_spec.logical_id.empty() || input_spec.kind.empty() ||
      input_spec.workspace_identity.empty() ||
      !valid_attachment_name(input_spec.attachment_name)) {
    throw StorageError(
        "artifact manifest has an incomplete or invalid identity");
  }
  std::error_code ec;
  const auto source_absolute =
      std::filesystem::absolute(source_path).lexically_normal();
  const auto source_relative = relative_to_root(root_, source_absolute);
  ArtifactSpec spec = input_spec;
  ArtifactPublicationLock publication_lock(root_fd_);
  std::ranges::sort(spec.exposed_relations);
  spec.exposed_relations.erase(
      std::ranges::unique(spec.exposed_relations).begin(),
      spec.exposed_relations.end());
  ScopedFd source_fd(open_relative_file(root_fd_, source_relative, O_RDONLY));
  ScopedFd staging_fd(open_child_directory(root_fd_, ".staging", true));
  ScopedFd artifacts_fd(open_child_directory(root_fd_, "artifacts", true));
  const auto kind_name = sha256_hex(spec.kind);
  ScopedFd kind_fd(open_child_directory(artifacts_fd.get(), kind_name, true));
  const auto temporary_name = create_staged_name(spec);
  ScopedFd temporary_fd(create_staged_file(staging_fd.get(), temporary_name));
  try {
    {
      SqliteDb sidecar(source_fd.get(), false, SqliteProfile::artifact_staging);
      auto check = sidecar.prepare("PRAGMA integrity_check");
      if (!check.step() || check.col_text(0) != "ok") {
        throw StorageError("cannot adopt corrupt artifact " +
                           source_path.string());
      }
      write_envelope(sidecar, spec);
      sidecar.backup_to_fd(temporary_fd.get());
    }
    fsync_fd(temporary_fd.get(), "artifact staging file");
    const auto record = publish_staged(
        spec, staging_fd.get(), temporary_fd.get(), temporary_name,
        artifacts_fd.get(), kind_fd.get(), kind_name, mapping_writer);
    if (source_relative != std::filesystem::path(record.relative_path)) {
      remove_relative_file(root_fd_, source_relative);
    }
    return record;
  } catch (...) {
    remove_staged_file(staging_fd.get(), temporary_name);
    throw;
  }
}

ArtifactRecord ArtifactStore::publish_staged(
    const ArtifactSpec &spec, const int staging_fd, const int staged_fd,
    const std::string_view staged_name, const int artifacts_fd,
    const int kind_fd, const std::string_view kind_name,
    const IdentityMappingWriter &mapping_writer) {
  try {
    validate_directory_descriptor(root_fd_, "artifacts", artifacts_fd);
    validate_directory_descriptor(artifacts_fd, kind_name, kind_fd);
    struct stat staged_stat{};
    if (::fstat(staged_fd, &staged_stat) != 0 ||
        !S_ISREG(staged_stat.st_mode)) {
      throw StorageError("artifact staging file is not regular");
    }
    struct stat named_stat{};
    const std::string staged_filename(staged_name);
    if (::fstatat(staging_fd, staged_filename.c_str(), &named_stat,
                  AT_SYMLINK_NOFOLLOW) != 0 ||
        named_stat.st_dev != staged_stat.st_dev ||
        named_stat.st_ino != staged_stat.st_ino) {
      throw StorageError("artifact staging path was replaced");
    }
    const auto hash = digest_descriptor(staged_fd);
    const auto relative =
        std::filesystem::path("artifacts") / kind_name / (hash + ".db");
    const std::string final_name = hash + ".db";
    ScopedFd existing_fd(::openat(kind_fd, final_name.c_str(),
                                  O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
    if (existing_fd.get() >= 0) {
      if (digest_descriptor(existing_fd.get()) != hash) {
        throw StorageError("content-addressed artifact path already contains a "
                           "different file");
      }
      remove_staged_file(staging_fd, staged_name);
    } else {
      if (errno != ENOENT) {
        throw StorageError("cannot inspect final artifact path without links");
      }
      fsync_fd(staged_fd, "artifact staging file");
      if (::renameat(staging_fd, staged_filename.c_str(), kind_fd,
                     final_name.c_str()) != 0) {
        throw StorageError("cannot publish artifact without following links");
      }
      try {
        validate_directory_descriptor(root_fd_, "artifacts", artifacts_fd);
        validate_directory_descriptor(artifacts_fd, kind_name, kind_fd);
      } catch (...) {
        (void)::unlinkat(kind_fd, final_name.c_str(), 0);
        throw;
      }
      ScopedFd published_fd(::openat(kind_fd, final_name.c_str(),
                                     O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
      struct stat published_stat{};
      if (published_fd.get() < 0 ||
          ::fstat(published_fd.get(), &published_stat) != 0 ||
          published_stat.st_dev != staged_stat.st_dev ||
          published_stat.st_ino != staged_stat.st_ino) {
        (void)::unlinkat(kind_fd, final_name.c_str(), 0);
        throw StorageError("published artifact path was replaced");
      }
      fsync_fd(kind_fd, "artifact kind directory");
      fsync_fd(artifacts_fd, "artifact directory");
      fsync_fd(root_fd_, "artifact root directory");
    }

    ArtifactRecord record;
    record.spec = spec;
    record.relative_path = relative.generic_string();
    record.content_hash = hash;
    record.byte_size = static_cast<std::int64_t>(staged_stat.st_size);
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
      auto clear_mappings = storage_.raw_db().prepare(
          "DELETE FROM artifact_identity_map WHERE artifact_id = ?");
      clear_mappings.bind(1, existing_id);
      clear_mappings.step_done();
      record.id = existing_id;
      if (mapping_writer) {
        mapping_writer(record);
      }
      validate_directory_descriptor(root_fd_, "artifacts", artifacts_fd);
      validate_directory_descriptor(artifacts_fd, kind_name, kind_fd);
      txn.commit();
      remove_staged_file(staging_fd, staged_name);
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
    if (mapping_writer) {
      mapping_writer(record);
    }
    validate_directory_descriptor(root_fd_, "artifacts", artifacts_fd);
    validate_directory_descriptor(artifacts_fd, kind_name, kind_fd);
    txn.commit();
    return record;
  } catch (...) {
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
      new ArtifactAttachment(attachment_lifetime_, name, previous_query_only));
}

void ArtifactStore::read_current(std::string_view logical_id,
                                 const SidecarReader &reader) {
  auto attachment = attach_current(logical_id);
  reader(storage_.raw_db(), attachment->name());
}

void ArtifactStore::release_attachment(std::string_view name,
                                       bool previous_query_only) noexcept {
  try {
    const std::string owned_name(name);
    try {
      storage_.raw_db().exec("DETACH DATABASE \"" + owned_name + "\"");
    } catch (...) {
      const auto ignored = std::current_exception();
      (void)ignored;
    }
    const auto it = std::ranges::find(attached_names_, name);
    if (it != attached_names_.end()) {
      attached_names_.erase(it);
    }
    storage_.attached_artifact_names_.erase(owned_name);
    if (storage_.attached_artifact_names_.empty()) {
      reset_query_only(storage_.artifact_query_only_before_attach_.value_or(
          previous_query_only));
      storage_.artifact_query_only_before_attach_.reset();
      query_only_before_attach_.reset();
    }
  } catch (...) {
    const auto ignored = std::current_exception();
    (void)ignored;
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
  ArtifactPublicationLock publication_lock(root_fd_);
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

auto SqliteDb::backup_to_fd(int destination_fd) const -> void {
  if (destination_fd < 0) {
    throw StorageError("cannot back up database to an invalid descriptor");
  }
  sqlite3_int64 size = 0;
  auto *serialized = sqlite3_serialize(db_, "main", &size, 0);
  if (serialized == nullptr || size < 0 ||
      std::cmp_greater(size, std::numeric_limits<std::size_t>::max())) {
    sqlite3_free(serialized);
    throw StorageError("cannot serialize descriptor-backed database");
  }
  if (::ftruncate(destination_fd, 0) != 0 ||
      ::lseek(destination_fd, 0, SEEK_SET) < 0) {
    sqlite3_free(serialized);
    throw StorageError("cannot reset descriptor-backed database");
  }
  const auto byte_count = static_cast<std::size_t>(size);
  std::size_t written = 0;
  while (written < byte_count) {
    const ssize_t count =
        ::write(destination_fd, serialized + written, byte_count - written);
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count <= 0) {
      sqlite3_free(serialized);
      throw StorageError("cannot write descriptor-backed database");
    }
    written += static_cast<std::size_t>(count);
  }
  sqlite3_free(serialized);
}

SqliteDb::SqliteDb(int source_fd, bool read_only, SqliteProfile profile)
    : profile_(profile) {
  const bool profile_is_read_only =
      profile == SqliteProfile::interactive_read ||
      profile == SqliteProfile::read_only_replay;
  if (profile_is_read_only != read_only) {
    throw StorageError("SQLite profile/open-mode mismatch for descriptor");
  }
  if (sqlite3_libversion_number() < 3037000) {
    throw StorageError(
        std::string("cidx requires SQLite >= 3.37 "
                    "(RETURNING and sqlite3_changes64 support); found ") +
        sqlite3_libversion());
  }
  struct stat source_stat{};
  if (source_fd < 0 || ::fstat(source_fd, &source_stat) != 0 ||
      !S_ISREG(source_stat.st_mode) || source_stat.st_size < 0) {
    throw StorageError("cannot load descriptor-backed database");
  }
  const auto size = static_cast<std::size_t>(source_stat.st_size);
  std::vector<unsigned char> image(size);
  std::size_t read_total = 0;
  while (read_total < size) {
    const ssize_t count =
        ::pread(source_fd, image.data() + read_total, size - read_total,
                static_cast<off_t>(read_total));
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count <= 0) {
      throw StorageError("cannot read descriptor-backed database");
    }
    read_total += static_cast<std::size_t>(count);
  }
  const int open_rc = sqlite3_open_v2(
      ":memory:", &db_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
  if (open_rc != SQLITE_OK) {
    const std::string message =
        (db_ != nullptr) ? sqlite3_errmsg(db_) : "out of memory";
    sqlite3_close(db_);
    db_ = nullptr;
    throw StorageError("cannot open descriptor-backed database: " + message);
  }
  auto *serialized = static_cast<unsigned char *>(sqlite3_malloc64(size));
  if (size > 0 && serialized == nullptr) {
    sqlite3_free(serialized);
    sqlite3_close(db_);
    db_ = nullptr;
    throw StorageError("cannot deserialize descriptor-backed database");
  }
  if (size > 0) {
    std::memcpy(serialized, image.data(), size);
  }
  if (sqlite3_deserialize(db_, "main", serialized,
                          static_cast<sqlite3_int64>(size),
                          static_cast<sqlite3_int64>(size),
                          SQLITE_DESERIALIZE_FREEONCLOSE |
                              SQLITE_DESERIALIZE_RESIZEABLE) != SQLITE_OK) {
    sqlite3_free(serialized);
    sqlite3_close(db_);
    db_ = nullptr;
    throw StorageError("cannot deserialize descriptor-backed database");
  }
  try {
    const auto settings = sqlite_profile_settings(profile_);
    if (sqlite3_busy_timeout(db_, settings.busy_timeout_ms) != SQLITE_OK) {
      throw StorageError("cannot configure SQLite busy timeout: " +
                         std::string(sqlite3_errmsg(db_)));
    }
    if (settings.foreign_keys) {
      exec("PRAGMA foreign_keys = ON");
    }
    if (settings.query_only) {
      exec("PRAGMA query_only = ON");
    }
  } catch (...) {
    sqlite3_close(db_);
    db_ = nullptr;
    throw;
  }
}

} // namespace cidx
