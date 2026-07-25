// Preprocessing facts for the v31 include tier (planning/cidx-include-hygiene
// M1). The indexer's original IncludeRecorder kept only the resolved target
// path, because header ownership was its only consumer. Include hygiene needs
// the whole directive: where it is, exactly which bytes to remove, how it was
// written, which conditional region encloses it, whether the target is
// multiple-include guarded, and which macros the source expanded from where.
//
// Collection is pure PPCallbacks; nothing touches the database until
// persist_include_facts() runs, after the header passes have minted file rows
// for every owned header (an include_edge references file(id), so the rows must
// exist first).
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "storage/records.hpp"

namespace clang {
class CompilerInstance;
class Preprocessor;
class SourceManager;
} // namespace clang

namespace cidx {
class Storage;
}

namespace cidx::ast {

// One `#include` (or #include_next/#import/#include_macros) directive as
// written. Paths are "as opened" (search dir + spelling), never symlink-
// resolved, matching the rest of the engine's ownership rules.
struct IncludeFact {
  std::string src_path; // file containing the directive
  std::string dst_path; // resolved target, or "" when unresolved
  std::string spelling; // written filename, without <> or ""
  bool is_angled = false;
  int64_t line = 0;
  int64_t col = 0;
  // [begin_offset, end_offset) bounds the exact removal range in src_path's
  // buffer: the whole directive line including its terminating newline, plus
  // any backslash-continued lines. Byte offsets from the start of the file, so
  // replacements never depend on line renumbering.
  int64_t begin_offset = 0;
  int64_t end_offset = 0;
  int64_t directive = kIncludeDirectiveInclude;
  std::string cond_fingerprint; // "" = unconditional top level
  bool resolved = true;
  bool is_system = false;
  bool guarded = false; // target has #pragma once or an include guard
};

// A macro expanded in src_path whose definition lives in def_path. This is the
// dependency the symbol-reference graph cannot see: a header that only supplies
// a macro has zero symbol references yet is genuinely required.
struct MacroUseFact {
  std::string src_path;
  std::string def_path;
  std::string name;
};

// Everything one TU's preprocessor observed.
struct IncludeFacts {
  std::vector<IncludeFact> includes;
  std::vector<MacroUseFact> macro_uses;
};

struct IncludeFactCounts {
  std::size_t emitted_facts = 0;
  std::size_t duplicates = 0;
};

// Deterministic identity of a normalized compilation configuration:
// sha1 over driver, working dir, lang mode, resource dir, and each argument in
// order, NUL-separated. Stable across runs and machines given the same inputs,
// which is what makes a cleanup plan's freshness check meaningful.
std::string include_config_digest(const IncludeConfig &c);

// Register the PPCallbacks that fill `out`. Call from
// BeginSourceFileAction; `out` must outlive the preprocessor.
void register_include_callbacks(clang::CompilerInstance &ci, IncludeFacts &out);

// Fill in each fact's `guarded` flag from HeaderSearch, which knows whether a
// target carries #pragma once or a recognized include guard only AFTER the
// header has been lexed. Call once the TU is fully parsed and before
// persisting -- deliberately NOT done from an EndOfMainFile callback, whose
// ordering against HandleTranslationUnit is not something this engine should
// depend on. Guardedness is the precondition for calling a repeated directive a
// duplicate: dropping the second #include of an UNGUARDED header (an X-macro,
// say) changes the program.
void resolve_include_guards(clang::Preprocessor &pp, IncludeFacts &out);

// Write one TU's facts to the database under `config`. Only directives whose
// SOURCE file is owned by a component (and therefore has a file row) are
// persisted; a target that is unowned, system, or unresolved is kept as an edge
// with a NULL dst_file_id so "why is this header present?" still answers.
// Existing facts for each touched source file are deleted first, so a deleted
// #include leaves no stale row.
void persist_include_facts(cidx::Storage &db, const IncludeFacts &facts,
                           const IncludeConfig &config);

// Count the unique fact identities that persist_include_facts will publish
// before it mutates storage, and report repeated upsert attempts separately.
// This is the budget preflight for the bulk include pass.
auto include_fact_count(cidx::Storage &db, const IncludeFacts &facts)
    -> IncludeFactCounts;

} // namespace cidx::ast
