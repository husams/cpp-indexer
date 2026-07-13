// Receiver provenance for a call site (edge_site recv_* columns).
//
// For a member or dependent-member call, classify the receiver expression: its
// value source (this/local/member/global/call_result), the parameter position
// when the receiver is a parameter, and whether it holds the dispatch type by
// value. Pure over the AST — emits nothing; the caller writes the fields onto
// the edge_site record. Split out of BodyWalker::emit_resolved_call.
#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace clang {
class ASTContext;
class Expr;
class FunctionDecl;
} // namespace clang

namespace cidx::lt {

// Receiver fields for an edge_site row (empty/nullopt when not a member call or
// the receiver could not be classified).
struct ReceiverProvenance {
  std::string src_kind;
  std::string type_usr;
  std::string decl_usr;
  std::optional<int64_t> param_pos;
  std::optional<int64_t> type_is_value;
};

// Classify the receiver of the call/construct expression `site` whose resolved
// target is `callee`. Handles explicit member receivers, recovered dependent
// member calls, and the implicit `this` of a method callee.
ReceiverProvenance classify_call_receiver(const clang::ASTContext &context,
                                          const clang::Expr *site,
                                          const clang::FunctionDecl *callee);

} // namespace cidx::lt
