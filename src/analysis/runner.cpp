#include "analysis/runner.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string_view>
#include <tuple>
#include <unistd.h>
#include <utility>

#include "application/souffle_rules.hpp"
#include "storage/storage.hpp"
#include "util/errors.hpp"
#include "util/hashing.hpp"
#include "util/subprocess.hpp"

namespace cidx::analysis {

namespace {

namespace fs = std::filesystem;
constexpr std::size_t kDefaultCaptureBudget = 1'048'576;

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

std::string read_file(const fs::path &path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw AnalysisEngineError("artifact_io", "cannot read " + path.string());
  }
  std::ostringstream buffer;
  buffer << input.rdbuf();
  return buffer.str();
}

std::string retained_input(const fs::path &root, std::string_view name,
                           std::string_view content) {
  const std::string hash = sha256_hex(std::string(content));
  std::error_code ec;
  fs::create_directories(root / hash, ec);
  if (ec) {
    throw AnalysisEngineError("artifact_io",
                              "cannot create retained input directory: " +
                                  ec.message());
  }
  const std::string filename = fact_file_name(name);
  const fs::path destination = root / hash / filename;
  if (!regular_file(destination)) {
    const fs::path temporary =
        destination.string() + ".tmp-" +
        std::to_string(static_cast<long long>(::getpid()));
    {
      std::ofstream output(temporary, std::ios::binary);
      if (!output) {
        throw AnalysisEngineError("artifact_io",
                                  "cannot retain generated input");
      }
      output.write(content.data(),
                   static_cast<std::streamsize>(content.size()));
      output.flush();
      if (!output) {
        throw AnalysisEngineError("artifact_io",
                                  "cannot flush generated input");
      }
    }
    fs::rename(temporary, destination, ec);
    if (ec && !regular_file(destination)) {
      throw AnalysisEngineError(
          "artifact_io", "cannot publish generated input: " + ec.message());
    }
    if (ec) {
      fs::remove(temporary, ec);
    }
  }
  return "analysis-input:" + hash + "/" + filename;
}

std::string detect_souffle_version(const std::string &executable) {
  const RunResult version = run({executable, "--version"}, 2.0, 4096);
  if (version.exit_code != 0 || version.out.empty()) {
    return "souffle:unknown";
  }
  std::string value = version.out;
  while (!value.empty() && (value.back() == '\n' || value.back() == '\r')) {
    value.pop_back();
  }
  return value;
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

FactValue decode_value(std::string_view text, const FactType type,
                       std::string_view relation_name) {
  if (type == FactType::string) {
    return std::string(text);
  }
  if (type == FactType::boolean) {
    if (text == "1" || text == "true") {
      return true;
    }
    if (text == "0" || text == "false") {
      return false;
    }
    throw AnalysisEngineError("engine_failure",
                              "invalid boolean in output relation " +
                                  std::string(relation_name));
  }
  std::int64_t value = 0;
  const auto [end, error] =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (error != std::errc{} || end != text.data() + text.size()) {
    throw AnalysisEngineError("engine_failure",
                              "invalid integer in output relation " +
                                  std::string(relation_name));
  }
  return value;
}

std::vector<FactColumn> declared_columns(std::string_view program,
                                         std::string_view relation_name) {
  std::istringstream lines{std::string(program)};
  std::string line;
  const std::string prefix = ".decl " + std::string(relation_name) + "(";
  std::string declaration;
  bool collecting = false;
  while (std::getline(lines, line)) {
    const std::size_t begin = line.find_first_not_of(" \t\r\f\v");
    if (!collecting) {
      if (begin == std::string::npos ||
          line.compare(begin, prefix.size(), prefix) != 0) {
        continue;
      }
      declaration = line.substr(begin + prefix.size());
      collecting = true;
    } else {
      declaration += line;
    }
    const std::size_t close = declaration.find(')');
    if (close == std::string::npos) {
      continue;
    }
    std::vector<FactColumn> columns;
    std::istringstream values(declaration.substr(0, close));
    std::string column;
    while (std::getline(values, column, ',')) {
      const std::size_t column_begin = column.find_first_not_of(" \t");
      const std::size_t column_end = column.find_last_not_of(" \t");
      if (column_begin == std::string::npos) {
        continue;
      }
      column = column.substr(column_begin, column_end - column_begin + 1);
      const std::size_t separator = column.find(':');
      if (separator == std::string::npos) {
        continue;
      }
      const std::string name = column.substr(0, separator);
      const std::string type_name = column.substr(separator + 1);
      FactType type = FactType::string;
      if (type_name.contains("number") || type_name.contains("unsigned")) {
        type = FactType::integer;
      } else if (type_name.contains("boolean")) {
        type = FactType::boolean;
      }
      columns.push_back({.name = name, .type = type});
    }
    return columns;
  }
  return {};
}

RelationDescriptor output_descriptor(const AnalysisPackage &package,
                                     const FactSnapshot &snapshot,
                                     std::string_view name,
                                     std::string_view program) {
  const auto requirement = std::ranges::find(package.output_relations, name,
                                             &RelationRequirement::name);
  std::vector<FactColumn> columns =
      requirement == package.output_relations.end() ||
              requirement->columns.empty()
          ? declared_columns(program, name)
          : requirement->columns;
  return RelationDescriptor{
      .name = std::string(name),
      .version = requirement == package.output_relations.end()
                     ? 1
                     : requirement->version,
      .catalog_version = requirement == package.output_relations.end()
                             ? snapshot.catalog_version
                             : requirement->catalog_version,
      .columns = std::move(columns)};
}

std::vector<FactRow> read_relation(const fs::path &path,
                                   const RelationDescriptor &descriptor,
                                   std::uintmax_t &output_bytes,
                                   const std::int64_t output_budget) {
  if (!regular_file(path)) {
    return {};
  }
  std::error_code size_error;
  const std::uintmax_t file_bytes = fs::file_size(path, size_error);
  if (size_error) {
    throw AnalysisEngineError("artifact_io",
                              "cannot stat output relation " + path.string());
  }
  if (output_budget > 0 &&
      output_bytes + file_bytes > static_cast<std::uintmax_t>(output_budget)) {
    throw AnalysisEngineError("output_budget_exceeded",
                              "analysis result files exceed output budget");
  }
  output_bytes += file_bytes;
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
        const std::string cell = line.substr(
            cell_start,
            tab == std::string::npos ? std::string::npos : tab - cell_start);
        const std::size_t column = row.size();
        if (column >= descriptor.columns.size()) {
          throw AnalysisEngineError(
              "engine_failure",
              "output row has too many columns in relation " + descriptor.name);
        }
        row.push_back(decode_value(cell, descriptor.columns[column].type,
                                   descriptor.name));
        if (tab == std::string::npos) {
          break;
        }
        cell_start = tab + 1;
      }
      if (row.size() != descriptor.columns.size()) {
        throw AnalysisEngineError(
            "engine_failure",
            "output row has the wrong column count in relation " +
                descriptor.name);
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

std::map<std::string, FactRelation> run_souffle(const AnalysisPackage &package,
                                                const FactSnapshot &snapshot,
                                                const AnalysisOptions &options,
                                                AnalysisRun &execution) {
  const auto executable = find_souffle();
  if (!executable || !regular_file(*executable) ||
      ::access(executable->c_str(), X_OK) != 0) {
    throw AnalysisEngineError(
        "engine_unavailable",
        "souffle executable not found; install souffle or set CIDX_SOUFFLE");
  }
  execution.engine_version = detect_souffle_version(*executable);
  TempDir temp;
  const fs::path facts_dir = temp.path / "facts";
  const fs::path output_dir = temp.path / "out";
  std::error_code ec;
  fs::create_directories(output_dir, ec);
  if (ec) {
    throw AnalysisEngineError("artifact_io", "cannot create " +
                                                 output_dir.string() + ": " +
                                                 ec.message());
  }
  std::string prelude;
  if (!package.prelude.empty()) {
    prelude = package.prelude;
  } else if (package.include_catalog_prelude) {
    prelude = cli::dlrules::k_prelude;
  }
  const FactExportStats export_stats =
      write_fact_files(snapshot, facts_dir.string(), prelude);
  (void)export_stats;
  const fs::path retained_root = options.artifact_root.value_or(
      fs::temp_directory_path() / ".cidx-analysis-inputs");
  std::vector<fs::path> fact_files;
  for (const auto &entry : fs::directory_iterator(facts_dir)) {
    if (regular_file(entry.path())) {
      fact_files.push_back(entry.path());
    }
  }
  std::ranges::sort(fact_files);
  for (const auto &fact_file : fact_files) {
    execution.generated_inputs.push_back(retained_input(
        retained_root, fact_file.filename().string(), read_file(fact_file)));
  }
  const fs::path program_path = temp.path / "program.dl";
  {
    std::ofstream output(program_path, std::ios::binary);
    if (!output) {
      throw AnalysisEngineError("artifact_io",
                                "cannot write " + program_path.string());
    }
    if (!prelude.empty()) {
      output << prelude << '\n';
    }
    output << package.program;
  }
  execution.generated_inputs.push_back(
      retained_input(retained_root, "program.dl", read_file(program_path)));
  const std::vector<std::string> argv = {*executable,
                                         "-F",
                                         facts_dir.string(),
                                         "-D",
                                         output_dir.string(),
                                         "-j",
                                         std::to_string(options.jobs),
                                         program_path.string()};
  const auto started = std::chrono::steady_clock::now();
  const double timeout = static_cast<double>(options.time_budget_ms) / 1000.0;
  std::size_t output_limit = kDefaultCaptureBudget;
  if (options.capture_budget > 0) {
    output_limit = static_cast<std::size_t>(options.capture_budget);
  }
  std::uintmax_t observed_output_bytes = 0;
  const auto output_guard = [&]() {
    if (options.output_budget <= 0) {
      return false;
    }
    std::uintmax_t total = 0;
    std::error_code scan_error;
    for (const auto &entry : fs::directory_iterator(output_dir, scan_error)) {
      if (scan_error || !regular_file(entry.path()) ||
          entry.path().extension() != ".csv") {
        continue;
      }
      std::error_code size_error;
      total += fs::file_size(entry.path(), size_error);
      if (size_error) {
        continue;
      }
      observed_output_bytes = total;
      if (std::cmp_greater(total, options.output_budget)) {
        return true;
      }
    }
    observed_output_bytes = total;
    return std::cmp_greater(total, options.output_budget);
  };
  const RunResult result = run(argv, timeout, output_limit, output_guard);
  execution.elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - started)
                             .count();
  execution.stdout_text = result.out;
  execution.stderr_text = result.err;
  execution.output_bytes = static_cast<std::int64_t>(
      observed_output_bytes != 0 ? observed_output_bytes
                                 : result.captured_bytes);
  execution.peak_bytes = static_cast<std::int64_t>(result.peak_bytes);
  execution.output_budget_terminated = result.output_guard_triggered;
  if (result.timed_out) {
    throw AnalysisEngineError("timeout", "souffle timed out", execution);
  }
  if (result.output_limited) {
    throw AnalysisEngineError("capture_budget_exceeded",
                              "analysis capture budget exceeded", execution);
  }
  if (result.output_guard_triggered) {
    throw AnalysisEngineError(
        "output_budget_exceeded",
        "analysis result output budget exceeded while Souffle was running",
        execution);
  }
  if (result.exit_code != 0) {
    std::string detail = result.err;
    if (detail.empty()) {
      detail = result.out;
    }
    throw AnalysisEngineError("engine_failure",
                              "souffle failed (exit " +
                                  std::to_string(result.exit_code) +
                                  "): " + detail,
                              execution);
  }

  std::map<std::string, FactRelation> relations;
  const std::string full_program =
      (prelude.empty() ? std::string{} : prelude + '\n') + package.program;
  if (options.output_budget > 0) {
    std::uintmax_t csv_bytes = 0;
    for (const std::string &name : output_relations(full_program)) {
      const fs::path output_path = output_dir / (name + ".csv");
      if (!regular_file(output_path)) {
        continue;
      }
      std::error_code size_error;
      const std::uintmax_t file_bytes = fs::file_size(output_path, size_error);
      if (size_error) {
        throw AnalysisEngineError("artifact_io",
                                  "cannot stat output relation " +
                                      output_path.string());
      }
      csv_bytes += file_bytes;
      if (std::cmp_greater(csv_bytes, options.output_budget)) {
        throw AnalysisEngineError("output_budget_exceeded",
                                  "analysis result files exceed output budget");
      }
    }
  }
  std::uintmax_t result_bytes = 0;
  for (const std::string &name : output_relations(full_program)) {
    const RelationDescriptor descriptor =
        output_descriptor(package, snapshot, name, full_program);
    FactRelation relation{.descriptor = descriptor, .rows = {}};
    relation.rows = read_relation(output_dir / (name + ".csv"), descriptor,
                                  result_bytes, options.output_budget);
    relations.insert_or_assign(name, std::move(relation));
  }
  execution.output_bytes = static_cast<std::int64_t>(result_bytes);
  std::ranges::sort(execution.generated_inputs);
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
    run.result_class = snapshot.truncated ? AnalysisResultClass::truncated_input
                                          : AnalysisResultClass::partial;
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

AnalysisEngineError::AnalysisEngineError(std::string code,
                                         const std::string &message)
    : std::runtime_error(message), code_(std::move(code)) {}

AnalysisEngineError::AnalysisEngineError(std::string code,
                                         const std::string &message,
                                         const AnalysisRun &partial_run)
    : std::runtime_error(message), code_(std::move(code)),
      partial_run_(std::make_shared<AnalysisRun>(partial_run)) {}

std::string AnalysisEngine::engine_version() const { return "unknown"; }

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
      << "prelude=" << prelude << '\n'
      << "include_catalog_prelude=" << (include_catalog_prelude ? 1 : 0) << '\n'
      << "program=" << program << '\n';
  for (const auto &requirement : requirements) {
    out << "require=" << requirement << '\n';
  }
  std::vector<std::string> outputs;
  outputs.reserve(output_relations.size());
  for (const auto &output : output_relations) {
    outputs.push_back(requirement_canonical(output));
  }
  std::ranges::sort(outputs);
  for (const auto &output : outputs) {
    out << "output=" << output << '\n';
  }
  return out.str();
}

std::string AnalysisOptions::canonical() const {
  return "jobs=" + std::to_string(jobs) +
         ";steps=" + std::to_string(step_budget) +
         ";time=" + std::to_string(time_budget_ms) +
         ";output=" + std::to_string(output_budget) +
         ";capture=" + std::to_string(capture_budget);
}

std::string AnalysisRun::canonical_result() const {
  std::ostringstream out;
  out << "run=" << run_id << '\n'
      << "input=" << input_hash << '\n'
      << "package=" << package_hash << '\n'
      << "status=" << analysis_status_name(status) << '\n'
      << "class=" << analysis_result_class_name(result_class) << '\n'
      << "engine=" << engine_version << '\n'
      << "output_budget_terminated=" << (output_budget_terminated ? 1 : 0)
      << '\n';
  std::vector<std::string> sorted_inputs = generated_inputs;
  std::ranges::sort(sorted_inputs);
  for (const auto &input : sorted_inputs) {
    out << "generated=" << input << '\n';
  }
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
  AnalysisRun result = base_run(package, snapshot, options);
  result.engine_version = engine_version();
  if (options.step_budget > 0) {
    throw AnalysisEngineError(
        "unsupported_budget",
        "Souffle step budgets are unsupported because recursive evaluation "
        "steps are not externally enforceable");
  }
  try {
    result.relations = run_souffle(package, snapshot, options, result);
  } catch (const AnalysisEngineError &error) {
    throw AnalysisEngineError(error.code(), error.what(), result);
  } catch (const std::exception &error) {
    throw AnalysisEngineError("engine_failure", error.what(), result);
  }
  return result;
}

std::string SouffleAnalysisEngine::engine_version() const {
  return "souffle:external";
}

AstgraphCallgraphEngine::AstgraphCallgraphEngine(CallgraphFunction callgraph)
    : callgraph_(std::move(callgraph)) {
  if (!callgraph_) {
    throw CidxError("astgraph analysis requires a callgraph function");
  }
}

std::string AstgraphCallgraphEngine::engine_version() const {
  return "astgraph-native";
}

AnalysisRun
AstgraphCallgraphEngine::execute(const AnalysisPackage &package,
                                 const FactSnapshot &snapshot,
                                 const AnalysisOptions &options) const {
  AnalysisRun result = base_run(package, snapshot, options);
  result.engine_version = engine_version();
  if (options.step_budget > 0) {
    throw AnalysisEngineError(
        "unsupported_budget",
        "native astgraph step budgets require an engine work counter");
  }
  if (!snapshot.artifact_path) {
    throw AnalysisEngineError("missing_tu",
                              "astgraph analysis requires an artifact path");
  }
  const auto started = std::chrono::steady_clock::now();
  std::vector<FactRow> calls;
  try {
    calls = callgraph_(*snapshot.artifact_path, options.jobs);
  } catch (const std::exception &error) {
    throw AnalysisEngineError("engine_failure", error.what());
  }
  result.elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - started)
                          .count();
  if (result.elapsed_ms > options.time_budget_ms) {
    throw AnalysisEngineError("timeout", "astgraph analysis timed out");
  }
  result.step_count = 0;
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
  result.output_bytes =
      static_cast<std::int64_t>(result.canonical_result().size());
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
  const auto invalid = [&result](AnalysisResultClass result_class,
                                 std::string code, std::string message) {
    result.status = AnalysisStatus::error;
    result.result_class = result_class;
    result.diagnostics.push_back(
        {.code = std::move(code), .message = std::move(message)});
  };
  if (package.name.empty() || package.version.empty() ||
      package.entry_point.empty() || package.engine.empty() ||
      package.program.empty()) {
    invalid(AnalysisResultClass::invalid_package, "invalid_package",
            "analysis package is incomplete");
    return result;
  }
  if (!package.content_hash.empty() &&
      package.content_hash != sha256_hex(package.program) &&
      package.content_hash != "sha256:" + sha256_hex(package.program)) {
    invalid(AnalysisResultClass::invalid_package, "invalid_package",
            "analysis package content hash does not match its program");
    return result;
  }
  if (options.jobs < 1 || options.step_budget < 0 ||
      options.time_budget_ms <= 0 || options.output_budget < 0 ||
      options.capture_budget < 0) {
    invalid(AnalysisResultClass::invalid_package, "invalid_options",
            "analysis options are invalid");
    return result;
  }
  std::vector<std::string> required_names;
  required_names.reserve(package.required_relations.size());
  for (const auto &requirement : package.required_relations) {
    required_names.push_back(requirement.name);
  }
  std::ranges::sort(required_names);
  if (std::ranges::adjacent_find(required_names) != required_names.end()) {
    invalid(AnalysisResultClass::invalid_package, "invalid_package",
            "analysis package declares a duplicate required relation");
    return result;
  }
  bool engine_started = false;
  const auto run_started = std::chrono::steady_clock::now();
  try {
    const FactSnapshot snapshot = provider.snapshot(request);
    result.elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - run_started)
                            .count();
    if (result.elapsed_ms > options.time_budget_ms) {
      invalid(AnalysisResultClass::timeout, "timeout",
              "fact provider exceeded the declared time budget");
      return result;
    }
    result.input_hash = snapshot.stable_hash();
    result.run_id = run_identity(package.canonical(), result.input_hash,
                                 options.canonical());
    for (const auto &requirement : package.required_relations) {
      validate_requirement(snapshot, requirement, result.diagnostics);
    }
    if (!result.diagnostics.empty()) {
      result.status = AnalysisStatus::unknown;
      result.result_class = std::ranges::any_of(result.diagnostics,
                                                [](const auto &diagnostic) {
                                                  return diagnostic.code ==
                                                         "unsupported_relation";
                                                })
                                ? AnalysisResultClass::unsupported_relation
                                : AnalysisResultClass::package_incompatible;
      return result;
    }
    if (snapshot.completeness == FactCompleteness::stale) {
      result.status = AnalysisStatus::unknown;
      result.result_class = AnalysisResultClass::stale_input;
      result.diagnostics.push_back(
          {.code = "stale_input", .message = "fact snapshot is stale"});
      return result;
    }
    if (snapshot.completeness == FactCompleteness::unknown) {
      result.status = AnalysisStatus::unknown;
      result.result_class = AnalysisResultClass::unknown;
      result.diagnostics.push_back(
          {.code = "missing_evidence",
           .message = "fact snapshot applicability is unknown"});
      return result;
    }
    engine_started = true;
    result = engine_->execute(package, snapshot, options);
    result.elapsed_ms =
        std::max(result.elapsed_ms,
                 std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::steady_clock::now() - run_started)
                     .count());
    result.input_hash = snapshot.stable_hash();
    result.package_hash = sha256_hex(package.canonical());
    result.run_id = run_identity(package.canonical(), result.input_hash,
                                 options.canonical());
    if (result.engine_version.empty()) {
      result.engine_version = engine_->engine_version();
    }
    if (result.elapsed_ms > options.time_budget_ms) {
      result.status = AnalysisStatus::error;
      result.result_class = AnalysisResultClass::timeout;
      result.diagnostics.push_back(
          {.code = "timeout",
           .message = "analysis exceeded the declared time budget"});
    } else if (options.step_budget > 0 &&
               result.step_count > options.step_budget) {
      result.status = AnalysisStatus::error;
      result.result_class = AnalysisResultClass::step_budget_exceeded;
      result.diagnostics.push_back(
          {.code = "step_budget_exceeded",
           .message = "analysis exceeded the declared step budget"});
    }
    if ((snapshot.completeness == FactCompleteness::partial ||
         snapshot.truncated) &&
        result.status == AnalysisStatus::complete) {
      result.status = AnalysisStatus::partial;
      result.result_class = snapshot.truncated
                                ? AnalysisResultClass::truncated_input
                                : AnalysisResultClass::partial;
    }
    result.output_bytes =
        static_cast<std::int64_t>(result.canonical_result().size());
    if (options.output_budget > 0 &&
        result.output_bytes > options.output_budget) {
      result.status = AnalysisStatus::error;
      result.result_class = AnalysisResultClass::output_budget_exceeded;
      result.diagnostics.push_back(
          {.code = "output_budget_exceeded",
           .message = "analysis result exceeds the declared output budget"});
    }
    for (const auto &requirement : package.output_relations) {
      const FactRelation *relation =
          result.relations.contains(requirement.name)
              ? &result.relations.at(requirement.name)
              : nullptr;
      if (relation == nullptr ||
          relation->descriptor.version != requirement.version ||
          relation->descriptor.catalog_version != requirement.catalog_version ||
          (!requirement.columns.empty() &&
           relation->descriptor.columns != requirement.columns)) {
        result.status = AnalysisStatus::error;
        result.result_class = AnalysisResultClass::package_incompatible;
        result.diagnostics.push_back(
            {.code = "package_incompatible",
             .message = "engine did not produce declared output relation: " +
                        requirement.name});
      }
    }
    return result;
  } catch (const FactProviderError &error) {
    result.status =
        error.code() == "unsupported_relation" || error.code() == "missing_tu"
            ? AnalysisStatus::unknown
            : AnalysisStatus::error;
    if (error.code() == "unsupported_relation") {
      result.result_class = AnalysisResultClass::unsupported_relation;
    } else if (error.code() == "missing_tu") {
      result.result_class = AnalysisResultClass::missing_tu;
    } else {
      result.result_class = AnalysisResultClass::provider_failure;
    }
    result.diagnostics.push_back(
        {.code = error.code(), .message = error.what()});
    return result;
  } catch (const AnalysisEngineError &error) {
    if (error.partial_run()) {
      result = *error.partial_run();
    }
    result.status = AnalysisStatus::error;
    if (error.code() == "timeout") {
      result.result_class = AnalysisResultClass::timeout;
    } else if (error.code() == "step_budget_exceeded") {
      result.result_class = AnalysisResultClass::step_budget_exceeded;
    } else if (error.code() == "unsupported_budget") {
      result.result_class = AnalysisResultClass::unsupported_budget;
    } else if (error.code() == "capture_budget_exceeded") {
      result.result_class = AnalysisResultClass::capture_budget_exceeded;
    } else if (error.code() == "output_budget_exceeded") {
      result.result_class = AnalysisResultClass::output_budget_exceeded;
    } else {
      result.result_class = AnalysisResultClass::engine_failure;
    }
    if (result.engine_version.empty() || result.engine_version == "unknown") {
      result.engine_version = engine_->engine_version();
    }
    result.diagnostics.push_back(
        {.code = error.code(), .message = error.what()});
    return result;
  } catch (const CidxError &error) {
    result.status = AnalysisStatus::error;
    result.result_class = engine_started
                              ? AnalysisResultClass::engine_failure
                              : AnalysisResultClass::provider_failure;
    result.diagnostics.push_back(
        {.code = engine_started ? "engine_failure" : "provider_failure",
         .message = error.what()});
    return result;
  } catch (const std::exception &error) {
    result.status = AnalysisStatus::error;
    result.result_class = engine_started
                              ? AnalysisResultClass::engine_failure
                              : AnalysisResultClass::provider_failure;
    result.diagnostics.push_back(
        {.code = engine_started ? "engine_failure" : "provider_failure",
         .message = error.what()});
    return result;
  }
}

AnalysisService::AnalysisService(EngineFactory factory)
    : factory_(std::move(factory)) {
  if (!factory_) {
    factory_ = [](const AnalysisPackage &package) {
      if (package.engine != "souffle") {
        throw AnalysisEngineError("engine_unavailable",
                                  "no engine is registered for " +
                                      package.engine);
      }
      return std::make_unique<SouffleAnalysisEngine>();
    };
  }
}

std::unique_ptr<FactProvider>
AnalysisService::provider_for(const ProviderDeclaration &declaration) const {
  switch (declaration.kind) {
  case ProviderKind::semantic_index:
    return std::make_unique<SqliteFactProvider>(declaration.path.string());
  case ProviderKind::astgraph:
    return std::make_unique<AstgraphFactProvider>(declaration.path.string());
  case ProviderKind::extension:
    return std::make_unique<ExtensionFactProvider>(declaration.path.string());
  case ProviderKind::composed:
    if (!declaration.left || !declaration.right) {
      throw FactProviderError("provider_failure",
                              "composed provider declaration is incomplete");
    }
    return std::make_unique<ComposedFactProvider>(
        provider_for(*declaration.left), provider_for(*declaration.right),
        declaration.joins);
  }
  throw FactProviderError("provider_failure", "unknown provider kind");
}

AnalysisRun AnalysisService::run(const AnalysisRequest &request) const {
  try {
    auto provider = provider_for(request.provider);
    auto engine = factory_(request.package);
    AnalysisRun result =
        AnalysisRunner(std::move(engine))
            .run(request.package, *provider, request.facts, request.options);
    if (request.publication && result.status != AnalysisStatus::error &&
        result.status != AnalysisStatus::unknown) {
      const auto &publication = *request.publication;
      if (publication.storage_path.empty()) {
        throw AnalysisEngineError(
            "publication_failure",
            "analysis publication requires the semantic storage path");
      }
      Storage storage(publication.storage_path.string());
      const IndexIdentity identity = storage.index_identity();
      const std::string workspace = identity.workspace;
      const ArtifactRecord record = AnalysisPublisher::publish(
          storage, publication.artifact_root, publication.namespace_name,
          result, workspace, publication.tu_identity);
      AnalysisPublication metadata;
      metadata.logical_id = record.spec.logical_id;
      metadata.namespace_name = publication.namespace_name;
      metadata.content_hash = record.content_hash;
      metadata.relative_path = record.relative_path;
      metadata.relations = record.spec.exposed_relations;
      result.publication = std::move(metadata);
    }
    return result;
  } catch (const FactProviderError &error) {
    AnalysisRun result;
    result.status =
        error.code() == "unsupported_relation" || error.code() == "missing_tu"
            ? AnalysisStatus::unknown
            : AnalysisStatus::error;
    if (error.code() == "unsupported_relation") {
      result.result_class = AnalysisResultClass::unsupported_relation;
    } else if (error.code() == "missing_tu") {
      result.result_class = AnalysisResultClass::missing_tu;
    } else {
      result.result_class = AnalysisResultClass::provider_failure;
    }
    result.diagnostics.push_back(
        {.code = error.code(), .message = error.what()});
    return result;
  } catch (const AnalysisEngineError &error) {
    AnalysisRun result;
    result.status = AnalysisStatus::error;
    if (error.code() == "unsupported_budget") {
      result.result_class = AnalysisResultClass::unsupported_budget;
    } else if (error.code() == "capture_budget_exceeded") {
      result.result_class = AnalysisResultClass::capture_budget_exceeded;
    } else if (error.code() == "output_budget_exceeded") {
      result.result_class = AnalysisResultClass::output_budget_exceeded;
    } else if (error.code() == "timeout") {
      result.result_class = AnalysisResultClass::timeout;
    } else if (error.code() == "publication_failure") {
      result.result_class = AnalysisResultClass::publication_failure;
    } else {
      result.result_class = AnalysisResultClass::engine_failure;
    }
    result.diagnostics.push_back(
        {.code = error.code(), .message = error.what()});
    return result;
  } catch (const std::exception &error) {
    AnalysisRun result;
    result.status = AnalysisStatus::error;
    result.result_class = AnalysisResultClass::provider_failure;
    result.diagnostics.push_back(
        {.code = "provider_failure", .message = error.what()});
    return result;
  }
}

namespace {

bool valid_publication_namespace(const std::string_view value) {
  if (value.empty() || value == "core" || value.starts_with("core.")) {
    return false;
  }
  if (std::isalpha(static_cast<unsigned char>(value.front())) == 0) {
    return false;
  }
  return std::ranges::all_of(value, [](const char character) {
    return std::isalnum(static_cast<unsigned char>(character)) != 0 ||
           character == '_' || character == '-' || character == '.';
  });
}

std::string publication_row(const FactRow &row) {
  return relation_row_canonical(row);
}

} // namespace

ArtifactRecord
AnalysisPublisher::publish(Storage &storage, const std::filesystem::path &root,
                           const std::string_view namespace_name,
                           const AnalysisRun &run,
                           const std::string_view workspace_identity,
                           const std::string_view tu_identity) {
  if (!valid_publication_namespace(namespace_name)) {
    throw AnalysisEngineError("publication_failure",
                              "analysis publication namespace is not allowed");
  }
  if (run.status == AnalysisStatus::error ||
      run.status == AnalysisStatus::unknown) {
    throw AnalysisEngineError("publication_failure",
                              "only known analysis results can be published");
  }
  const std::string logical_id = "analysis:" + std::string(namespace_name);
  ArtifactSpec spec;
  spec.logical_id = logical_id;
  spec.kind = "analysis-result";
  spec.artifact_schema = "cidx-analysis/v1";
  spec.producer_version = "cidx-analysis 1";
  spec.engine_version = "cidx " + run.engine_version;
  spec.workspace_identity = std::string(workspace_identity);
  spec.tu_identity = std::string(tu_identity);
  spec.configuration_identity = run.package_hash;
  spec.input_fact_set_identity = run.input_hash;
  spec.completeness = run.status == AnalysisStatus::partial
                          ? ArtifactCompleteness::partial
                          : ArtifactCompleteness::complete;
  spec.truncation = run.result_class == AnalysisResultClass::truncated_input
                        ? ArtifactTruncation::truncated
                        : ArtifactTruncation::none;
  spec.trust = ArtifactTrust::producer_verified;
  spec.evidence = "derived";
  const std::string logical_hash = sha256_hex(logical_id);
  spec.attachment_name =
      "analysis_" + logical_hash.substr(logical_hash.starts_with("sha256:")
                                            ? std::string_view("sha256:").size()
                                            : 0,
                                        16);
  spec.exposed_relations = {"result"};
  ArtifactStore artifacts(storage, root);
  return artifacts.publish(spec, [&run, namespace_name](SqliteDb &db) {
    db.exec("CREATE TABLE result(namespace TEXT NOT NULL, relation_name TEXT "
            "NOT NULL, ordinal INTEGER NOT NULL, row_text TEXT NOT NULL, "
            "PRIMARY KEY(namespace, relation_name, ordinal))");
    db.exec("CREATE TABLE analysis_meta(key TEXT PRIMARY KEY, value TEXT NOT "
            "NULL)");
    const auto put = [&db](std::string_view key, std::string_view value) {
      auto statement =
          db.prepare("INSERT INTO analysis_meta(key, value) VALUES (?, ?)");
      statement.bind(1, key);
      statement.bind(2, value);
      statement.step_done();
    };
    put("namespace", namespace_name);
    put("run_id", run.run_id);
    put("result_class", analysis_result_class_name(run.result_class));
    put("engine_version", run.engine_version);
    put("stdout", run.stdout_text);
    put("stderr", run.stderr_text);
    put("elapsed_ms", std::to_string(run.elapsed_ms));
    put("peak_bytes", std::to_string(run.peak_bytes));
    put("step_count", std::to_string(run.step_count));
    put("output_bytes", std::to_string(run.output_bytes));
    put("output_budget_terminated", run.output_budget_terminated ? "1" : "0");
    put("generated_inputs", [&run] {
      std::ostringstream inputs;
      for (const auto &input : run.generated_inputs) {
        inputs << input << '\n';
      }
      return inputs.str();
    }());
    for (const auto &[name, relation] : run.relations) {
      std::vector<std::string> rows;
      rows.reserve(relation.rows.size());
      for (const auto &row : relation.rows) {
        rows.push_back(publication_row(row));
      }
      std::ranges::sort(rows);
      for (std::size_t ordinal = 0; ordinal < rows.size(); ++ordinal) {
        auto statement = db.prepare(
            "INSERT INTO result(namespace, relation_name, ordinal, row_text) "
            "VALUES (?, ?, ?, ?)");
        statement.bind(1, namespace_name);
        statement.bind(2, std::string_view(name));
        statement.bind(3, static_cast<std::int64_t>(ordinal));
        statement.bind(4, std::string_view(rows[ordinal]));
        statement.step_done();
      }
    }
  });
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

std::string analysis_result_class_name(const AnalysisResultClass result_class) {
  switch (result_class) {
  case AnalysisResultClass::complete:
    return "complete";
  case AnalysisResultClass::partial:
    return "partial";
  case AnalysisResultClass::unknown:
    return "unknown";
  case AnalysisResultClass::invalid_package:
    return "invalid_package";
  case AnalysisResultClass::package_incompatible:
    return "package_incompatible";
  case AnalysisResultClass::unsupported_relation:
    return "unsupported_relation";
  case AnalysisResultClass::stale_input:
    return "stale_input";
  case AnalysisResultClass::missing_tu:
    return "missing_tu";
  case AnalysisResultClass::truncated_input:
    return "truncated_input";
  case AnalysisResultClass::timeout:
    return "timeout";
  case AnalysisResultClass::step_budget_exceeded:
    return "step_budget_exceeded";
  case AnalysisResultClass::unsupported_budget:
    return "unsupported_budget";
  case AnalysisResultClass::capture_budget_exceeded:
    return "capture_budget_exceeded";
  case AnalysisResultClass::output_budget_exceeded:
    return "output_budget_exceeded";
  case AnalysisResultClass::provider_failure:
    return "provider_failure";
  case AnalysisResultClass::engine_failure:
    return "engine_failure";
  case AnalysisResultClass::publication_failure:
    return "publication_failure";
  }
  throw CidxError("invalid analysis result class");
}

} // namespace cidx::analysis
