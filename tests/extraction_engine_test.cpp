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

#include "extract/artifact.hpp"
#include "extract/engine.hpp"
#include "extract/extension_facts.hpp"
#include "extract/plan_ir.hpp"
#include "storage/sqlite.hpp"
#include "storage/storage.hpp"

#include "clang/AST/ASTConsumer.h"
#include "clang/Frontend/FrontendAction.h"
#include "clang/Tooling/Tooling.h"

#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
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
  PlanConsumer(const ExtractionPlan &plan, RunResult &result)
      : plan_(plan), result_(result) {}

  void HandleTranslationUnit(clang::ASTContext &context) override {
    try {
      result_.report = execute_plan(plan_, context, result_.sink);
      result_.sink.canonicalize();
    } catch (const PlanNotValidated &ex) {
      result_.plan_not_validated_message = ex.what();
    }
  }

private:
  const ExtractionPlan &plan_;
  RunResult &result_;
};

class PlanAction final : public clang::ASTFrontendAction {
public:
  PlanAction(const ExtractionPlan &plan, RunResult &result)
      : plan_(plan), result_(result) {}

  std::unique_ptr<clang::ASTConsumer>
  CreateASTConsumer(clang::CompilerInstance &, llvm::StringRef) override {
    return std::make_unique<PlanConsumer>(plan_, result_);
  }

private:
  const ExtractionPlan &plan_;
  RunResult &result_;
};

RunResult run_plan(const ExtractionPlan &plan, const std::string &code) {
  RunResult result;
  const bool ran = clang::tooling::runToolOnCode(
      std::make_unique<PlanAction>(plan, result), code, "test.cpp");
  REQUIRE(ran);
  return result;
}

// Like run_plan(), but maps additional virtual files (e.g. a header) so
// main_file-vs-translation_unit scope has something to actually distinguish.
RunResult
run_plan_with_files(const ExtractionPlan &plan, const std::string &code,
                    const clang::tooling::FileContentMappings &files) {
  RunResult result;
  const bool ran = clang::tooling::runToolOnCodeWithArgs(
      std::make_unique<PlanAction>(plan, result), code, {"-std=c++20"},
      "test.cpp", "clang-tool",
      std::make_shared<clang::PCHContainerOperations>(), files);
  REQUIRE(ran);
  return result;
}

std::string make_temp_dir() {
  std::string tmpl = "/tmp/cidx_extract_artifact_XXXXXX";
  std::vector<char> buffer(tmpl.begin(), tmpl.end());
  buffer.push_back('\0');
  char *path = ::mkdtemp(buffer.data());
  REQUIRE(path != nullptr);
  return path;
}

RuleBudget generous_budget() { return RuleBudget{1000, 1000, 100000, true}; }

ExtractionRule logging_boundary_rule() {
  ExtractionRule rule;
  rule.id = "audit.logs_to";
  rule.matcher_expression =
      "callExpr(callee(functionDecl(hasName(\"log_audit_event\")).bind("
      "\"callee\"))).bind(\"call\")";
  rule.bindings = {Binding{"call", EndpointDomain::expression},
                   Binding{"callee", EndpointDomain::declaration}};
  EmitOperation emit;
  emit.relation = EmitRelation{"audit", "logs_to", "call", "callee", true};
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
  rule.bindings = {Binding{"call", EndpointDomain::expression},
                   Binding{"target", EndpointDomain::declaration}};
  EmitOperation emit;
  emit.unknown = EmitUnknown{"audit", "unclassified_dispatch", "call"};
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
  rule.bindings = {Binding{"record", EndpointDomain::declaration}};
  EmitOperation emit;
  emit.node = EmitNode{"banking.appstate", "boundary", "record",
                       IdentityRecipe{IdentityKind::usr, {}}};
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
  rule.bindings = {Binding{"record", EndpointDomain::declaration}};
  EmitOperation emit;
  emit.unknown = EmitUnknown{"banking.appstate",
                             "boundary_registration_unverifiable", "record"};
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
    ctor_rule.bindings = {Binding{"call", EndpointDomain::expression}};
    EmitOperation emit;
    emit.unknown = EmitUnknown{"trace", "ctor_call", "call"};
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
    rule.budget = RuleBudget{1, 1, 100000, true};
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
    rule.bindings = {Binding{"x", EndpointDomain::declaration}};
    EmitOperation emit;
    emit.relation = EmitRelation{"audit", "self", "x", "x", true};
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
    base_rule.bindings = {Binding{"fn", EndpointDomain::declaration}};
    EmitOperation emit;
    emit.unknown = EmitUnknown{"scope", "fn_seen", "fn"};
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
    rule.budget = RuleBudget{1000, 1000, 1, true};
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
    RunResult result = run_plan(plan, "void log_audit_event(const char *msg);\n"
                                      "void f() { log_audit_event(\"t\"); }\n");
    REQUIRE(result.sink.relations().size() == 1);

    const std::string root = make_temp_dir();
    cidx::Storage storage(root + "/index.sqlite");
    PublicationRequest request;
    request.artifact_root = root + "/artifacts";
    request.namespace_name = "banking.audit";
    request.workspace_identity = "workspace:test";
    request.tu_identity = "tu:test";

    const ExtensionPublication publication = publish_extension_artifact(
        storage, request, plan, result.report, result.sink);

    REQUIRE(publication.logical_id == "extension:banking.audit");
    CHECK(publication.artifact_identity == result.report.artifact_identity);

    cidx::ArtifactStore artifacts(storage, request.artifact_root);
    const auto current = artifacts.current(publication.logical_id);
    REQUIRE(current.has_value());
    CHECK(current->content_hash == publication.content_hash);
    CHECK(current->spec.configuration_identity ==
          result.report.artifact_identity);
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

} // TEST_SUITE("clang")
