// Definition pass (B2): finds function-like definitions and creates their
// definition rows. Statement extraction is a separate registered pass.
#pragma once

#include "clang/AST/RecursiveASTVisitor.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace clang {
class ASTContext;
class FunctionDecl;
class NamedDecl;
} // namespace clang

namespace cidx::ast {

class DeclarationIdentityResolver;
class DefinitionScopeEmitter;
class StatementFactPorts;
struct PassMetrics;

class FunctionDefinitionVisitor
    : public clang::RecursiveASTVisitor<FunctionDefinitionVisitor> {
public:
  FunctionDefinitionVisitor(clang::ASTContext &context,
                            DeclarationIdentityResolver &identity,
                            DefinitionScopeEmitter &definitions,
                            std::string target_file, int64_t file_id);

  bool VisitFunctionDecl(clang::FunctionDecl *decl);
  auto run_statement_pass(StatementFactPorts &ports,
                          PassMetrics *metrics = nullptr) -> void;
  [[nodiscard]] auto definition_count() const -> std::size_t {
    return definitions_found_.size();
  }
  [[nodiscard]] auto file_id() const -> std::int64_t { return file_id_; }

private:
  bool is_indexable_definition(const clang::FunctionDecl *decl) const;
  void index_definition(clang::FunctionDecl *decl,
                        const clang::NamedDecl *keyed, int64_t fn_sym);

  struct DefinitionFact {
    clang::FunctionDecl *decl = nullptr;
    int64_t symbol_id = 0;
    int64_t definition_id = 0;
  };

  clang::ASTContext &context_;
  DeclarationIdentityResolver &identity_;
  DefinitionScopeEmitter &definitions_;
  std::string target_file_;
  int64_t file_id_;
  std::vector<DefinitionFact> definitions_found_;
};

} // namespace cidx::ast
