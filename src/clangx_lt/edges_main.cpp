// cidx_lt_edges — Phase-2a LibTooling edge-pass tool.
//
// Writes DECLARATION-LEVEL edges (contains/inherits/field_of/method_of/
// overrides/friend/specializes/instantiates + template_param/template_arg)
// into an existing index.db whose symbol table was populated by
// `cidx index --no-graph` (Phase-1-parity symbols). Body/uses edges are a
// later phase. See wiki pages/planning/cidx-libtooling-ast-layer.
//
//   cidx_lt_edges --db <index.db> --target <abs-file> [--target ...] \
//                 -p <compile-db-dir> <main-source>
//
// Targets run in order (main file, then owned headers) to mirror cidx's
// per-file edge sequencing.

#include "clangx_lt/edge_action.hpp"
#include "clangx_lt/storage_edge_sink.hpp"

#include "storage/storage.hpp"

#include "clang/Frontend/FrontendAction.h"
#include "clang/Tooling/ArgumentsAdjusters.h"
#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"

namespace {

llvm::cl::OptionCategory EdgesCategory("cidx-lt-edges options");
llvm::cl::opt<std::string> DbPath("db", llvm::cl::Required,
                                  llvm::cl::desc("index.db to write edges to"),
                                  llvm::cl::cat(EdgesCategory));
llvm::cl::list<std::string>
    Targets("target", llvm::cl::OneOrMore,
            llvm::cl::desc("absolute file path to walk (repeatable, in order)"),
            llvm::cl::cat(EdgesCategory));

class EdgeActionFactory : public clang::tooling::FrontendActionFactory {
public:
  EdgeActionFactory(cidx::lt::EdgeSink &sink,
                    std::vector<std::string> targets)
      : sink_(sink), targets_(std::move(targets)) {}

  std::unique_ptr<clang::FrontendAction> create() override {
    return std::make_unique<cidx::lt::EdgeAction>(sink_, targets_);
  }

private:
  cidx::lt::EdgeSink &sink_;
  std::vector<std::string> targets_;
};

} // namespace

int main(int argc, const char **argv) {
  auto parser =
      clang::tooling::CommonOptionsParser::create(argc, argv, EdgesCategory);
  if (!parser) {
    llvm::errs() << parser.takeError();
    return 1;
  }

  cidx::Storage db(DbPath);
  cidx::lt::StorageEdgeSink sink(db);
  std::vector<std::string> targets(Targets.begin(), Targets.end());

  clang::tooling::ClangTool tool(parser->getCompilations(),
                                 parser->getSourcePathList());
#ifdef CIDX_LT_RESOURCE_DIR
  tool.appendArgumentsAdjuster(clang::tooling::getInsertArgumentAdjuster(
      {"-resource-dir", CIDX_LT_RESOURCE_DIR},
      clang::tooling::ArgumentInsertPosition::BEGIN));
#endif

  // One transaction around all walks (cidx wraps per-file transactions; a
  // single one is equivalent for a single-TU tool run).
  auto txn = db.transaction();
  EdgeActionFactory factory(sink, std::move(targets));
  const int rc = tool.run(&factory);
  txn.commit();
  return rc;
}
