#include "clangx_lt/kind_map.hpp"

#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"

namespace cidx::lt {

int cidx_symbol_kind(const clang::Decl *decl) {
  using clang::Decl;
  switch (decl->getKind()) {
  // Explicit full specializations surface as plain record cursors in libclang;
  // partial specializations map to a cursor kind outside the frozen 17-entry
  // map and are therefore not symbols.
  case Decl::ClassTemplateSpecialization:
  case Decl::CXXRecord:
  case Decl::Record: {
    const auto *rd = llvm::cast<clang::RecordDecl>(decl);
    if (rd->isUnion())
      return 3; // union
    if (rd->isStruct())
      return 2; // struct
    return 4;   // class
  }
  case Decl::Enum:             return 5;  // enum
  case Decl::Field:            return 6;  // member (field)
  case Decl::EnumConstant:     return 7;  // enum-constant
  case Decl::Function:         return 8;  // function
  case Decl::Var:              return 9;  // variable
  case Decl::Typedef:          return 20; // typedef
  case Decl::CXXMethod:        return 21; // method
  case Decl::Namespace:        return 22; // namespace
  case Decl::CXXConstructor:   return 24; // constructor
  case Decl::CXXDestructor:    return 25; // destructor
  case Decl::FunctionTemplate: return 30; // function-template
  case Decl::ClassTemplate:    return 31; // class-template
  case Decl::TypeAlias:        return 36; // type-alias
  default:                     return -1;
  }
}

const char *cidx_kind_name_from_int(int kind) {
  switch (kind) {
  case 2:  return "struct";
  case 3:  return "union";
  case 4:  return "class";
  case 5:  return "enum";
  case 6:  return "member";
  case 7:  return "enum-constant";
  case 8:  return "function";
  case 9:  return "variable";
  case 20: return "typedef";
  case 21: return "method";
  case 22: return "namespace";
  case 24: return "constructor";
  case 25: return "destructor";
  case 30: return "function-template";
  case 31: return "class-template";
  case 36: return "type-alias";
  default: return nullptr;
  }
}

const char *cidx_symbol_kind_name(const clang::Decl *decl) {
  // Same mapping as cidx_symbol_kind, by storage NAME (kind_name in
  // ast_cursor.cpp; symbol_kind seed in storage.cpp).
  return cidx_kind_name_from_int(cidx_symbol_kind(decl));
}

const char *cidx_stub_kind_name(const clang::Decl *decl) {
  const char *name = cidx_symbol_kind_name(decl);
  return name != nullptr ? name : "function";
}

} // namespace cidx::lt
