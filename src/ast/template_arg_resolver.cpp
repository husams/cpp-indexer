#include "ast/template_arg_resolver.hpp"

#include "ast/edge_sink.hpp"
#include "ast/names.hpp"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"

#include <set>

namespace cidx::lt {

namespace {

bool starts_with(const std::string &s, const std::string &p) {
  return s.rfind(p, 0) == 0;
}

bool ends_with(const std::string &s, const std::string &p) {
  return s.size() >= p.size() && s.compare(s.size() - p.size(), p.size(), p) == 0;
}

std::string trim_copy(std::string s) {
  const auto b = s.find_first_not_of(" \t");
  if (b == std::string::npos)
    return {};
  const auto e = s.find_last_not_of(" \t");
  return s.substr(b, e - b + 1);
}

// template_arg_base_name (ast_templates.cpp:257): strip elaboration keywords,
// cv-qualifiers, reference/pointer suffixes, template args, leading `::`.
std::string base_name(std::string text) {
  std::string s = trim_copy(std::move(text));
  for (const std::string prefix : {"typename ", "class ", "struct ", "enum "}) {
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
  if (starts_with(s, "::"))
    s = s.substr(2);
  return s;
}

bool type_arg_symbol_kind(const std::string &kind) {
  return kind == "class" || kind == "struct" || kind == "union" ||
         kind == "enum" || kind == "typedef" || kind == "type-alias" ||
         kind == "class-template";
}

// pick_template_arg_symbol: prefer non-instantiation candidates; only an
// unambiguous single candidate resolves.
std::optional<int64_t>
pick(const std::vector<TypeArgCandidate> &candidates) {
  std::vector<const TypeArgCandidate *> typed;
  for (const TypeArgCandidate &c : candidates)
    if (type_arg_symbol_kind(c.kind_name))
      typed.push_back(&c);
  if (typed.empty())
    return std::nullopt;
  std::vector<const TypeArgCandidate *> non_inst;
  for (const TypeArgCandidate *c : typed)
    if (!c->is_instantiation)
      non_inst.push_back(c);
  const auto &pool = !non_inst.empty() ? non_inst : typed;
  if (pool.size() == 1)
    return pool[0]->id;
  return std::nullopt;
}

} // namespace

TemplateArgResolver::TemplateArgResolver(const clang::ASTContext &context,
                                         EdgeSink &sink)
    : context_(context), sink_(sink) {}

std::optional<int64_t>
TemplateArgResolver::resolve(const std::optional<std::string> &literal,
                             const clang::Decl *scope_decl) const {
  if (!literal || literal->empty())
    return std::nullopt;
  const std::string base = base_name(*literal);
  if (base.empty())
    return std::nullopt;

  // cursor_scope_qual_names: qualified names of the enclosing record/namespace
  // scopes, innermost first.
  std::vector<std::string> names;
  if (base.find("::") != std::string::npos)
    names.push_back(base);
  const clang::Decl *d = scope_decl;
  while (d != nullptr && !llvm::isa<clang::TranslationUnitDecl>(d)) {
    if (const auto *nd = llvm::dyn_cast<clang::NamedDecl>(d)) {
      if (llvm::isa<clang::NamespaceDecl>(nd) ||
          llvm::isa<clang::RecordDecl>(nd)) {
        const std::string qual = qualified_name(context_, nd);
        if (!qual.empty())
          names.push_back(qual + "::" + base);
      }
    }
    const clang::DeclContext *dc = d->getDeclContext();
    d = dc != nullptr ? llvm::dyn_cast<clang::Decl>(dc) : nullptr;
  }
  names.push_back(base);

  std::set<std::string> seen;
  for (const std::string &name : names) {
    if (!seen.insert(name).second)
      continue;
    if (auto id = pick(sink_.type_arg_candidates(name, /*qualified=*/true)))
      return id;
  }
  const size_t pos = base.rfind("::");
  const std::string tail =
      pos == std::string::npos ? base : base.substr(pos + 2);
  return pick(sink_.type_arg_candidates(tail, /*qualified=*/false));
}

} // namespace cidx::lt
