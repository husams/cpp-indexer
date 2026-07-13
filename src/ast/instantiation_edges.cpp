#include "ast/instantiation_edges.hpp"

#include "ast/edge_records.hpp"
#include "ast/edge_sink.hpp"
#include "ast/mint_builder.hpp"
#include "ast/template_arg_resolver.hpp"
#include "ast/usr.hpp"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclTemplate.h"

#include <string>

namespace cidx::lt {

void emit_instantiation_edges(const clang::ASTContext &context, EdgeSink &sink,
                              MintBuilder &mint,
                              const TemplateArgResolver &resolver,
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
  const auto *m = llvm::dyn_cast<clang::CXXMethodDecl>(callee);
  if (m == nullptr)
    return;
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
  const clang::PrintingPolicy &policy = context.getPrintingPolicy();
  for (unsigned ai = 0; ai < args.size(); ++ai) {
    if (args[ai].getKind() != clang::TemplateArgument::Type)
      continue;
    TemplateArgRecord ta;
    ta.owner_id = type_id;
    ta.position = static_cast<int64_t>(ai);
    ta.arg_kind = 1;
    const std::string sp = args[ai].getAsType().getAsString(policy);
    if (!sp.empty())
      ta.literal = sp;
    if (const clang::TagDecl *td = args[ai].getAsType()->getAsTagDecl()) {
      const std::string ru = usr_for_decl(td);
      if (!ru.empty())
        ta.ref_id = sink.lookup_symbol_id(ru);
    }
    if (!ta.ref_id)
      ta.ref_id = resolver.resolve(ta.literal, ospec);
    sink.add_template_arg(ta);
  }
}

} // namespace cidx::lt
