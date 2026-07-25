// Registry and execution contract for composable extraction passes.
#pragma once

#include "ast/fact_emitters.hpp"
#include "ast/fact_records.hpp"
#include "ast/indexing_plan.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace clang {
class ASTContext;
class Preprocessor;
} // namespace clang

namespace cidx::ast {

class ExtractionPassRegistry;

enum class FrontendCapability : std::uint8_t {
  ast,
  preprocessor,
  cfg,
  templates,
};

struct PassBudget {
  std::size_t max_visited_constructs = 0;
  std::size_t max_emitted_facts = 0;
  std::size_t max_diagnostics = 0;
  bool declared = false;
};

struct FrontendSession {
  clang::ASTContext *ast_context = nullptr;
  clang::Preprocessor *preprocessor = nullptr;
  DeclarationPassPorts *declaration_ports = nullptr;
  StatementFactPorts *statement_ports = nullptr;
  NamespacePassPorts *namespace_ports = nullptr;
  DefinitionScopeEmitter *definition_ports = nullptr;
  EvidenceEmitter *evidence = nullptr;
  PresentationIntentEmitter *presentation_intents = nullptr;
  IndexingLifecycle *lifecycle = nullptr;
  bool cfg_available = false;
  bool templates_available = false;
  std::map<std::string, PassBudget> budget_overrides;

  [[nodiscard]] auto supports(FrontendCapability capability) const -> bool;
};

enum class PassScope : std::uint8_t {
  translation_unit,
  main_file,
  owned_header,
};

enum class TraversalMode : std::uint8_t {
  declaration,
  body,
  preprocessing,
  lifecycle,
};

class PassBudgetExceeded final : public std::runtime_error {
public:
  PassBudgetExceeded(std::string pass_id, std::string dimension);

  [[nodiscard]] auto pass_id() const -> const std::string & { return pass_id_; }
  [[nodiscard]] auto dimension() const -> const std::string & {
    return dimension_;
  }

private:
  std::string pass_id_;
  std::string dimension_;
};

class FrontendCapabilityUnavailable final : public std::runtime_error {
public:
  FrontendCapabilityUnavailable(std::string pass_id,
                                FrontendCapability capability);
  [[nodiscard]] auto pass_id() const -> const std::string & { return pass_id_; }
  [[nodiscard]] auto capability() const -> FrontendCapability {
    return capability_;
  }

private:
  std::string pass_id_;
  FrontendCapability capability_;
};

struct ExtractionPassDescriptor {
  std::string id;
  std::uint32_t version = 1;
  std::vector<FrontendCapability> required_capabilities;
  std::vector<std::string> consumed_fact_families;
  std::vector<std::string> produced_fact_families;
  std::vector<std::uint32_t> catalog_versions;
  std::vector<std::string> dependencies;
  PassScope scope = PassScope::translation_unit;
  TraversalMode traversal = TraversalMode::lifecycle;
  FactCompleteness completeness = FactCompleteness::complete;
  FactTrust trust = FactTrust::trusted;
  PassBudget budget;

  [[nodiscard]] auto stable_key() const -> std::string;
};

struct PassMetrics {
  std::size_t visited_constructs = 0;
  std::size_t emitted_facts = 0;
  std::size_t unknown_constructs = 0;
  std::size_t duplicates = 0;
  std::size_t diagnostics = 0;
  std::chrono::microseconds elapsed{};
  bool budget_exhausted = false;
  std::vector<std::string> diagnostic_messages;

  void note_visited(std::size_t count = 1);
  void note_emitted(std::size_t count = 1);
  void note_unknown(std::size_t count = 1) { unknown_constructs += count; }
  void note_duplicate(std::size_t count = 1) { duplicates += count; }
  void note_diagnostic(std::string message);

  void bind(std::string pass_id, PassBudget budget);

private:
  void enforce(std::size_t value, std::size_t limit, const char *dimension);
  std::string pass_id_;
  PassBudget budget_;
};

class BudgetedStatementFactPorts final : public StatementFactPorts {
public:
  BudgetedStatementFactPorts(StatementFactPorts &ports, PassMetrics &metrics);

  auto lookup_symbol_id(const std::string &, const std::optional<std::string> &)
      -> std::optional<std::int64_t> override;
  auto mint_symbol(const MintRequest &) -> std::int64_t override;
  auto file_id_for_path(const std::string &)
      -> std::optional<std::int64_t> override;
  auto type_arg_candidates(const std::string &, bool)
      -> std::vector<TypeArgCandidate> override;
  auto symbol_ids_by_qual_name_kind(const std::string &, const std::string &)
      -> std::vector<std::int64_t> override;
  auto add_edge(const EdgeRecord &) -> std::int64_t override;
  auto ensure_edge(const EdgeRecord &) -> std::int64_t override;
  void add_edge_site(const EdgeSiteRecord &) override;
  void add_call_arg(const CallArgRecord &) override;
  void add_template_param(const TemplateParamRecord &) override;
  void add_template_arg(const TemplateArgRecord &) override;
  auto intern_type_node(const TypeNodeRecord &) -> std::int64_t override;
  void add_type_edge(std::int64_t, std::int64_t, std::int64_t,
                     std::int64_t) override;
  void replace_parameters(std::int64_t,
                          const std::vector<ParameterRecord> &) override;
  void add_symbol_type(std::int64_t, std::int64_t, std::int64_t) override;
  void emit(const EvidenceRecord &) override;

private:
  StatementFactPorts &ports_;
  PassMetrics &metrics_;
};

class BudgetedDeclarationPassPorts final : public DeclarationPassPorts {
public:
  BudgetedDeclarationPassPorts(DeclarationPassPorts &ports,
                               PassMetrics &metrics);

  auto lookup_symbol_id(const std::string &, const std::optional<std::string> &)
      -> std::optional<std::int64_t> override;
  auto mint_symbol(const MintRequest &) -> std::int64_t override;
  auto file_id_for_path(const std::string &)
      -> std::optional<std::int64_t> override;
  auto type_arg_candidates(const std::string &, bool)
      -> std::vector<TypeArgCandidate> override;
  auto symbol_ids_by_qual_name_kind(const std::string &, const std::string &)
      -> std::vector<std::int64_t> override;
  auto add_edge(const EdgeRecord &) -> std::int64_t override;
  auto ensure_edge(const EdgeRecord &) -> std::int64_t override;
  void add_edge_site(const EdgeSiteRecord &) override;
  void add_call_arg(const CallArgRecord &) override;
  void add_template_param(const TemplateParamRecord &) override;
  void add_template_arg(const TemplateArgRecord &) override;
  auto intern_type_node(const TypeNodeRecord &) -> std::int64_t override;
  void add_type_edge(std::int64_t, std::int64_t, std::int64_t,
                     std::int64_t) override;
  void replace_parameters(std::int64_t,
                          const std::vector<ParameterRecord> &) override;
  void add_symbol_type(std::int64_t, std::int64_t, std::int64_t) override;

private:
  DeclarationPassPorts &ports_;
  PassMetrics &metrics_;
};

class BudgetedPresentationIntentEmitter final
    : public PresentationIntentEmitter {
public:
  BudgetedPresentationIntentEmitter(PresentationIntentEmitter &emitter,
                                    PassMetrics &metrics)
      : emitter_(emitter), metrics_(metrics) {}
  void emit(const PresentationIntent &intent) override;

private:
  PresentationIntentEmitter &emitter_;
  PassMetrics &metrics_;
};

class BudgetedNamespacePassPorts final : public NamespacePassPorts {
public:
  BudgetedNamespacePassPorts(NamespacePassPorts &ports, PassMetrics &metrics);

  auto lookup_symbol_id(const std::string &, const std::optional<std::string> &)
      -> std::optional<std::int64_t> override;
  auto mint_symbol(const MintRequest &) -> std::int64_t override;
  auto file_id_for_path(const std::string &)
      -> std::optional<std::int64_t> override;
  auto type_arg_candidates(const std::string &, bool)
      -> std::vector<TypeArgCandidate> override;
  auto symbol_ids_by_qual_name_kind(const std::string &, const std::string &)
      -> std::vector<std::int64_t> override;
  auto add_edge(const EdgeRecord &) -> std::int64_t override;
  auto ensure_edge(const EdgeRecord &) -> std::int64_t override;
  void add_edge_site(const EdgeSiteRecord &) override;
  void add_call_arg(const CallArgRecord &) override;
  void add_template_param(const TemplateParamRecord &) override;
  void add_template_arg(const TemplateArgRecord &) override;

private:
  NamespacePassPorts &ports_;
  PassMetrics &metrics_;
};

class BudgetedDefinitionScopeEmitter final : public DefinitionScopeEmitter {
public:
  BudgetedDefinitionScopeEmitter(DefinitionScopeEmitter &definitions,
                                 PassMetrics &metrics);

  auto get_or_create_definition(std::int64_t, std::int64_t, std::int64_t,
                                std::int64_t, std::int64_t, std::int64_t,
                                const std::optional<std::string> &)
      -> std::int64_t override;
  void add_def_edge(std::int64_t, std::int64_t, std::int64_t) override;
  auto body_edge_count(std::int64_t) -> std::size_t override;
  void copy_body_edges_to_def_edge(std::int64_t, std::int64_t) override;

private:
  DefinitionScopeEmitter &definitions_;
  PassMetrics &metrics_;
};

struct PassExecutionContext {
  PassMetrics &metrics;
  FrontendSession *session = nullptr;
};

using FrontendPassProvider = std::function<void(
    FrontendSession &, ExtractionPassRegistry &, IndexingPlan &)>;

struct PassExecutionRecord {
  ExtractionPassDescriptor descriptor;
  PassMetrics metrics;
};

struct PassExecutionReport {
  std::vector<PassExecutionRecord> passes;

  [[nodiscard]] auto find(const std::string &id) const
      -> const PassExecutionRecord *;
};

class ExtractionPassRegistry {
public:
  using Runner = std::function<void(PassExecutionContext &)>;

  void register_pass(ExtractionPassDescriptor descriptor, Runner runner);

  [[nodiscard]] auto descriptor(const std::string &id) const
      -> const ExtractionPassDescriptor &;
  [[nodiscard]] auto descriptors() const
      -> std::vector<ExtractionPassDescriptor>;
  [[nodiscard]] auto run(const IndexingPlan &plan,
                         FrontendSession *session = nullptr) const
      -> PassExecutionReport;

private:
  struct Entry {
    ExtractionPassDescriptor descriptor;
    Runner runner;
  };
  std::vector<Entry> entries_;
};

void register_frontend_pass_provider(FrontendPassProvider provider);
void clear_frontend_pass_providers();
[[nodiscard]] auto frontend_pass_providers()
    -> std::vector<FrontendPassProvider>;

} // namespace cidx::ast
