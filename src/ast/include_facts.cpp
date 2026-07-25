#include "ast/include_facts.hpp"

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

void persist_include_facts(cidx::Storage &db, const IncludeFacts &facts,
                           const IncludeConfig &config) {
  // Retire every configuration previously recorded for THIS TU, then write the
  // current one. A TU is imported once per file, so exactly one configuration
  // is current; a changed compile command produces a new digest, and the old
  // config -- with its edges, sites, and macro uses -- would otherwise linger
  // indefinitely and feed a stale build world into validation. The delete is
  // scoped to this tu_file_id (cascading to its own rows only), so a shared
  // header's facts recorded under a DIFFERENT TU's configuration are untouched.
  IncludeConfig cfg = config;
  cfg.digest = include_config_digest(cfg);
  db.delete_include_configs_for_tu(cfg.tu_file_id);
  const int64_t config_id = db.add_include_config(cfg);
  const auto normalized = db.include_config_by_id(config_id);
  if (!normalized || !normalized->translation_unit_config_id) {
    throw CidxError("include config has no normalized identity");
  }
  const int64_t normalized_id = *normalized->translation_unit_config_id;

  // Resolve each source file to its row once. A directive in a file no
  // component owns has nothing to hang off (and could never be edited), so it
  // is dropped rather than silently attributed elsewhere.
  std::unordered_map<std::string, std::optional<int64_t>> src_ids;
  const auto src_id_for =
      [&](const std::string &path) -> std::optional<int64_t> {
    const std::string abs = cidx::pathutil::abspath(path);
    auto it = src_ids.find(abs);
    if (it != src_ids.end()) {
      return it->second;
    }
    std::optional<int64_t> id;
    if (const std::optional<cidx::File> f = db.get_file(abs)) {
      id = f->id;
    }
    src_ids.emplace(abs, id);
    return id;
  };

  std::unordered_map<std::string, std::optional<int64_t>> dst_ids;
  const auto dst_id_for =
      [&](const std::string &path) -> std::optional<int64_t> {
    if (path.empty()) {
      return std::nullopt;
    }
    const std::string abs = cidx::pathutil::abspath(path);
    auto it = dst_ids.find(abs);
    if (it != dst_ids.end()) {
      return it->second;
    }
    std::optional<int64_t> id;
    if (const std::optional<cidx::File> f = db.get_file(abs)) {
      id = f->id;
    }
    dst_ids.emplace(abs, id);
    return id;
  };

  for (const IncludeFact &f : facts.includes) {
    if (f.src_path.empty()) {
      continue; // -include from the command line: no editable directive
    }
    const std::optional<int64_t> sid = src_id_for(f.src_path);
    if (!sid) {
      continue;
    }
    record_header_applicability(db, sid, normalized_id);

    IncludeEdge e;
    e.src_file_id = *sid;
    e.dst_path = f.resolved ? cidx::pathutil::abspath(f.dst_path) : f.spelling;
    e.dst_file_id = f.resolved ? dst_id_for(f.dst_path) : std::nullopt;
    record_header_applicability(db, e.dst_file_id, normalized_id);
    e.config_id = config_id;
    e.is_system = f.is_system;
    e.count = 1;
    const int64_t eid = db.add_include_edge(e);

    IncludeSite s;
    s.edge_id = eid;
    s.line = f.line;
    s.col = f.col;
    s.begin_offset = f.begin_offset;
    s.end_offset = f.end_offset;
    s.spelling = f.spelling;
    s.is_angled = f.is_angled;
    s.directive = f.directive;
    s.cond_fingerprint = f.cond_fingerprint;
    s.resolved = f.resolved;
    s.guarded = f.guarded;
    db.add_include_site(s);
  }

  for (const MacroUseFact &m : facts.macro_uses) {
    const std::optional<int64_t> sid = src_id_for(m.src_path);
    if (!sid) {
      continue;
    }
    IncludeMacroUse u;
    u.src_file_id = *sid;
    u.def_path = cidx::pathutil::abspath(m.def_path);
    u.name = m.name;
    u.config_id = config_id;
    u.count = 1;
    db.add_include_macro_use(u);
  }
}

auto include_fact_count(cidx::Storage &db, const IncludeFacts &facts)
    -> IncludeFactCounts {
  // These are the four distinct identities created by add_include_config:
  // normalized config, include config, translation-unit applicability, and TU
  // file applicability. The remaining identities mirror every upsert in
  // persist_include_facts so repeated directives are counted once and exposed
  // as duplicate attempts rather than over-budgeting the pass.
  constexpr std::size_t config_facts = 4;
  std::size_t attempted = config_facts;
  std::set<int64_t> file_applicability;
  std::set<std::tuple<int64_t, std::string>> edges;
  std::set<std::tuple<int64_t, std::string, int64_t>> sites;
  std::set<std::tuple<int64_t, std::string, std::string>> macro_uses;

  const auto record = [&attempted](auto &identities, auto identity) {
    ++attempted;
    return identities.emplace(std::move(identity)).second;
  };

  const auto file_id_for = [&db](const std::string &path) {
    if (path.empty()) {
      return std::optional<int64_t>{};
    }
    return db.get_file(cidx::pathutil::abspath(path))
        .transform([](const cidx::File &file) { return file.id; });
  };
  for (const IncludeFact &fact : facts.includes) {
    const auto source_id = file_id_for(fact.src_path);
    if (!source_id) {
      continue;
    }
    record(file_applicability, *source_id);
    const std::string dst_path =
        fact.resolved ? cidx::pathutil::abspath(fact.dst_path) : fact.spelling;
    if (fact.resolved) {
      if (const auto destination_id = file_id_for(fact.dst_path)) {
        record(file_applicability, *destination_id);
      }
    }
    const auto edge_identity = std::make_tuple(*source_id, dst_path);
    record(edges, edge_identity);
    record(sites, std::make_tuple(*source_id, dst_path, fact.begin_offset));
  }
  for (const MacroUseFact &fact : facts.macro_uses) {
    if (const auto source_id = file_id_for(fact.src_path)) {
      record(macro_uses,
             std::make_tuple(*source_id, cidx::pathutil::abspath(fact.def_path),
                             fact.name));
    }
  }

  const std::size_t unique_facts = config_facts + file_applicability.size() +
                                   edges.size() + sites.size() +
                                   macro_uses.size();
  return {.emitted_facts = unique_facts,
          .duplicates = attempted - unique_facts};
}

} // namespace cidx::ast
