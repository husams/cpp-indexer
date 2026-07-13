// Phase-0 LibTooling AST probe entry point.
//
// Runs the symbol visitor over files from a compile_commands.json and prints
// `<usr>\t<kind>\t<qualified-name>` per persisted decl, to validate that the
// Clang C++ API reproduces cidx's USR/kind identity. Throwaway de-risk tool, not
// the real extraction layer. See wiki pages/planning/cidx-libtooling-ast-layer.
//
//   cidx_lt_probe -p <dir-with-compile_commands.json> <file>

#include "ast/symbol_action.hpp"

#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"

namespace {
llvm::cl::OptionCategory ProbeCategory("cidx-lt-probe options");
} // namespace

int main(int argc, const char **argv) {
  auto parser =
      clang::tooling::CommonOptionsParser::create(argc, argv, ProbeCategory);
  if (!parser) {
    llvm::errs() << parser.takeError();
    return 1;
  }
  clang::tooling::ClangTool tool(parser->getCompilations(),
                                 parser->getSourcePathList());
#ifdef CIDX_LT_RESOURCE_DIR
  // ClangTool resolves builtin headers relative to the tool binary, which is
  // not co-located with a clang install — point it at the build-time LLVM's
  // resource dir instead (the real layer will reuse toolchain.cpp for this).
  tool.appendArgumentsAdjuster(clang::tooling::getInsertArgumentAdjuster(
      {"-resource-dir", CIDX_LT_RESOURCE_DIR},
      clang::tooling::ArgumentInsertPosition::BEGIN));
#endif
  return tool.run(
      clang::tooling::newFrontendActionFactory<cidx::lt::SymbolAction>().get());
}
