// Definition pass (B2): finds function-like definitions and creates their
// definition rows. Statement extraction is a separate registered pass.
#pragma once

#include "clang/AST/RecursiveASTVisitor.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace clang {
class ASTContext;
class Decl;
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
  using FileRouter =
      std::function<std::optional<std::int64_t>(const std::string &)>;

  struct PendingBody {
    const clang::FunctionDecl *canonical_owner = nullptr;
    clang::FunctionDecl *definition = nullptr;
    std::int64_t symbol_id = 0;
    std::int64_t definition_id = 0;
    std::int64_t file_id = -1;
    std::string file;
  };

  class PendingBodyQueue {
  public:
    auto contains(const clang::FunctionDecl *canonical_owner) const -> bool {
      return seen_.contains(canonical_owner);
    }

    auto schedule(PendingBody body) -> bool {
      if (body.canonical_owner == nullptr ||
          !seen_.insert(body.canonical_owner).second) {
        return false;
      }
      ordered_.push_back(std::move(body));
      return true;
    }

    template <typename Consumer> void drain(Consumer &&consume) {
      for (const PendingBody &body : ordered_) {
        consume(body);
      }
      ordered_.clear();
      seen_.clear();
    }

  private:
    std::vector<PendingBody> ordered_;
    std::unordered_set<const clang::FunctionDecl *> seen_;
  };

  FunctionDefinitionVisitor(clang::ASTContext &context,
                            DeclarationIdentityResolver &identity,
                            DefinitionScopeEmitter &definitions,
                            std::string target_file, int64_t file_id,
                            PassMetrics *metrics = nullptr,
                            FileRouter router = {});

  bool VisitDecl(clang::Decl *decl);
  bool VisitFunctionDecl(clang::FunctionDecl *decl);
  auto
  run_statement_pass(StatementFactPorts &ports, PassMetrics *metrics = nullptr,
                     DefinitionScopeEmitter *statement_definitions = nullptr)
      -> void;
  [[nodiscard]] auto definition_count() const -> std::size_t {
    return definition_count_;
  }
  [[nodiscard]] auto statement_body_count() const -> std::size_t {
    return statement_body_count_;
  }
  [[nodiscard]] auto file_id() const -> std::int64_t { return file_id_; }

private:
  bool is_indexable_definition(const clang::FunctionDecl *decl);
  void index_definition(clang::FunctionDecl *decl,
                        const clang::NamedDecl *keyed, int64_t fn_sym,
                        const clang::FunctionDecl *canonical_owner);

  clang::ASTContext &context_;
  DeclarationIdentityResolver &identity_;
  DefinitionScopeEmitter &definitions_;
  std::string target_file_;
  int64_t file_id_;
  std::size_t definition_count_ = 0;
  std::size_t statement_body_count_ = 0;
  PendingBodyQueue pending_bodies_;
  PassMetrics *metrics_ = nullptr;
  FileRouter router_;
};

} // namespace cidx::ast
