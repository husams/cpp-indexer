#include "extract/matcher_root_binding.hpp"

#include "clang/ASTMatchers/Dynamic/Diagnostics.h"
#include "clang/ASTMatchers/Dynamic/Parser.h"
#include "llvm/ADT/StringRef.h"

namespace cidx::extract {

namespace {

// Records only the MOST RECENT bind id seen during a parse (overwriting on
// every call): since actOnMatcherExpression fires bottom-up, the last call
// in a single top-level parse is the outermost/root matcher.
class RootBindingSema final
    : public clang::ast_matchers::dynamic::Parser::RegistrySema {
public:
  clang::ast_matchers::dynamic::VariantMatcher actOnMatcherExpression(
      clang::ast_matchers::dynamic::MatcherCtor ctor,
      clang::ast_matchers::dynamic::SourceRange name_range,
      llvm::StringRef bind_id,
      llvm::ArrayRef<clang::ast_matchers::dynamic::ParserValue> args,
      clang::ast_matchers::dynamic::Diagnostics *error) override {
    last_bind_id_ = bind_id.str();
    return RegistrySema::actOnMatcherExpression(ctor, name_range, bind_id, args,
                                                error);
  }

  [[nodiscard]] const std::string &last_bind_id() const {
    return last_bind_id_;
  }

private:
  std::string last_bind_id_;
};

} // namespace

std::string root_binding_of(const std::string &matcher_expression) {
  llvm::StringRef code(matcher_expression);
  clang::ast_matchers::dynamic::Diagnostics diagnostics;
  RootBindingSema sema;
  auto matcher = clang::ast_matchers::dynamic::Parser::parseMatcherExpression(
      code, &sema, &diagnostics);
  if (!matcher) {
    return {};
  }
  return sema.last_bind_id();
}

} // namespace cidx::extract
