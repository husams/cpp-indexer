#include "analysis/runner.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string_view>
#include <tuple>
#include <unistd.h>

#include "cli/souffle_rules.hpp"
#include "util/errors.hpp"
#include "util/hashing.hpp"
#include "util/pathutil.hpp"
#include "util/subprocess.hpp"

namespace cidx::analysis {

namespace {

namespace fs = std::filesystem;
constexpr double kSouffleTimeout = 600.0;

std::string relation_row_canonical(const FactRow &row) {
  std::ostringstream out;
  for (const auto &value : row) {
    out << fact_value_text(value) << '\x1f';
  }
  return out.str();
}

std::string requirement_canonical(const RelationRequirement &requirement) {
  std::ostringstream out;
  out << requirement.name << ':' << requirement.version << ':'
      << requirement.catalog_version;
  for (const auto &column : requirement.columns) {
    out << ':' << column.name << '=' << fact_type_name(column.type);
  }
  return out.str();
}

std::string request_canonical(const FactRequest &request) {
  std::vector<std::string> names = request.relations;
  std::ranges::sort(names);
  std::ostringstream out;
  for (const auto &name : names) {
    out << name << '\n';
  }
  out << "workspace=" << request.workspace_identity.value_or("") << '\n'
      << "tu=" << request.tu_identity.value_or("");
  return out.str();
}

std::string run_identity(std::string_view package, std::string_view input,
                         std::string_view options) {
  std::string material;
  material.reserve(package.size() + input.size() + options.size() + 2);
  material.append(package);
  material.push_back('\0');
  material.append(input);
  material.push_back('\0');
  material.append(options);
  return sha256_hex(material);
}

struct TempDir {
  fs::path path;

  TempDir() {
    const std::string templ =
        (fs::temp_directory_path() / "cidx-analysis-XXXXXX").string();
    std::vector<char> buffer(templ.begin(), templ.end());
    buffer.push_back('\0');
    if (::mkdtemp(buffer.data()) == nullptr) {
      throw CidxError("cannot create temporary analysis directory");
    }
    path = buffer.data();
  }

  ~TempDir() noexcept {
    std::error_code ec;
    fs::remove_all(path, ec);
  }

  TempDir(const TempDir &) = delete;
  TempDir &operator=(const TempDir &) = delete;
};

bool regular_file(const fs::path &path) {
  std::error_code ec;
  return fs::is_regular_file(path, ec) && !ec;
}

std::optional<std::string> find_souffle() {
  if (const char *configured = std::getenv("CIDX_SOUFFLE");
      configured != nullptr && *configured != '\0') {
    return std::string(configured);
  }
  const char *path = std::getenv("PATH");
  if (path == nullptr) {
    return std::nullopt;
  }
  std::istringstream directories(path);
  std::string directory;
  while (std::getline(directories, directory, ':')) {
    if (directory.empty()) {
      continue;
    }
    const fs::path candidate = fs::path(directory) / "souffle";
    if (regular_file(candidate) && ::access(candidate.c_str(), X_OK) == 0) {
      return candidate.string();
    }
  }
  return std::nullopt;
}

std::vector<std::string> output_relations(std::string_view program) {
  std::vector<std::string> names;
  std::istringstream lines{std::string(program)};
  std::string line;
  while (std::getline(lines, line)) {
    const std::size_t begin = line.find_first_not_of(" \t\r\f\v");
    if (begin == std::string::npos || line.compare(begin, 7, ".output") != 0) {
      continue;
    }
    const std::size_t name_begin =
        line.find_first_not_of(" \t\r\f\v", begin + 7);
    if (name_begin == std::string::npos) {
      continue;
    }
    std::size_t name_end = name_begin;
    while (name_end < line.size() &&
           ((line[name_end] >= 'a' && line[name_end] <= 'z') ||
            (line[name_end] >= 'A' && line[name_end] <= 'Z') ||
            (line[name_end] >= '0' && line[name_end] <= '9' &&
             name_end != name_begin) ||
            line[name_end] == '_')) {
      ++name_end;
    }
    if (name_end > name_begin) {
      names.push_back(line.substr(name_begin, name_end - name_begin));
    }
  }
  std::ranges::sort(names);
  names.erase(std::ranges::unique(names).begin(), names.end());
  return names;
}

std::vector<FactRow> read_relation(const fs::path &path) {
  if (!regular_file(path)) {
    return {};
  }
  std::ifstream input(path, std::ios::binary);
  std::ostringstream buffer;
  buffer << input.rdbuf();
  const std::string text = buffer.str();
  std::vector<FactRow> rows;
  std::size_t start = 0;
  while (start <= text.size()) {
    const std::size_t end = text.find('\n', start);
    const std::string line = text.substr(
        start, end == std::string::npos ? std::string::npos : end - start);
    if (!line.empty()) {
      FactRow row;
      std::size_t cell_start = 0;
      while (cell_start <= line.size()) {
        const std::size_t tab = line.find('\t', cell_start);
        row.emplace_back(line.substr(cell_start, tab == std::string::npos
                                                     ? std::string::npos
                                                     : tab - cell_start));
        if (tab == std::string::npos) {
          break;
        }
        cell_start = tab + 1;
      }
      rows.push_back(std::move(row));
    }
    if (end == std::string::npos) {
      break;
    }
    start = end + 1;
  }
  std::ranges::sort(rows, {}, relation_row_canonical);
  return rows;
}

std::map<std::string, FactRelation>
run_souffle(const AnalysisPackage &package, const FactSnapshot &snapshot,
            const AnalysisOptions &options, std::vector<std::string> &inputs) {
  const auto executable = find_souffle();
  if (!executable || !regular_file(*executable) ||
      ::access(executable->c_str(), X_OK) != 0) {
    throw CidxError(
        "souffle executable not found; install souffle or set CIDX_SOUFFLE");
  }
  TempDir temp;
  const fs::path facts_dir = temp.path / "facts";
  const fs::path output_dir = temp.path / "out";
  std::error_code ec;
  fs::create_directories(output_dir, ec);
  if (ec) {
    throw CidxError("cannot create " + output_dir.string() + ": " +
                    ec.message());
  }
  const FactExportStats export_stats =
      write_fact_files(snapshot, facts_dir.string(), cli::dlrules::k_prelude);
  (void)export_stats;
  const fs::path program_path = temp.path / "program.dl";
  {
    std::ofstream output(program_path, std::ios::binary);
    if (!output) {
      throw CidxError("cannot write " + program_path.string());
    }
    output << cli::dlrules::k_prelude << '\n' << package.program;
  }
  inputs.push_back(program_path.string());
  const std::vector<std::string> argv = {*executable,
                                         "-F",
                                         facts_dir.string(),
                                         "-D",
                                         output_dir.string(),
                                         "-j",
                                         std::to_string(options.jobs),
                                         program_path.string()};
  const RunResult result = run(argv, kSouffleTimeout);
  if (result.timed_out) {
    throw CidxError("souffle timed out after " +
                    std::to_string(static_cast<int>(kSouffleTimeout)) + "s");
  }
  if (result.exit_code != 0) {
    std::string detail = result.err;
    if (detail.empty()) {
      detail = result.out;
    }
    throw CidxError("souffle failed (exit " + std::to_string(result.exit_code) +
                    "): " + detail);
  }

  std::map<std::string, FactRelation> relations;
  const std::string full_program =
      std::string(cli::dlrules::k_prelude) + '\n' + package.program;
  for (const std::string &name : output_relations(full_program)) {
    FactRelation relation{
        .descriptor =
            RelationDescriptor{.name = name,
                               .version = 1,
                               .catalog_version = snapshot.catalog_version,
                               .columns = {}},
        .rows = {}};
    const auto rows = read_relation(output_dir / (name + ".csv"));
    if (!rows.empty()) {
      relation.descriptor.columns.reserve(rows.front().size());
      for (std::size_t index = 0; index < rows.front().size(); ++index) {
        relation.descriptor.columns.push_back(
            {.name = "col" + std::to_string(index), .type = FactType::string});
      }
    }
    relation.rows = rows;
    relations.insert_or_assign(name, std::move(relation));
  }
  return relations;
}

AnalysisRun base_run(const AnalysisPackage &package,
                     const FactSnapshot &snapshot,
                     const AnalysisOptions &options) {
  AnalysisRun run;
  run.input_hash = snapshot.stable_hash();
  run.package_hash = sha256_hex(package.canonical());
  run.run_id =
      run_identity(package.canonical(), run.input_hash, options.canonical());
  if (snapshot.completeness == FactCompleteness::partial ||
      snapshot.truncated) {
    run.status = AnalysisStatus::partial;
    if (snapshot.truncated) {
      run.diagnostics.push_back({.code = "truncated_budget",
                                 .message = "fact snapshot was truncated"});
    }
  }
  return run;
}

void validate_requirement(const FactSnapshot &snapshot,
                          const RelationRequirement &requirement,
                          std::vector<AnalysisDiagnostic> &diagnostics) {
  const FactRelation *relation = snapshot.find_relation(requirement.name);
  if (relation == nullptr) {
    diagnostics.push_back(
        {.code = "unsupported_relation",
         .message = "required relation is unavailable: " + requirement.name});
    return;
  }
  if (relation->descriptor.version != requirement.version ||
      relation->descriptor.catalog_version != requirement.catalog_version) {
    diagnostics.push_back(
        {.code = "package_incompatible",
         .message = "relation version mismatch: " + requirement.name});
    return;
  }
  if (!requirement.columns.empty() &&
      relation->descriptor.columns != requirement.columns) {
    diagnostics.push_back(
        {.code = "package_incompatible",
         .message = "relation columns mismatch: " + requirement.name});
  }
}

} // namespace

std::string AnalysisPackage::canonical() const {
  std::vector<std::string> requirements;
  requirements.reserve(required_relations.size());
  for (const auto &requirement : required_relations) {
    requirements.push_back(requirement_canonical(requirement));
  }
  std::ranges::sort(requirements);
  std::ostringstream out;
  out << "name=" << name << '\n'
      << "version=" << version << '\n'
      << "entry=" << entry_point << '\n'
      << "engine=" << engine << '\n'
      << "content_hash=" << content_hash << '\n'
      << "program=" << program << '\n';
  for (const auto &requirement : requirements) {
    out << "require=" << requirement << '\n';
  }
  return out.str();
}

std::string AnalysisOptions::canonical() const {
  return "jobs=" + std::to_string(jobs) +
         ";steps=" + std::to_string(step_budget) +
         ";output=" + std::to_string(output_budget);
}

std::string AnalysisRun::canonical_result() const {
  std::ostringstream out;
  out << "run=" << run_id << '\n'
      << "input=" << input_hash << '\n'
      << "package=" << package_hash << '\n'
      << "status=" << analysis_status_name(status) << '\n';
  for (const auto &[name, relation] : relations) {
    out << "relation=" << name << '\n';
    std::vector<std::string> rows;
    rows.reserve(relation.rows.size());
    for (const FactRow &row : relation.rows) {
      rows.push_back(relation_row_canonical(row));
    }
    std::ranges::sort(rows);
    for (const auto &row : rows) {
      out << "row=" << row << '\n';
    }
  }
  std::vector<AnalysisDiagnostic> sorted = diagnostics;
  std::ranges::sort(sorted, {}, [](const auto &diagnostic) {
    return diagnostic.code + '\x1f' + diagnostic.message;
  });
  for (const auto &diagnostic : sorted) {
    out << "diagnostic=" << diagnostic.code << ':' << diagnostic.message
        << '\n';
  }
  return out.str();
}

std::string AnalysisRun::artifact_hash() const {
  return sha256_hex(canonical_result());
}

AnalysisRun
SouffleAnalysisEngine::execute(const AnalysisPackage &package,
                               const FactSnapshot &snapshot,
                               const AnalysisOptions &options) const {
  if (options.jobs < 1) {
    throw CidxError("analysis jobs must be at least 1");
  }
  AnalysisRun result = base_run(package, snapshot, options);
  result.relations =
      run_souffle(package, snapshot, options, result.generated_inputs);
  if (options.output_budget > 0 &&
      result.canonical_result().size() >
          static_cast<std::size_t>(options.output_budget)) {
    throw CidxError("analysis output budget exceeded");
  }
  return result;
}

AstgraphCallgraphEngine::AstgraphCallgraphEngine(CallgraphFunction callgraph)
    : callgraph_(std::move(callgraph)) {
  if (!callgraph_) {
    throw CidxError("astgraph analysis requires a callgraph function");
  }
}

AnalysisRun
AstgraphCallgraphEngine::execute(const AnalysisPackage &package,
                                 const FactSnapshot &snapshot,
                                 const AnalysisOptions &options) const {
  if (options.jobs < 1) {
    throw CidxError("analysis jobs must be at least 1");
  }
  if (!snapshot.artifact_path) {
    throw CidxError("astgraph analysis requires an artifact path");
  }
  AnalysisRun result = base_run(package, snapshot, options);
  const auto calls = callgraph_(*snapshot.artifact_path, options.jobs);
  FactRelation relation{
      .descriptor =
          RelationDescriptor{
              .name = "call",
              .version = 1,
              .catalog_version = snapshot.catalog_version,
              .columns = {{.name = "caller_node", .type = FactType::integer},
                          {.name = "caller_usr", .type = FactType::string},
                          {.name = "caller_name", .type = FactType::string},
                          {.name = "callee_node", .type = FactType::integer},
                          {.name = "callee_usr", .type = FactType::string},
                          {.name = "callee_name", .type = FactType::string},
                          {.name = "line", .type = FactType::integer}}},
      .rows = {}};
  relation.rows = calls;
  result.relations.insert_or_assign(relation.descriptor.name,
                                    std::move(relation));
  return result;
}

AnalysisRunner::AnalysisRunner(std::unique_ptr<AnalysisEngine> engine)
    : engine_(std::move(engine)) {
  if (!engine_) {
    throw CidxError("analysis runner requires an engine");
  }
}

AnalysisRun AnalysisRunner::run(const AnalysisPackage &package,
                                const FactProvider &provider,
                                const FactRequest &request,
                                const AnalysisOptions &options) const {
  AnalysisRun result;
  result.package_hash = sha256_hex(package.canonical());
  result.run_id = run_identity(package.canonical(), request_canonical(request),
                               options.canonical());
  if (package.name.empty() || package.version.empty() ||
      package.program.empty()) {
    result.status = AnalysisStatus::error;
    result.diagnostics.push_back(
        {.code = "invalid_input", .message = "analysis package is incomplete"});
    return result;
  }
  if (options.jobs < 1 || options.step_budget < 0 ||
      options.output_budget < 0) {
    result.status = AnalysisStatus::error;
    result.diagnostics.push_back(
        {.code = "invalid_input", .message = "analysis options are invalid"});
    return result;
  }
  try {
    const FactSnapshot snapshot = provider.snapshot(request);
    result.input_hash = snapshot.stable_hash();
    result.run_id = run_identity(package.canonical(), result.input_hash,
                                 options.canonical());
    for (const auto &requirement : package.required_relations) {
      validate_requirement(snapshot, requirement, result.diagnostics);
    }
    if (!result.diagnostics.empty()) {
      result.status = AnalysisStatus::unknown;
      return result;
    }
    if (snapshot.completeness == FactCompleteness::stale) {
      result.status = AnalysisStatus::unknown;
      result.diagnostics.push_back(
          {.code = "stale_input", .message = "fact snapshot is stale"});
      return result;
    }
    if (snapshot.completeness == FactCompleteness::unknown) {
      result.status = AnalysisStatus::unknown;
      result.diagnostics.push_back(
          {.code = "missing_evidence",
           .message = "fact snapshot applicability is unknown"});
      return result;
    }
    result = engine_->execute(package, snapshot, options);
    result.input_hash = snapshot.stable_hash();
    result.package_hash = sha256_hex(package.canonical());
    result.run_id = run_identity(package.canonical(), result.input_hash,
                                 options.canonical());
    return result;
  } catch (const CidxError &error) {
    result.status = AnalysisStatus::error;
    result.diagnostics.push_back(
        {.code = "backend_error", .message = error.what()});
    return result;
  } catch (const std::exception &error) {
    result.status = AnalysisStatus::error;
    result.diagnostics.push_back(
        {.code = "backend_error", .message = error.what()});
    return result;
  }
}

std::string analysis_status_name(AnalysisStatus status) {
  switch (status) {
  case AnalysisStatus::complete:
    return "complete";
  case AnalysisStatus::partial:
    return "partial";
  case AnalysisStatus::unknown:
    return "unknown";
  case AnalysisStatus::error:
    return "error";
  }
  throw CidxError("invalid analysis status");
}

} // namespace cidx::analysis
