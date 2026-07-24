#include "ui/assets.hpp"

#include <fstream>
#include <sstream>
#include <stdexcept>

#include "util/errors.hpp"

#ifndef CIDX_UI_ASSET_DIR
#define CIDX_UI_ASSET_DIR "web"
#endif

namespace cidx::ui {
namespace {

std::string read_asset(const std::string &name) {
  std::ifstream in(std::string(CIDX_UI_ASSET_DIR) + "/" + name,
                   std::ios::binary);
  if (!in) {
    throw CidxError("cidx ui: missing asset " + name);
  }
  std::ostringstream contents;
  contents << in.rdbuf();
  return contents.str();
}

void replace_all(std::string &text, const std::string &needle,
                 const std::string &replacement) {
  std::size_t pos = 0;
  while ((pos = text.find(needle, pos)) != std::string::npos) {
    text.replace(pos, needle.size(), replacement);
    pos += replacement.size();
  }
}

std::string script_safe(std::string value) {
  replace_all(value, "<", "\\u003c");
  replace_all(value, ">", "\\u003e");
  replace_all(value, "&", "\\u0026");
  return value;
}

} // namespace

std::string render_html(const json_out::Value &view) {
  std::string html = read_asset("index.html");
  replace_all(html, "__CIDX_STYLES__", read_asset("styles.css"));
  replace_all(html, "__CIDX_CYTOSCAPE__",
              read_asset("vendor/cytoscape.min.js"));
  replace_all(html, "__CIDX_APP__", read_asset("app.js"));
  replace_all(html, "__CIDX_GRAPH_VIEW__",
              script_safe(json_out::dumps_indent2(view)));
  return html;
}

} // namespace cidx::ui
