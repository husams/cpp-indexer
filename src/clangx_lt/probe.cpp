// Phase-0 LibTooling probe for the cidx clangx-LT redesign.
//
// Purpose: prove the Clang C++ API (LibTooling) compiles, links (clang-cpp),
// and runs against the installed LLVM, producing USR-keyed symbols whose USRs
// match libclang's clang_getCursorUSR (they share generateUSRForDecl). This is
// a throwaway de-risking tool, NOT the real extraction layer.
//
// Build:  cmake -DCIDX_CLANGX_LIBTOOLING=ON -DCMAKE_PREFIX_PATH="$(brew --prefix llvm)" ...
// Run:    cidx_lt_probe -p <dir-with-compile_commands.json> <file>
// Output: one line per emitted decl:  <USR>\t<kind-int>\t<qualified-name>
//
// kind-int mirrors the schema's CXCursorKind integers (frozen 17-entry map in
// storage.cpp) so output is directly diff-able against the current symbol pass.

#include "clang/AST/ASTConsumer.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendAction.h"
#include "clang/Index/USRGeneration.h"
#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"

#include <string>

using namespace clang;
using namespace clang::tooling;

namespace {

// Map clang::Decl kinds -> the CXCursorKind integers cidx stores in symbol.kind
// (see storage.cpp:463-485). Returns -1 for decls cidx does not persist, so the
// probe drops exactly what to_symbol() drops.
int cidxKind(const Decl *D) {
  switch (D->getKind()) {
  case Decl::CXXRecord:
  case Decl::Record: {
    const auto *RD = cast<RecordDecl>(D);
    if (RD->isUnion())
      return 3;                       // union
    if (RD->isStruct())
      return 2;                       // struct
    return 4;                         // class
  }
  case Decl::Enum:            return 5;    // enum
  case Decl::Field:           return 6;    // member (field)
  case Decl::EnumConstant:    return 7;    // enum-constant
  case Decl::Function:        return 8;    // function
  case Decl::Var:             return 9;    // variable
  case Decl::Typedef:         return 20;   // typedef
  case Decl::CXXMethod:       return 21;   // method
  case Decl::Namespace:       return 22;   // namespace
  case Decl::CXXConstructor:  return 24;   // constructor
  case Decl::CXXDestructor:   return 25;   // destructor
  case Decl::FunctionTemplate:return 30;   // function-template
  case Decl::ClassTemplate:   return 31;   // class-template
  case Decl::TypeAlias:       return 36;   // type-alias
  default:                    return -1;
  }
}

std::string usrOf(const Decl *D) {
  SmallString<128> Buf;
  if (index::generateUSRForDecl(D, Buf))
    return {};                        // generation failed
  return std::string(Buf);
}

class ProbeVisitor : public RecursiveASTVisitor<ProbeVisitor> {
public:
  ProbeVisitor(ASTContext &Ctx, StringRef MainFile)
      : Ctx(Ctx), SM(Ctx.getSourceManager()), MainFile(MainFile) {}

  bool VisitNamedDecl(NamedDecl *D) {
    // cidx only indexes decls whose expansion location is the target file.
    const SourceLocation Loc = SM.getExpansionLoc(D->getLocation());
    if (Loc.isInvalid())
      return true;
    if (SM.getFilename(Loc) != MainFile)
      return true;
    const int kind = cidxKind(D);
    if (kind < 0)
      return true;
    const std::string usr = usrOf(D);
    if (usr.empty())
      return true;
    llvm::outs() << usr << '\t' << kind << '\t'
                 << D->getQualifiedNameAsString() << '\n';
    return true;
  }

private:
  ASTContext &Ctx;
  SourceManager &SM;
  std::string MainFile;
};

class ProbeConsumer : public ASTConsumer {
public:
  explicit ProbeConsumer(StringRef MainFile) : MainFile(MainFile) {}
  void HandleTranslationUnit(ASTContext &Ctx) override {
    ProbeVisitor V(Ctx, MainFile);
    V.TraverseDecl(Ctx.getTranslationUnitDecl());
  }

private:
  std::string MainFile;
};

class ProbeAction : public ASTFrontendAction {
public:
  std::unique_ptr<ASTConsumer> CreateASTConsumer(CompilerInstance &CI,
                                                 StringRef InFile) override {
    return std::make_unique<ProbeConsumer>(InFile);
  }
};

llvm::cl::OptionCategory ProbeCategory("cidx-lt-probe options");

} // namespace

int main(int argc, const char **argv) {
  auto ExpectedParser = CommonOptionsParser::create(argc, argv, ProbeCategory);
  if (!ExpectedParser) {
    llvm::errs() << ExpectedParser.takeError();
    return 1;
  }
  CommonOptionsParser &OP = ExpectedParser.get();
  ClangTool Tool(OP.getCompilations(), OP.getSourcePathList());
  return Tool.run(newFrontendActionFactory<ProbeAction>().get());
}
