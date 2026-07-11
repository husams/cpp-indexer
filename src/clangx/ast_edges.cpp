#include "clangx/ast.hpp"
#include "clangx/ast_internal.hpp"

#include <optional>
#include <string>
#include <vector>

#include "clangx/clang_raii.hpp"

namespace cidx {
using namespace ast_detail;

// M4: txn-free inner work — caller MUST own an open transaction.
// graph_enabled_ check and edge deletion are ALSO done by caller (index_edges).
void AstIndexer::index_edges_notxn(const ParsedTu &tu,
                                   const std::string &filename,
                                   int64_t file_id) {

  // B1: declaration-level edges. Use the parent-aware walk so that
  // CXX_BASE_SPECIFIER handlers can get the enclosing record from the walk
  // parent (spec §1.4: semantic_parent and lexical_parent are both NULL on
  // that cursor kind — probed in geometry.hpp Circle:Shape).
  for_file_cursors_p(tu, filename, [&](CXCursor cursor, CXCursor walk_parent) {
    const CXCursorKind ck = ::clang_getCursorKind(cursor);

    // -- contains (kind=3): namespace/record → child symbol ---------------
    // Emitted FIRST so it fires regardless of which specific handler runs
    // below (each handler may early-return before reaching the end).
    // src = the enclosing namespace or record; dst = this cursor.
    // Covers: NAMESPACE_DECL → any indexed child,
    //         record/class_template → nested type/enum/typedef/union.
    // Does NOT duplicate field_of (members) or method_of (methods) —
    // those emit child→parent, while contains emits parent→child.
    {
      const CXCursorKind pk = ::clang_getCursorKind(walk_parent);
      const bool parent_is_ns = (pk == CXCursor_Namespace);
      const bool parent_is_record =
          (pk == CXCursor_ClassDecl || pk == CXCursor_StructDecl ||
           pk == CXCursor_ClassTemplate || pk == CXCursor_UnionDecl);
      // For a namespace parent: any indexable child qualifies.
      // For a record parent: only nested types/enums/typedefs qualify
      //   (fields + methods are covered by field_of/method_of).
      const bool child_is_nested_type =
          (ck == CXCursor_ClassDecl || ck == CXCursor_StructDecl ||
           ck == CXCursor_UnionDecl || ck == CXCursor_EnumDecl ||
           ck == CXCursor_TypedefDecl || ck == CXCursor_TypeAliasDecl ||
           ck == CXCursor_ClassTemplate || ck == CXCursor_FunctionTemplate);
      const bool emit = parent_is_ns || (parent_is_record && child_is_nested_type);
      if (emit) {
        const std::string child_usr =
            CxString(::clang_getCursorUSR(cursor)).str();
        const std::string parent_usr =
            CxString(::clang_getCursorUSR(walk_parent)).str();
        if (!child_usr.empty() && !parent_usr.empty()) {
          const auto child_sym = db_.lookup_symbol(child_usr);
          const auto parent_sym = db_.lookup_symbol(parent_usr);
          if (child_sym && parent_sym) {
            Edge e;
            e.src_id = parent_sym->id;
            e.dst_id = child_sym->id;
            e.kind = 3; // contains
            e.count = 1;
            db_.add_edge(e);
          }
        }
      }
    }

    // -- uses (kind=7): TYPE references in signatures / fields / vars ------
    // A class named only as a parameter, return, field, variable, or
    // typedef-underlying type never appears as a body DECL_REF_EXPR, so the
    // body-descent `uses` pass misses it. Emit those signature-level uses
    // here. Emitted alongside contains (before any handler's early return).
    // Local-variable types inside bodies are handled in body_descent_visitor;
    // the walk here does not descend into bodies.
    if (is_function_like(ck)) {
      const std::string fn_usr =
          CxString(::clang_getCursorUSR(cursor)).str();
      if (!fn_usr.empty()) {
        const auto fn_sym = db_.lookup_symbol(fn_usr);
        if (fn_sym) {
          // return type (constructors/destructors have none worth recording)
          if (ck != CXCursor_Constructor && ck != CXCursor_Destructor) {
            emit_type_use(db_, fn_sym->id,
                          ::clang_getCursorResultType(cursor), file_id,
                          cursor, 0);
          }
          const int nargs = ::clang_Cursor_getNumArguments(cursor);
          for (int ai = 0; ai < nargs; ++ai) {
            const CXCursor arg =
                ::clang_Cursor_getArgument(cursor, static_cast<unsigned>(ai));
            emit_type_use(db_, fn_sym->id, ::clang_getCursorType(arg),
                          file_id, arg, 0);
          }
        }
      }
    } else if (ck == CXCursor_FieldDecl || ck == CXCursor_VarDecl) {
      // FIELD_DECL: field type. VAR_DECL: file-scope variable type (locals are
      // reached via body_descent). src = the field/variable symbol itself.
      //
      // Stage 3/4: a member/file-scope `X<B>` mints the X<B> instance entity
      // (its own composes/aggregates/associates via T->B), exactly like an
      // alias. Minted FIRST -- before the uses-emit below -- so the member
      // reliably gets a structural uses(7) edge -> the X<B> instance (keyed on
      // the spec USR), order-independent within the TU. Stage 4's
      // cpp_materialise_field_relations reads that edge to give the owning
      // record `A composes/associates X<B>` (the un-collapsed instance).
      mint_instance_from_type(db_, ::clang_getCursorType(cursor));
      const std::string sym_usr =
          CxString(::clang_getCursorUSR(cursor)).str();
      if (!sym_usr.empty()) {
        const auto sym = db_.lookup_symbol(sym_usr);
        if (sym) {
          emit_type_use(db_, sym->id, ::clang_getCursorType(cursor),
                        file_id, cursor, 0);
          // v27: an out-of-line static DATA MEMBER definition
          // (`int C::x = ...;`) is a per-backend body -- redefined in each
          // backend. Record its `definition` row (so it counts toward
          // multi_def / "list redefined") and its initializer's calls.
          if (ck == CXCursor_VarDecl &&
              ::clang_isCursorDefinition(cursor) != 0) {
            const CXCursor sp = ::clang_getCursorSemanticParent(cursor);
            const CXCursorKind spk = ::clang_getCursorKind(sp);
            if (spk == CXCursor_ClassDecl || spk == CXCursor_StructDecl ||
                spk == CXCursor_UnionDecl || spk == CXCursor_ClassTemplate) {
              const ExpansionLoc vstart = cursor_extent_start(cursor);
              const ExpansionLoc vend = cursor_extent_end(cursor);
              const int64_t vdef_id = db_.get_or_create_definition(
                  sym->id, file_id, static_cast<int64_t>(vstart.line),
                  static_cast<int64_t>(vstart.col),
                  static_cast<int64_t>(vend.line),
                  static_cast<int64_t>(vend.col),
                  static_var_init_text(cursor));
              emit_static_init_def_edges(db_, cursor, vdef_id);
            }
          }
        }
      }
    } else if (ck == CXCursor_TypedefDecl || ck == CXCursor_TypeAliasDecl) {
      // Stage 2: a named alias of a class-template specialization mints the
      // X<B> instance entity (own composes/aggregates/associates via T->B).
      // Minted FIRST -- before the uses-emit below -- so an alias OF a template
      // instance (`using IntBox = Box<int>;`) reliably gets a structural uses(7)
      // edge -> the X<B> instance (keyed on the spec USR), exactly like a
      // `Box<int> field;` member (FIELD_DECL, above). Without this order the
      // instance is not yet minted when emit runs, so the alias would resolve to
      // no underlying target at all.
      mint_named_instance(db_, cursor);
      const std::string td_usr =
          CxString(::clang_getCursorUSR(cursor)).str();
      if (!td_usr.empty()) {
        const auto td_sym = db_.lookup_symbol(td_usr);
        if (td_sym) {
          emit_type_use(db_, td_sym->id,
                        ::clang_getTypedefDeclUnderlyingType(cursor),
                        file_id, cursor, 0);
        }
      }
    }

    // -- CXX_BASE_SPECIFIER: inherits ----------------------------------
    // Derived class is the enclosing record from the walk parent, NOT from
    // semantic_parent (which is NULL — spec §1.4 gotcha).
    if (ck == CXCursor_CXXBaseSpecifier) {
      // walk_parent is the record cursor that contains this base-specifier; its
      // USR is the derived class.  That record can be a plain CLASS/STRUCT_DECL
      // OR a class template / its partial specialization -- libclang emits the
      // CXX_BASE_SPECIFIER as a direct child of the CLASS_TEMPLATE cursor.
      // Missing the template kinds here dropped `inherits` edges for every
      // class template with a concrete (non-dependent) base
      // (e.g. `template <class T> class D : public Base`).
      const CXCursorKind pk = ::clang_getCursorKind(walk_parent);
      if (pk != CXCursor_ClassDecl && pk != CXCursor_StructDecl &&
          pk != CXCursor_ClassTemplate &&
          pk != CXCursor_ClassTemplatePartialSpecialization) {
        return; // unexpected parent; skip
      }
      const std::string derived_usr =
          CxString(::clang_getCursorUSR(walk_parent)).str();
      if (derived_usr.empty()) {
        return;
      }
      const CXCursor base_ref = ::clang_getCursorReferenced(cursor);
      if (::clang_Cursor_isNull(base_ref)) {
        return;
      }
      const std::string base_usr =
          CxString(::clang_getCursorUSR(base_ref)).str();
      if (base_usr.empty()) {
        return;
      }
      // A CLASS_TEMPLATE_PARTIAL_SPECIALIZATION is not indexed by index_symbols
      // (its cursor kind is not a top-level SYMBOL kind) and may only be minted
      // later in this same edge pass (named-instance / instantiates path), so
      // lookup can miss it here.  Mint it now -- symmetric with the base (dst)
      // mint below -- so the `inherits` edge is recorded regardless of ordering.
      // mint_symbol_id upserts by USR, so a later mint of the same partial spec
      // is a no-op merge.  kind_name() has every other accepted parent kind, so
      // the "class-template" default only ever applies to the partial spec.
      const auto src_sym = db_.lookup_symbol(derived_usr);
      int64_t src_id = 0;
      if (src_sym) {
        src_id = src_sym->id;
      } else {
        const RefDeclLoc derived_dl = ref_decl_loc(db_, walk_parent);
        const char *derived_kn = kind_name(::clang_getCursorKind(walk_parent));
        const std::string derived_kind = derived_kn != nullptr
                                             ? std::string(derived_kn)
                                             : std::string("class-template");
        src_id = db_.mint_symbol_id(
            derived_usr,
            CxString(::clang_getCursorSpelling(walk_parent)).str(),
            qualified_name(walk_parent),
            CxString(::clang_getCursorDisplayName(walk_parent)).str(),
            derived_kind, derived_dl.file_id, derived_dl.line, derived_dl.col,
            derived_dl.path);
      }
      const RefDeclLoc base_dl = ref_decl_loc(db_, base_ref);
      const int64_t dst_id = db_.mint_symbol_id(
          base_usr,
          CxString(::clang_getCursorSpelling(base_ref)).str(),
          qualified_name(base_ref),
          CxString(::clang_getCursorDisplayName(base_ref)).str(),
          stub_kind(base_ref), base_dl.file_id, base_dl.line, base_dl.col,
          base_dl.path);
      Edge e;
      e.src_id = src_id;
      e.dst_id = dst_id;
      e.kind = 2; // inherits
      e.count = 1;
      const CX_CXXAccessSpecifier acc = ::clang_getCXXAccessSpecifier(cursor);
      if (acc == CX_CXXPublic) {
        e.base_access = 1;
      } else if (acc == CX_CXXProtected) {
        e.base_access = 2;
      } else if (acc == CX_CXXPrivate) {
        e.base_access = 3;
      }
      e.is_virtual = static_cast<int64_t>(::clang_isVirtualBase(cursor));
      db_.add_edge(e);
      // CRTP / template base: also link the specialization instance to its
      // primary template via instantiates(5).  A template used AS A BASE CLASS
      // (`class Cache : public Singleton<Cache>`) is the one instantiation site
      // not covered by the variable/member/call/using paths, so without this
      // the entity roll-up never sees `Singleton<Cache> instantiates Singleton`.
      const CXCursor base_primary =
          ::clang_getSpecializedCursorTemplate(base_ref);
      if (!::clang_Cursor_isNull(base_primary) &&
          !is_invalid_kind(::clang_getCursorKind(base_primary))) {
        const std::string base_prim_usr =
            CxString(::clang_getCursorUSR(base_primary)).str();
        if (!base_prim_usr.empty() && base_prim_usr != base_usr) {
          const auto base_prim_sym = db_.lookup_symbol(base_prim_usr);
          if (base_prim_sym) {
            Edge inst;
            inst.src_id = dst_id;
            inst.dst_id = base_prim_sym->id;
            inst.kind = 5; // instantiates
            inst.count = 1;
            db_.add_edge(inst);
          }
        }
      }
      return;
    }

    // -- FIELD_DECL: field_of ------------------------------------------
    if (ck == CXCursor_FieldDecl) {
      const std::string member_usr =
          CxString(::clang_getCursorUSR(cursor)).str();
      if (member_usr.empty()) {
        return;
      }
      const CXCursor owner = ::clang_getCursorSemanticParent(cursor);
      if (::clang_Cursor_isNull(owner) ||
          is_invalid_kind(::clang_getCursorKind(owner))) {
        return;
      }
      const std::string owner_usr =
          CxString(::clang_getCursorUSR(owner)).str();
      if (owner_usr.empty()) {
        return;
      }
      const auto src_sym = db_.lookup_symbol(member_usr);
      const auto dst_sym = db_.lookup_symbol(owner_usr);
      if (!src_sym || !dst_sym) {
        return;
      }
      Edge e;
      e.src_id = src_sym->id;
      e.dst_id = dst_sym->id;
      e.kind = 8; // field_of
      e.count = 1;
      db_.add_edge(e);
      return;
    }

    // -- CXX_METHOD/CONSTRUCTOR/DESTRUCTOR: method_of ------------------
    if (ck == CXCursor_CXXMethod || ck == CXCursor_Constructor ||
        ck == CXCursor_Destructor) {
      const std::string method_usr =
          CxString(::clang_getCursorUSR(cursor)).str();
      if (method_usr.empty()) {
        return;
      }
      const CXCursor owner = ::clang_getCursorSemanticParent(cursor);
      if (::clang_Cursor_isNull(owner) ||
          is_invalid_kind(::clang_getCursorKind(owner))) {
        return;
      }
      const std::string owner_usr =
          CxString(::clang_getCursorUSR(owner)).str();
      if (owner_usr.empty()) {
        return;
      }
      const auto src_sym = db_.lookup_symbol(method_usr);
      const auto dst_sym = db_.lookup_symbol(owner_usr);
      if (!src_sym || !dst_sym) {
        return;
      }
      Edge e;
      e.src_id = src_sym->id;
      e.dst_id = dst_sym->id;
      e.kind = 9; // method_of
      e.count = 1;
      db_.add_edge(e);

      // -- overrides (CXX_METHOD only): emit for each overridden method --
      if (ck == CXCursor_CXXMethod) {
        CxOverriddenCursors overridden(cursor);
        for (unsigned oi = 0; oi < overridden.size(); ++oi) {
          const std::string ov_usr =
              CxString(::clang_getCursorUSR(overridden[oi])).str();
          if (ov_usr.empty()) {
            continue;
          }
          const RefDeclLoc ov_dl = ref_decl_loc(db_, overridden[oi]);
          const int64_t dst_ov = db_.mint_symbol_id(
              ov_usr,
              CxString(::clang_getCursorSpelling(overridden[oi])).str(),
              qualified_name(overridden[oi]),
              CxString(::clang_getCursorDisplayName(overridden[oi])).str(),
              stub_kind(overridden[oi]), ov_dl.file_id, ov_dl.line,
              ov_dl.col, ov_dl.path);
          Edge oe;
          oe.src_id = src_sym->id;
          oe.dst_id = dst_ov;
          oe.kind = 6; // overrides
          oe.count = 1;
          db_.add_edge(oe);
        }
      }
      return;
    }

    // -- FRIEND_DECL: friend (Layer-0 kind 17 -> befriends entity_edge) --
    // Mirrors ast.py FRIEND_DECL handler. `friend class B;` inside record A;
    // the friend target B is a child TYPE_REF whose referenced declaration is
    // the friended record. Lookup-only (no stub), record-friends only.
    if (ck == CXCursor_FriendDecl) {
      const CXCursor owner = ::clang_getCursorSemanticParent(cursor);
      if (::clang_Cursor_isNull(owner)) {
        return;
      }
      const CXCursorKind owner_kind = ::clang_getCursorKind(owner);
      if (owner_kind != CXCursor_ClassDecl &&
          owner_kind != CXCursor_StructDecl) {
        return;
      }
      const std::string owner_usr =
          CxString(::clang_getCursorUSR(owner)).str();
      if (owner_usr.empty()) {
        return;
      }
      const auto src_sym = db_.lookup_symbol(owner_usr);
      if (!src_sym) {
        return;
      }
      // Collect direct TYPE_REF children (the friended type references).
      struct FriendCtx {
        std::vector<CXCursor> type_refs;
      } fctx;
      ::clang_visitChildren(
          cursor,
          [](CXCursor c, CXCursor /*parent*/, CXClientData data) {
            auto *ctx = static_cast<FriendCtx *>(data);
            if (::clang_getCursorKind(c) == CXCursor_TypeRef) {
              ctx->type_refs.push_back(c);
            }
            return CXChildVisit_Continue;
          },
          &fctx);
      for (const CXCursor &tref : fctx.type_refs) {
        const CXCursor friend_decl = ::clang_getCursorReferenced(tref);
        if (::clang_Cursor_isNull(friend_decl)) {
          continue;
        }
        const CXCursorKind fk = ::clang_getCursorKind(friend_decl);
        if (fk != CXCursor_ClassDecl && fk != CXCursor_StructDecl &&
            fk != CXCursor_ClassTemplate) {
          continue;
        }
        const std::string friend_usr =
            CxString(::clang_getCursorUSR(friend_decl)).str();
        if (friend_usr.empty()) {
          continue;
        }
        const auto dst_sym = db_.lookup_symbol(friend_usr);
        if (!dst_sym) {
          continue;
        }
        Edge e;
        e.src_id = src_sym->id;
        e.dst_id = dst_sym->id;
        e.kind = 17; // friend
        e.count = 1;
        db_.add_edge(e);
      }
      return;
    }

    // -- CLASS_TEMPLATE/FUNCTION_TEMPLATE: template_param --
    if (ck == CXCursor_ClassTemplate || ck == CXCursor_FunctionTemplate) {
      const std::string tmpl_usr =
          CxString(::clang_getCursorUSR(cursor)).str();
      if (tmpl_usr.empty()) {
        return;
      }
      const auto tmpl_sym = db_.lookup_symbol(tmpl_usr);
      if (!tmpl_sym) {
        return;
      }
      // A member function template (FUNCTION_TEMPLATE whose semantic parent is
      // a record/class-template) is a method too, but the CXX_METHOD method_of
      // block above never sees it (its cursor kind is FUNCTION_TEMPLATE), so it
      // would lack a method_of edge. Emit method_of here for this case.
      if (ck == CXCursor_FunctionTemplate) {
        const CXCursor owner = ::clang_getCursorSemanticParent(cursor);
        if (!::clang_Cursor_isNull(owner) &&
            !is_invalid_kind(::clang_getCursorKind(owner))) {
          const CXCursorKind ok = ::clang_getCursorKind(owner);
          if (ok == CXCursor_ClassDecl || ok == CXCursor_StructDecl ||
              ok == CXCursor_ClassTemplate ||
              ok == CXCursor_ClassTemplatePartialSpecialization) {
            const std::string owner_usr =
                CxString(::clang_getCursorUSR(owner)).str();
            if (!owner_usr.empty()) {
              const auto owner_sym = db_.lookup_symbol(owner_usr);
              if (owner_sym) {
                Edge mo;
                mo.src_id = tmpl_sym->id;
                mo.dst_id = owner_sym->id;
                mo.kind = 9; // method_of
                mo.count = 1;
                db_.add_edge(mo);
              }
            }
          }
        }
      }
      // Enumerate template parameters (TEMPLATE_TYPE_PARAMETER,
      // TEMPLATE_NON_TYPE_PARAMETER, TEMPLATE_TEMPLATE_PARAMETER children).
      struct ParamCtx {
        Storage *db = nullptr;
        int64_t owner_id = -1;
        int64_t pos = 0;
      };
      ParamCtx pctx;
      pctx.db = &db_;
      pctx.owner_id = tmpl_sym->id;
      pctx.pos = 0;
      ::clang_visitChildren(
          cursor,
          [](CXCursor c, CXCursor /*parent*/,
             CXClientData d) noexcept -> CXChildVisitResult {
            auto *pc = static_cast<ParamCtx *>(d);
            const CXCursorKind pk = ::clang_getCursorKind(c);
            int64_t param_kind = 0;
            if (pk == CXCursor_TemplateTypeParameter) {
              param_kind = 1;
            } else if (pk == CXCursor_NonTypeTemplateParameter) {
              param_kind = 2;
            } else if (pk == CXCursor_TemplateTemplateParameter) {
              param_kind = 3;
            } else {
              return CXChildVisit_Continue;
            }
            TemplateParam p;
            p.owner_id = pc->owner_id;
            p.position = pc->pos++;
            p.param_kind = param_kind;
            const std::string nm =
                CxString(::clang_getCursorSpelling(c)).str();
            if (!nm.empty()) {
              p.name = nm;
            }
            pc->db->add_template_param(p);
            return CXChildVisit_Continue;
          },
          &pctx);
      return;
    }

    // -- STRUCT_DECL/CLASS_DECL: specializes (when it is a specialization) --
    if ((ck == CXCursor_StructDecl || ck == CXCursor_ClassDecl) &&
        ::clang_isCursorDefinition(cursor)) {
      const CXCursor primary = ::clang_getSpecializedCursorTemplate(cursor);
      if (!::clang_Cursor_isNull(primary) &&
          !is_invalid_kind(::clang_getCursorKind(primary))) {
        const std::string spec_usr =
            CxString(::clang_getCursorUSR(cursor)).str();
        const std::string prim_usr =
            CxString(::clang_getCursorUSR(primary)).str();
        if (!spec_usr.empty() && !prim_usr.empty() && spec_usr != prim_usr) {
          const auto spec_sym = db_.lookup_symbol(spec_usr);
          if (spec_sym) {
            const RefDeclLoc prim_dl = ref_decl_loc(db_, primary);
            const int64_t prim_id = db_.mint_symbol_id(
                prim_usr,
                CxString(::clang_getCursorSpelling(primary)).str(),
                qualified_name(primary),
                CxString(::clang_getCursorDisplayName(primary)).str(),
                stub_kind(primary), prim_dl.file_id, prim_dl.line,
                prim_dl.col, prim_dl.path);
            // An explicit instantiation (`template class Foo<int>;`) is a
            // concrete INSTANCE of the template, not a specialization of it:
            // record it as `instantiates` (kind=5, instance -> primary) so it
            // surfaces under ClassTemplate.instantiations(). A true explicit
            // specialization (`template <> class Foo<bool> {...}`) stays
            // `specializes` (kind=4).
            Edge e;
            e.src_id = spec_sym->id;
            e.dst_id = prim_id;
            e.kind = is_explicit_instantiation(cursor) ? 5 : 4;
            e.count = 1;
            db_.add_edge(e);

            // template_arg rows for the specialization's arguments. For TYPE
            // args we always store the type spelling in `literal` (e.g. 'bool',
            // 'int') so the binding is distinguishable even when the arg is a
            // builtin with no declaration to resolve a ref_id from.
            const int nargs = ::clang_Cursor_getNumTemplateArguments(cursor);
            for (int ai = 0; ai < nargs; ++ai) {
              const enum CXTemplateArgumentKind tak =
                  ::clang_Cursor_getTemplateArgumentKind(
                      cursor, static_cast<unsigned>(ai));
              TemplateArg ta;
              ta.owner_id = spec_sym->id;
              ta.position = static_cast<int64_t>(ai);
              if (tak == CXTemplateArgumentKind_Type) {
                ta.arg_kind = 1;
                const CXType arg_type =
                    ::clang_Cursor_getTemplateArgumentType(
                        cursor, static_cast<unsigned>(ai));
                const std::string spelling =
                    CxString(::clang_getTypeSpelling(arg_type)).str();
                if (!spelling.empty()) {
                  ta.literal = spelling;
                }
                const CXCursor arg_decl =
                    ::clang_getTypeDeclaration(arg_type);
                if (!::clang_Cursor_isNull(arg_decl) &&
                    !is_invalid_kind(::clang_getCursorKind(arg_decl))) {
                  const std::string ref_usr =
                      CxString(::clang_getCursorUSR(arg_decl)).str();
                  if (!ref_usr.empty()) {
                    if (const auto rsym = db_.lookup_symbol(ref_usr)) {
                      ta.ref_id = rsym->id;
                    }
                  }
                }
                if (!ta.ref_id) {
                  ta.ref_id =
                      resolve_template_arg_ref_id(db_, ta.literal, cursor);
                }
              } else if (tak == CXTemplateArgumentKind_Integral) {
                ta.arg_kind = 2;
                ta.literal =
                    std::to_string(::clang_Cursor_getTemplateArgumentValue(
                        cursor, static_cast<unsigned>(ai)));
              } else {
                ta.arg_kind = static_cast<int64_t>(tak);
              }
              db_.add_template_arg(ta);
            }
          }
        }
      }
      return;
    }

  });

  // B2: body descent for calls + uses — recurse into each function-like
  // definition whose enclosing file matches filename.
  for_file_cursors(tu, filename, [&](CXCursor cursor) {
    if (!is_function_like(::clang_getCursorKind(cursor))) {
      return;
    }
    if (!::clang_isCursorDefinition(cursor)) {
      return;
    }
    const std::string fn_usr =
        CxString(::clang_getCursorUSR(cursor)).str();
    if (fn_usr.empty()) {
      return;
    }
    const auto fn_sym = db_.lookup_symbol(fn_usr);
    if (!fn_sym) {
      return;
    }
    // v27: this function's body in THIS file is a per-backend definition.
    // Create its `definition` row, descend, then snapshot the calls/uses it just
    // emitted into `def_edge` -- immune to a later TU wiping `edge`.
    const ExpansionLoc dstart = cursor_extent_start(cursor);
    const ExpansionLoc dend = cursor_extent_end(cursor);
    const int64_t def_id = db_.get_or_create_definition(
        fn_sym->id, file_id, static_cast<int64_t>(dstart.line),
        static_cast<int64_t>(dstart.col), static_cast<int64_t>(dend.line),
        static_cast<int64_t>(dend.col));
    body_descent(cursor, fn_sym->id, file_id);
    db_.copy_body_edges_to_def_edge(def_id, fn_sym->id);
  });

  // B3: namespace uses -- qualifiers / using-directives / using-declarations.
  emit_namespace_uses(db_, tu, filename, file_id);
}

} // namespace cidx
