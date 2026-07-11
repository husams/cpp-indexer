// cidx_lt_index — interleaved LibTooling indexer (Phase-2 gate tool).
//
// Runs the full symbol+edge sequence over one TU, writing through the real
// cidx::Storage, with AstIndexer's exact per-file ordering (symbols/edges for
// the main file, then the two-pass header sequence). The DB must already
// carry the file/directory rows (a prior `cidx index --no-graph` run whose
// symbol rows were cleared).
//
//   cidx_lt_index --db <index.db> --target <abs-file> [--target ...] \
//                 -p <compile-db-dir> <main-source>

#include "clangx_lt/index_consumer.hpp"
#include "clangx_lt/storage_edge_sink.hpp"
#include "clangx_lt/storage_symbol_sink.hpp"

#include "storage/storage.hpp"

#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendAction.h"
#include "clang/Tooling/ArgumentsAdjusters.h"
#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"

namespace {

llvm::cl::OptionCategory IndexCategory("cidx-lt-index options");
llvm::cl::opt<std::string> DbPath("db", llvm::cl::Required,
                                  llvm::cl::desc("index.db to write to"),
                                  llvm::cl::cat(IndexCategory));
llvm::cl::list<std::string>
    Targets("target", llvm::cl::OneOrMore,
            llvm::cl::desc("absolute file path (main first, then headers)"),
            llvm::cl::cat(IndexCategory));

class IndexAction : public clang::ASTFrontendAction {
public:
  IndexAction(cidx::lt::StorageSymbolSink &symbols, cidx::lt::EdgeSink &edges,
              std::vector<std::pair<std::string, int64_t>> targets)
      : symbols_(symbols), edges_(edges), targets_(std::move(targets)) {}

  std::unique_ptr<clang::ASTConsumer>
  CreateASTConsumer(clang::CompilerInstance & /*ci*/,
                    llvm::StringRef /*file*/) override {
    return std::make_unique<cidx::lt::IndexConsumer>(symbols_, edges_,
                                                     targets_);
  }

private:
  cidx::lt::StorageSymbolSink &symbols_;
  cidx::lt::EdgeSink &edges_;
  std::vector<std::pair<std::string, int64_t>> targets_;
};

class IndexActionFactory : public clang::tooling::FrontendActionFactory {
public:
  IndexActionFactory(cidx::lt::StorageSymbolSink &symbols,
                     cidx::lt::EdgeSink &edges,
                     std::vector<std::pair<std::string, int64_t>> targets)
      : symbols_(symbols), edges_(edges), targets_(std::move(targets)) {}

  std::unique_ptr<clang::FrontendAction> create() override {
    return std::make_unique<IndexAction>(symbols_, edges_, targets_);
  }

private:
  cidx::lt::StorageSymbolSink &symbols_;
  cidx::lt::EdgeSink &edges_;
  std::vector<std::pair<std::string, int64_t>> targets_;
};

} // namespace

int main(int argc, const char **argv) {
  auto parser =
      clang::tooling::CommonOptionsParser::create(argc, argv, IndexCategory);
  if (!parser) {
    llvm::errs() << parser.takeError();
    return 1;
  }

  cidx::Storage db(DbPath);
  cidx::lt::StorageSymbolSink symbols(db);
  cidx::lt::StorageEdgeSink edges(db);

  // Resolve each target's file_id up front (rows created by the prep run).
  std::vector<std::pair<std::string, int64_t>> targets;
  for (const std::string &t : Targets) {
    const auto row = db.get_file(t);
    if (!row) {
      llvm::errs() << "cidx_lt_index: no file row for " << t << "\n";
      return 1;
    }
    targets.emplace_back(t, row->id);
  }

  clang::tooling::ClangTool tool(parser->getCompilations(),
                                 parser->getSourcePathList());
#ifdef CIDX_LT_RESOURCE_DIR
  tool.appendArgumentsAdjuster(clang::tooling::getInsertArgumentAdjuster(
      {"-resource-dir", CIDX_LT_RESOURCE_DIR},
      clang::tooling::ArgumentInsertPosition::BEGIN));
#endif

  auto txn = db.transaction();
  IndexActionFactory factory(symbols, edges, std::move(targets));
  const int rc = tool.run(&factory);
  txn.commit();
  return rc;
}
