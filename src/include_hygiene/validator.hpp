// Removal safety validation (planning/cidx-include-hygiene M3).
//
// The reference analyzer says an include is unreferenced. That is a claim about
// the symbol graph, NOT a proof that removing it is safe: a header can be a
// transitive provider, supply a macro or pragma, or exist for a registration
// side effect. This layer is the apply-safety gate -- it actually compiles the
// code without the directive, using an in-memory overlay so nothing on disk is
// touched.
//
// What it proves and what it does NOT:
//   * PROVES: every recorded affected translation unit still parses, under
//     every configuration cidx has on record for it.
//   * DOES NOT PROVE behavioral equivalence. A header removed for a static
//     registration, a configuration-changing macro, or a pragma can compile
//     perfectly and still change the program. Those candidates must stay
//     manual_review; compile success must never be reported as absolute safety.
//   * DOES NOT PROVE anything about configurations cidx never indexed.
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "include_hygiene/plan.hpp"

namespace cidx {
class Storage;
}

namespace cidx::hygiene {

// One removal: delete [begin_offset, end_offset) from `abs_path`.
struct Removal {
  std::string abs_path;
  int64_t begin_offset = 0;
  int64_t end_offset = 0;
};

// A translation unit to compile, with the exact arguments it was indexed with.
struct TuTarget {
  std::string tu_path;
  std::string config_digest;
  std::vector<std::string> arguments;
  std::string working_dir;
};

class RemovalValidator {
public:
  explicit RemovalValidator(cidx::Storage &db);

  // Every TU/configuration that must still compile when `abs_path` changes:
  // the file itself when it is a TU, plus every TU whose include closure
  // reaches it. An edit to a header is only proven when ALL of them pass.
  // Returns nullopt when the reverse closure cannot be established (the file is
  // reached by something cidx has no compile command for), which is a reason to
  // refuse, never to skip.
  std::optional<std::vector<TuTarget>> affected_tus(const std::string &abs_path);

  // Compile `targets` with `overlay` (absolute path -> the exact bytes the
  // compiler should see for that file) layered over the real filesystem. An
  // empty overlay validates the tree as it stands on disk.
  //
  // Content, not removals, is the parameter deliberately: the executor formats
  // its buffers after computing them, so the bytes finally written are not
  // byte-identical to original-minus-directive. Proving the removal and then
  // writing something else would make the proof a lie. This lets both stages
  // prove exactly the bytes they mean.
  std::vector<ValidationRecord>
  validate(const std::vector<TuTarget> &targets,
           const std::map<std::string, std::string> &overlay,
           const std::string &stage);

  // Convenience: build the overlay for `removals` by reading each file and
  // cutting the ranges out.
  static std::map<std::string, std::string>
  overlay_for(const std::vector<Removal> &removals);

  // True when every record passed.
  static bool all_ok(const std::vector<ValidationRecord> &records);

  // The file's bytes with `removals` applied, in ORIGINAL offset order --
  // never by successive line renumbering, which would invalidate every later
  // offset. Shared by the validator's overlay and the executor's real write, so
  // what was proven is exactly what gets written.
  static std::string apply_removals(const std::string &content,
                                    std::vector<Removal> removals);

private:
  cidx::Storage &db_;
};

} // namespace cidx::hygiene
