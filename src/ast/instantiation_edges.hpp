// Instantiation edges for a called template specialization (the "B3
// instantiates" family).
//
// When a resolved, non-recovered call target is a template specialization,
// emit: caller -> primary template (instantiates, lookup-only), the
// specialization -> primary promotion, and for instantiated members the owning
// type's method_of / instantiates / template_arg rows. Split out of
// CallEmitter::emit_resolved_call so the CALLS-edge path stays readable.
#pragma once

#include <cstdint>
#include <string>

namespace clang {
class ASTContext;
class FunctionDecl;
} // namespace clang

namespace cidx::lt {

class EdgeSink;
class MintBuilder;
class TemplateArgumentEncoder;

// Emit the instantiation edge family for a call from `src_id` to the resolved
// target `dst_id` / `callee` (USR `callee_usr`). Only meaningful for a
// non-recovered call whose callee is a template specialization; a no-op
// otherwise.
void emit_instantiation_edges(const clang::ASTContext &context, EdgeSink &sink,
                              MintBuilder &mint,
                              const TemplateArgumentEncoder &targ_encoder,
                              int64_t src_id, int64_t dst_id,
                              const clang::FunctionDecl *callee,
                              const std::string &callee_usr);

} // namespace cidx::lt
