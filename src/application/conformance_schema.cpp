#include "application/conformance_schema.hpp"

#include <algorithm>

#include "util/errors.hpp"
#include "util/json_read.hpp"

namespace cidx::application {

namespace {

using cidx::json_out::Value;

const Value *find_member(const Value &object, const std::string &key) {
  if (object.t != Value::T::Obj) {
    return nullptr;
  }
  for (const auto &[member_key, member_value] : object.o) {
    if (member_key == key) {
      return &member_value;
    }
  }
  return nullptr;
}

std::string require_string(const Value &object, const std::string &key,
                           const std::string &context) {
  const Value *member = find_member(object, key);
  if (member == nullptr || member->t != Value::T::Str) {
    throw CidxError("conformance schema: " + context + " missing string '" +
                    key + "'");
  }
  return member->s;
}

// "field=value" -> {"field", "value"}. Both operation-map.json and
// sidecar-operation-map.json use exactly this shape for their `observation`
// column.
OperationExpectation parse_expectation(const std::string &operation,
                                       const std::string &observation) {
  const auto equals = observation.find('=');
  if (equals == std::string::npos) {
    throw CidxError("conformance schema: malformed observation for operation "
                    "'" +
                    operation + "': " + observation);
  }
  return OperationExpectation{
      .operation = operation,
      .field = observation.substr(0, equals),
      .value = observation.substr(equals + 1),
  };
}

} // namespace

ConformanceSchema
ConformanceSchema::parse(const std::string &operation_map_json,
                         const std::string &observation_map_json) {
  ConformanceSchema schema;

  const Value operations_doc = json_read::parse(operation_map_json);
  const Value *operations = find_member(operations_doc, "operations");
  if (operations == nullptr || operations->t != Value::T::Arr) {
    throw CidxError(
        "conformance schema: operation map missing 'operations' array");
  }
  for (const Value &row : operations->a) {
    const std::string operation =
        require_string(row, "operation", "operation-map row");
    const std::string observation = require_string(
        row, "observation", "operation-map row '" + operation + "'");
    schema.expectations_.push_back(parse_expectation(operation, observation));
  }

  const Value observations_doc = json_read::parse(observation_map_json);
  const Value *observations = find_member(observations_doc, "observations");
  if (observations == nullptr || observations->t != Value::T::Arr) {
    throw CidxError(
        "conformance schema: observation map missing 'observations' array");
  }
  for (const Value &row : observations->a) {
    const std::string field =
        require_string(row, "field", "observation-map row");
    const Value *allowed = find_member(row, "allowed");
    if (allowed == nullptr) {
      continue; // typed (integer/sequence) fields are range-checked by callers.
    }
    if (allowed->t != Value::T::Arr) {
      throw CidxError("conformance schema: 'allowed' for field '" + field +
                      "' is not an array");
    }
    std::vector<std::string> values;
    values.reserve(allowed->a.size());
    for (const Value &entry : allowed->a) {
      // Boolean-valued fields (e.g. sidecarValidated) list true/false in
      // `allowed`; render them the same way callers render recorded values.
      if (entry.t == Value::T::Bool) {
        values.emplace_back(entry.b ? "true" : "false");
      } else if (entry.t == Value::T::Str) {
        values.push_back(entry.s);
      } else {
        throw CidxError(
            "conformance schema: non-string/bool allowed value for field '" +
            field + "'");
      }
    }
    schema.allowed_.emplace_back(field, std::move(values));
  }

  return schema;
}

bool ConformanceSchema::is_allowed(const std::string &field,
                                   const std::string &value) const {
  for (const auto &[allowed_field, allowed_values] : allowed_) {
    if (allowed_field == field) {
      return std::ranges::find(allowed_values, value) != allowed_values.end();
    }
  }
  return true; // no declared allowed-list for this field: nothing to check.
}

std::optional<OperationExpectation>
ConformanceSchema::expectation_for(const std::string &operation) const {
  for (const auto &expectation : expectations_) {
    if (expectation.operation == operation) {
      return expectation;
    }
  }
  return std::nullopt;
}

} // namespace cidx::application
