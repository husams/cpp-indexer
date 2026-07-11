// ASTConsumer that runs the edge visitor over a translation unit, once per
// target file in order (main file first, then owned headers — mirroring
// cidx's index_edges / index_headers sequencing).
#pragma once

#include "clang/AST/ASTConsumer.h"

#include <string>
#include <vector>

namespace clang {
class ASTContext;
}

namespace cidx::lt {

class EdgeSink;

class EdgeConsumer : public clang::ASTConsumer {
public:
  EdgeConsumer(EdgeSink &sink, std::vector<std::string> target_files);

  void HandleTranslationUnit(clang::ASTContext &context) override;

private:
  EdgeSink &sink_;
  std::vector<std::string> target_files_;
};

} // namespace cidx::lt
