#include "ast/include_facts.hpp"

#include "profile/index_profile.hpp"
#include "storage/storage.hpp"
#include "util/errors.hpp"
#include "util/hashing.hpp"
#include "util/pathutil.hpp"

#include "clang/Basic/SourceManager.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Lex/HeaderSearch.h"
#include "clang/Lex/MacroInfo.h"
#include "clang/Lex/PPCallbacks.h"
#include "clang/Lex/Preprocessor.h"

#include <memory>
#include <set>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

namespace cidx::ast {

namespace {

void record_header_applicability(cidx::Storage &db,
                                 const std::optional<int64_t> &file_id,
                                 int64_t config_id) {
  if (file_id) {
    db.add_file_config(cidx::FileConfigApplicability{
        .file_id = *file_id, .config_id = config_id, .role = "header"});
  }
}

struct IncludePersistenceState {
  cidx::Storage &db;
  IncludeFactStats &stats;
  int64_t normalized_id;
  std::unordered_map<std::string, std::optional<int64_t>> path_ids;
  std::set<int64_t> file_applicability;
  std::set<std::tuple<int64_t, std::string>> edge_identities;
  std::set<std::tuple<int64_t, std::string, int64_t>> site_identities;
  std::set<std::tuple<int64_t, std::string, std::string>> macro_identities;

  auto file_id_for(const std::string &path) -> std::optional<int64_t> {
    if (path.empty()) {
      return std::nullopt;
    }
    const std::string abs = cidx::pathutil::abspath(path);
    if (const auto it = path_ids.find(abs); it != path_ids.end()) {
      return it->second;
    }
    ++stats.path_resolution_queries;
    std::optional<int64_t> id;
    if (const std::optional<cidx::File> file = db.get_file(abs)) {
      id = file->id;
    }
    path_ids.emplace(abs, id);
    return id;
  }
};

void persist_include_edge_and_site(IncludePersistenceState &state,
                                   const IncludeFact &fact, int64_t config_id,
                                   int64_t source_id) {
  IncludeEdge edge;
  edge.src_file_id = source_id;
  edge.dst_path =
      fact.resolved ? cidx::pathutil::abspath(fact.dst_path) : fact.spelling;
  edge.dst_file_id =
      fact.resolved ? state.file_id_for(fact.dst_path) : std::nullopt;
  if (edge.dst_file_id) {
    ++state.stats.attempted;
    state.file_applicability.insert(*edge.dst_file_id);
  }
  record_header_applicability(state.db, edge.dst_file_id, state.normalized_id);
  edge.config_id = config_id;
  edge.is_system = fact.is_system;
  edge.count = 1;
  const int64_t edge_id = state.db.add_include_edge(edge);
  ++state.stats.attempted;
  ++state.stats.inserted_or_updated;
  if (!state.edge_identities.emplace(source_id, edge.dst_path).second) {
    ++state.stats.ignored;
  }

  IncludeSite site;
  site.edge_id = edge_id;
  site.line = fact.line;
  site.col = fact.col;
  site.begin_offset = fact.begin_offset;
  site.end_offset = fact.end_offset;
  site.spelling = fact.spelling;
  site.is_angled = fact.is_angled;
  site.directive = fact.directive;
  site.cond_fingerprint = fact.cond_fingerprint;
  site.resolved = fact.resolved;
  site.guarded = fact.guarded;
  state.db.add_include_site(site);
  ++state.stats.attempted;
  ++state.stats.inserted_or_updated;
  if (!state.site_identities
           .emplace(source_id, edge.dst_path, fact.begin_offset)
           .second) {
    ++state.stats.ignored;
  }
}

void persist_include_fact(IncludePersistenceState &state,
                          const IncludeFact &fact, int64_t config_id) {
  if (fact.src_path.empty()) {
    return;
  }
  const std::optional<int64_t> source_id = state.file_id_for(fact.src_path);
  if (!source_id) {
    ++state.stats.ignored;
    return;
  }
  record_header_applicability(state.db, source_id, state.normalized_id);
  ++state.stats.attempted;
  state.file_applicability.insert(*source_id);
  persist_include_edge_and_site(state, fact, config_id, *source_id);
}

void persist_macro_use(IncludePersistenceState &state, const MacroUseFact &fact,
                       int64_t config_id) {
  const std::optional<int64_t> source_id = state.file_id_for(fact.src_path);
  if (!source_id) {
    ++state.stats.ignored;
    return;
  }
  IncludeMacroUse use;
  use.src_file_id = *source_id;
  use.def_path = cidx::pathutil::abspath(fact.def_path);
  use.name = fact.name;
  use.config_id = config_id;
  use.count = 1;
  state.db.add_include_macro_use(use);
  ++state.stats.attempted;
  ++state.stats.inserted_or_updated;
  if (!state.macro_identities.emplace(*source_id, use.def_path, use.name)
           .second) {
    ++state.stats.ignored;
  }
}

// The enclosing #if/#elif/#else stack at a directive. Clang only lexes taken
// branches, so an include inside a false branch produces NO fact for this
// configuration -- which is exactly right: it is not part of this TU. The
// fingerprint distinguishes "same header, different branch" (never a duplicate)
// from "same header, same branch" (a duplicate candidate).
class ConditionalTracker {
public:
  void push(const clang::SourceManager &sm, clang::SourceLocation loc) {
    stack_.push_back({.key = region_key(sm, loc), .branch = 0});
  }
  void next_branch() {
    if (!stack_.empty()) {
      ++stack_.back().branch;
    }
  }
  void pop() {
    if (!stack_.empty()) {
      stack_.pop_back();
    }
  }
  // "" at unconditional top level; else a digest of the whole region stack so
  // nesting is distinguished without storing an unbounded string.
  [[nodiscard]] std::string fingerprint() const {
    if (stack_.empty()) {
      return "";
    }
    std::string joined;
    for (const Region &r : stack_) {
      joined += r.key;
      joined += '#';
      joined += std::to_string(r.branch);
      joined += '\0';
    }
    return cidx::sha1_hex(joined);
  }

private:
  struct Region {
    std::string key;
    int branch = 0;
  };

  // Identity of one #if: the file and offset of its directive. Stable across
  // runs; independent of the condition's text.
  static std::string region_key(const clang::SourceManager &sm,
                                clang::SourceLocation loc) {
    const clang::FileID fid = sm.getFileID(sm.getExpansionLoc(loc));
    const clang::OptionalFileEntryRef fe = sm.getFileEntryRefForID(fid);
    const std::string path =
        fe ? fe->getName().str() : std::string("<builtin>");
    return path + "@" +
           std::to_string(sm.getFileOffset(sm.getExpansionLoc(loc)));
  }

  std::vector<Region> stack_;
};

// Byte offset of the start of the line containing `offset`.
std::size_t line_start(llvm::StringRef buf, std::size_t offset) {
  while (offset > 0 && buf[offset - 1] != '\n') {
    --offset;
  }
  return offset;
}

// Byte offset just past the newline that ends the logical line starting at
// `offset`, following backslash line continuations. Returns buf.size() at EOF
// with no trailing newline.
std::size_t logical_line_end(llvm::StringRef buf, std::size_t offset) {
  const std::size_t n = buf.size();
  while (offset < n) {
    if (buf[offset] == '\\') {
      // A continuation is a backslash followed by (optional \r then) \n.
      std::size_t k = offset + 1;
      if (k < n && buf[k] == '\r') {
        ++k;
      }
      if (k < n && buf[k] == '\n') {
        offset = k + 1; // the physical line continues
        continue;
      }
    }
    if (buf[offset] == '\n') {
      return offset + 1;
    }
    ++offset;
  }
  return n;
}

// True when everything in [from, to) is horizontal whitespace. An indented
// directive owns its indentation and removes it with the line; a directive that
// shares a line with real code (pathological but legal after macro tricks) is
// removed by directive extent only, never taking the neighbouring code with it.
bool blank_between(llvm::StringRef buf, std::size_t from, std::size_t to) {
  for (std::size_t i = from; i < to; ++i) {
    if (buf[i] != ' ' && buf[i] != '\t') {
      return false;
    }
  }
  return true;
}

int64_t directive_kind_of(llvm::StringRef name) {
  if (name == "include") {
    return kIncludeDirectiveInclude;
  }
  if (name == "include_next") {
    return kIncludeDirectiveIncludeNext;
  }
  if (name == "import") {
    return kIncludeDirectiveImport;
  }
  if (name == "__include_macros") {
    return kIncludeDirectiveIncludeMacros;
  }
  return kIncludeDirectiveUnknown;
}

class IncludeFactRecorder : public clang::PPCallbacks {
public:
  IncludeFactRecorder(clang::Preprocessor &pp, IncludeFacts &out)
      : sm_(pp.getSourceManager()), out_(out) {}

  void InclusionDirective(
      clang::SourceLocation hash_loc, const clang::Token &include_tok,
      llvm::StringRef file_name, bool is_angled,
      clang::CharSourceRange filename_range, clang::OptionalFileEntryRef file,
      llvm::StringRef /*search_path*/, llvm::StringRef /*relative_path*/,
      const clang::Module * /*suggested_module*/, bool /*module_imported*/,
      clang::SrcMgr::CharacteristicKind file_type) override {
    if (profile::active()) {
      profile::add_counter("include_path_resolution_queries");
    }
    const clang::SourceLocation loc = sm_.getExpansionLoc(hash_loc);
    const clang::FileID fid = sm_.getFileID(loc);
    const clang::OptionalFileEntryRef src = sm_.getFileEntryRefForID(fid);

    IncludeFact f;
    // A directive from a `<command line>` / builtin buffer (-include foo.h) has
    // no editable source file: src_path stays empty, so persist drops it. The
    // fact is still recorded because the header two-pass must index the target.
    f.src_path = src ? src->getName().str() : std::string();
    // clang_getFileName parity: the path AS OPENED, never symlink-resolved
    // (/var vs /private/var matters for ownership).
    f.dst_path = file ? file->getName().str() : std::string();
    f.resolved = file.has_value();
    f.spelling = file_name.str();
    f.is_angled = is_angled;
    f.line = static_cast<int64_t>(sm_.getExpansionLineNumber(loc));
    f.col = static_cast<int64_t>(sm_.getExpansionColumnNumber(loc));
    f.directive =
        directive_kind_of((include_tok.getIdentifierInfo() != nullptr)
                              ? include_tok.getIdentifierInfo()->getName()
                              : llvm::StringRef());
    f.cond_fingerprint = cond_.fingerprint();
    f.is_system = file_type != clang::SrcMgr::C_User;
    compute_removal_range(fid, loc, filename_range, f);
    // guarded is resolved at EndOfMainFile: the target has not been read yet,
    // so HeaderSearch cannot know its guard state at this point.
    out_.includes.push_back(std::move(f));
  }

  void MacroExpands(const clang::Token &tok, const clang::MacroDefinition &md,
                    clang::SourceRange range,
                    const clang::MacroArgs * /*args*/) override {
    record_macro_dependency(tok, md, range.getBegin());
  }

  void If(clang::SourceLocation loc, clang::SourceRange /*ConditionRange*/,
          ConditionValueKind /*ConditionValue*/) override {
    cond_.push(sm_, loc);
  }
  void Ifdef(clang::SourceLocation loc, const clang::Token &tok,
             const clang::MacroDefinition &md) override {
    // #ifdef/#ifndef select a branch on a macro's DEFINEDNESS without ever
    // expanding it, so MacroExpands never fires. If the tested macro is
    // supplied by a header, removing that header silently flips the branch --
    // and still compiles. Record the dependency here so the header reads as
    // used, exactly as an expansion would.
    record_macro_dependency(tok, md, tok.getLocation());
    cond_.push(sm_, loc);
  }
  void Ifndef(clang::SourceLocation loc, const clang::Token &tok,
              const clang::MacroDefinition &md) override {
    record_macro_dependency(tok, md, tok.getLocation());
    cond_.push(sm_, loc);
  }
  void Defined(const clang::Token &tok, const clang::MacroDefinition &md,
               clang::SourceRange range) override {
    // defined(X) inside #if/#elif -- same definedness test, same silent branch
    // flip if the providing header is removed.
    record_macro_dependency(tok, md, range.getBegin());
  }
  void Elif(clang::SourceLocation /*Loc*/,
            clang::SourceRange /*ConditionRange*/,
            ConditionValueKind /*ConditionValue*/,
            clang::SourceLocation /*IfLoc*/) override {
    cond_.next_branch();
  }
  void Elifdef(clang::SourceLocation /*Loc*/, const clang::Token &tok,
               const clang::MacroDefinition &md) override {
    record_macro_dependency(tok, md, tok.getLocation());
    cond_.next_branch();
  }
  void Elifndef(clang::SourceLocation /*Loc*/, const clang::Token &tok,
                const clang::MacroDefinition &md) override {
    record_macro_dependency(tok, md, tok.getLocation());
    cond_.next_branch();
  }
  void Else(clang::SourceLocation /*Loc*/,
            clang::SourceLocation /*IfLoc*/) override {
    cond_.next_branch();
  }
  void Endif(clang::SourceLocation /*Loc*/,
             clang::SourceLocation /*IfLoc*/) override {
    cond_.pop();
  }

private:
  // Record that the token `tok` (a macro name), used at `use_loc`, depends on
  // the header that DEFINES it -- whether the dependency is an expansion or a
  // definedness test. Deduped so a macro used thousands of times is one fact.
  void record_macro_dependency(const clang::Token &tok,
                               const clang::MacroDefinition &md,
                               clang::SourceLocation use_loc) {
    const clang::MacroInfo *mi = md.getMacroInfo();
    if (mi == nullptr || mi->isBuiltinMacro()) {
      return; // undefined (#ifndef of a never-defined guard) or builtin: no
              // header supplies it
    }
    const clang::SourceLocation def =
        sm_.getExpansionLoc(mi->getDefinitionLoc());
    const clang::SourceLocation use = sm_.getExpansionLoc(use_loc);
    if (def.isInvalid() || use.isInvalid()) {
      return;
    }
    const clang::OptionalFileEntryRef def_fe =
        sm_.getFileEntryRefForID(sm_.getFileID(def));
    const clang::OptionalFileEntryRef use_fe =
        sm_.getFileEntryRefForID(sm_.getFileID(use));
    if (!def_fe || !use_fe) {
      return; // command-line -D or builtin: no header supplies it
    }
    MacroUseFact m;
    m.src_path = use_fe->getName().str();
    m.def_path = def_fe->getName().str();
    if (m.src_path == m.def_path) {
      return; // self-supplied: no include depends on it
    }
    m.name = (tok.getIdentifierInfo() != nullptr)
                 ? tok.getIdentifierInfo()->getName().str()
                 : std::string();
    if (m.name.empty()) {
      return;
    }
    if (seen_macro_uses_.insert(m.src_path + "\0" + m.def_path + "\0" + m.name)
            .second) {
      out_.macro_uses.push_back(std::move(m));
    }
  }

  // The bytes `apply` would delete: the whole directive line including its
  // newline and any backslash-continued lines. Falls back to the directive's
  // own extent when the line holds anything else.
  void compute_removal_range(clang::FileID fid, clang::SourceLocation hash_loc,
                             clang::CharSourceRange filename_range,
                             IncludeFact &f) const {
    bool invalid = false;
    const llvm::StringRef buf = sm_.getBufferData(fid, &invalid);
    const std::size_t hash_off = sm_.getFileOffset(hash_loc);
    if (invalid) {
      f.begin_offset = static_cast<int64_t>(hash_off);
      f.end_offset = static_cast<int64_t>(hash_off);
      return;
    }
    // End of the filename token is the last thing the directive owns; scan on
    // to the end of the logical line so a trailing `// why` comment travels
    // with the directive it annotates.
    clang::SourceLocation fn_end = filename_range.getEnd();
    std::size_t from = hash_off;
    if (fn_end.isValid() && sm_.getFileID(fn_end) == fid) {
      from = sm_.getFileOffset(fn_end);
    }
    const std::size_t start = line_start(buf, hash_off);
    f.begin_offset = static_cast<int64_t>(
        blank_between(buf, start, hash_off) ? start : hash_off);
    f.end_offset = static_cast<int64_t>(logical_line_end(buf, from));
  }

  clang::SourceManager &sm_;
  IncludeFacts &out_;
  ConditionalTracker cond_;
  std::unordered_set<std::string> seen_macro_uses_;
};

} // namespace

void resolve_include_guards(clang::Preprocessor &pp, IncludeFacts &out) {
  clang::HeaderSearch &hs = pp.getHeaderSearchInfo();
  clang::FileManager &fm = pp.getSourceManager().getFileManager();
  std::unordered_map<std::string, bool> guarded;
  for (IncludeFact &f : out.includes) {
    if (!f.resolved) {
      continue;
    }
    auto it = guarded.find(f.dst_path);
    if (it == guarded.end()) {
      bool g = false;
      if (auto fe = fm.getOptionalFileRef(f.dst_path)) {
        g = hs.isFileMultipleIncludeGuarded(*fe);
      }
      it = guarded.emplace(f.dst_path, g).first;
    }
    f.guarded = it->second;
  }
}

std::string include_config_digest(const IncludeConfig &c) {
  std::string data;
  data += c.driver.value_or("");
  data += '\0';
  data += c.working_dir.value_or("");
  data += '\0';
  data += c.lang_mode.value_or("");
  data += '\0';
  data += c.resource_dir.value_or("");
  data += '\0';
  for (const std::string &a : c.arguments) {
    data += a;
    data += '\0';
  }
  return cidx::sha1_hex(data);
}

void register_include_callbacks(clang::CompilerInstance &ci,
                                IncludeFacts &out) {
  ci.getPreprocessor().addPPCallbacks(
      std::make_unique<IncludeFactRecorder>(ci.getPreprocessor(), out));
}

auto persist_include_facts(cidx::Storage &db, const IncludeFacts &facts,
                           const IncludeConfig &config) -> IncludeFactStats {
  IncludeFactStats stats{.attempted = 4, .inserted_or_updated = 4};
  // Retire every configuration previously recorded for THIS TU, then write the
  // current one. A TU is imported once per file, so exactly one configuration
  // is current; a changed compile command produces a new digest, and the old
  // config -- with its edges, sites, and macro uses -- would otherwise linger
  // indefinitely and feed a stale build world into validation. The delete is
  // scoped to this tu_file_id (cascading to its own rows only), so a shared
  // header's facts recorded under a DIFFERENT TU's configuration are untouched.
  IncludeConfig cfg = config;
  cfg.digest = include_config_digest(cfg);
  const IncludeDeletionStats deletion =
      db.delete_include_configs_for_tu(cfg.tu_file_id);
  stats.deleted = deletion.direct;
  stats.cascade_deleted = deletion.cascade;
  const int64_t config_id = db.add_include_config(cfg);
  const auto normalized = db.include_config_by_id(config_id);
  if (!normalized || !normalized->translation_unit_config_id) {
    throw CidxError("include config has no normalized identity");
  }
  const int64_t normalized_id = *normalized->translation_unit_config_id;

  IncludePersistenceState state{
      .db = db, .stats = stats, .normalized_id = normalized_id};

  for (const IncludeFact &fact : facts.includes) {
    persist_include_fact(state, fact, config_id);
  }
  for (const MacroUseFact &fact : facts.macro_uses) {
    persist_macro_use(state, fact, config_id);
  }
  stats.inserted_or_updated += state.file_applicability.size();
  const std::uint64_t unique_facts =
      4 + state.file_applicability.size() + state.edge_identities.size() +
      state.site_identities.size() + state.macro_identities.size();
  stats.duplicates = stats.attempted - unique_facts;
  return stats;
}

} // namespace cidx::ast
