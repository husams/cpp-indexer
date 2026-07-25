#include "ast/fact_batch.hpp"

#include <algorithm>
#include <tuple>

namespace cidx::ast {

namespace {

template <typename T, typename Key>
void canonicalize_records(std::vector<T> &records, Key key) {
  std::ranges::sort(records, {}, key);
  records.erase(std::unique(records.begin(), records.end(),
                            [&key](const T &left, const T &right) -> auto {
                              return key(left) == key(right);
                            }),
                records.end());
}

} // namespace

void FactBatch::canonicalize() {
  canonicalize_records(
      symbols,
      [](const SymbolRecord &record)
          -> std::tuple<const std::string &, const std::string &,
                        const std::string &, const long long &,
                        const long long &, const long long &,
                        const long long &> {
        return std::tie(record.file, record.usr, record.spelling, record.line,
                        record.col, record.end_line, record.end_col);
      });
  canonicalize_records(
      relations,
      [](const EdgeRecord &record)
          -> std::tuple<const long long &, const long long &, const long long &,
                        const long long &, const std::optional<long long> &,
                        const std::optional<long long> &> {
        return std::tie(record.src_id, record.dst_id, record.kind, record.count,
                        record.base_access, record.is_virtual);
      });
  canonicalize_records(
      edge_sites,
      [](const EdgeSiteRecord &record)
          -> std::tuple<const long long &, const long long &, const long long &,
                        const long long &, const long long &,
                        const std::optional<std::string> &,
                        const std::optional<std::string> &,
                        const std::optional<std::string> &,
                        const std::optional<long long> &,
                        const std::optional<long long> &> {
        return std::tie(record.edge_id, record.file_id, record.line, record.col,
                        record.conditional, record.recv_src_kind,
                        record.recv_type_usr, record.recv_decl_usr,
                        record.recv_param_pos, record.recv_type_is_value);
      });
  canonicalize_records(
      call_args,
      [](const CallArgRecord &record)
          -> std::tuple<const long long &, const long long &, const long long &,
                        const long long &, const long long &,
                        const std::string &, const std::optional<std::string> &,
                        const std::optional<std::string> &,
                        const std::optional<std::string> &,
                        const std::optional<long long> &> {
        return std::tie(record.edge_id, record.position, record.file_id,
                        record.line, record.col, record.src_kind,
                        record.type_usr, record.decl_usr, record.callee_usr,
                        record.type_is_value);
      });
  canonicalize_records(
      template_params,
      [](const TemplateParamRecord &record)
          -> std::tuple<const long long &, const long long &, const long long &,
                        const std::optional<std::string> &,
                        const std::optional<std::string> &,
                        const std::optional<long long> &,
                        const std::optional<long long> &,
                        const std::optional<long long> &> {
        return std::tie(record.owner_id, record.position, record.param_kind,
                        record.name, record.default_txt, record.type_id,
                        record.default_type_id, record.default_ref_id);
      });
  canonicalize_records(
      template_args,
      [](const TemplateArgRecord &record)
          -> std::tuple<const long long &, const long long &, const long long &,
                        const long long &, const std::optional<long long> &,
                        const std::optional<std::string> &,
                        const std::optional<long long> &> {
        return std::tie(record.owner_id, record.position, record.pack_index,
                        record.arg_kind, record.ref_id, record.literal,
                        record.type_id);
      });
  canonicalize_records(
      type_nodes,
      [](const TypeNodeRecord &record)
          -> std::tuple<const std::string &, const std::string &,
                        const long long &, const bool &, const bool &,
                        const bool &, const std::optional<std::string> &,
                        const std::optional<long long> &> {
        return std::tie(record.type_key, record.spelling, record.kind,
                        record.is_const, record.is_volatile, record.is_restrict,
                        record.decl_usr, record.canonical_id);
      });
  canonicalize_records(type_edges,
                       [](const TypeEdgeRecord &record)
                           -> std::tuple<const long long &, const long long &,
                                         const long long &, const long long &> {
                         return std::tie(record.src_id, record.kind,
                                         record.position, record.dst_id);
                       });
  canonicalize_records(
      parameters,
      [](const ParameterFactRecord &record)
          -> std::tuple<const long long &, const long long &, const long long &,
                        const std::optional<std::string> &,
                        const std::optional<long long> &,
                        const std::optional<long long> &,
                        const std::optional<long long> &,
                        const std::optional<std::string> &,
                        const std::optional<std::string> &,
                        const std::optional<std::string> &,
                        const std::optional<long long> &,
                        const std::optional<long long> &,
                        const std::optional<long long> &> {
        const ParameterRecord &parameter = record.parameter;
        return std::tie(record.owner_id, parameter.position,
                        parameter.pack_index, parameter.name, parameter.type_id,
                        parameter.declared_type_id, parameter.adjusted_type_id,
                        parameter.default_text, parameter.default_origin,
                        parameter.reference_semantics, parameter.file_id,
                        parameter.line, parameter.col);
      });
  canonicalize_records(symbol_types,
                       [](const SymbolTypeRecord &record)
                           -> std::tuple<const long long &, const long long &,
                                         const long long &> {
                         return std::tie(record.symbol_id, record.kind,
                                         record.type_id);
                       });
  canonicalize_records(
      definitions,
      [](const DefinitionFactRecord &record)
          -> std::tuple<const long long &, const long long &, const long long &,
                        const long long &, const long long &, const long long &,
                        const long long &, const std::optional<std::string> &> {
        return std::tie(record.id, record.symbol_id, record.file_id,
                        record.line, record.col, record.end_line,
                        record.end_col, record.init_text);
      });
  canonicalize_records(definition_edges,
                       [](const DefinitionEdgeRecord &record)
                           -> std::tuple<const long long &, const long long &,
                                         const long long &> {
                         return std::tie(record.definition_id,
                                         record.destination_id, record.kind);
                       });
  canonicalize_records(
      evidence,
      [](const EvidenceRecord &record)
          -> std::tuple<const std::string &, const std::string &,
                        const std::string &, const long long &,
                        const long long &, const cidx::ast::FactCompleteness &,
                        const cidx::ast::FactTrust &, const std::string &> {
        return std::tie(record.producer, record.construct, record.file,
                        record.line, record.col, record.completeness,
                        record.trust, record.detail);
      });
}

FactBatchRecorder::FactBatchRecorder(std::string producer) {
  batch_.producer = std::move(producer);
}

void FactBatchRecorder::emit(const SymbolRecord &symbol) {
  if (!symbol_ids_.contains(symbol.usr)) {
    symbol_ids_.emplace(symbol.usr, next_id_++);
  }
  batch_.symbols.push_back(symbol);
}

void FactBatchRecorder::emit(const EvidenceRecord &evidence) {
  batch_.evidence.push_back(evidence);
}

auto FactBatchRecorder::lookup_symbol_id(
    const std::string &usr,
    const std::optional<std::string> & /*identity_source*/)
    -> std::optional<std::int64_t> {
  if (const auto found = symbol_ids_.find(usr); found != symbol_ids_.end()) {
    return found->second;
  }
  return std::nullopt;
}

auto FactBatchRecorder::mint_symbol(const MintRequest &request)
    -> std::int64_t {
  if (const auto found = symbol_ids_.find(request.usr);
      found != symbol_ids_.end()) {
    return found->second;
  }
  const std::int64_t id = next_id_++;
  symbol_ids_.emplace(request.usr, id);
  batch_.symbols.push_back(SymbolRecord{
      .file = request.identity_source.value_or(request.decl_path.value_or("")),
      .usr = request.usr,
      .spelling = request.spelling,
      .kind = -1,
      .qual_name = request.qual_name,
      .display_name = request.display_name,
      .type_info = request.type_info,
      .decl_line = request.decl_line,
      .decl_col = request.decl_col,
      .is_instantiation = request.is_instantiation,
      .linkage = request.linkage});
  return id;
}

auto FactBatchRecorder::file_id_for_path(const std::string & /*path*/)
    -> std::optional<std::int64_t> {
  return std::nullopt;
}

auto FactBatchRecorder::type_arg_candidates(const std::string &name,
                                            bool qualified)
    -> std::vector<TypeArgCandidate> {
  std::vector<TypeArgCandidate> result;
  for (const SymbolRecord &symbol : batch_.symbols) {
    const std::optional<std::string> &candidate =
        qualified ? symbol.qual_name
                  : std::optional<std::string>(symbol.spelling);
    if (candidate && *candidate == name) {
      const auto found = symbol_ids_.find(symbol.usr);
      if (found != symbol_ids_.end()) {
        result.push_back({.id = found->second,
                          .kind_name = {},
                          .is_instantiation = symbol.is_instantiation});
      }
    }
  }
  return result;
}

auto FactBatchRecorder::symbol_ids_by_qual_name_kind(
    const std::string &qual_name, const std::string & /*kind_name*/)
    -> std::vector<std::int64_t> {
  std::vector<std::int64_t> result;
  for (const SymbolRecord &symbol : batch_.symbols) {
    if (symbol.qual_name && *symbol.qual_name == qual_name) {
      if (const auto found = symbol_ids_.find(symbol.usr);
          found != symbol_ids_.end()) {
        result.push_back(found->second);
      }
    }
  }
  return result;
}

auto FactBatchRecorder::edge_key(const EdgeRecord &edge) -> std::string {
  return std::to_string(edge.src_id) + ":" + std::to_string(edge.dst_id) + ":" +
         std::to_string(edge.kind);
}

auto FactBatchRecorder::add_edge(const EdgeRecord &edge) -> std::int64_t {
  const std::string key = edge_key(edge);
  if (const auto found = edge_ids_.find(key); found != edge_ids_.end()) {
    for (EdgeRecord &record : batch_.relations) {
      if (edge_key(record) == key) {
        record.count += edge.count;
        break;
      }
    }
    return found->second;
  }
  const std::int64_t id = next_id_++;
  edge_ids_.emplace(key, id);
  batch_.relations.push_back(edge);
  return id;
}

auto FactBatchRecorder::ensure_edge(const EdgeRecord &edge) -> std::int64_t {
  const std::string key = edge_key(edge);
  if (const auto found = edge_ids_.find(key); found != edge_ids_.end()) {
    return found->second;
  }
  const std::int64_t id = next_id_++;
  edge_ids_.emplace(key, id);
  batch_.relations.push_back(edge);
  return id;
}

void FactBatchRecorder::add_edge_site(const EdgeSiteRecord &site) {
  batch_.edge_sites.push_back(site);
}

void FactBatchRecorder::add_call_arg(const CallArgRecord &arg) {
  batch_.call_args.push_back(arg);
}

void FactBatchRecorder::add_template_param(const TemplateParamRecord &param) {
  batch_.template_params.push_back(param);
}

void FactBatchRecorder::add_template_arg(const TemplateArgRecord &arg) {
  batch_.template_args.push_back(arg);
}

auto FactBatchRecorder::intern_type_node(const TypeNodeRecord &node)
    -> std::int64_t {
  if (const auto found = type_ids_.find(node.type_key);
      found != type_ids_.end()) {
    return found->second;
  }
  const std::int64_t id = next_id_++;
  type_ids_.emplace(node.type_key, id);
  batch_.type_nodes.push_back(node);
  return id;
}

void FactBatchRecorder::add_type_edge(std::int64_t src_id, std::int64_t kind,
                                      std::int64_t position,
                                      std::int64_t dst_id) {
  batch_.type_edges.push_back(
      {.src_id = src_id, .kind = kind, .position = position, .dst_id = dst_id});
}

void FactBatchRecorder::replace_parameters(
    std::int64_t owner_id, const std::vector<ParameterRecord> &parameters) {
  std::erase_if(batch_.parameters,
                [owner_id](const ParameterFactRecord &record) -> bool {
                  return record.owner_id == owner_id;
                });
  for (const ParameterRecord &parameter : parameters) {
    batch_.parameters.push_back({.owner_id = owner_id, .parameter = parameter});
  }
}

void FactBatchRecorder::add_symbol_type(std::int64_t symbol_id,
                                        std::int64_t kind,
                                        std::int64_t type_id) {
  batch_.symbol_types.push_back(
      {.symbol_id = symbol_id, .kind = kind, .type_id = type_id});
}

auto FactBatchRecorder::get_or_create_definition(
    std::int64_t symbol_id, std::int64_t file_id, std::int64_t line,
    std::int64_t col, std::int64_t end_line, std::int64_t end_col,
    const std::optional<std::string> &init_text) -> std::int64_t {
  const std::int64_t id = next_id_++;
  batch_.definitions.push_back({.id = id,
                                .symbol_id = symbol_id,
                                .file_id = file_id,
                                .line = line,
                                .col = col,
                                .end_line = end_line,
                                .end_col = end_col,
                                .init_text = init_text});
  return id;
}

void FactBatchRecorder::add_def_edge(std::int64_t definition_id,
                                     std::int64_t destination_id,
                                     std::int64_t kind) {
  batch_.definition_edges.push_back({.definition_id = definition_id,
                                     .destination_id = destination_id,
                                     .kind = kind});
}

void FactBatchRecorder::copy_body_edges_to_def_edge(std::int64_t definition_id,
                                                    std::int64_t symbol_id) {
  for (const EdgeRecord &edge : batch_.relations) {
    if (edge.src_id == symbol_id) {
      add_def_edge(definition_id, edge.dst_id, edge.kind);
    }
  }
}

auto FactBatchRecorder::lookup_display_name(std::int64_t symbol_id)
    -> std::optional<std::string> {
  const auto found = display_names_.find(symbol_id);
  return found == display_names_.end()
             ? std::nullopt
             : std::optional<std::string>(found->second);
}

void FactBatchRecorder::update_display_name(std::int64_t symbol_id,
                                            const std::string &display) {
  display_names_[symbol_id] = display;
  for (SymbolRecord &symbol : batch_.symbols) {
    const auto found = symbol_ids_.find(symbol.usr);
    if (found != symbol_ids_.end() && found->second == symbol_id) {
      symbol.display_name = display;
    }
  }
}

auto FactBatchRecorder::canonical_batch() const -> FactBatch {
  FactBatch result = batch_;
  result.canonicalize();
  return result;
}

} // namespace cidx::ast
