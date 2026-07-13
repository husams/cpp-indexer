// Template-argument emission for a called template specialization.
//
// When a resolved call target is an instantiation member (function-template
// specialization or member of a class-template instantiation), libclang's
// cursor API exposes its template arguments. This reproduces that emission:
//   - free-function specializations mirror the full cursor-API arg list and
//     rewrite the callable's display name;
//   - method specializations fall back to the explicit `<...>` args written at
//     the call site.
// Split out of CallEmitter::emit_resolved_call so that call orchestration stays
// separate from the argument-record shaping.
#pragma once

#include <cstdint>

namespace clang {
class ASTContext;
class Expr;
class FunctionDecl;
} // namespace clang

namespace cidx::lt {

class EdgeSink;
class TemplateArgResolver;

// Emit template_arg rows for `callee` (the resolved call target, minted as
// `dst_id`) when it is an instantiation member. `site` is the call/construct
// expression, used to recover explicit method template arguments. A no-op when
// `dst_id` is negative or `callee` is not an instantiation member.
void emit_callable_template_args(clang::ASTContext &context, EdgeSink &sink,
                                 const TemplateArgResolver &resolver,
                                 const clang::FunctionDecl *callee,
                                 const clang::Expr *site, int64_t dst_id);

} // namespace cidx::lt
