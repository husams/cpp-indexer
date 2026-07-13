#include "ast/instantiation_edges.hpp"

#include "ast/edge_records.hpp"
#include "ast/edge_sink.hpp"
#include "ast/mint_builder.hpp"
#include "ast/template_argument_encoder.hpp"
#include "ast/usr.hpp"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclTemplate.h"

#include <string>

namespace cidx::lt {

namespace {

// Class-template-specialization owner: mint the instantiation owner, its
// method_of(9) edge, the owner -> primary instantiates(5) edge, and the
// owner's template_arg rows through the one canonical encoder.
void emit_spec_owner(EdgeSink &sink, MintBuilder &mint,
                     const TemplateArgumentEncoder &targ_encoder,
                     int64_t dst_id,
                     const clang::ClassTemplateSpecializationDecl *ospec) {
  const clang::ClassTemplateDecl *cls_prim = ospec->getSpecializedTemplate();
  auto oreq = mint.build(ospec);
  if (!oreq || cls_prim == nullptr)
    return;
  oreq->is_instantiation = true;
  const int64_t type_id = sink.mint_symbol(*oreq);
  EdgeRecord mo;
  mo.src_id = dst_id;
  mo.dst_id = type_id;
  mo.kind = 9;
  sink.add_edge(mo);
  const std::string cp = usr_for_decl(cls_prim);
  if (!cp.empty())
    if (const auto cpid = sink.lookup_symbol_id(cp)) {
      EdgeRecord ie;
      ie.src_id = type_id;
      ie.dst_id = *cpid;
      ie.kind = 5;
      sink.add_edge(ie);
    }
  const clang::TemplateArgumentList &args = ospec->getTemplateArgs();
  for (unsigned ai = 0; ai < args.size(); ++ai)
    targ_encoder.emit(type_id, static_cast<int64_t>(ai), args[ai]);
}

// Owner-type promotion for an instantiated member: method_of(9) to the owning
// record, and — when the owner is itself a class-template specialization — a
// minted instantiation owner with its instantiates(5) edge and template_arg
// rows through the one canonical encoder (all argument kinds).
void emit_owner_promotion(EdgeSink &sink, MintBuilder &mint,
                          const TemplateArgumentEncoder &targ_encoder,
                          int64_t dst_id, const clang::CXXMethodDecl *m) {
  const clang::CXXRecordDecl *owner = m->getParent();
  if (owner == nullptr)
    return;
  const auto *ospec =
      llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(owner);
  if (ospec == nullptr) {
    const std::string ou = usr_for_decl(owner);
    if (!ou.empty())
      if (const auto oid = sink.lookup_symbol_id(ou)) {
        EdgeRecord mo;
        mo.src_id = dst_id;
        mo.dst_id = *oid;
        mo.kind = 9;
        sink.add_edge(mo);
      }
    return;
  }
  emit_spec_owner(sink, mint, targ_encoder, dst_id, ospec);
}

} // namespace

void emit_instantiation_edges(const clang::ASTContext &context, EdgeSink &sink,
                              MintBuilder &mint,
                              const TemplateArgumentEncoder &targ_encoder,
                              int64_t src_id, int64_t dst_id,
                              const clang::FunctionDecl *callee,
                              const std::string &callee_usr) {
  // B3 instantiates: callee is a template specialization -> caller -> primary
  // template (lookup-only) + instantiation-member promotion.
  const clang::NamedDecl *primary = nullptr;
  if (const clang::FunctionTemplateDecl *ft = callee->getPrimaryTemplate())
    primary = ft;
  else if (callee->getMemberSpecializationInfo() != nullptr)
    primary = llvm::dyn_cast_or_null<clang::NamedDecl>(
        callee->getMemberSpecializationInfo()->getInstantiatedFrom());
  if (primary == nullptr)
    return;
  const std::string prim_usr = usr_for_decl(primary);
  if (prim_usr.empty() || prim_usr == callee_usr)
    return;
  const auto prim = sink.lookup_symbol_id(prim_usr);
  if (!prim)
    return;

  EdgeRecord inst;
  inst.src_id = src_id;
  inst.dst_id = *prim;
  inst.kind = 5;
  sink.add_edge(inst);

  // mint_instantiation_nodes: callable spec -> primary; owner type promotion
  // for instantiated members.
  EdgeRecord ib;
  ib.src_id = dst_id;
  ib.dst_id = *prim;
  ib.kind = 5;
  sink.add_edge(ib);
  if (const auto *m = llvm::dyn_cast<clang::CXXMethodDecl>(callee))
    emit_owner_promotion(sink, mint, targ_encoder, dst_id, m);
}

} // namespace cidx::lt
