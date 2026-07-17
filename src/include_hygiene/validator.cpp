#include "include_hygiene/validator.hpp"

#include "include_hygiene/graph.hpp"
#include "storage/storage.hpp"
#include "util/pathutil.hpp"

#include "clang/Basic/Diagnostic.h"
#include "clang/Basic/DiagnosticOptions.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendActions.h"
#include "clang/Tooling/ArgumentsAdjusters.h"
#include "clang/Tooling/CompilationDatabase.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/Support/VirtualFileSystem.h"

#include <algorithm>
#include <fstream>
#include <sstream>

namespace cidx::hygiene {

namespace {

// Collects the first error, which is what a reviewer needs to see: the reason
// the removal was refused.
class FirstErrorCollector : public clang::DiagnosticConsumer {
public:
  void HandleDiagnostic(clang::DiagnosticsEngine::Level level,
                        const clang::Diagnostic &info) override {
    clang::DiagnosticConsumer::HandleDiagnostic(level, info);
    if (level < clang::DiagnosticsEngine::Error || !first_.empty()) {
      return;
    }
    llvm::SmallString<256> msg;
    info.FormatDiagnostic(msg);
    std::string where;
    if (info.hasSourceManager() && info.getLocation().isValid()) {
      const clang::SourceManager &sm = info.getSourceManager();
      const clang::SourceLocation loc = sm.getExpansionLoc(info.getLocation());
      where = sm.getFilename(loc).str() + ":" +
              std::to_string(sm.getExpansionLineNumber(loc)) + ": ";
    }
    first_ = where + std::string(msg);
  }

  const std::string &first() const { return first_; }

private:
  std::string first_;
};

std::string read_file(const std::string &path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return {};
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

} // namespace

RemovalValidator::RemovalValidator(cidx::Storage &db) : db_(db) {}

std::string RemovalValidator::apply_removals(const std::string &content,
                                             std::vector<Removal> removals) {
  // Descending by offset: each erase then leaves every EARLIER offset valid.
  // Applying ascending would shift every subsequent range by the bytes already
  // removed -- the classic off-by-N that silently deletes the wrong code.
  std::sort(removals.begin(), removals.end(),
            [](const Removal &a, const Removal &b) {
              return a.begin_offset > b.begin_offset;
            });
  std::string out = content;
  int64_t last_begin = -1;
  for (const Removal &r : removals) {
    if (r.begin_offset < 0 || r.end_offset > static_cast<int64_t>(out.size()) ||
        r.end_offset < r.begin_offset) {
      continue; // out of range for this buffer: the caller's hash check catches it
    }
    if (last_begin >= 0 && r.end_offset > last_begin) {
      continue; // overlaps the previous removal; never delete the same bytes twice
    }
    out.erase(static_cast<std::size_t>(r.begin_offset),
              static_cast<std::size_t>(r.end_offset - r.begin_offset));
    last_begin = r.begin_offset;
  }
  return out;
}

std::optional<std::vector<TuTarget>>
RemovalValidator::affected_tus(const std::string &abs_path) {
  std::vector<TuTarget> out;

  // Candidate roots: the file itself, plus every file whose include closure
  // reaches it. A header edit must hold for all of them.
  const IncludeGraph g = IncludeGraph::load(db_, /*include_system=*/false);
  std::vector<std::string> roots{abs_path};
  for (const std::string &p : g.transitive_to(abs_path, 0)) {
    roots.push_back(p);
  }
  std::sort(roots.begin(), roots.end());
  roots.erase(std::unique(roots.begin(), roots.end()), roots.end());

  bool any_tu = false;
  for (const std::string &root : roots) {
    const std::optional<File> f = db_.get_file(root);
    if (!f) {
      continue;
    }
    // A file is a TU exactly when it owns a recorded configuration. A header
    // has none; it is covered through the TUs that include it.
    for (const IncludeConfig &c : db_.include_configs_for_tu(f->id)) {
      TuTarget t;
      t.tu_path = root;
      t.config_digest = c.digest;
      t.arguments = c.arguments;
      t.working_dir = c.working_dir.value_or(".");
      out.push_back(std::move(t));
      any_tu = true;
    }
  }
  if (!any_tu) {
    // Nothing cidx can compile reaches this file: the reverse closure is
    // incomplete, so no removal here can be proven. Refuse -- do NOT report a
    // vacuous pass.
    return std::nullopt;
  }
  std::sort(out.begin(), out.end(), [](const TuTarget &a, const TuTarget &b) {
    return std::tie(a.tu_path, a.config_digest) <
           std::tie(b.tu_path, b.config_digest);
  });
  return out;
}

std::map<std::string, std::string>
RemovalValidator::overlay_for(const std::vector<Removal> &removals) {
  std::map<std::string, std::vector<Removal>> by_file;
  for (const Removal &r : removals) {
    by_file[r.abs_path].push_back(r);
  }
  std::map<std::string, std::string> out;
  for (const auto &[path, rs] : by_file) {
    out[path] = apply_removals(read_file(path), rs);
  }
  return out;
}

std::vector<ValidationRecord>
RemovalValidator::validate(const std::vector<TuTarget> &targets,
                           const std::map<std::string, std::string> &edited,
                           const std::string &stage) {
  std::vector<ValidationRecord> out;
  for (const TuTarget &t : targets) {
    ValidationRecord rec;
    rec.stage = stage;
    rec.tu_path = t.tu_path;
    rec.config = t.config_digest;

    // An in-memory overlay over the real filesystem: the edit is visible to
    // the compiler and to nothing else. Nothing on disk is touched, so a
    // failed validation cannot leave a broken tree behind.
    llvm::IntrusiveRefCntPtr<llvm::vfs::OverlayFileSystem> overlay(
        new llvm::vfs::OverlayFileSystem(llvm::vfs::getRealFileSystem()));
    llvm::IntrusiveRefCntPtr<llvm::vfs::InMemoryFileSystem> mem(
        new llvm::vfs::InMemoryFileSystem);
    overlay->pushOverlay(mem);
    for (const auto &[path, content] : edited) {
      mem->addFile(path, 0, llvm::MemoryBuffer::getMemBufferCopy(content, path));
    }

    clang::tooling::FixedCompilationDatabase cdb(t.working_dir, t.arguments);
    clang::tooling::ClangTool tool(
        cdb, {t.tu_path}, std::make_shared<clang::PCHContainerOperations>(),
        overlay);
#ifdef CIDX_CLANG_RESOURCE_DIR
    tool.appendArgumentsAdjuster(clang::tooling::getInsertArgumentAdjuster(
        {"-resource-dir", CIDX_CLANG_RESOURCE_DIR},
        clang::tooling::ArgumentInsertPosition::BEGIN));
#endif
    FirstErrorCollector diags;
    tool.setDiagnosticConsumer(&diags);

    // Syntax-only: this gate asks "does it still compile?", so there is no
    // reason to pay for codegen.
    clang::tooling::ClangTool &t_ref = tool;
    std::unique_ptr<clang::tooling::FrontendActionFactory> factory =
        clang::tooling::newFrontendActionFactory<clang::SyntaxOnlyAction>();
    const int rc = t_ref.run(factory.get());

    rec.ok = rc == 0 && diags.first().empty();
    if (!rec.ok) {
      rec.diagnostic = diags.first().empty()
                           ? "compilation failed (no diagnostic captured)"
                           : diags.first();
    }
    out.push_back(std::move(rec));
  }
  return out;
}

bool RemovalValidator::all_ok(const std::vector<ValidationRecord> &records) {
  return std::all_of(records.begin(), records.end(),
                     [](const ValidationRecord &r) { return r.ok; });
}

} // namespace cidx::hygiene
