#include "clangx_lt/kind_map.hpp"

#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"

namespace cidx::lt {

int cidx_symbol_kind(const clang::Decl *decl) {
  using clang::Decl;
  switch (decl->getKind()) {
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

} // namespace cidx::lt
