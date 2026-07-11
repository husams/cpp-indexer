#include "clangx/ast_internal.hpp"

#include <algorithm>
#include <cctype>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "clangx/clang_raii.hpp"

// ---- v7 graph extraction ----------------------------------------------------

namespace cidx::ast_detail {

// CursorKinds whose descendant is a conditional branch for cond_depth tracking.
bool is_cond_cursor(CXCursorKind kind) {
  return kind == CXCursor_IfStmt || kind == CXCursor_ForStmt ||
         kind == CXCursor_WhileStmt || kind == CXCursor_DoStmt ||
         kind == CXCursor_SwitchStmt || kind == CXCursor_CaseStmt ||
         kind == CXCursor_ConditionalOperator;
}

// True when a STRUCT/CLASS_DECL whose clang_getSpecializedCursorTemplate is
// non-null is an explicit INSTANTIATION (`template class Foo<int>;`) rather
// than an explicit SPECIALIZATION (`template <> class Foo<bool> { ... };`).
// Both report is_definition()==true and a non-null specialized template, so the
// stable libclang C API cannot tell them apart directly -- the written syntax
// can. We tokenize the cursor's extent: the token immediately after the
// `template` keyword is `class`/`struct` for an instantiation (optionally after
// `extern`) and `<` for a specialization.
bool is_explicit_instantiation(CXCursor cursor) {
  CXTranslationUnit tu = ::clang_Cursor_getTranslationUnit(cursor);
  if (tu == nullptr) {
    return false;
  }
  const CXSourceRange extent = ::clang_getCursorExtent(cursor);
  CXToken *tokens = nullptr;
  unsigned n = 0;
  ::clang_tokenize(tu, extent, &tokens, &n);
  if (tokens == nullptr) {
    return false;
  }
  bool result = false;
  for (unsigned i = 0; i < n; ++i) {
    const std::string s =
        CxString(::clang_getTokenSpelling(tu, tokens[i])).str();
    if (s == "template") {
      if (i + 1 < n) {
        const std::string nxt =
            CxString(::clang_getTokenSpelling(tu, tokens[i + 1])).str();
        result = (nxt == "class" || nxt == "struct");
      }
      break;
    }
    if (s == "class" || s == "struct") {
      break;
    }
  }
  ::clang_disposeTokens(tu, tokens, n);
  return result;
}

// Write template_arg rows for a FUNCTION/METHOD template specialization from the
// cursor-level libclang API. Returns the number of args written -- 0 means the
// cursor exposed none (notably every METHOD-template specialization, where
// clang_Cursor_getNumTemplateArguments returns -1; use the token fallback).
// Mirrors ast.py:_index_cursor_template_args and the explicit-instantiation
// handler's TYPE/INTEGRAL branch.
std::optional<int64_t>
resolve_template_arg_ref_id(Storage &db,
                            const std::optional<std::string> &literal,
                            CXCursor scope_cursor);

std::optional<size_t> matching_template_close(const std::string &text,
                                              size_t start) {
  int depth = 0;
  for (size_t i = start; i < text.size(); ++i) {
    if (text[i] == '<') {
      ++depth;
    } else if (text[i] == '>') {
      --depth;
      if (depth == 0) {
        return i;
      }
    }
  }
  return std::nullopt;
}

std::string render_callable_template_display_name(
    const std::string &display_name, const std::vector<std::string> &literals) {
  std::string rendered_args = "<";
  for (size_t i = 0; i < literals.size(); ++i) {
    if (i != 0) {
      rendered_args += ", ";
    }
    rendered_args += literals[i];
  }
  rendered_args += ">";

  const size_t start = display_name.find('<');
  const size_t params = display_name.find('(');
  if (start != std::string::npos &&
      (params == std::string::npos || start < params)) {
    if (const auto end = matching_template_close(display_name, start)) {
      return display_name.substr(0, start) + rendered_args +
             display_name.substr(*end + 1);
    }
  }

  if (params != std::string::npos) {
    return display_name.substr(0, params) + rendered_args +
           display_name.substr(params);
  }
  return display_name + rendered_args;
}

void update_callable_template_display_name(
    Storage &db, int64_t owner_id, const std::vector<std::string> &literals) {
  if (literals.empty()) {
    return;
  }
  if (std::find(literals.begin(), literals.end(), "?") != literals.end()) {
    return;
  }
  const std::optional<Symbol> sym = db.lookup_symbol_by_id(owner_id);
  if (!sym || !sym->display_name || sym->display_name->empty()) {
    return;
  }
  const std::string display =
      render_callable_template_display_name(*sym->display_name, literals);
  if (display != *sym->display_name) {
    db.update_symbol(sym->usr, {{"display_name", display}});
  }
}

int index_cursor_template_args(Storage &db, int64_t owner_id,
                               CXCursor cursor) {
  const int nargs = ::clang_Cursor_getNumTemplateArguments(cursor);
  if (nargs <= 0) {
    return 0;
  }
  std::vector<std::string> display_args;
  for (int ai = 0; ai < nargs; ++ai) {
    const enum CXTemplateArgumentKind tak =
        ::clang_Cursor_getTemplateArgumentKind(cursor,
                                                 static_cast<unsigned>(ai));
    TemplateArg ta;
    ta.owner_id = owner_id;
    ta.position = static_cast<int64_t>(ai);
    if (tak == CXTemplateArgumentKind_Type) {
      ta.arg_kind = 1;
      const CXType arg_type = ::clang_Cursor_getTemplateArgumentType(
          cursor, static_cast<unsigned>(ai));
      const std::string spelling =
          CxString(::clang_getTypeSpelling(arg_type)).str();
      if (!spelling.empty()) {
        ta.literal = spelling;
      }
      const CXCursor arg_decl = ::clang_getTypeDeclaration(arg_type);
      if (!::clang_Cursor_isNull(arg_decl) &&
          !is_invalid_kind(::clang_getCursorKind(arg_decl))) {
        const std::string ref_usr =
            CxString(::clang_getCursorUSR(arg_decl)).str();
        if (!ref_usr.empty()) {
          if (const auto rsym = db.lookup_symbol(ref_usr)) {
            ta.ref_id = rsym->id;
          }
        }
      }
      if (!ta.ref_id) {
        ta.ref_id = resolve_template_arg_ref_id(db, ta.literal, cursor);
      }
      display_args.push_back(ta.literal.value_or("?"));
    } else if (tak == CXTemplateArgumentKind_Integral) {
      ta.arg_kind = 2;
      ta.literal = std::to_string(::clang_Cursor_getTemplateArgumentValue(
          cursor, static_cast<unsigned>(ai)));
      display_args.push_back(*ta.literal);
    } else if (tak == CXTemplateArgumentKind_Declaration ||
               tak == CXTemplateArgumentKind_NullPtr ||
               tak == CXTemplateArgumentKind_Expression) {
      ta.arg_kind = 2;
      display_args.push_back("?");
    } else if (tak == CXTemplateArgumentKind_Template ||
               tak == CXTemplateArgumentKind_TemplateExpansion) {
      ta.arg_kind = 3;
      display_args.push_back("?");
    } else if (tak == CXTemplateArgumentKind_Pack) {
      ta.arg_kind = 4;
      display_args.push_back("?");
    } else {
      continue;
    }
    db.add_template_arg(ta);
  }
  update_callable_template_display_name(db, owner_id, display_args);
  return nargs;
}

// Best-effort template_arg.arg_kind for a token-derived method-template arg:
// a bare numeric / char / bool / nullptr literal is a non-type value (2), else a
// type (1). Reads the literal itself, not a mangled format. Mirrors
// ast.py:_method_targ_kind_from_literal.
int64_t method_targ_kind_from_literal(const std::string &text) {
  if (text == "true" || text == "false" || text == "nullptr") {
    return 2;
  }
  if (!text.empty() && (text.front() == '\'' || text.front() == '"')) {
    return 2;
  }
  std::string head = text;
  while (!head.empty() && (head.front() == '+' || head.front() == '-')) {
    head.erase(head.begin());
  }
  if (!head.empty() &&
      std::isdigit(static_cast<unsigned char>(head.front())) != 0) {
    return 2;
  }
  return 1;
}

// Minimal spacing when re-joining tokens into a type spelling: keep
// identifiers/keywords apart (`const int`) but hug punctuation (`int*`,
// `Pair<int,char>`). Mirrors ast.py:_needs_space.
bool targ_needs_space(const std::string &a, const std::string &b) {
  if (a.empty() || b.empty()) {
    return false;
  }
  const char al = a.back();
  const char bf = b.front();
  const bool a_word =
      std::isalnum(static_cast<unsigned char>(al)) != 0 || al == '_';
  const bool b_word =
      std::isalnum(static_cast<unsigned char>(bf)) != 0 || bf == '_';
  return a_word && b_word;
}

std::string trim_copy(std::string s) {
  const auto not_space = [](unsigned char c) { return std::isspace(c) == 0; };
  s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
  s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
  return s;
}

bool starts_with(const std::string &s, const std::string &prefix) {
  return s.size() >= prefix.size() &&
         s.compare(0, prefix.size(), prefix) == 0;
}

bool ends_with(const std::string &s, const std::string &suffix) {
  return s.size() >= suffix.size() &&
         s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string template_arg_base_name(std::string text) {
  std::string s = trim_copy(std::move(text));
  for (const std::string prefix :
       {"typename ", "class ", "struct ", "enum "}) {
    if (starts_with(s, prefix)) {
      s = trim_copy(s.substr(prefix.size()));
      break;
    }
  }
  bool changed = true;
  while (changed) {
    changed = false;
    for (const std::string prefix : {"const ", "volatile "}) {
      if (starts_with(s, prefix)) {
        s = trim_copy(s.substr(prefix.size()));
        changed = true;
      }
    }
    for (const std::string suffix : {" const", " volatile"}) {
      if (ends_with(s, suffix)) {
        s = trim_copy(s.substr(0, s.size() - suffix.size()));
        changed = true;
      }
    }
    for (const std::string suffix : {"&&", "&", "*"}) {
      if (ends_with(s, suffix)) {
        s = trim_copy(s.substr(0, s.size() - suffix.size()));
        changed = true;
      }
    }
  }
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '<') {
      s = trim_copy(s.substr(0, i));
      break;
    }
  }
  if (starts_with(s, "::")) {
    s = s.substr(2);
  }
  return s;
}

bool type_arg_symbol_kind(const std::string &kind) {
  return kind == "class" || kind == "struct" || kind == "union" ||
         kind == "enum" || kind == "typedef" || kind == "type-alias" ||
         kind == "class-template";
}

std::vector<std::string> cursor_scope_qual_names(
                                                 CXCursor cursor) {
  std::vector<std::string> out;
  std::set<std::string> seen;
  if (::clang_Cursor_isNull(cursor)) {
    return out;
  }
  CXCursor c = ::clang_getCursorSemanticParent(cursor);
  while (!::clang_Cursor_isNull(c)) {
    const CXCursorKind kind = ::clang_getCursorKind(c);
    if (is_invalid_kind(kind) || kind == CXCursor_TranslationUnit) {
      break;
    }
    const std::string qn = qualified_name(c);
    if (!qn.empty() && seen.insert(qn).second) {
      out.push_back(qn);
    }
    c = ::clang_getCursorSemanticParent(c);
  }
  return out;
}

std::vector<Symbol> type_arg_symbol_candidates(Storage &db,
                                               const std::string &name,
                                               bool qualified) {
  const std::vector<Symbol> hits =
      qualified ? db.lookup_symbols_by_qual_name(name)
                : db.lookup_symbols_by_name(name);
  std::vector<Symbol> out;
  std::set<int64_t> seen;
  for (const Symbol &sym : hits) {
    if (type_arg_symbol_kind(sym.kind) && seen.insert(sym.id).second) {
      out.push_back(sym);
    }
  }
  return out;
}

std::optional<int64_t> pick_template_arg_symbol(
    const std::vector<Symbol> &candidates) {
  if (candidates.empty()) {
    return std::nullopt;
  }
  std::vector<Symbol> non_inst;
  for (const Symbol &sym : candidates) {
    if (!sym.is_instantiation) {
      non_inst.push_back(sym);
    }
  }
  const std::vector<Symbol> &pool =
      !non_inst.empty() ? non_inst : candidates;
  if (pool.size() == 1) {
    return pool[0].id;
  }
  return std::nullopt;
}

std::optional<int64_t>
resolve_template_arg_ref_id(Storage &db,
                            const std::optional<std::string> &literal,
                            CXCursor scope_cursor) {
  if (!literal || literal->empty()) {
    return std::nullopt;
  }
  const std::string base = template_arg_base_name(*literal);
  if (base.empty()) {
    return std::nullopt;
  }
  std::vector<std::string> names;
  if (base.find("::") != std::string::npos) {
    names.push_back(base);
  }
  for (const std::string &scope : cursor_scope_qual_names(scope_cursor)) {
    names.push_back(scope + "::" + base);
  }
  names.push_back(base);
  std::set<std::string> seen_names;
  for (const std::string &name : names) {
    if (!seen_names.insert(name).second) {
      continue;
    }
    const auto ref_id =
        pick_template_arg_symbol(type_arg_symbol_candidates(db, name, true));
    if (ref_id) {
      return ref_id;
    }
  }
  const size_t pos = base.rfind("::");
  const std::string tail = pos == std::string::npos ? base : base.substr(pos + 2);
  return pick_template_arg_symbol(type_arg_symbol_candidates(db, tail, false));
}

// Token fallback for METHOD-template explicit args (`obj.m<T,...>()`). libclang's
// cursor API returns -1 for methods, so recover the EXPLICIT `<...>` arguments
// from the call tokens and store each as its literal source spelling -- as
// written; TYPE args get a best-effort ref_id when their spelling resolves to an
// indexed type in scope. Top-level commas split args; `<`/`>`
// depth tracking (incl. `>>` closers) keeps nested args whole. Deduced calls
// (no explicit `<...>`) yield nothing. Mirrors
// ast.py:_index_method_template_args_from_tokens.
void index_method_template_args_from_tokens(Storage &db,
                                            int64_t owner_id,
                                            CXCursor call_cursor,
                                            const std::string &method_name) {
  CXTranslationUnit tu = ::clang_Cursor_getTranslationUnit(call_cursor);
  if (tu == nullptr) {
    return;
  }
  const CXSourceRange extent = ::clang_getCursorExtent(call_cursor);
  CXToken *tokens = nullptr;
  unsigned n = 0;
  ::clang_tokenize(tu, extent, &tokens, &n);
  if (tokens == nullptr) {
    return;
  }
  std::vector<std::string> toks;
  toks.reserve(n);
  for (unsigned i = 0; i < n; ++i) {
    toks.push_back(CxString(::clang_getTokenSpelling(tu, tokens[i])).str());
  }
  ::clang_disposeTokens(tu, tokens, n);

  size_t ni = toks.size();
  for (size_t i = 0; i < toks.size(); ++i) {
    if (toks[i] == method_name) {
      ni = i;
      break;
    }
  }
  if (ni == toks.size() || ni + 1 >= toks.size() || toks[ni + 1] != "<") {
    return;  // no explicit template arguments at the call site
  }
  int depth = 0;
  std::vector<std::vector<std::string>> groups(1);
  for (size_t i = ni + 1; i < toks.size(); ++i) {
    const std::string &tok = toks[i];
    const int opens = static_cast<int>(std::count(tok.begin(), tok.end(), '<'));
    const int closes = static_cast<int>(std::count(tok.begin(), tok.end(), '>'));
    if (opens > 0) {
      const int before = depth;
      depth += opens;
      if (before == 0) {  // outermost '<' opens the list, don't record it
        if (opens > 1) {
          groups.back().push_back(std::string(static_cast<size_t>(opens - 1), '<'));
        }
        continue;
      }
      groups.back().push_back(tok);
      continue;
    }
    if (closes > 0) {
      const int before = depth;
      depth -= closes;
      if (depth <= 0) {  // '>>' can close a nested bracket AND the outer list
        const int inner = before - 1;
        if (inner > 0) {
          groups.back().push_back(std::string(static_cast<size_t>(inner), '>'));
        }
        break;
      }
      groups.back().push_back(tok);
      continue;
    }
    if (depth == 1 && tok == ",") {
      groups.emplace_back();
      continue;
    }
    groups.back().push_back(tok);
  }
  int64_t pos = 0;
  std::vector<std::string> display_args;
  for (const auto &g : groups) {
    if (g.empty()) {
      continue;
    }
    std::string literal;
    for (size_t i = 0; i < g.size(); ++i) {
      if (i != 0 && targ_needs_space(g[i - 1], g[i])) {
        literal += ' ';
      }
      literal += g[i];
    }
    // .strip() -- trim surrounding whitespace to match Python's join().strip()
    const auto not_space = [](unsigned char c) { return std::isspace(c) == 0; };
    literal.erase(literal.begin(),
                  std::find_if(literal.begin(), literal.end(), not_space));
    literal.erase(std::find_if(literal.rbegin(), literal.rend(), not_space).base(),
                  literal.end());
    if (literal.empty()) {
      continue;
    }
    TemplateArg ta;
    ta.owner_id = owner_id;
    ta.position = pos++;
    ta.arg_kind = method_targ_kind_from_literal(literal);
    ta.literal = literal;
    if (ta.arg_kind == 1) {
      ta.ref_id = resolve_template_arg_ref_id(db, ta.literal, call_cursor);
    }
    db.add_template_arg(ta);
    display_args.push_back(literal);
  }
  update_callable_template_display_name(db, owner_id, display_args);
}

// Stage 2/3 core: mint a NAMED template instance `X<B>` from a CXType that is a
// class-template specialization. `X<B>` is named -- by an alias, a member, or a
// variable -- but a plain parse mints no `X<B>` symbol; only the primary `X` and
// any explicit `X<int>` decls exist. Mint the `X<B>` instance as its own entity
// (is_named_instance=1) so the roll-up can give it composes/aggregates/
// associates B (T->B substituted into the primary's members). Emits the symbol,
// an instantiates(5) edge X<B> -> X, and template_arg rows (TYPE args, T->B).
// Shared by three call sites that each name a concrete `X<B>`:
//   - `using Y = X<B>;` / `typedef X<B> Y;` (alias underlying type)  [Stage 2]
//   - a member `X<B> field;`                (FieldDecl type)         [Stage 3]
//   - a variable/local `X<B> v;`            (VarDecl type)           [Stage 3]
// Gated to a NON-system primary (a `std::vector<F>` member/alias/local is left
// to collapse onto the primary). Mirrors ast.py:_mint_instance_from_type.
void mint_instance_from_type(Storage &db, CXType type_obj) {
  // Stage 4: peel pointer / reference / array wrappers so an `X<B>* m_;` /
  // `X<B>& m_;` member mints the SAME instance as a by-value `X<B> m_;`. Mirrors
  // named_type_decl's stripping so emit_type_use (which peels the same way) and
  // minting agree on the spec decl USR. A `std::vector<X<B>>` is NOT a wrapper
  // kind here (it is a specialization whose primary is a system template) -- it
  // is left to collapse onto std::vector, never peeled to the inner X<B>, so no
  // std:: explosion and no inner mint (deferred, see plan).
  for (int i = 0; i < 32; ++i) {
    const CXTypeKind tk = type_obj.kind;
    if (tk == CXType_Pointer || tk == CXType_LValueReference ||
        tk == CXType_RValueReference) {
      type_obj = ::clang_getPointeeType(type_obj);
    } else if (tk == CXType_ConstantArray || tk == CXType_IncompleteArray ||
               tk == CXType_VariableArray || tk == CXType_DependentSizedArray) {
      type_obj = ::clang_getArrayElementType(type_obj);
    } else {
      break;
    }
  }
  const CXCursor decl = ::clang_getTypeDeclaration(type_obj);
  if (::clang_Cursor_isNull(decl) ||
      is_invalid_kind(::clang_getCursorKind(decl))) {
    return;
  }
  const CXCursor primary = ::clang_getSpecializedCursorTemplate(decl);
  if (::clang_Cursor_isNull(primary) ||
      is_invalid_kind(::clang_getCursorKind(primary))) {
    return; // type is not a class-template specialization
  }
  // Skip std:: / system templates -- keep them collapsed onto the primary.
  if (::clang_Location_isInSystemHeader(
          ::clang_getCursorLocation(primary)) != 0) {
    return;
  }
  const std::string inst_usr =
      CxString(::clang_getCursorUSR(decl)).str();
  const std::string prim_usr =
      CxString(::clang_getCursorUSR(primary)).str();
  if (inst_usr.empty() || prim_usr.empty() || inst_usr == prim_usr) {
    return;
  }

  const RefDeclLoc inst_dl = ref_decl_loc(db, decl);
  const int64_t inst_id = db.mint_symbol_id(
      inst_usr, CxString(::clang_getCursorSpelling(decl)).str(),
      qualified_name(decl),
      CxString(::clang_getTypeSpelling(type_obj)).str(),  // 'X<B>'
      stub_kind(decl), inst_dl.file_id, inst_dl.line, inst_dl.col,
      inst_dl.path, /*is_instantiation=*/true, /*is_named_instance=*/true);

  const RefDeclLoc prim_dl = ref_decl_loc(db, primary);
  // Mirror ast.py:2096 -- `_KIND_MAP.get(primary.kind, "class-template")`: the
  // primary of a class-template instantiation defaults to "class-template", NOT
  // stub_kind()'s generic "function" fallback.  This matters when `primary` is a
  // CLASS_TEMPLATE_PARTIAL_SPECIALIZATION cursor (an instantiation that matches a
  // partial spec), whose kind has no kind_name() entry -- otherwise C++ stored
  // the stub as "function" while Python stored "class-template" (parity break).
  const char *prim_kn = kind_name(::clang_getCursorKind(primary));
  const std::string prim_kind =
      prim_kn != nullptr ? std::string(prim_kn) : std::string("class-template");
  const int64_t prim_id = db.mint_symbol_id(
      prim_usr, CxString(::clang_getCursorSpelling(primary)).str(),
      qualified_name(primary),
      CxString(::clang_getCursorDisplayName(primary)).str(), prim_kind,
      prim_dl.file_id, prim_dl.line, prim_dl.col, prim_dl.path);
  Edge e;
  e.src_id = inst_id;
  e.dst_id = prim_id;
  e.kind = 5; // instantiates: X<B> -> X
  e.count = 1;
  db.add_edge(e);

  // template_arg rows on the instance. The Type API exposes TYPE args only,
  // which is all the roll-up needs (T->B); non-type args are skipped.
  const int nargs = ::clang_Type_getNumTemplateArguments(type_obj);
  for (int ai = 0; ai < nargs; ++ai) {
    const CXType arg_type = ::clang_Type_getTemplateArgumentAsType(
        type_obj, static_cast<unsigned>(ai));
    TemplateArg ta;
    ta.owner_id = inst_id;
    ta.position = static_cast<int64_t>(ai);
    ta.arg_kind = 1;
    const std::string spelling =
        CxString(::clang_getTypeSpelling(arg_type)).str();
    if (!spelling.empty()) {
      ta.literal = spelling;
    }
    const CXCursor arg_decl = ::clang_getTypeDeclaration(arg_type);
    if (!::clang_Cursor_isNull(arg_decl) &&
        !is_invalid_kind(::clang_getCursorKind(arg_decl))) {
      const std::string ref_usr =
          CxString(::clang_getCursorUSR(arg_decl)).str();
      if (!ref_usr.empty()) {
        if (const auto rsym = db.lookup_symbol(ref_usr)) {
          ta.ref_id = rsym->id;
        }
      }
    }
    if (!ta.ref_id) {
      ta.ref_id = resolve_template_arg_ref_id(db, ta.literal,
                                              ::clang_getNullCursor());
    }
    db.add_template_arg(ta);
  }
}

// Stage 2: mint the `X<B>` instance named by a `using`/typedef alias. Thin
// wrapper over mint_instance_from_type using the alias's underlying type.
// Mirrors ast.py:_mint_named_instance.
void mint_named_instance(Storage &db, CXCursor cursor) {
  mint_instance_from_type(db,
                          ::clang_getTypedefDeclUnderlyingType(cursor));
}


} // namespace cidx::ast_detail
