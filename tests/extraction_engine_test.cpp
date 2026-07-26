// extraction_engine_test — HSE-64: execute a validated ExtractionPlan against
// a REAL parsed translation unit (Clang C++ API throughout) -> "clang" label.
//
// Covers: acceptance-criterion examples (positive/negative/unknown banking
// application-state boundary, a logging-boundary relation, an explicit
// "unknown" finding distinct from silent absence), as-is vs
// ignore-unless-spelled traversal divergence, determinism/byte-identical
// re-run, budget enforcement, and refusal to execute an unvalidated plan.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "analysis/facts.hpp"
#include "extract/artifact.hpp"
#include "extract/engine.hpp"
#include "extract/extension_facts.hpp"
#include "extract/plan_identity.hpp"
#include "extract/plan_ir.hpp"
#include "storage/sqlite.hpp"
#include "storage/storage.hpp"

#include "clang/AST/ASTConsumer.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendAction.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Tooling/Tooling.h"

#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

using namespace cidx::extract;

namespace {

struct RunResult {
  ExecutionReport report;
  InMemoryExtensionFactSink sink;
  std::optional<std::string> plan_not_validated_message;
};

class PlanConsumer final : public clang::ASTConsumer {
public:
  PlanConsumer(const ExtractionPlan &plan, clang::Preprocessor &preprocessor,
               const ExecutionInput &input, const ExecutionOptions &options,
               RunResult &result)
      : plan_(plan), preprocessor_(preprocessor), input_(input),
        options_(options), result_(result) {}

  void HandleTranslationUnit(clang::ASTContext &context) override {
    try {
      result_.report = execute_plan(plan_, context, preprocessor_, result_.sink,
                                    input_, options_);
      result_.sink.canonicalize();
    } catch (const PlanNotValidated &ex) {
      result_.plan_not_validated_message = ex.what();
    }
  }

private:
  const ExtractionPlan &plan_;
  clang::Preprocessor &preprocessor_;
  const ExecutionInput &input_;
  const ExecutionOptions &options_;
  RunResult &result_;
};

class PlanAction final : public clang::ASTFrontendAction {
public:
  PlanAction(const ExtractionPlan &plan, const ExecutionInput &input,
             const ExecutionOptions &options, RunResult &result)
      : plan_(plan), input_(input), options_(options), result_(result) {}

  std::unique_ptr<clang::ASTConsumer>
  CreateASTConsumer(clang::CompilerInstance &compiler,
                    llvm::StringRef /*in_file*/) override {
    return std::make_unique<PlanConsumer>(plan_, compiler.getPreprocessor(),
                                          input_, options_, result_);
  }

private:
  const ExtractionPlan &plan_;
  const ExecutionInput &input_;
  const ExecutionOptions &options_;
  RunResult &result_;
};

RunResult run_plan(const ExtractionPlan &plan, const std::string &code,
                   const ExecutionInput &input = {},
                   const ExecutionOptions &options = {}) {
  RunResult result;
  const bool ran = clang::tooling::runToolOnCode(
      std::make_unique<PlanAction>(plan, input, options, result), code,
      "test.cpp");
  REQUIRE(ran);
  return result;
}

// Like run_plan(), but maps additional virtual files (e.g. a header) so
// main_file-vs-translation_unit scope has something to actually distinguish.
RunResult run_plan_with_files(const ExtractionPlan &plan,
                              const std::string &code,
                              const clang::tooling::FileContentMappings &files,
                              const ExecutionInput &input = {}) {
  RunResult result;
  const ExecutionOptions options;
  const bool ran = clang::tooling::runToolOnCodeWithArgs(
      std::make_unique<PlanAction>(plan, input, options, result), code,
      {"-std=c++20"}, "test.cpp", "clang-tool",
      std::make_shared<clang::PCHContainerOperations>(), files);
  REQUIRE(ran);
  return result;
}

// Like run_plan(), but with extra compiler invocation arguments appended
// after "-std=c++20" (e.g. -I/some/path) -- used to prove the tool/config
// applicability fingerprint reacts to the invocation itself, not merely to
// which files it happened to resolve.
RunResult run_plan_with_args(const ExtractionPlan &plan,
                             const std::string &code,
                             const std::vector<std::string> &extra_args) {
  RunResult result;
  const ExecutionInput input;
  const ExecutionOptions options;
  std::vector<std::string> args = {"-std=c++20"};
  args.insert(args.end(), extra_args.begin(), extra_args.end());
  const bool ran = clang::tooling::runToolOnCodeWithArgs(
      std::make_unique<PlanAction>(plan, input, options, result), code, args,
      "test.cpp");
  REQUIRE(ran);
  return result;
}

// `depth` nested struct DEFINITIONS (R0 contains R1 contains R2 ... contains
// a leaf field), not `depth` sibling structs: a nested TagDecl is a child in
// its enclosing RecordDecl's own DeclContext regardless of whether any data
// member of that nested type exists, so hasDescendant() searching from R0
// must walk all the way down through every level. Used to reproduce the
// round-4 review's "a single (non-nested) hasDescendant performs quadratic
// work over N nested records" finding.
std::string nested_records_source(int depth) {
  std::string source;
  for (int i = 0; i < depth; ++i) {
    source += "struct R" + std::to_string(i) + " {\n";
  }
  source += "  int leaf_marker;\n";
  for (int i = 0; i < depth; ++i) {
    source += "};\n";
  }
  return source;
}

// `struct_count` structs, each with a constructor whose SINGLE
// member-initializer hides a chain of `chain_length` calls
// (`x(leaf_value() + leaf_value() + ... + leaf_value())`).
// CXXConstructorDecl::inits() is a list CXXCtorInitializer/getInit() exprs
// live in that is entirely separate from both DeclContext::decls() (which
// never contains member-initializer expressions) and the -- here empty --
// function body, so a NodeBudgetCounter that never walks it undercounts the
// TU by roughly struct_count * (2 * chain_length - 1) real AST nodes no
// matter how long the hidden chain runs.
std::string ctor_initializer_hidden_calls_source(int struct_count,
                                                 int chain_length) {
  std::string source = "int leaf_value();\n";
  for (int i = 0; i < struct_count; ++i) {
    const std::string name = "S" + std::to_string(i);
    source += "struct " + name + " {\n";
    source += "  int x;\n";
    source += "  " + name + "() : x(";
    for (int j = 0; j < chain_length; ++j) {
      if (j > 0) {
        source += " + ";
      }
      source += "leaf_value()";
    }
    source += ") {}\n";
    source += "};\n";
  }
  return source;
}

// `enum_count` scoped enums, each with a SINGLE enumerator whose initializer
// hides a chain of `chain_length` calls (`A = leaf_value() + ... +
// leaf_value()`). clang::EnumConstantDecl does not derive from
// clang::VarDecl (it derives directly from clang::ValueDecl), so a
// NodeBudgetCounter that only special-cases VarDecl/FieldDecl/FunctionDecl
// never walks EnumConstantDecl::getInitExpr() even though `enumDecl` is
// itself an allow-listed matcher_catalog.cpp node and
// RecursiveASTVisitor::TraverseEnumConstantDecl walks the initializer
// unconditionally.
std::string enum_constant_hidden_calls_source(int enum_count,
                                              int chain_length) {
  // The enumerator value must be a constant expression, so leaf_value()
  // needs a constexpr-evaluable body rather than a bare declaration -- the
  // call itself is still a real CallExpr node in the AST either way.
  std::string source = "constexpr int leaf_value() { return 1; }\n";
  for (int i = 0; i < enum_count; ++i) {
    const std::string name = "E" + std::to_string(i);
    source += "enum class " + name + " {\n  A = ";
    for (int j = 0; j < chain_length; ++j) {
      if (j > 0) {
        source += " + ";
      }
      source += "leaf_value()";
    }
    source += "\n};\n";
  }
  return source;
}

// `function_count` functions, each with a trailing decltype(...) return type
// whose operand hides a chain of `chain_length` calls
// (`decltype((leaf_value() + ... + leaf_value(), 0))`). Neither decls() nor
// getBody() ever surfaces a decltype operand -- it lives on the function's
// declared return type, which RecursiveASTVisitor's
// TraverseDecltypeTypeLoc walks as part of traversing that type.
std::string decltype_return_type_hidden_calls_source(int function_count,
                                                     int chain_length) {
  std::string source = "int leaf_value();\n";
  for (int i = 0; i < function_count; ++i) {
    const std::string name = "target" + std::to_string(i);
    source += "auto " + name + "() -> decltype((";
    for (int j = 0; j < chain_length; ++j) {
      if (j > 0) {
        source += " + ";
      }
      source += "leaf_value()";
    }
    source += ", 0));\n";
  }
  return source;
}

// `function_count` functions, each with a SINGLE parameter whose default
// argument hides a chain of `chain_length` calls
// (`void targetN(int x = leaf_value() + ... + leaf_value());`).
// clang::ParmVarDecl::getDefaultArg() lives on the parameter, and a
// parameter is never linked into its owning FunctionDecl's own decls()
// chain (RecursiveASTVisitor instead reaches it while traversing the
// function's FunctionProtoTypeLoc), so a NodeBudgetCounter that only
// enumerates decls() never sees it without an explicit parameters() walk.
std::string parameter_default_argument_hidden_calls_source(int function_count,
                                                           int chain_length) {
  std::string source = "int leaf_value();\n";
  for (int i = 0; i < function_count; ++i) {
    const std::string name = "target" + std::to_string(i);
    source += "void " + name + "(int x = ";
    for (int j = 0; j < chain_length; ++j) {
      if (j > 0) {
        source += " + ";
      }
      source += "leaf_value()";
    }
    source += ");\n";
  }
  return source;
}

// `function_count` functions, each with a noexcept(expr) specifier hiding a
// chain of `chain_length` calls
// (`void targetN() noexcept(leaf_value() + ... + leaf_value() >= 0);`). A
// noexcept-specifier's expression must itself be a converted constant
// expression, so leaf_value() needs a constexpr-evaluable body (the call is
// still a real CallExpr node in the AST). The expression is carried on the
// function's clang::FunctionProtoType, not reachable through decls() or the
// (here absent) body.
std::string noexcept_specifier_hidden_calls_source(int function_count,
                                                   int chain_length) {
  std::string source = "constexpr int leaf_value() { return 1; }\n";
  for (int i = 0; i < function_count; ++i) {
    const std::string name = "target" + std::to_string(i);
    source += "void " + name + "() noexcept(";
    for (int j = 0; j < chain_length; ++j) {
      if (j > 0) {
        source += " + ";
      }
      source += "leaf_value()";
    }
    source += " >= 0);\n";
  }
  return source;
}

// `template_count` class templates, each declaring a SINGLE method whose
// body hides `chain_length` calls
// (`template <typename T> struct BoxN { void method() { leaf_value(); ...
// } };`). clang::ClassTemplateDecl is never itself a clang::DeclContext --
// the templated clang::CXXRecordDecl it wraps is -- so without walking
// tmpl->getTemplatedDecl(), the ENTIRE primary template pattern (including
// every method body) is invisible to a counter that only enumerates
// DeclContext::decls() reachable from the translation unit.
std::string class_template_body_hidden_calls_source(int template_count,
                                                    int chain_length) {
  std::string source = "void leaf_value();\n";
  for (int i = 0; i < template_count; ++i) {
    const std::string name = "Box" + std::to_string(i);
    source +=
        "template <typename T> struct " + name + " {\n  void method() {\n";
    for (int j = 0; j < chain_length; ++j) {
      source += "    leaf_value();\n";
    }
    source += "  }\n};\n";
  }
  return source;
}

std::string make_temp_dir() {
  std::string tmpl = "/tmp/cidx_extract_artifact_XXXXXX";
  std::vector<char> buffer(tmpl.begin(), tmpl.end());
  buffer.push_back('\0');
  char *path = ::mkdtemp(buffer.data());
  REQUIRE(path != nullptr);
  return path;
}

RuleBudget generous_budget() {
  return RuleBudget{.max_matches = 1000,
                    .max_emitted_facts = 1000,
                    .max_visited_nodes = 100000,
                    .declared = true};
}

ExtractionRule logging_boundary_rule() {
  ExtractionRule rule;
  rule.id = "audit.logs_to";
  rule.matcher_expression =
      "callExpr(callee(functionDecl(hasName(\"log_audit_event\")).bind("
      "\"callee\"))).bind(\"call\")";
  rule.bindings = {
      Binding{.name = "call", .domain = EndpointDomain::expression},
      Binding{.name = "callee", .domain = EndpointDomain::declaration}};
  EmitOperation emit;
  emit.relation = EmitRelation{.namespace_name = "audit",
                               .relation_kind = "logs_to",
                               .from_binding = "call",
                               .to_binding = "callee",
                               .with_evidence = true};
  rule.emits = {emit};
  rule.budget = generous_budget();
  rule.producer_package = "banking.audit";
  rule.producer_version = 1;
  return rule;
}

ExtractionRule dispatch_unknown_rule() {
  ExtractionRule rule;
  rule.id = "audit.unclassified_dispatch";
  rule.matcher_expression =
      "callExpr(callee(functionDecl(hasName(\"dispatch\")).bind(\"target\"))"
      ").bind(\"call\")";
  rule.bindings = {
      Binding{.name = "call", .domain = EndpointDomain::expression},
      Binding{.name = "target", .domain = EndpointDomain::declaration}};
  EmitOperation emit;
  emit.unknown = EmitUnknown{.namespace_name = "audit",
                             .reason_code = "unclassified_dispatch",
                             .binding = "call"};
  rule.emits = {emit};
  rule.completeness = DeclaredCompleteness::unknown_capable;
  rule.budget = generous_budget();
  rule.producer_package = "banking.audit";
  rule.producer_version = 1;
  return rule;
}

ExtractionRule appstate_boundary_rule() {
  ExtractionRule rule;
  rule.id = "banking.appstate.boundary";
  rule.matcher_expression =
      "cxxRecordDecl(hasDescendant(cxxMethodDecl(hasName(\"commit\"))), "
      "hasDescendant(cxxMethodDecl(hasName(\"rollback\")))).bind(\"record\")";
  rule.bindings = {
      Binding{.name = "record", .domain = EndpointDomain::declaration}};
  EmitOperation emit;
  emit.node = EmitNode{
      .namespace_name = "banking.appstate",
      .node_kind = "boundary",
      .binding = "record",
      .identity = IdentityRecipe{.kind = IdentityKind::usr, .components = {}}};
  rule.emits = {emit};
  rule.budget = generous_budget();
  rule.producer_package = "banking.appstate";
  rule.producer_version = 1;
  return rule;
}

ExtractionRule appstate_unverifiable_rule() {
  ExtractionRule rule;
  rule.id = "banking.appstate.unverifiable_registration";
  rule.matcher_expression = "cxxRecordDecl(hasDescendant(cxxMethodDecl(hasName("
                            "\"register_as_boundary\")))).bind(\"record\")";
  rule.bindings = {
      Binding{.name = "record", .domain = EndpointDomain::declaration}};
  EmitOperation emit;
  emit.unknown =
      EmitUnknown{.namespace_name = "banking.appstate",
                  .reason_code = "boundary_registration_unverifiable",
                  .binding = "record"};
  rule.emits = {emit};
  rule.completeness = DeclaredCompleteness::unknown_capable;
  rule.budget = generous_budget();
  rule.producer_package = "banking.appstate";
  rule.producer_version = 1;
  return rule;
}

ExtractionPlan plan_with(std::vector<ExtractionRule> rules) {
  ExtractionPlan plan;
  plan.plan_id = "banking.audit.plan";
  plan.catalog_versions = {1};
  plan.rules = std::move(rules);
  return plan;
}

} // namespace

TEST_SUITE("clang") {

  TEST_CASE("a rule binds a call/declaration and emits one namespaced relation "
            "with source evidence") {
    ExtractionPlan plan = plan_with({logging_boundary_rule()});
    RunResult result =
        run_plan(plan, "void log_audit_event(const char *msg);\n"
                       "void do_transfer() { log_audit_event(\"t\"); "
                       "}\n");
    REQUIRE(result.sink.relations().size() == 1);
    const auto &fact = result.sink.relations()[0];
    CHECK(fact.namespace_name == "audit");
    CHECK(fact.relation_kind == "logs_to");
    // runToolOnCode resolves the virtual file through the real filesystem
    // path conventions of the host SourceManager, so only the basename is
    // pinned here (matches ast_pass_test's treatment of the same fixture).
    CHECK(fact.evidence.file.ends_with("test.cpp"));
    CHECK(fact.evidence.line == 2);
    CHECK(fact.provenance.completeness == DeclaredCompleteness::complete);
  }

  TEST_CASE("a near-miss call site produces no relation fact") {
    ExtractionPlan plan = plan_with({logging_boundary_rule()});
    RunResult result =
        run_plan(plan, "void log_debug_event(const char *msg);\n"
                       "void do_transfer() { log_debug_event(\"t\"); "
                       "}\n");
    CHECK(result.sink.relations().empty());
  }

  TEST_CASE("a deliberately unclassifiable call is reported unknown, not "
            "silently dropped") {
    ExtractionPlan plan = plan_with({dispatch_unknown_rule()});
    RunResult result = run_plan(plan, "void dispatch(int code);\n"
                                      "void run() { dispatch(1); }\n");
    REQUIRE(result.sink.unknowns().size() == 1);
    CHECK(result.sink.unknowns()[0].reason_code == "unclassified_dispatch");
    CHECK(result.sink.unknowns()[0].provenance.completeness ==
          DeclaredCompleteness::unknown_capable);
  }

  TEST_CASE("banking example: positive boundary recognized, near-miss absent, "
            "unsupported case reported unknown") {
    ExtractionPlan plan =
        plan_with({appstate_boundary_rule(), appstate_unverifiable_rule()});
    RunResult result = run_plan(plan, "struct AccountLedger {\n"
                                      "  void commit();\n"
                                      "  void rollback();\n"
                                      "};\n"
                                      "struct ReadOnlyLedger {\n"
                                      "  void commit();\n"
                                      "};\n"
                                      "struct PluginLedger {\n"
                                      "  void register_as_boundary();\n"
                                      "};\n");
    REQUIRE(result.sink.nodes().size() == 1);
    CHECK(result.sink.nodes()[0].node_kind == "boundary");
    CHECK(result.sink.nodes()[0].identity.find("AccountLedger") !=
          std::string::npos);

    REQUIRE(result.sink.unknowns().size() == 1);
    CHECK(result.sink.unknowns()[0].reason_code ==
          "boundary_registration_unverifiable");
    CHECK(result.sink.unknowns()[0].identity.find("PluginLedger") !=
          std::string::npos);
  }

  TEST_CASE("as-is and ignore-unless-spelled traversal disagree on an implicit "
            "constructor call") {
    const std::string code = "struct Counter { Counter() {} };\n"
                             "struct Holder { Counter c; };\n"
                             "void make() { Holder h; }\n";
    ExtractionRule ctor_rule;
    ctor_rule.id = "trace.ctor_calls";
    ctor_rule.matcher_expression = "cxxConstructExpr().bind(\"call\")";
    ctor_rule.bindings = {
        Binding{.name = "call", .domain = EndpointDomain::expression}};
    EmitOperation emit;
    emit.unknown = EmitUnknown{.namespace_name = "trace",
                               .reason_code = "ctor_call",
                               .binding = "call"};
    ctor_rule.emits = {emit};
    ctor_rule.completeness = DeclaredCompleteness::unknown_capable;
    ctor_rule.budget = generous_budget();
    ctor_rule.producer_package = "trace";
    ctor_rule.producer_version = 1;

    ExtractionRule as_is_rule = ctor_rule;
    as_is_rule.traversal = TraversalMode::as_is;
    ExtractionRule spelled_rule = ctor_rule;
    spelled_rule.id = "trace.ctor_calls.spelled";
    spelled_rule.traversal = TraversalMode::ignore_unless_spelled;

    RunResult as_is_result = run_plan(plan_with({as_is_rule}), code);
    RunResult spelled_result = run_plan(plan_with({spelled_rule}), code);

    const auto *as_is_stats = as_is_result.report.find("trace.ctor_calls");
    const auto *spelled_stats =
        spelled_result.report.find("trace.ctor_calls.spelled");
    REQUIRE(as_is_stats != nullptr);
    REQUIRE(spelled_stats != nullptr);
    // as_is sees the compiler-generated member/base construct calls that
    // ignore_unless_spelled deliberately skips (they have no direct source
    // spelling), so it must see strictly more constructor-call matches.
    CHECK(as_is_stats->matches > spelled_stats->matches);
  }

  TEST_CASE("re-running an unchanged plan against an unchanged TU produces a "
            "byte-identical canonical fact batch") {
    ExtractionPlan plan =
        plan_with({logging_boundary_rule(), appstate_boundary_rule()});
    const std::string code =
        "void log_audit_event(const char *msg);\n"
        "void do_transfer() { log_audit_event(\"t\"); }\n"
        "struct AccountLedger { void commit(); void rollback(); };\n";
    RunResult first = run_plan(plan, code);
    RunResult second = run_plan(plan, code);
    CHECK(first.sink.canonical_text() == second.sink.canonical_text());
    CHECK(first.report.plan_hash == second.report.plan_hash);
  }

  TEST_CASE("a rule budget bounds matches and emitted facts") {
    ExtractionRule rule = logging_boundary_rule();
    rule.budget = RuleBudget{.max_matches = 1,
                             .max_emitted_facts = 1,
                             .max_visited_nodes = 100000,
                             .declared = true};
    ExtractionPlan plan = plan_with({rule});
    RunResult result = run_plan(plan, "void log_audit_event(const char *msg);\n"
                                      "void a() { log_audit_event(\"1\"); }\n"
                                      "void b() { log_audit_event(\"2\"); }\n"
                                      "void c() { log_audit_event(\"3\"); }\n");
    const auto *stats = result.report.find(rule.id);
    REQUIRE(stats != nullptr);
    CHECK(stats->budget_exhausted);
    CHECK(result.sink.relations().size() <= 1);
  }

  TEST_CASE("execute_plan refuses to run a plan that fails validation and "
            "emits nothing") {
    ExtractionRule rule = logging_boundary_rule();
    rule.budget.declared = false; // invalid: budgets are mandatory
    ExtractionPlan plan = plan_with({rule});
    RunResult result = run_plan(plan, "void log_audit_event(const char *msg);\n"
                                      "void f() { log_audit_event(\"t\"); }\n");
    REQUIRE(result.plan_not_validated_message.has_value());
    CHECK(result.sink.nodes().empty());
    CHECK(result.sink.relations().empty());
    CHECK(result.sink.attributes().empty());
    CHECK(result.sink.unknowns().empty());
  }

  // --- PR #66 review regression coverage ------------------------------

  TEST_CASE("execute_plan refuses a binding declared as the wrong domain "
            "for the node its matcher actually constructs (PR #66 review "
            "repro: callExpr().bind(\"x\") declared as declaration)") {
    ExtractionRule rule;
    rule.id = "audit.bad_domain";
    rule.matcher_expression = "callExpr().bind(\"x\")";
    rule.bindings = {
        Binding{.name = "x", .domain = EndpointDomain::declaration}};
    EmitOperation emit;
    emit.relation = EmitRelation{.namespace_name = "audit",
                                 .relation_kind = "self",
                                 .from_binding = "x",
                                 .to_binding = "x",
                                 .with_evidence = true};
    rule.emits = {emit};
    rule.budget = generous_budget();
    rule.producer_package = "audit";
    rule.producer_version = 1;
    ExtractionPlan plan = plan_with({rule});
    RunResult result = run_plan(plan, "void log_audit_event(const char *msg);\n"
                                      "void f() { log_audit_event(\"t\"); }\n");
    REQUIRE(result.plan_not_validated_message.has_value());
    CHECK(result.sink.relations().empty());
  }

  TEST_CASE("artifact_identity changes when the TU source changes even "
            "though the plan and matched facts look identical (PR #66 "
            "review repro: two different function bodies, same location)") {
    ExtractionPlan plan = plan_with({logging_boundary_rule()});
    RunResult first = run_plan(
        plan, "void log_audit_event(const char *msg);\n"
              "void do_transfer() { log_audit_event(\"t\"); int a = 1; }\n");
    RunResult second = run_plan(
        plan, "void log_audit_event(const char *msg);\n"
              "void do_transfer() { log_audit_event(\"t\"); int a = 2; }\n");
    // Same plan -> same plan_hash; the call site/callee identity is
    // unaffected by the unrelated local, so both runs match the same
    // relation -- but they must NOT be mistaken for the same artifact.
    REQUIRE(first.report.plan_hash == second.report.plan_hash);
    REQUIRE(first.sink.relations().size() == 1);
    REQUIRE(second.sink.relations().size() == 1);
    CHECK(first.report.artifact_identity != second.report.artifact_identity);
    CHECK(first.sink.relations()[0].provenance.artifact_identity !=
          second.sink.relations()[0].provenance.artifact_identity);
  }

  TEST_CASE("main_file scope excludes matches from an included header; "
            "translation_unit scope includes them") {
    ExtractionRule base_rule;
    base_rule.matcher_expression = "functionDecl(isDefinition()).bind(\"fn\")";
    base_rule.bindings = {
        Binding{.name = "fn", .domain = EndpointDomain::declaration}};
    EmitOperation emit;
    emit.unknown = EmitUnknown{
        .namespace_name = "scope", .reason_code = "fn_seen", .binding = "fn"};
    base_rule.emits = {emit};
    base_rule.completeness = DeclaredCompleteness::unknown_capable;
    base_rule.budget = generous_budget();
    base_rule.producer_package = "scope";
    base_rule.producer_version = 1;

    ExtractionRule main_file_rule = base_rule;
    main_file_rule.id = "scope.main_file";
    main_file_rule.scope = PlanScope::main_file;

    ExtractionRule tu_rule = base_rule;
    tu_rule.id = "scope.translation_unit";
    tu_rule.scope = PlanScope::translation_unit;

    const std::string code = "#include \"header.h\"\nvoid in_main() {}\n";
    const clang::tooling::FileContentMappings files = {
        {"header.h", "void in_header() {}\n"}};

    RunResult main_file_result =
        run_plan_with_files(plan_with({main_file_rule}), code, files);
    RunResult tu_result =
        run_plan_with_files(plan_with({tu_rule}), code, files);

    CHECK(main_file_result.sink.unknowns().size() == 1);
    CHECK(tu_result.sink.unknowns().size() == 2);
  }

  TEST_CASE(
      "max_visited_nodes interrupts before any match is attempted on a "
      "multi-node TU (PR #66 review repro: budget=1 on a multi-node TU)") {
    ExtractionRule rule = logging_boundary_rule();
    rule.budget = RuleBudget{.max_matches = 1000,
                             .max_emitted_facts = 1000,
                             .max_visited_nodes = 1,
                             .declared = true};
    ExtractionPlan plan = plan_with({rule});
    RunResult result = run_plan(plan, "void log_audit_event(const char *msg);\n"
                                      "void f() { log_audit_event(\"t\"); }\n");
    const auto *stats = result.report.find(rule.id);
    REQUIRE(stats != nullptr);
    CHECK(stats->budget_exhausted);
    CHECK(stats->matches == 0);
    CHECK(result.sink.relations().empty());
    bool found_diagnostic = false;
    for (const auto &diagnostic : result.report.diagnostics) {
      found_diagnostic =
          found_diagnostic || diagnostic.code == "visited_node_budget_exceeded";
    }
    CHECK(found_diagnostic);
  }

  TEST_CASE("publish_extension_artifact registers a content-addressed "
            "artifact that is current, valid, and readable back") {
    ExtractionPlan plan = plan_with({logging_boundary_rule()});
    // The pinned workspace/TU identity flows into execute_plan()'s
    // ExecutionInput; publish_extension_artifact() reads it back from the
    // resulting report rather than from an independent request field (PR
    // #66 review: "the new test even executes with empty identities and
    // then publishes as workspace:test/tu:test").
    RunResult result =
        run_plan(plan,
                 "void log_audit_event(const char *msg);\n"
                 "void f() { log_audit_event(\"t\"); }\n",
                 ExecutionInput{.workspace_identity = "workspace:test",
                                .tu_identity = "tu:test"});
    REQUIRE(result.sink.relations().size() == 1);
    REQUIRE(result.report.workspace_identity == "workspace:test");
    REQUIRE(result.report.tu_identity == "tu:test");

    const std::string root = make_temp_dir();
    cidx::Storage storage(root + "/index.sqlite");
    PublicationRequest request;
    request.artifact_root = root + "/artifacts";
    request.namespace_name = "banking.audit";

    const ExtensionPublication publication = publish_extension_artifact(
        storage, request, plan, result.report, result.sink);

    REQUIRE(publication.logical_id == "extension:banking.audit");
    CHECK(publication.artifact_identity == result.report.artifact_identity);

    cidx::ArtifactStore artifacts(storage, request.artifact_root);
    const auto current = artifacts.current(publication.logical_id);
    REQUIRE(current.has_value());
    if (current.has_value()) {
      CHECK(current->content_hash == publication.content_hash);
      CHECK(current->spec.configuration_identity ==
            result.report.artifact_identity);
    }
    CHECK(artifacts.validate(publication.logical_id).usable());

    // Queryable through the existing platform adapter, with no new
    // provider code: read the published sidecar's own relation table
    // directly, the same way HSE-66's ExtensionFactProvider would.
    const auto published_path = std::filesystem::path(request.artifact_root) /
                                publication.relative_path;
    cidx::SqliteDb published_db(published_path.string(), true,
                                cidx::SqliteProfile::read_only_replay);
    auto rows = published_db.prepare(
        "SELECT kind_name, identity, secondary FROM extension WHERE "
        "fact_kind = 'relation'");
    REQUIRE(rows.step());
    CHECK(rows.col_text(0) == "logs_to");
  }

  // --- PR #66 review round 2 regression coverage ------------------------

  TEST_CASE("artifact_identity changes when an included header's content "
            "changes, even though the main file is unchanged (round-2 "
            "repro: the fingerprint previously covered only main-file "
            "bytes, target triple, and LangStd)") {
    ExtractionPlan plan = plan_with({logging_boundary_rule()});
    const std::string code = "#include \"header.h\"\n"
                             "void log_audit_event(const char *msg);\n"
                             "void do_transfer() { log_audit_event(\"t\"); "
                             "}\n";
    RunResult first =
        run_plan_with_files(plan, code, {{"header.h", "// version 1\n"}});
    RunResult second = run_plan_with_files(
        plan, code, {{"header.h", "// version 2, different content\n"}});
    REQUIRE(first.report.plan_hash == second.report.plan_hash);
    CHECK(first.report.artifact_identity != second.report.artifact_identity);
  }

  TEST_CASE(
      "estimated matcher-evaluation work (visited nodes x traversal "
      "combinators) is bounded even when the raw AST alone fits "
      "max_visited_nodes (round-2 repro: nested hasDescendant/hasAncestor "
      "re-traverse the TU during matchAST, which a one-time AST-size check "
      "cannot see)") {
    ExtractionRule rule;
    rule.id = "audit.descendant_bound";
    rule.matcher_expression =
        "cxxRecordDecl(allOf(hasDescendant(cxxMethodDecl(hasName(\"a\"))), "
        "hasDescendant(cxxMethodDecl(hasName(\"b\"))), "
        "hasDescendant(cxxMethodDecl(hasName(\"c\"))), "
        "hasDescendant(cxxMethodDecl(hasName(\"d\"))), "
        "hasDescendant(cxxMethodDecl(hasName(\"e\"))))).bind(\"record\")";
    rule.bindings = {
        Binding{.name = "record", .domain = EndpointDomain::declaration}};
    EmitOperation emit;
    emit.unknown = EmitUnknown{
        .namespace_name = "audit", .reason_code = "seen", .binding = "record"};
    rule.emits = {emit};
    rule.completeness = DeclaredCompleteness::unknown_capable;
    // 5 repeated-subtree-traversal combinators -> effective threshold is
    // max_visited_nodes / 6 = 10. Generous enough (60) that a plain
    // one-time AST-size check would not reject this small fixture, but the
    // combinator-multiplied estimate is exceeded by its function bodies.
    rule.budget = RuleBudget{.max_matches = 1000,
                             .max_emitted_facts = 1000,
                             .max_visited_nodes = 60,
                             .declared = true};
    rule.producer_package = "audit";
    rule.producer_version = 1;
    ExtractionPlan plan = plan_with({rule});

    RunResult result =
        run_plan(plan, "struct AccountLedger {\n"
                       "  void commit() {\n"
                       "    int a=1; int b=2; int c=3; int d=4; int e=5;\n"
                       "    int f=6; int g=7; int h=8; int i=9; int j=10;\n"
                       "  }\n"
                       "  void rollback() {}\n"
                       "};\n");
    const auto *stats = result.report.find(rule.id);
    REQUIRE(stats != nullptr);
    CHECK(stats->budget_exhausted);
    CHECK(stats->matches == 0);
    CHECK(result.sink.unknowns().empty());
  }

  TEST_CASE("hard_output_cap bounds the TOTAL emitted facts across the "
            "whole plan, not per rule (round-2 repro: every rule received "
            "the full cap and stats.emitted reset per rule, so N rules "
            "could publish up to N times the configured maximum)") {
    ExtractionRule rule_a = logging_boundary_rule();
    rule_a.id = "audit.logs_to.a";
    ExtractionRule rule_b = rule_a;
    rule_b.id = "audit.logs_to.b";
    ExtractionPlan plan = plan_with({rule_a, rule_b});

    ExecutionOptions options;
    options.hard_output_cap = 3;

    RunResult result = run_plan(plan,
                                "void log_audit_event(const char *msg);\n"
                                "void a() { log_audit_event(\"1\"); }\n"
                                "void b() { log_audit_event(\"2\"); }\n"
                                "void c() { log_audit_event(\"3\"); }\n"
                                "void d() { log_audit_event(\"4\"); }\n"
                                "void e() { log_audit_event(\"5\"); }\n",
                                ExecutionInput{}, options);

    // Each rule alone matches all 5 call sites and could emit up to 5
    // relations (well under its own max_emitted_facts); without the fix the
    // two rules together would emit up to 6 (3 each). The shared cap must
    // hold the TOTAL to exactly 3.
    CHECK(result.sink.relations().size() == 3);
  }

  TEST_CASE("publish_extension_artifact refuses to publish an execution "
            "with no pinned workspace/TU identity") {
    ExtractionPlan plan = plan_with({logging_boundary_rule()});
    RunResult result = run_plan(plan, "void log_audit_event(const char *msg);\n"
                                      "void f() { log_audit_event(\"t\"); }\n");
    REQUIRE(result.report.workspace_identity.empty());
    REQUIRE(result.report.tu_identity.empty());

    const std::string root = make_temp_dir();
    cidx::Storage storage(root + "/index.sqlite");
    PublicationRequest request;
    request.artifact_root = root + "/artifacts";
    request.namespace_name = "banking.audit";

    CHECK_THROWS_AS((void)publish_extension_artifact(
                        storage, request, plan, result.report, result.sink),
                    ExtensionPublicationError);
  }

  TEST_CASE("publish_extension_artifact refuses to publish a sink/report "
            "pair from different executions (round-2 repro: publication "
            "metadata was unbound to the execution that produced the "
            "report and facts)") {
    ExtractionPlan plan = plan_with({logging_boundary_rule()});
    RunResult first =
        run_plan(plan,
                 "void log_audit_event(const char *msg);\n"
                 "void f() { log_audit_event(\"t\"); }\n",
                 ExecutionInput{.workspace_identity = "workspace:a",
                                .tu_identity = "tu:a"});
    RunResult second =
        run_plan(plan,
                 "void log_audit_event(const char *msg);\n"
                 "void g() { log_audit_event(\"t\"); }\n",
                 ExecutionInput{.workspace_identity = "workspace:b",
                                .tu_identity = "tu:b"});

    const std::string root = make_temp_dir();
    cidx::Storage storage(root + "/index.sqlite");
    PublicationRequest request;
    request.artifact_root = root + "/artifacts";
    request.namespace_name = "banking.audit";

    // first.sink's facts carry first.report's artifact_identity; publishing
    // them against second.report (a different pinned execution) must be
    // refused.
    CHECK_THROWS_AS((void)publish_extension_artifact(storage, request, plan,
                                                     second.report, first.sink),
                    ExtensionPublicationError);
  }

  // --- PR #66 review round 3 regression coverage ------------------------

  TEST_CASE(
      "artifact_identity changes when the compiler invocation's header "
      "search configuration changes, even when neither -I path is ever "
      "actually consulted resolving the TU (round-3 repro: two different "
      "-I paths, same resolved source, were previously indistinguishable)") {
    ExtractionPlan plan = plan_with({logging_boundary_rule()});
    const std::string code = "void log_audit_event(const char *msg);\n"
                             "void do_transfer() { log_audit_event(\"t\"); "
                             "}\n";
    const std::string config_a = make_temp_dir();
    const std::string config_b = make_temp_dir();
    RunResult first = run_plan_with_args(plan, code, {"-I" + config_a});
    RunResult second = run_plan_with_args(plan, code, {"-I" + config_b});
    REQUIRE(first.report.plan_hash == second.report.plan_hash);
    CHECK(first.report.artifact_identity != second.report.artifact_identity);
  }

  TEST_CASE(
      "main_file scope anchors on the outermost/root binding, not on "
      "rule.bindings declaration order (round-3 repro: a call site in the "
      "main file whose CALLEE is declared in an included header was "
      "incorrectly excluded from main_file scope when \"callee\" happened "
      "to be listed before \"call\" in rule.bindings)") {
    ExtractionRule rule = logging_boundary_rule();
    rule.scope = PlanScope::main_file;
    // The header-resident binding listed FIRST: the buggy implementation
    // probed rule.bindings in declared order and used whichever resolved
    // first, so it picked "callee" (in header.h, out of main-file scope)
    // even though the call EXPRESSION itself is in the main file.
    rule.bindings = {
        Binding{.name = "callee", .domain = EndpointDomain::declaration},
        Binding{.name = "call", .domain = EndpointDomain::expression}};

    const std::string code = "#include \"header.h\"\nvoid do_transfer() { "
                             "log_audit_event(\"t\"); }\n";
    const clang::tooling::FileContentMappings files = {
        {"header.h", "void log_audit_event(const char *msg);\n"}};

    RunResult result = run_plan_with_files(plan_with({rule}), code, files);
    CHECK(result.sink.relations().size() == 1);

    // Same rule, bindings declared in the OPPOSITE order: main_file scope
    // filtering must give the identical result either way, since it always
    // anchors on the matcher's root ("call"), never on declaration order.
    ExtractionRule reordered_rule = rule;
    reordered_rule.bindings = {
        Binding{.name = "call", .domain = EndpointDomain::expression},
        Binding{.name = "callee", .domain = EndpointDomain::declaration}};
    RunResult reordered_result =
        run_plan_with_files(plan_with({reordered_rule}), code, files);
    CHECK(reordered_result.sink.relations().size() ==
          result.sink.relations().size());
  }

  TEST_CASE("publish_extension_artifact refuses to publish when the supplied "
            "plan does not match the plan that produced this execution report "
            "(round-3 repro: only sink/report agreement with EACH OTHER was "
            "checked, never report.plan_hash against plan_hash(the actual plan "
            "argument), so a caller could publish plan B's metadata alongside "
            "plan A's report/facts)") {
    ExtractionPlan plan_a = plan_with({logging_boundary_rule()});
    ExtractionRule other_rule = logging_boundary_rule();
    other_rule.id =
        "audit.logs_to.other"; // different id -> different plan_hash
    ExtractionPlan plan_b = plan_with({other_rule});
    REQUIRE(plan_hash(plan_a) != plan_hash(plan_b));

    RunResult result =
        run_plan(plan_a,
                 "void log_audit_event(const char *msg);\n"
                 "void f() { log_audit_event(\"t\"); }\n",
                 ExecutionInput{.workspace_identity = "workspace:x",
                                .tu_identity = "tu:x"});

    const std::string root = make_temp_dir();
    cidx::Storage storage(root + "/index.sqlite");
    PublicationRequest request;
    request.artifact_root = root + "/artifacts";
    request.namespace_name = "banking.audit";

    // plan_b never produced result.report/result.sink (plan_a did); this
    // must be refused even though the sink and report agree with each
    // other.
    CHECK_THROWS_AS((void)publish_extension_artifact(
                        storage, request, plan_b, result.report, result.sink),
                    ExtensionPublicationError);
  }

  TEST_CASE("publishing facts that differ only by namespace does not silently "
            "collapse one into the other (round-3 repro: the extension table's "
            "PRIMARY KEY omitted namespace, so INSERT OR IGNORE dropped the "
            "second insert as a \"duplicate\" of the first)") {
    ExtractionRule rule_a = logging_boundary_rule();
    rule_a.id = "audit.logs_to.security";
    ExtractionRule rule_b = rule_a;
    rule_b.id = "audit.logs_to.compliance";
    // Bound to a single reference (rather than re-evaluating
    // rule_b.emits[0].relation for both the guard and the mutation) and
    // guarded with a real `if`/throw -- not REQUIRE, which expands through
    // doctest machinery the optional-access checker's dataflow does not see
    // through -- so the checker can prove the checked and mutated optionals
    // are the same, known-engaged object.
    std::optional<EmitRelation> &relation_b = rule_b.emits[0].relation;
    if (!relation_b.has_value()) {
      throw std::runtime_error("test fixture error: emits[0].relation not set");
    }
    relation_b->namespace_name = "compliance";
    ExtractionPlan plan = plan_with({rule_a, rule_b});

    RunResult result =
        run_plan(plan,
                 "void log_audit_event(const char *msg);\n"
                 "void f() { log_audit_event(\"t\"); }\n",
                 ExecutionInput{.workspace_identity = "workspace:x",
                                .tu_identity = "tu:x"});
    REQUIRE(result.sink.relations().size() == 2);

    const std::string root = make_temp_dir();
    cidx::Storage storage(root + "/index.sqlite");
    PublicationRequest request;
    request.artifact_root = root + "/artifacts";
    request.namespace_name = "banking.audit";

    const ExtensionPublication publication = publish_extension_artifact(
        storage, request, plan, result.report, result.sink);

    const auto published_path = std::filesystem::path(request.artifact_root) /
                                publication.relative_path;
    cidx::SqliteDb published_db(published_path.string(), true,
                                cidx::SqliteProfile::read_only_replay);
    auto rows = published_db.prepare(
        "SELECT COUNT(*) FROM extension WHERE fact_kind = 'relation' AND "
        "kind_name = 'logs_to'");
    REQUIRE(rows.step());
    CHECK(rows.col_int64(0) == 2);
  }

  TEST_CASE(
      "a fact's persisted completeness is downgraded when its emitting "
      "rule's budget was exhausted, even though the rule declares complete "
      "(critic repro: provenance.completeness was stamped once from the "
      "rule's STATIC DeclaredCompleteness before any budget check, so a "
      "truncated rule's already-emitted facts kept claiming complete)") {
    ExtractionRule rule = logging_boundary_rule();
    // completeness stays DeclaredCompleteness::complete (the default set by
    // logging_boundary_rule()); max_emitted_facts=1 forces budget
    // exhaustion after the first of several matching call sites.
    rule.budget = RuleBudget{.max_matches = 1000,
                             .max_emitted_facts = 1,
                             .max_visited_nodes = 100000,
                             .declared = true};
    ExtractionPlan plan = plan_with({rule});

    RunResult result =
        run_plan(plan,
                 "void log_audit_event(const char *msg);\n"
                 "void a() { log_audit_event(\"1\"); }\n"
                 "void b() { log_audit_event(\"2\"); }\n",
                 ExecutionInput{.workspace_identity = "workspace:x",
                                .tu_identity = "tu:x"});
    const auto *stats = result.report.find(rule.id);
    REQUIRE(stats != nullptr);
    REQUIRE(stats->budget_exhausted);
    REQUIRE(result.sink.relations().size() == 1);
    // The one fact that WAS emitted must not claim `complete`: the rule's
    // run as a whole was truncated, so no fact it produced can vouch for
    // being a complete view.
    CHECK(result.sink.relations()[0].provenance.completeness ==
          DeclaredCompleteness::partial);

    const std::string root = make_temp_dir();
    cidx::Storage storage(root + "/index.sqlite");
    PublicationRequest request;
    request.artifact_root = root + "/artifacts";
    request.namespace_name = "banking.audit";
    const ExtensionPublication publication = publish_extension_artifact(
        storage, request, plan, result.report, result.sink);
    const auto published_path = std::filesystem::path(request.artifact_root) /
                                publication.relative_path;
    cidx::SqliteDb published_db(published_path.string(), true,
                                cidx::SqliteProfile::read_only_replay);
    auto rows = published_db.prepare(
        "SELECT COUNT(*) FROM extension WHERE fact_kind = 'relation' AND "
        "completeness = 'complete'");
    REQUIRE(rows.step());
    CHECK(rows.col_int64(0) == 0);
  }

  // --- PR #66 review round 4 regression coverage ------------------------

  TEST_CASE(
      "a published extension artifact is readable back through the "
      "platform's ExtensionFactProvider adapter, not only via a raw SQLite "
      "SELECT (round-4 repro: the publisher wrote only extension/"
      "extension_meta, so ExtensionFactProvider::snapshot() threw \"prepare "
      "failed ... no such table: meta\")") {
    ExtractionPlan plan = plan_with({logging_boundary_rule()});
    RunResult result =
        run_plan(plan,
                 "void log_audit_event(const char *msg);\n"
                 "void f() { log_audit_event(\"t\"); }\n",
                 ExecutionInput{.workspace_identity = "workspace:test",
                                .tu_identity = "tu:test"});
    REQUIRE(result.sink.relations().size() == 1);

    const std::string root = make_temp_dir();
    cidx::Storage storage(root + "/index.sqlite");
    PublicationRequest request;
    request.artifact_root = root + "/artifacts";
    request.namespace_name = "banking.audit";
    const ExtensionPublication publication = publish_extension_artifact(
        storage, request, plan, result.report, result.sink);
    const auto published_path = std::filesystem::path(request.artifact_root) /
                                publication.relative_path;

    const cidx::analysis::ExtensionFactProvider provider(
        published_path.string());
    const cidx::analysis::FactSnapshot snapshot =
        provider.snapshot(cidx::analysis::FactRequest{
            .relations = {"extension"},
            .workspace_identity = std::string("workspace:test"),
            .tu_identity = std::string("tu:test")});
    CHECK(snapshot.workspace_identity == "workspace:test");
    // Bound to a single reference and guarded with a real `if`/throw --
    // not REQUIRE, which expands through doctest machinery the
    // optional-access checker's dataflow does not see through -- mirrors
    // relation_of()'s treatment of the same pattern above.
    const std::optional<std::string> &tu_identity = snapshot.tu_identity;
    if (!tu_identity.has_value()) {
      throw std::runtime_error("test fixture error: snapshot.tu_identity not "
                               "set");
    }
    CHECK(*tu_identity == "tu:test");
    CHECK(snapshot.completeness == cidx::analysis::FactCompleteness::complete);
    const auto *extension = snapshot.find_relation("extension");
    REQUIRE(extension != nullptr);
    CHECK_FALSE(extension->rows.empty());
  }

  TEST_CASE(
      "a SINGLE (non-nested) hasDescendant against nested records performs "
      "quadratic matcher-evaluation work a one-time linear AST-size "
      "estimate cannot see, and execute_plan now refuses to run it instead "
      "of silently exceeding the declared budget (round-4 repro: "
      "cxxRecordDecl(hasDescendant(cxxMethodDecl(hasName(\"__never__\")))) "
      "at max_visited_nodes=1,000,000 against 100/200/300 nested records "
      "never set budget_exhausted while measured execution time grew "
      "11ms -> 37ms -> 114ms)") {
    ExtractionRule rule;
    rule.id = "audit.single_descendant_bound";
    rule.matcher_expression =
        "cxxRecordDecl(hasDescendant(cxxMethodDecl(hasName(\"__never__\")))"
        ").bind(\"record\")";
    rule.bindings = {
        Binding{.name = "record", .domain = EndpointDomain::declaration}};
    EmitOperation emit;
    emit.unknown = EmitUnknown{
        .namespace_name = "audit", .reason_code = "seen", .binding = "record"};
    rule.emits = {emit};
    rule.completeness = DeclaredCompleteness::unknown_capable;
    // Generous enough for the RAW node count of 60 nested records (the old
    // linear "visited / (combinators + 1)" estimate would never trip here
    // either) but far too small for a single hasDescendant's WORST-CASE
    // quadratic cost over that many records.
    rule.budget = RuleBudget{.max_matches = 1000,
                             .max_emitted_facts = 1000,
                             .max_visited_nodes = 5000,
                             .declared = true};
    rule.producer_package = "audit";
    rule.producer_version = 1;
    ExtractionPlan plan = plan_with({rule});

    RunResult result = run_plan(plan, nested_records_source(60));
    const auto *stats = result.report.find(rule.id);
    REQUIRE(stats != nullptr);
    CHECK(stats->budget_exhausted);
    CHECK(result.sink.unknowns().empty());
  }

  TEST_CASE(
      "a constructor's member-initializer list is invisible to "
      "NodeBudgetCounter unless CXXConstructorDecl::inits() is walked, so a "
      "hidden call chain living ONLY in a member-initializer slips under "
      "max_visited_nodes with no hasDescendant/hasAncestor combinator "
      "involved at all (P1 repro distinct from the round-4 quadratic-"
      "combinator finding above: this matcher -- "
      "cxxConstructorDecl().bind(\"ctor\") -- has ZERO traversal-work "
      "combinators, so estimated_work_exceeded is unconditionally false and "
      "budget_exhausted can only trip through the raw visited-node "
      "pre-check, isolating the coverage gap itself rather than the "
      "quadratic-cost estimate)") {
    ExtractionRule rule;
    rule.id = "audit.ctor_initializer_hidden_calls";
    rule.matcher_expression = "cxxConstructorDecl().bind(\"ctor\")";
    rule.bindings = {
        Binding{.name = "ctor", .domain = EndpointDomain::declaration}};
    EmitOperation emit;
    emit.unknown = EmitUnknown{
        .namespace_name = "audit", .reason_code = "seen", .binding = "ctor"};
    rule.emits = {emit};
    rule.completeness = DeclaredCompleteness::unknown_capable;
    // Measured directly against this exact source: ignoring
    // member-initializers (the pre-fix behavior) visits 142 nodes; walking
    // CXXCtorInitializer::getInit() as well visits 1332 -- a ~9x spread.
    // 300 sits with a wide margin on both sides of that boundary.
    rule.budget = RuleBudget{.max_matches = 1000,
                             .max_emitted_facts = 1000,
                             .max_visited_nodes = 300,
                             .declared = true};
    rule.producer_package = "audit";
    rule.producer_version = 1;
    ExtractionPlan plan = plan_with({rule});

    RunResult result =
        run_plan(plan, ctor_initializer_hidden_calls_source(10, 30));
    const auto *stats = result.report.find(rule.id);
    REQUIRE(stats != nullptr);
    CHECK(stats->budget_exhausted);
  }

  TEST_CASE(
      "an enumerator initializer is invisible to NodeBudgetCounter unless "
      "EnumConstantDecl::getInitExpr() is walked, so a hidden call chain "
      "living ONLY in an enum-class enumerator's value slips under "
      "max_visited_nodes even though `enumDecl` is itself an allow-listed "
      "matcher_catalog.cpp node (distinct coverage gap from the "
      "member-initializer P1: EnumConstantDecl derives from ValueDecl, not "
      "VarDecl, so it fell through every existing dyn_cast branch)") {
    ExtractionRule rule;
    rule.id = "audit.enum_constant_hidden_calls";
    rule.matcher_expression = "enumDecl().bind(\"e\")";
    rule.bindings = {
        Binding{.name = "e", .domain = EndpointDomain::declaration}};
    EmitOperation emit;
    emit.unknown = EmitUnknown{
        .namespace_name = "audit", .reason_code = "seen", .binding = "e"};
    rule.emits = {emit};
    rule.completeness = DeclaredCompleteness::unknown_capable;
    // Measured directly against this exact source by bisecting
    // max_visited_nodes: ignoring enumerator initializers (the pre-fix
    // behavior) exhausts the budget somewhere in [110, 120); walking
    // EnumConstantDecl::getInitExpr() as well moves that boundary out past
    // [1300, 1400) -- roughly a 12x spread. 700 sits with a wide margin on
    // both sides.
    rule.budget = RuleBudget{.max_matches = 1000,
                             .max_emitted_facts = 1000,
                             .max_visited_nodes = 700,
                             .declared = true};
    rule.producer_package = "audit";
    rule.producer_version = 1;
    ExtractionPlan plan = plan_with({rule});

    RunResult result =
        run_plan(plan, enum_constant_hidden_calls_source(10, 30));
    const auto *stats = result.report.find(rule.id);
    REQUIRE(stats != nullptr);
    CHECK(stats->budget_exhausted);
  }

  TEST_CASE("a decltype(expr) return type is invisible to NodeBudgetCounter "
            "unless the function's declared return type is followed into its "
            "underlying expression, so a hidden call chain living ONLY in a "
            "trailing decltype(...) return type slips under max_visited_nodes "
            "even though `functionDecl` is itself an allow-listed "
            "matcher_catalog.cpp node (distinct coverage gap from the "
            "member-initializer/enumerator P1s: this content lives on the "
            "declared TYPE, not in any Decl or Stmt subtree the counter's "
            "existing branches ever reach)") {
    ExtractionRule rule;
    rule.id = "audit.decltype_return_type_hidden_calls";
    rule.matcher_expression = "functionDecl().bind(\"fn\")";
    rule.bindings = {
        Binding{.name = "fn", .domain = EndpointDomain::declaration}};
    EmitOperation emit;
    emit.unknown = EmitUnknown{
        .namespace_name = "audit", .reason_code = "seen", .binding = "fn"};
    rule.emits = {emit};
    rule.completeness = DeclaredCompleteness::unknown_capable;
    // Measured directly against this exact source by bisecting
    // max_visited_nodes: ignoring the decltype operand (the pre-fix
    // behavior) exhausts the budget somewhere in [100, 110); walking it as
    // well moves that boundary out past [1300, 1400) -- roughly a 13x
    // spread. 700 sits with a wide margin on both sides.
    rule.budget = RuleBudget{.max_matches = 1000,
                             .max_emitted_facts = 1000,
                             .max_visited_nodes = 700,
                             .declared = true};
    rule.producer_package = "audit";
    rule.producer_version = 1;
    ExtractionPlan plan = plan_with({rule});

    RunResult result =
        run_plan(plan, decltype_return_type_hidden_calls_source(10, 30));
    const auto *stats = result.report.find(rule.id);
    REQUIRE(stats != nullptr);
    CHECK(stats->budget_exhausted);
  }

  TEST_CASE(
      "a parameter's default argument is invisible to NodeBudgetCounter "
      "unless FunctionDecl::parameters() is walked, so a hidden call chain "
      "living ONLY in a default argument slips under max_visited_nodes even "
      "though `functionDecl` is itself an allow-listed matcher_catalog.cpp "
      "node (a parameter is never linked into its owning FunctionDecl's own "
      "decls() chain)") {
    ExtractionRule rule;
    rule.id = "audit.parameter_default_argument_hidden_calls";
    rule.matcher_expression = "functionDecl().bind(\"fn\")";
    rule.bindings = {
        Binding{.name = "fn", .domain = EndpointDomain::declaration}};
    EmitOperation emit;
    emit.unknown = EmitUnknown{
        .namespace_name = "audit", .reason_code = "seen", .binding = "fn"};
    rule.emits = {emit};
    rule.completeness = DeclaredCompleteness::unknown_capable;
    // Measured directly against this exact source by bisecting
    // max_visited_nodes: ignoring parameter default arguments (the pre-fix
    // behavior) exhausts the budget somewhere in [100, 110); walking
    // ParmVarDecl::getDefaultArg() as well moves that boundary out past
    // [1300, 1400) -- roughly a 13x spread. 700 sits with a wide margin on
    // both sides.
    rule.budget = RuleBudget{.max_matches = 1000,
                             .max_emitted_facts = 1000,
                             .max_visited_nodes = 700,
                             .declared = true};
    rule.producer_package = "audit";
    rule.producer_version = 1;
    ExtractionPlan plan = plan_with({rule});

    RunResult result =
        run_plan(plan, parameter_default_argument_hidden_calls_source(10, 30));
    const auto *stats = result.report.find(rule.id);
    REQUIRE(stats != nullptr);
    CHECK(stats->budget_exhausted);
  }

  TEST_CASE(
      "a noexcept(expr) specifier is invisible to NodeBudgetCounter unless "
      "the function's FunctionProtoType::getNoexceptExpr() is walked, so a "
      "hidden call chain living ONLY in a noexcept specifier slips under "
      "max_visited_nodes even though `functionDecl` is itself an "
      "allow-listed matcher_catalog.cpp node (the expression lives on the "
      "function's TYPE, not reachable through decls() or getBody())") {
    ExtractionRule rule;
    rule.id = "audit.noexcept_specifier_hidden_calls";
    rule.matcher_expression = "functionDecl().bind(\"fn\")";
    rule.bindings = {
        Binding{.name = "fn", .domain = EndpointDomain::declaration}};
    EmitOperation emit;
    emit.unknown = EmitUnknown{
        .namespace_name = "audit", .reason_code = "seen", .binding = "fn"};
    rule.emits = {emit};
    rule.completeness = DeclaredCompleteness::unknown_capable;
    // Measured directly against this exact source by bisecting
    // max_visited_nodes: ignoring the noexcept operand (the pre-fix
    // behavior) exhausts the budget somewhere in [100, 110); walking
    // FunctionProtoType::getNoexceptExpr() as well moves that boundary out
    // past [1300, 1400) -- roughly a 13x spread. 700 sits with a wide
    // margin on both sides.
    rule.budget = RuleBudget{.max_matches = 1000,
                             .max_emitted_facts = 1000,
                             .max_visited_nodes = 700,
                             .declared = true};
    rule.producer_package = "audit";
    rule.producer_version = 1;
    ExtractionPlan plan = plan_with({rule});

    RunResult result =
        run_plan(plan, noexcept_specifier_hidden_calls_source(10, 30));
    const auto *stats = result.report.find(rule.id);
    REQUIRE(stats != nullptr);
    CHECK(stats->budget_exhausted);
  }

  TEST_CASE(
      "an entire class template pattern's method body is invisible to "
      "NodeBudgetCounter unless TemplateDecl::getTemplatedDecl() is walked, "
      "so a hidden call chain living ONLY in an uninstantiated template's "
      "method body slips under max_visited_nodes even though "
      "`classTemplateDecl` is itself an allow-listed matcher_catalog.cpp "
      "node (clang::ClassTemplateDecl is never itself a DeclContext -- the "
      "templated CXXRecordDecl it wraps is)") {
    ExtractionRule rule;
    rule.id = "audit.class_template_body_hidden_calls";
    rule.matcher_expression = "classTemplateDecl().bind(\"ctd\")";
    rule.bindings = {
        Binding{.name = "ctd", .domain = EndpointDomain::declaration}};
    EmitOperation emit;
    emit.unknown = EmitUnknown{
        .namespace_name = "audit", .reason_code = "seen", .binding = "ctd"};
    rule.emits = {emit};
    rule.completeness = DeclaredCompleteness::unknown_capable;
    // Measured directly against this exact source by bisecting
    // max_visited_nodes: ignoring the templated pattern's body (the pre-fix
    // behavior) exhausts the budget somewhere in [100, 110); walking
    // tmpl->getTemplatedDecl() as well moves that boundary out past
    // [1000, 1200) -- roughly a 9x spread. 500 sits with a wide margin on
    // both sides.
    rule.budget = RuleBudget{.max_matches = 1000,
                             .max_emitted_facts = 1000,
                             .max_visited_nodes = 500,
                             .declared = true};
    rule.producer_package = "audit";
    rule.producer_version = 1;
    ExtractionPlan plan = plan_with({rule});

    RunResult result =
        run_plan(plan, class_template_body_hidden_calls_source(10, 30));
    const auto *stats = result.report.find(rule.id);
    REQUIRE(stats != nullptr);
    CHECK(stats->budget_exhausted);
  }

  TEST_CASE(
      "artifact_identity changes when the frontend's LangOptions differ "
      "even though source, target, predefines, and header search are all "
      "unchanged (round-4 repro: -fno-elide-constructors alone left "
      "artifact_identity unchanged because the fingerprint only ever hashed "
      "predefines, target/ABI, LangStd, and header search)") {
    ExtractionPlan plan = plan_with({logging_boundary_rule()});
    const std::string code = "void log_audit_event(const char *msg);\n"
                             "void f() { log_audit_event(\"t\"); }\n";
    RunResult default_run = run_plan_with_args(plan, code, {});
    RunResult no_elide_run =
        run_plan_with_args(plan, code, {"-fno-elide-constructors"});
    REQUIRE(default_run.report.plan_hash == no_elide_run.report.plan_hash);
    CHECK(default_run.report.artifact_identity !=
          no_elide_run.report.artifact_identity);
  }

} // TEST_SUITE("clang")
