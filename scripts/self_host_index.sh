#!/usr/bin/env bash
# Build a throwaway self-index of CIDX's own repository under /tmp and run
# the HSE-71 self-host architecture-policy report against it.
#
# This is the "index the exact CIDX build/profile used by CI" half of
# HSE-71: it never touches the checked-in index.db (regenerating that
# remains a separate, canonical-checkout-only step per CLAUDE.md) and
# always writes into a fresh directory under /tmp so repeated runs never
# leak stale state or worktree-specific absolute paths into a shared
# artifact.
#
# Usage:
#   scripts/self_host_index.sh [--repo-root DIR] [--build-dir DIR] [--keep]
#
# Requires a built `cidx` binary; pass --build-dir to reuse an existing
# CMake build directory instead of configuring/building one.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR=""
KEEP=0
REPORT_ARGS=()

while [[ $# -gt 0 ]]; do
  case "$1" in
    --repo-root) REPO_ROOT="$2"; shift 2 ;;
    --build-dir) BUILD_DIR="$2"; shift 2 ;;
    --keep) KEEP=1; shift ;;
    *) REPORT_ARGS+=("$1"); shift ;;
  esac
done

WORK_DIR="$(mktemp -d /tmp/cidx-self-host.XXXXXX)"
cleanup() {
  if [[ "$KEEP" -eq 0 ]]; then
    rm -rf "$WORK_DIR"
  else
    echo "kept self-host workspace: $WORK_DIR" >&2
  fi
}
trap cleanup EXIT

if [[ -z "$BUILD_DIR" ]]; then
  BUILD_DIR="$WORK_DIR/build"
  cmake -S "$REPO_ROOT" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release >&2
  cmake --build "$BUILD_DIR" -j"$(getconf _NPROCESSORS_ONLN)" --target cidx >&2
fi

CIDX_BIN="$BUILD_DIR/cidx"
if [[ ! -x "$CIDX_BIN" ]]; then
  echo "error: cidx binary not found at $CIDX_BIN" >&2
  exit 1
fi

# INDEXER_CACHE controls where `cidx` reads/writes index.db; pointing it at
# a fresh /tmp directory (rather than the repo root) keeps this run fully
# throwaway and out of the way of any canonical index.db in REPO_ROOT.
INDEXER_CACHE_DIR="$WORK_DIR/cache"
mkdir -p "$INDEXER_CACHE_DIR"
export INDEXER_CACHE="$INDEXER_CACHE_DIR"
INDEX_DB="$INDEXER_CACHE_DIR/index.db"

"$CIDX_BIN" import --db "$BUILD_DIR" --name cpp-indexer-self-host >&2
if [[ ! -f "$INDEX_DB" ]]; then
  echo "error: expected index.db under $INDEXER_CACHE_DIR after import" >&2
  exit 1
fi
"$CIDX_BIN" index >&2
"$CIDX_BIN" resolve >&2

python3 "$REPO_ROOT/scripts/self_host_architecture_report.py" \
  --repo-root "$REPO_ROOT" \
  --index-db "$INDEX_DB" \
  --expected-repo-root "$REPO_ROOT" \
  "${REPORT_ARGS[@]}"
