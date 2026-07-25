#include "ast/pass_registry.hpp"

#include <algorithm>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace cidx::ast {

namespace {

std::mutex provider_mutex;
std::vector<FrontendPassProvider> provider_list;

template <typename T>
void append_keys(std::string &out, const std::vector<T> &values) {
  out += std::to_string(values.size());
  out.push_back(':');
  for (const T &value : values) {
    if constexpr (std::is_enum_v<T>) {
      out += std::to_string(std::to_underlying(value));
    } else {
      out += std::to_string(value);
    }
    out.push_back(';');
  }
}

void append_keys(std::string &out, const std::vector<std::string> &values) {
  out += std::to_string(values.size());
  out.push_back(':');
  for (const std::string &value : values) {
    out += std::to_string(value.size());
    out.push_back(':');
    out += value;
    out.push_back(';');
  }
}

} // namespace

auto ExtractionPassDescriptor::stable_key() const -> std::string {
  std::string key = "id=" + std::to_string(id.size()) + ":" + id +
                    "|version=" + std::to_string(version);
  key += "|capabilities=";
  append_keys(key, required_capabilities);
  key += "|inputs=";
  append_keys(key, consumed_fact_families);
  key += "|facts=";
  append_keys(key, produced_fact_families);
  key += "|catalog=";
  append_keys(key, catalog_versions);
  key += "|dependencies=";
  append_keys(key, dependencies);
  key += "|scope=" + std::to_string(static_cast<std::uint8_t>(scope));
  key += "|traversal=" + std::to_string(static_cast<std::uint8_t>(traversal));
  key += "|completeness=" +
         std::to_string(static_cast<std::uint8_t>(completeness));
  key += "|trust=" + std::to_string(static_cast<std::uint8_t>(trust));
  key += "|budget=" + std::to_string(budget.max_visited_constructs) + ',' +
         std::to_string(budget.max_emitted_facts) + ',' +
         std::to_string(budget.max_diagnostics) + ',' +
         std::to_string(budget.declared);
  return key;
}

PassBudgetExceeded::PassBudgetExceeded(std::string pass_id,
                                       std::string dimension)
    : std::runtime_error("pass budget exceeded: " + pass_id + " (" + dimension +
                         ")"),
      pass_id_(std::move(pass_id)), dimension_(std::move(dimension)) {}

FrontendCapabilityUnavailable::FrontendCapabilityUnavailable(
    std::string pass_id, FrontendCapability capability)
    : std::runtime_error("frontend capability unavailable for pass: " +
                         pass_id),
      pass_id_(std::move(pass_id)), capability_(capability) {}

auto FrontendSession::supports(FrontendCapability capability) const -> bool {
  switch (capability) {
  case FrontendCapability::ast:
    return ast_context != nullptr;
  case FrontendCapability::preprocessor:
    return preprocessor != nullptr;
  case FrontendCapability::cfg:
    return cfg_available;
  case FrontendCapability::templates:
    return templates_available;
  }
  return false;
}

void PassMetrics::bind(std::string pass_id, PassBudget budget) {
  pass_id_ = std::move(pass_id);
  budget_ = budget;
}

void PassMetrics::enforce(std::size_t value, std::size_t limit,
                          const char *dimension) {
  if (limit != 0 && value > limit) {
    budget_exhausted = true;
    diagnostic_messages.emplace_back(std::string("budget exceeded: ") +
                                     dimension);
    throw PassBudgetExceeded(pass_id_, dimension);
  }
}

void PassMetrics::note_visited(std::size_t count) {
  visited_constructs += count;
  enforce(visited_constructs, budget_.max_visited_constructs, "visited");
}

void PassMetrics::note_emitted(std::size_t count) {
  emitted_facts += count;
  enforce(emitted_facts, budget_.max_emitted_facts, "emitted");
}

void PassMetrics::note_diagnostic(std::string message) {
  ++diagnostics;
  diagnostic_messages.push_back(std::move(message));
  enforce(diagnostics, budget_.max_diagnostics, "diagnostics");
}

BudgetedStatementFactPorts::BudgetedStatementFactPorts(
    StatementFactPorts &ports, PassMetrics &metrics)
    : ports_(ports), metrics_(metrics) {}

auto BudgetedStatementFactPorts::lookup_symbol_id(
    const std::string &usr, const std::optional<std::string> &source)
    -> std::optional<std::int64_t> {
  return ports_.lookup_symbol_id(usr, source);
}
auto BudgetedStatementFactPorts::mint_symbol(const MintRequest &request)
    -> std::int64_t {
  metrics_.note_emitted();
  return ports_.mint_symbol(request);
}
auto BudgetedStatementFactPorts::file_id_for_path(const std::string &path)
    -> std::optional<std::int64_t> {
  return ports_.file_id_for_path(path);
}
auto BudgetedStatementFactPorts::type_arg_candidates(const std::string &name,
                                                     bool qualified)
    -> std::vector<TypeArgCandidate> {
  return ports_.type_arg_candidates(name, qualified);
}
auto BudgetedStatementFactPorts::symbol_ids_by_qual_name_kind(
    const std::string &qual_name, const std::string &kind_name)
    -> std::vector<std::int64_t> {
  return ports_.symbol_ids_by_qual_name_kind(qual_name, kind_name);
}
auto BudgetedStatementFactPorts::add_edge(const EdgeRecord &edge)
    -> std::int64_t {
  metrics_.note_emitted();
  return ports_.add_edge(edge);
}
auto BudgetedStatementFactPorts::ensure_edge(const EdgeRecord &edge)
    -> std::int64_t {
  metrics_.note_emitted();
  return ports_.ensure_edge(edge);
}
void BudgetedStatementFactPorts::add_edge_site(const EdgeSiteRecord &site) {
  metrics_.note_emitted();
  ports_.add_edge_site(site);
}
void BudgetedStatementFactPorts::add_call_arg(const CallArgRecord &arg) {
  metrics_.note_emitted();
  ports_.add_call_arg(arg);
}
void BudgetedStatementFactPorts::add_template_param(
    const TemplateParamRecord &param) {
  metrics_.note_emitted();
  ports_.add_template_param(param);
}
void BudgetedStatementFactPorts::add_template_arg(
    const TemplateArgRecord &arg) {
  metrics_.note_emitted();
  ports_.add_template_arg(arg);
}
auto BudgetedStatementFactPorts::intern_type_node(const TypeNodeRecord &node)
    -> std::int64_t {
  metrics_.note_emitted();
  return ports_.intern_type_node(node);
}
void BudgetedStatementFactPorts::add_type_edge(std::int64_t src_id,
                                               std::int64_t kind,
                                               std::int64_t position,
                                               std::int64_t dst_id) {
  metrics_.note_emitted();
  ports_.add_type_edge(src_id, kind, position, dst_id);
}
void BudgetedStatementFactPorts::replace_parameters(
    std::int64_t owner_id, const std::vector<ParameterRecord> &parameters) {
  metrics_.note_emitted(parameters.size());
  ports_.replace_parameters(owner_id, parameters);
}
void BudgetedStatementFactPorts::add_symbol_type(std::int64_t symbol_id,
                                                 std::int64_t kind,
                                                 std::int64_t type_id) {
  metrics_.note_emitted();
  ports_.add_symbol_type(symbol_id, kind, type_id);
}
void BudgetedStatementFactPorts::emit(const EvidenceRecord &evidence) {
  metrics_.note_emitted();
  ports_.emit(evidence);
}

BudgetedDeclarationPassPorts::BudgetedDeclarationPassPorts(
    DeclarationPassPorts &ports, PassMetrics &metrics)
    : ports_(ports), metrics_(metrics) {}

auto BudgetedDeclarationPassPorts::lookup_symbol_id(
    const std::string &usr, const std::optional<std::string> &source)
    -> std::optional<std::int64_t> {
  return ports_.lookup_symbol_id(usr, source);
}
auto BudgetedDeclarationPassPorts::mint_symbol(const MintRequest &request)
    -> std::int64_t {
  metrics_.note_emitted();
  return ports_.mint_symbol(request);
}
auto BudgetedDeclarationPassPorts::file_id_for_path(const std::string &path)
    -> std::optional<std::int64_t> {
  return ports_.file_id_for_path(path);
}
auto BudgetedDeclarationPassPorts::type_arg_candidates(const std::string &name,
                                                       bool qualified)
    -> std::vector<TypeArgCandidate> {
  return ports_.type_arg_candidates(name, qualified);
}
auto BudgetedDeclarationPassPorts::symbol_ids_by_qual_name_kind(
    const std::string &qual_name, const std::string &kind_name)
    -> std::vector<std::int64_t> {
  return ports_.symbol_ids_by_qual_name_kind(qual_name, kind_name);
}
auto BudgetedDeclarationPassPorts::add_edge(const EdgeRecord &edge)
    -> std::int64_t {
  metrics_.note_emitted();
  return ports_.add_edge(edge);
}
auto BudgetedDeclarationPassPorts::ensure_edge(const EdgeRecord &edge)
    -> std::int64_t {
  metrics_.note_emitted();
  return ports_.ensure_edge(edge);
}
void BudgetedDeclarationPassPorts::add_edge_site(const EdgeSiteRecord &site) {
  metrics_.note_emitted();
  ports_.add_edge_site(site);
}
void BudgetedDeclarationPassPorts::add_call_arg(const CallArgRecord &arg) {
  metrics_.note_emitted();
  ports_.add_call_arg(arg);
}
void BudgetedDeclarationPassPorts::add_template_param(
    const TemplateParamRecord &param) {
  metrics_.note_emitted();
  ports_.add_template_param(param);
}
void BudgetedDeclarationPassPorts::add_template_arg(
    const TemplateArgRecord &arg) {
  metrics_.note_emitted();
  ports_.add_template_arg(arg);
}
auto BudgetedDeclarationPassPorts::intern_type_node(const TypeNodeRecord &node)
    -> std::int64_t {
  metrics_.note_emitted();
  return ports_.intern_type_node(node);
}
void BudgetedDeclarationPassPorts::add_type_edge(std::int64_t src_id,
                                                 std::int64_t kind,
                                                 std::int64_t position,
                                                 std::int64_t dst_id) {
  metrics_.note_emitted();
  ports_.add_type_edge(src_id, kind, position, dst_id);
}
void BudgetedDeclarationPassPorts::replace_parameters(
    std::int64_t owner_id, const std::vector<ParameterRecord> &parameters) {
  metrics_.note_emitted(parameters.size());
  ports_.replace_parameters(owner_id, parameters);
}
void BudgetedDeclarationPassPorts::add_symbol_type(std::int64_t symbol_id,
                                                   std::int64_t kind,
                                                   std::int64_t type_id) {
  metrics_.note_emitted();
  ports_.add_symbol_type(symbol_id, kind, type_id);
}
void BudgetedPresentationIntentEmitter::emit(const PresentationIntent &intent) {
  metrics_.note_emitted();
  emitter_.emit(intent);
}

BudgetedNamespacePassPorts::BudgetedNamespacePassPorts(
    NamespacePassPorts &ports, PassMetrics &metrics)
    : ports_(ports), metrics_(metrics) {}

auto BudgetedNamespacePassPorts::lookup_symbol_id(
    const std::string &usr, const std::optional<std::string> &source)
    -> std::optional<std::int64_t> {
  return ports_.lookup_symbol_id(usr, source);
}
auto BudgetedNamespacePassPorts::mint_symbol(const MintRequest &request)
    -> std::int64_t {
  metrics_.note_emitted();
  return ports_.mint_symbol(request);
}
auto BudgetedNamespacePassPorts::file_id_for_path(const std::string &path)
    -> std::optional<std::int64_t> {
  return ports_.file_id_for_path(path);
}
auto BudgetedNamespacePassPorts::type_arg_candidates(const std::string &name,
                                                     bool qualified)
    -> std::vector<TypeArgCandidate> {
  return ports_.type_arg_candidates(name, qualified);
}
auto BudgetedNamespacePassPorts::symbol_ids_by_qual_name_kind(
    const std::string &qual_name, const std::string &kind_name)
    -> std::vector<std::int64_t> {
  return ports_.symbol_ids_by_qual_name_kind(qual_name, kind_name);
}
auto BudgetedNamespacePassPorts::add_edge(const EdgeRecord &edge)
    -> std::int64_t {
  metrics_.note_emitted();
  return ports_.add_edge(edge);
}
auto BudgetedNamespacePassPorts::ensure_edge(const EdgeRecord &edge)
    -> std::int64_t {
  metrics_.note_emitted();
  return ports_.ensure_edge(edge);
}
void BudgetedNamespacePassPorts::add_edge_site(const EdgeSiteRecord &site) {
  metrics_.note_emitted();
  ports_.add_edge_site(site);
}
void BudgetedNamespacePassPorts::add_call_arg(const CallArgRecord &arg) {
  metrics_.note_emitted();
  ports_.add_call_arg(arg);
}
void BudgetedNamespacePassPorts::add_template_param(
    const TemplateParamRecord &param) {
  metrics_.note_emitted();
  ports_.add_template_param(param);
}
void BudgetedNamespacePassPorts::add_template_arg(
    const TemplateArgRecord &arg) {
  metrics_.note_emitted();
  ports_.add_template_arg(arg);
}

BudgetedDefinitionScopeEmitter::BudgetedDefinitionScopeEmitter(
    DefinitionScopeEmitter &definitions, PassMetrics &metrics)
    : definitions_(definitions), metrics_(metrics) {}

auto BudgetedDefinitionScopeEmitter::get_or_create_definition(
    std::int64_t symbol_id, std::int64_t file_id, std::int64_t line,
    std::int64_t col, std::int64_t end_line, std::int64_t end_col,
    const std::optional<std::string> &init_text) -> std::int64_t {
  metrics_.note_emitted();
  return definitions_.get_or_create_definition(symbol_id, file_id, line, col,
                                               end_line, end_col, init_text);
}
void BudgetedDefinitionScopeEmitter::add_def_edge(std::int64_t definition_id,
                                                  std::int64_t destination_id,
                                                  std::int64_t kind) {
  metrics_.note_emitted();
  definitions_.add_def_edge(definition_id, destination_id, kind);
}
auto BudgetedDefinitionScopeEmitter::body_edge_count(std::int64_t symbol_id)
    -> std::size_t {
  return definitions_.body_edge_count(symbol_id);
}
void BudgetedDefinitionScopeEmitter::copy_body_edges_to_def_edge(
    std::int64_t definition_id, std::int64_t symbol_id) {
  metrics_.note_emitted(body_edge_count(symbol_id));
  definitions_.copy_body_edges_to_def_edge(definition_id, symbol_id);
}

auto PassExecutionReport::find(const std::string &id) const
    -> const PassExecutionRecord * {
  const auto found = std::ranges::find_if(
      passes, [&id](const PassExecutionRecord &record) -> bool {
        return record.descriptor.id == id;
      });
  return found == passes.end() ? nullptr : &*found;
}

void ExtractionPassRegistry::register_pass(ExtractionPassDescriptor descriptor,
                                           Runner runner) {
  const bool source_pass = descriptor.consumed_fact_families.empty();
  if (descriptor.id.empty() || descriptor.version == 0 || !runner ||
      descriptor.required_capabilities.empty() ||
      descriptor.produced_fact_families.empty() ||
      descriptor.catalog_versions.empty() || !descriptor.budget.declared ||
      (source_pass && !descriptor.dependencies.empty()) ||
      std::ranges::any_of(
          descriptor.consumed_fact_families,
          [](const std::string &family) { return family.empty(); }) ||
      std::ranges::any_of(
          descriptor.produced_fact_families,
          [](const std::string &family) { return family.empty(); })) {
    throw std::invalid_argument(
        "invalid extraction pass registration: incomplete metadata");
  }
  if (std::ranges::any_of(entries_, [&descriptor](const Entry &entry) -> bool {
        return entry.descriptor.id == descriptor.id;
      })) {
    throw std::invalid_argument("duplicate extraction pass: " + descriptor.id);
  }
  entries_.push_back(
      {.descriptor = std::move(descriptor), .runner = std::move(runner)});
}

auto ExtractionPassRegistry::descriptor(const std::string &id) const
    -> const ExtractionPassDescriptor & {
  const auto found =
      std::ranges::find_if(entries_, [&id](const Entry &entry) -> bool {
        return entry.descriptor.id == id;
      });
  if (found == entries_.end()) {
    throw std::invalid_argument("unknown extraction pass: " + id);
  }
  return found->descriptor;
}

auto ExtractionPassRegistry::descriptors() const
    -> std::vector<ExtractionPassDescriptor> {
  std::vector<ExtractionPassDescriptor> result;
  result.reserve(entries_.size());
  for (const Entry &entry : entries_) {
    result.push_back(entry.descriptor);
  }
  std::ranges::sort(result, {}, &ExtractionPassDescriptor::stable_key);
  return result;
}

auto ExtractionPassRegistry::run(const IndexingPlan &plan,
                                 FrontendSession *session) const
    -> PassExecutionReport {
  if (plan.steps().empty()) {
    throw std::invalid_argument("an extraction plan must contain a pass");
  }
  PassExecutionReport report;
  for (const IndexingPlanStep &step : plan.steps()) {
    const auto found =
        std::ranges::find_if(entries_, [&step](const Entry &entry) -> bool {
          return entry.descriptor.id == step.pass_id;
        });
    if (found == entries_.end()) {
      throw std::invalid_argument("plan references unknown extraction pass: " +
                                  step.pass_id);
    }
    for (const std::string &dependency : found->descriptor.dependencies) {
      if (!report.find(dependency)) {
        throw std::invalid_argument("pass dependency is not ordered: " +
                                    found->descriptor.id + " -> " + dependency);
      }
    }
    if (report.find(found->descriptor.id)) {
      throw std::invalid_argument("extraction pass appears twice: " +
                                  found->descriptor.id);
    }

    PassMetrics metrics;
    PassBudget budget = found->descriptor.budget;
    if (session != nullptr) {
      if (const auto override_budget =
              session->budget_overrides.find(found->descriptor.id);
          override_budget != session->budget_overrides.end()) {
        budget = override_budget->second;
      }
    }
    metrics.bind(found->descriptor.id, budget);
    if (session != nullptr) {
      for (const FrontendCapability capability :
           found->descriptor.required_capabilities) {
        if (!session->supports(capability)) {
          throw FrontendCapabilityUnavailable(found->descriptor.id, capability);
        }
      }
    }
    const auto started = std::chrono::steady_clock::now();
    FrontendSession pass_session;
    std::optional<BudgetedStatementFactPorts> statement_ports;
    std::optional<BudgetedDeclarationPassPorts> declaration_ports;
    std::optional<BudgetedNamespacePassPorts> namespace_ports;
    std::optional<BudgetedDefinitionScopeEmitter> definition_ports;
    std::optional<BudgetedPresentationIntentEmitter> presentation_intents;
    if (session != nullptr) {
      pass_session = *session;
      if (session->statement_ports != nullptr) {
        statement_ports.emplace(*session->statement_ports, metrics);
        pass_session.statement_ports = &*statement_ports;
        pass_session.evidence = &*statement_ports;
      }
      if (session->presentation_intents != nullptr) {
        presentation_intents.emplace(*session->presentation_intents, metrics);
        pass_session.presentation_intents = &*presentation_intents;
      }
      if (session->declaration_ports != nullptr) {
        declaration_ports.emplace(*session->declaration_ports, metrics);
        pass_session.declaration_ports = &*declaration_ports;
      }
      if (session->namespace_ports != nullptr) {
        namespace_ports.emplace(*session->namespace_ports, metrics);
        pass_session.namespace_ports = &*namespace_ports;
      }
      if (session->definition_ports != nullptr) {
        definition_ports.emplace(*session->definition_ports, metrics);
        pass_session.definition_ports = &*definition_ports;
      }
    }
    PassExecutionContext context{.metrics = metrics,
                                 .session = session != nullptr ? &pass_session
                                                               : nullptr};
    found->runner(context);
    metrics.elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - started);
    if ((budget.max_visited_constructs != 0 &&
         metrics.visited_constructs > budget.max_visited_constructs) ||
        (budget.max_emitted_facts != 0 &&
         metrics.emitted_facts > budget.max_emitted_facts) ||
        (budget.max_diagnostics != 0 &&
         metrics.diagnostics > budget.max_diagnostics)) {
      metrics.budget_exhausted = true;
      metrics.note_diagnostic("deterministic pass budget exceeded");
    }
    report.passes.push_back(
        {.descriptor = found->descriptor, .metrics = std::move(metrics)});
  }
  return report;
}

void register_frontend_pass_provider(FrontendPassProvider provider) {
  if (!provider) {
    throw std::invalid_argument("frontend pass provider must be callable");
  }
  const std::scoped_lock lock(provider_mutex);
  provider_list.push_back(std::move(provider));
}

void clear_frontend_pass_providers() {
  const std::scoped_lock lock(provider_mutex);
  provider_list.clear();
}

auto frontend_pass_providers() -> std::vector<FrontendPassProvider> {
  const std::scoped_lock lock(provider_mutex);
  return provider_list;
}

} // namespace cidx::ast
