// Body-pass payloads: edge sites, call args, and value-source provenance
// (mirrors EdgeSite/CallArg rows and ast_body.cpp's ValueSource). Split from
// edge_records.hpp to keep decl-pass and body-pass payloads separately
// readable; both are plain data.
#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace cidx::lt {

struct EdgeSiteRecord {
  int64_t edge_id = 0;
  int64_t file_id = 0;
  int64_t line = 0;
  int64_t col = 0;
  int64_t conditional = 0;
  std::optional<std::string> recv_src_kind;
  std::optional<std::string> recv_type_usr;
  std::optional<std::string> recv_decl_usr;
  std::optional<int64_t> recv_param_pos;
  std::optional<int64_t> recv_type_is_value;
};

struct CallArgRecord {
  int64_t edge_id = 0;
  int64_t file_id = 0;
  int64_t line = 0;
  int64_t col = 0;
  int64_t position = 0;
  std::string src_kind;
  std::optional<std::string> type_usr;
  std::optional<std::string> decl_usr;
  std::optional<std::string> callee_usr;
  std::optional<int64_t> type_is_value;
};

// classify_value_source result (ast_body.cpp ValueSource).
struct ValueSource {
  std::string src_kind; // local|construct|member|global|call_result|literal|this|unknown
  std::string type_usr;
  std::string decl_usr;
  std::string callee_usr; // call_result only
};

} // namespace cidx::lt
