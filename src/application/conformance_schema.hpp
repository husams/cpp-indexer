// Loads the DECLARED conformance contract -- spec/tla/conformance's
// operation-map.json / observation-map.json (and the sidecar variants) --
// from the checked-in JSON files, so ConformanceRecorder replays real C++
// traces against the actual schema instead of an invented, ad hoc mirror of
// it (HSE-89 review fix: the schema files are the single source of truth).
#pragma once

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace cidx::application {

// One row of operation-map.json / sidecar-operation-map.json: an abstract
// TLA+ specAction, and the one field=value observation the spec says that
// action implies.
struct OperationExpectation {
  std::string operation;
  std::string field;
  std::string value;
};

class ConformanceSchema {
public:
  // Parses operation_map_json/observation_map_json (the raw file contents,
  // not paths -- callers read the checked-in files themselves so the schema
  // has no filesystem dependency of its own). Throws CidxError on malformed
  // JSON or a shape that does not match the documented contract.
  static ConformanceSchema parse(const std::string &operation_map_json,
                                 const std::string &observation_map_json);

  // True if `value` is one of the declared `allowed` values for `field` in
  // observation-map.json/sidecar-observation-map.json. Fields with no
  // declared `allowed` list (e.g. integer-typed fields) always return true --
  // callers must range/type-check those separately.
  [[nodiscard]] bool is_allowed(const std::string &field,
                                const std::string &value) const;

  // The one field=value observation-map.json/sidecar-operation-map.json says
  // `operation` implies, if that operation is declared.
  [[nodiscard]] std::optional<OperationExpectation>
  expectation_for(const std::string &operation) const;

private:
  std::vector<std::pair<std::string, std::vector<std::string>>> allowed_;
  std::vector<OperationExpectation> expectations_;
};

} // namespace cidx::application
