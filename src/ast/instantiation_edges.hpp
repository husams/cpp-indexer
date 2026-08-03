// Callable template-specialization identity, shared by the declaration pass
// and the call path (the "B3 instantiates" family grown into the one canonical
// handler).
//
// A FunctionDecl that specializes or instantiates a template has ONE identity
// regardless of where it is first seen: symbol flags come from its
// TemplateSpecializationKind, template arguments from
// getTemplateSpecializationArgs(), the structural edge to its primary is
// specializes(4) for explicit specializations and instantiates(5) for
// instantiations, and emission is idempotent so a later call site never
// duplicates or re-counts what the declaration already recorded.
#pragma once

#include "clang/AST/Type.h"
#include "clang/Basic/Specifiers.h"
#include "llvm/ADT/STLFunctionalExtras.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace clang {
class ASTContext;
class ClassTemplateDecl;
class ClassTemplateSpecializationDecl;
class CXXMethodDecl;
class FunctionDecl;
class FunctionTemplateDecl;
class NamedDecl;
} // namespace clang

namespace cidx::ast {

class DeclarationIdentityResolver;
class RelationFactEmitter;
class MintBuilder;
class PresentationIntentEmitter;
class TemplateArgumentEncoder;

// Identity of a callable that specializes/instantiates a template: the primary
// it relates to (its function template) and how, from
// getTemplateSpecializationKind(). nullopt for ordinary callables and for
// non-templated members of class-template specializations.
struct CallableTemplateInfo {
  const clang::NamedDecl *primary = nullptr;
  clang::TemplateSpecializationKind tsk = clang::TSK_Undeclared;
  bool is_instantiation = false; // implicit or explicit instantiation
};
std::optional<CallableTemplateInfo>
callable_template_info(const clang::FunctionDecl *fd);

// Emit the declaration-owned identity of callable `fd` (minted as `dst_id`):
//   - fd -> primary structural edge, specializes(4) or instantiates(5),
//     idempotent (minting a stub when the primary is outside this batch);
//   - template_arg rows from the FULL specialization argument list, optionally
//     overlaid with the as-written types `written` where positions align;
//   - a typed presentation intent carrying the encoded argument literals;
//   - method_of(9) owner promotion for member-function templates, including
//     the minted class-template-specialization owner with its own identity.
// Safe to call from both the declaration pass and every call site.
void emit_callable_template_identity(
    DeclarationIdentityResolver &identity, RelationFactEmitter &relations,
    PresentationIntentEmitter *presentation_intents, MintBuilder &mint,
    const TemplateArgumentEncoder &targ_encoder, int64_t dst_id,
    const clang::FunctionDecl *fd, const CallableTemplateInfo &info,
    const std::vector<clang::QualType> &written);

// Emit method_of(9) for a concrete callable member. Ordinary members of a
// class-template specialization use this path without becoming callable
// template instantiations themselves.
void emit_method_owner(DeclarationIdentityResolver &identity,
                       RelationFactEmitter &relations, MintBuilder &mint,
                       const TemplateArgumentEncoder &targ_encoder,
                       int64_t dst_id, const clang::CXXMethodDecl *method);

// An instantiated record is never traversed, so its fields (which carry the
// substituted types) are minted here: each field of the instantiation gets its
// own symbol (qual_name "X<int>::f", concrete type_info) plus a field_of(8)
// edge back to the instance record `inst_id`, mirroring how instance methods
// get method_of. No-op for authored (non-instantiation) specializations, whose
// fields the ordinary traversal already records.
void emit_instance_fields(DeclarationIdentityResolver &identity,
                          RelationFactEmitter &relations,
                          const MintBuilder &mint,
                          const clang::ClassTemplateSpecializationDecl *spec,
                          int64_t inst_id);

// The TSK names an explicit instantiation (`template ...;` / `extern
// template ...;`).
bool is_explicit_instantiation_kind(clang::TemplateSpecializationKind tsk);

// Explicit callable instantiations never appear as lexical decls; they live
// only in specialization lists. Enumerate them for one template:
//   - a function template's explicitly instantiated specializations;
//   - for a class template, the explicitly instantiated ORDINARY members of
//     each of its specializations, plus explicit instantiations of member
//     function templates (`template int Gadget<char>::conv<long>(long);`).
// Callers gate each decl by its getPointOfInstantiation() — the statement
// that owns it — not by the template's own location.
void for_each_explicit_callable_instantiation(
    const clang::FunctionTemplateDecl *tmpl,
    llvm::function_ref<void(const clang::FunctionDecl *)> fn);
void for_each_explicit_callable_instantiation(
    const clang::ClassTemplateDecl *tmpl,
    llvm::function_ref<void(const clang::FunctionDecl *)> fn);

} // namespace cidx::ast
