#!/usr/bin/env bash
# e2e_lt_gcc_driver.sh — prove the LibTooling engine (CIDX_INDEX_ENGINE=lt)
# indexes GCC-toolchain code identically to the libclang engine, using cidx's
# driver introspection to replicate the GCC driver's include search paths.
#
#   CIDX=<path/to/cidx> [DRIVER=g++] ./scripts/e2e_lt_gcc_driver.sh
#
# Two checks:
#   1. driver-replication proof: with -nostdinc++ (clang adds NO C++ std search
#      of its own), <string> resolves ONLY if cidx replicates the driver's
#      libstdc++ -isystem paths. A bare clang++ -nostdinc++ fails here.
#   2. dual-engine parity: classic vs LT produce byte-identical Layer-0 tables.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CIDX="${CIDX:-$(dirname "$SCRIPT_DIR")/build-static/cidx}"
DRIVER="${DRIVER:-g++}"
[ -x "$CIDX" ] || { echo "error: cidx not found at $CIDX (set CIDX=...)" >&2; exit 1; }

WORK="$(mktemp -d)"; trap 'rm -rf "$WORK"' EXIT
PROJ="$WORK/proj"; mkdir -p "$PROJ"

cat > "$PROJ/gcc_driver_test.cpp" <<'SRC'
#include <string>
#include <vector>
#include <memory>
namespace app {
struct Shape { virtual ~Shape() = default; virtual double area() const = 0; };
struct Circle : Shape {
  explicit Circle(double r) : r_(r) {}
  double area() const override { return 3.14 * r_ * r_; }
  double r_;
};
template <class T> T maxv(const std::vector<T>& xs) {
  T best{};
  for (const auto& x : xs) if (x > best) best = x;
  return best;
}
double total(const std::vector<std::shared_ptr<Shape>>& shapes) {
  double s = 0;
  for (const auto& sh : shapes) s += sh->area();
  return s;
}
std::string label(const Circle& c) {
  return std::string("circle:") + std::to_string(c.area());
}
} // namespace app
SRC
( cd "$PROJ" && git init -q . )

index_both() {
  local cc="$1"
  printf '[{"directory":"%s","command":"%s %s -std=c++17 -c gcc_driver_test.cpp -o t.o","file":"gcc_driver_test.cpp"}]\n' \
    "$PROJ" "$DRIVER" "$cc" > "$PROJ/compile_commands.json"
  A="$WORK/a"; B="$WORK/b"; rm -rf "$A" "$B"; mkdir -p "$A" "$B"
  INDEXER_CACHE="$A" "$CIDX" import --db "$PROJ" >/dev/null
  INDEXER_CACHE="$A" "$CIDX" index >/dev/null 2>&1 || true
  INDEXER_CACHE="$B" "$CIDX" import --db "$PROJ" >/dev/null
  CIDX_INDEX_ENGINE=lt INDEXER_CACHE="$B" "$CIDX" index >/dev/null 2>&1 || true
}

fail=0

# --- 1. driver-replication proof (-nostdinc++) --------------------------------
index_both "-nostdinc++"
for eng in a b; do
  db="$WORK/$eng/index.db"
  fatal=$(sqlite3 "$db" "SELECT COUNT(*) FROM diagnostic WHERE severity>=4")
  syms=$(sqlite3 "$db" "SELECT COUNT(*) FROM symbol")
  name=$([ "$eng" = a ] && echo classic || echo lt)
  if [ "$fatal" = 0 ] && [ "$syms" -gt 0 ]; then
    echo "PASS  driver-replication [$name]: <string> resolved via $DRIVER (-nostdinc++), $syms symbols"
  else
    echo "FAIL  driver-replication [$name]: $fatal fatal diag(s), $syms symbols"; fail=1
  fi
done

# --- 2. dual-engine parity ----------------------------------------------------
index_both ""
tables_ok=1
while IFS='|' read -r label query; do
  if ! diff <(sqlite3 "$WORK/a/index.db" "$query") \
            <(sqlite3 "$WORK/b/index.db" "$query") >/dev/null; then
    echo "FAIL  parity [$label]"; tables_ok=0; fail=1
  fi
done <<SQL
symbol|SELECT usr,kind,line,col,is_definition,resolved FROM symbol ORDER BY usr
edge|SELECT s.usr,d.usr,e.kind,e.count FROM edge e JOIN symbol s ON s.id=e.src_id JOIN symbol d ON d.id=e.dst_id ORDER BY 1,2,3
edge_site|SELECT s.usr,d.usr,e.kind,es.line,es.col,COALESCE(es.recv_src_kind,'') FROM edge_site es JOIN edge e ON e.id=es.edge_id JOIN symbol s ON s.id=e.src_id JOIN symbol d ON d.id=e.dst_id ORDER BY 1,2,3,4,5
call_arg|SELECT s.usr,d.usr,ca.position,ca.src_kind FROM call_arg ca JOIN edge e ON e.id=ca.edge_id JOIN symbol s ON s.id=e.src_id JOIN symbol d ON d.id=e.dst_id ORDER BY 1,2,3
template_arg|SELECT s.usr,ta.position,ta.arg_kind,COALESCE(ta.literal,'') FROM template_arg ta JOIN symbol s ON s.id=ta.owner_id ORDER BY 1,2
definition|SELECT s.usr,d.line,d.col FROM definition d JOIN symbol s ON s.id=d.symbol_id ORDER BY 1,2
def_edge|SELECT s.usr,dd.usr,de.kind FROM def_edge de JOIN definition df ON df.id=de.src_def_id JOIN symbol s ON s.id=df.symbol_id JOIN symbol dd ON dd.id=de.dst_id ORDER BY 1,2,3
SQL
[ "$tables_ok" = 1 ] && echo "PASS  dual-engine parity: all 7 Layer-0 tables byte-identical (driver=$DRIVER)"

exit $fail
