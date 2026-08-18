#include "include_hygiene/plan.hpp"

#include "cli/json_out.hpp"
#include "util/errors.hpp"
#include "util/hashing.hpp"
#include "util/json_read.hpp"
#include "util/pathutil.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

namespace cidx::hygiene {

using cidx::json_out::Value;

const char *plan_state_name(PlanState s) {
  switch (s) {
  case PlanState::Accepted:
    return "validated_for_apply";
  case PlanState::Rejected:
    return "rejected";
  case PlanState::ManualReview:
    return "manual_review";
  }
  return "manual_review";
}

std::optional<PlanState> plan_state_from(const std::string &name) {
  if (name == "validated_for_apply") {
    return PlanState::Accepted;
  }
  if (name == "rejected") {
    return PlanState::Rejected;
  }
  if (name == "manual_review") {
    return PlanState::ManualReview;
  }
  return std::nullopt;
}

std::optional<std::string> hash_file(const std::string &abs_path) {
  std::ifstream in(abs_path, std::ios::binary);
  if (!in) {
    return std::nullopt;
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  return cidx::sha1_hex(ss.str());
}

namespace {

Value str_array(const std::vector<std::string> &v) {
  json_out::Array a;
  for (const std::string &s : v) {
    a.push_back(Value::of(s));
  }
  return Value::arr(std::move(a));
}

Value validations_json(const std::vector<ValidationRecord> &v) {
  json_out::Array a;
  for (const ValidationRecord &r : v) {
    a.push_back(Value::obj({{"stage", Value::of(r.stage)},
                            {"tu", Value::of(r.tu_path)},
                            {"config", Value::of(r.config)},
                            {"ok", Value::of(r.ok)},
                            {"diagnostic", Value::of(r.diagnostic)}}));
  }
  return Value::arr(std::move(a));
}

// -- typed readers: a plan is untrusted, so every access is checked ----------

[[noreturn]] void bad(const std::string &what) {
  throw CidxError("cleanup plan: " + what);
}

const Value *member(const Value &obj, std::string_view key) {
  if (obj.t != Value::T::Obj) {
    return nullptr;
  }
  for (const auto &m : obj.o) {
    if (m.first == key) {
      return &m.second;
    }
  }
  return nullptr;
}

const Value &require(const Value &obj, std::string_view key, Value::T type) {
  const Value *v = member(obj, key);
  if (v == nullptr) {
    bad("missing required field '" + std::string(key) + "'");
  }
  if (v->t != type) {
    bad("field '" + std::string(key) + "' has the wrong type");
  }
  return *v;
}

std::string opt_str(const Value &obj, std::string_view key) {
  const Value *v = member(obj, key);
  return (v != nullptr && v->t == Value::T::Str) ? v->s : std::string();
}

int64_t opt_int(const Value &obj, std::string_view key) {
  const Value *v = member(obj, key);
  return (v != nullptr && v->t == Value::T::Int) ? v->i : 0;
}

bool opt_bool(const Value &obj, std::string_view key) {
  const Value *v = member(obj, key);
  return v != nullptr && v->t == Value::T::Bool && v->b;
}

std::vector<std::string> opt_str_array(const Value &obj, std::string_view key) {
  std::vector<std::string> out;
  const Value *v = member(obj, key);
  if (v == nullptr || v->t != Value::T::Arr) {
    return out;
  }
  for (const Value &e : v->a) {
    if (e.t != Value::T::Str) {
      bad("field '" + std::string(key) + "' must be an array of strings");
    }
    out.push_back(e.s);
  }
  return out;
}

std::vector<ValidationRecord> read_validations(const Value &obj,
                                               std::string_view key) {
  std::vector<ValidationRecord> out;
  const Value *v = member(obj, key);
  if (v == nullptr || v->t != Value::T::Arr) {
    return out;
  }
  for (const Value &e : v->a) {
    if (e.t != Value::T::Obj) {
      bad("field '" + std::string(key) + "' must be an array of objects");
    }
    ValidationRecord r;
    r.stage = opt_str(e, "stage");
    r.tu_path = opt_str(e, "tu");
    r.config = opt_str(e, "config");
    r.ok = opt_bool(e, "ok");
    r.diagnostic = opt_str(e, "diagnostic");
    out.push_back(std::move(r));
  }
  return out;
}

} // namespace

std::string serialize(const CleanupPlan &p) {
  json_out::Array items;
  for (const PlanItem &it : p.items) {
    items.push_back(Value::obj({
        {"id", Value::of(it.id)},
        {"file", Value::of(it.file)},
        {"line", Value::of(it.line)},
        {"col", Value::of(it.col)},
        {"begin_offset", Value::of(it.begin_offset)},
        {"end_offset", Value::of(it.end_offset)},
        {"directive_text", Value::of(it.directive_text)},
        {"resolved_target", Value::of(it.resolved_target)},
        {"classification", Value::of(it.classification)},
        {"state", Value::of(std::string(plan_state_name(it.state)))},
        {"reason", Value::of(it.reason)},
        {"evidence",
         Value::obj(
             {{"owners", str_array(it.owners)},
              {"header_symbols", str_array(it.header_symbols)},
              {"intersection_count", Value::of(it.intersection_count)},
              {"reference_kinds_searched",
               str_array(it.reference_kinds_searched)},
              {"macro_uses", str_array(it.macro_uses)},
              {"guarded", Value::of(it.guarded)},
              {"reverse_dependants", str_array(it.reverse_dependants)}})},
        {"affected_tus", str_array(it.affected_tus)},
        {"configs", str_array(it.configs)},
        {"validations", validations_json(it.validations)},
    }));
  }

  json_out::Array hashes;
  for (const auto &[file, hash] : p.file_hashes) { // std::map: sorted
    hashes.push_back(
        Value::obj({{"file", Value::of(file)}, {"sha1", Value::of(hash)}}));
  }

  return json_out::dumps_indent2(Value::obj({
      {"format_version", Value::of(p.format_version)},
      {"repo_root", Value::of(p.repo_root)},
      {"db_path", Value::of(p.db_path)},
      {"schema_version", Value::of(p.schema_version)},
      {"resolved_at", Value::of(p.resolved_at)},
      {"config_digests", str_array(p.config_digests)},
      {"file_hashes", Value::arr(std::move(hashes))},
      {"limitations", str_array(p.limitations)},
      {"validations", validations_json(p.validations)},
      {"items", Value::arr(std::move(items))},
  }));
}

CleanupPlan deserialize(const std::string &text) {
  const Value root = json_read::parse(text); // throws on malformed JSON
  if (root.t != Value::T::Obj) {
    bad("top level must be an object");
  }
  CleanupPlan p;
  p.format_version =
      static_cast<int>(require(root, "format_version", Value::T::Int).i);
  if (p.format_version != kPlanFormatVersion) {
    // Refuse rather than guess: a field's meaning may have changed, and this
    // artifact describes edits to source.
    bad("unsupported format_version " + std::to_string(p.format_version) +
        " (this build writes and reads version " +
        std::to_string(kPlanFormatVersion) + "); regenerate the plan");
  }
  p.repo_root = require(root, "repo_root", Value::T::Str).s;
  p.db_path = opt_str(root, "db_path");
  p.schema_version = static_cast<int>(opt_int(root, "schema_version"));
  p.resolved_at = opt_str(root, "resolved_at");
  p.config_digests = opt_str_array(root, "config_digests");
  p.limitations = opt_str_array(root, "limitations");
  p.validations = read_validations(root, "validations");

  if (const Value *hashes = member(root, "file_hashes");
      hashes != nullptr && hashes->t == Value::T::Arr) {
    for (const Value &h : hashes->a) {
      if (h.t != Value::T::Obj) {
        bad("file_hashes must be an array of objects");
      }
      p.file_hashes[require(h, "file", Value::T::Str).s] =
          require(h, "sha1", Value::T::Str).s;
    }
  }

  const Value &items = require(root, "items", Value::T::Arr);
  for (const Value &e : items.a) {
    if (e.t != Value::T::Obj) {
      bad("items must be an array of objects");
    }
    PlanItem it;
    it.id = require(e, "id", Value::T::Str).s;
    it.file = require(e, "file", Value::T::Str).s;
    it.line = opt_int(e, "line");
    it.col = opt_int(e, "col");
    it.begin_offset = require(e, "begin_offset", Value::T::Int).i;
    it.end_offset = require(e, "end_offset", Value::T::Int).i;
    if (it.begin_offset < 0 || it.end_offset < it.begin_offset) {
      bad("item '" + it.id + "' has an inverted or negative byte range");
    }
    it.directive_text = require(e, "directive_text", Value::T::Str).s;
    it.resolved_target = opt_str(e, "resolved_target");
    it.classification = opt_str(e, "classification");
    const std::string state = require(e, "state", Value::T::Str).s;
    if (const std::optional<PlanState> s = plan_state_from(state)) {
      it.state = *s;
    } else {
      bad("item '" + it.id + "' has an unknown state '" + state + "'");
    }
    it.reason = opt_str(e, "reason");
    if (const Value *ev = member(e, "evidence");
        ev != nullptr && ev->t == Value::T::Obj) {
      it.owners = opt_str_array(*ev, "owners");
      it.header_symbols = opt_str_array(*ev, "header_symbols");
      it.intersection_count = opt_int(*ev, "intersection_count");
      it.reference_kinds_searched =
          opt_str_array(*ev, "reference_kinds_searched");
      it.macro_uses = opt_str_array(*ev, "macro_uses");
      it.guarded = opt_bool(*ev, "guarded");
      it.reverse_dependants = opt_str_array(*ev, "reverse_dependants");
    }
    it.affected_tus = opt_str_array(e, "affected_tus");
    it.configs = opt_str_array(e, "configs");
    it.validations = read_validations(e, "validations");
    p.items.push_back(std::move(it));
  }
  return p;
}

std::vector<std::string> staleness_reasons(const CleanupPlan &p,
                                           int current_schema_version) {
  std::vector<std::string> out;
  if (p.schema_version != current_schema_version) {
    out.push_back("index schema changed: plan was made against v" +
                  std::to_string(p.schema_version) + ", this build is v" +
                  std::to_string(current_schema_version));
  }
  for (const auto &[file, want] : p.file_hashes) {
    const std::string abs = pathutil::join(p.repo_root, file);
    const std::optional<std::string> have = hash_file(abs);
    if (!have) {
      out.push_back("file has gone: " + file);
    } else if (*have != want) {
      out.push_back("file changed since the plan was made: " + file);
    }
  }
  return out;
}

} // namespace cidx::hygiene
