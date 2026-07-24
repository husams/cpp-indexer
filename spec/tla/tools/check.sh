#!/usr/bin/env bash

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MODEL_DIR="${TLA_MODEL_DIR:-$ROOT/models}"
MODULE_DIR="${TLA_MODULE_DIR:-$ROOT/modules}"
PROTECTED_DIR="${TLA_PROTECTED_DIR:-$ROOT/protected}"
MODEL_SOURCE_DIR="${TLA_MODEL_SOURCE_DIR:-$ROOT/models}"
TOOLS_VERSION="1.8.0"
TOOLS_URL="https://github.com/tlaplus/tlaplus/releases/download/v${TOOLS_VERSION}/tla2tools.jar"
TOOLS_SHA256="cc4803dce2a8ffaf0f5920a9dc39df4b5ee34ab4cb53fb58ac557277a7e516b3"

die() {
  echo "TLA_TOOLCHAIN_STATUS=FAIL reason=$1" >&2
  exit 10
}

checksum() {
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$1" | awk '{print $1}'
  elif command -v shasum >/dev/null 2>&1; then
    shasum -a 256 "$1" | awk '{print $1}'
  else
    die "no SHA-256 utility (need sha256sum or shasum)"
  fi
}

JAVA_BIN="${JAVA_BIN:-}"
if [[ -z "$JAVA_BIN" && -n "${JAVA_HOME:-}" ]]; then
  JAVA_BIN="$JAVA_HOME/bin/java"
fi
JAVA_BIN="${JAVA_BIN:-java}"
command -v "$JAVA_BIN" >/dev/null 2>&1 || die "java-17-not-found"

JAVA_VERSION_OUTPUT="$("$JAVA_BIN" -version 2>&1 || true)"
JAVA_MAJOR="$(printf '%s\n' "$JAVA_VERSION_OUTPUT" | sed -n 's/.*version "\([0-9][0-9]*\).*/\1/p' | head -n 1)"
[[ "$JAVA_MAJOR" == "17" ]] || die "java-major-version-${JAVA_MAJOR:-unknown}-expected-17"

JAR="${TLA_TOOLS_JAR:-${TMPDIR:-/tmp}/tla2tools-${TOOLS_VERSION}.jar}"
if [[ ! -f "$JAR" ]]; then
  command -v curl >/dev/null 2>&1 || die "curl-not-found-and-tool-jar-is-missing"
  curl --fail --location --silent --show-error "$TOOLS_URL" --output "$JAR" \
    || die "download-failed"
fi
[[ "$(checksum "$JAR")" == "$TOOLS_SHA256" ]] || die "tla2tools-sha256-mismatch"

WORK="$(mktemp -d "${TMPDIR:-/tmp}/cidx-tla.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT

# TLC resolves EXTENDS modules beside the root module.  Flatten only into a
# disposable directory; no generated or protected repository file is changed.
cp "$MODULE_DIR"/*.tla "$WORK"/
cp "$PROTECTED_DIR"/*.tla "$WORK"/
cp "$MODEL_SOURCE_DIR"/*.tla "$WORK"/

required_invariants() {
  case "$1" in
    CidxRepositorySmoke)
      printf '%s\n' \
        TypeInvariant \
        RepositoryStructureInvariant \
        SharedStatusInvariant \
        ProtectedInvariant
      ;;
    CidxResultSmoke)
      printf '%s\n' \
        SharedResultTypeInvariant \
        SharedStatusInvariant \
        TrustedOutcomeInvariant
      ;;
    CidxWorkspaceLifecycleSmoke)
      printf '%s\n' \
        LifecycleTypeInvariant \
        WorkspaceContainmentInvariant \
        ScopedSymbolIdentityInvariant \
        ConfigurationApplicabilityInvariant \
        NoPartialPublicationInvariant \
        GenerationPublicationInvariant \
        FailedGenerationInvariant \
        InvalidationInvariant \
        ReadHonestyInvariant \
        ProtectedInvariant
      ;;
    *)
      echo "TLA_CONFIG_STATUS=FAIL reason=unknown-model-$1" >&2
      exit 25
      ;;
  esac
}

required_properties() {
  case "$1" in
    CidxRepositorySmoke|CidxResultSmoke)
      printf '%s\n' Fairness
      ;;
    CidxWorkspaceLifecycleSmoke)
      printf '%s\n' RebuildEventuallySettles
      ;;
    *)
      echo "TLA_CONFIG_STATUS=FAIL reason=unknown-model-$1" >&2
      exit 25
      ;;
  esac
}

run_model() {
  local model="$1"
  local spec="$WORK/${model}.tla"
  local cfg="$MODEL_DIR/${model}.cfg"
  local required_file="$WORK/${model}.required-invariants"
  local actual_file="$WORK/${model}.actual-invariants"
  local required_properties_file="$WORK/${model}.required-properties"
  local actual_properties_file="$WORK/${model}.actual-properties"
  local syntax_log="$WORK/${model}.syntax.log"
  local tlc_log="$WORK/${model}.tlc.log"

  required_invariants "$model" | LC_ALL=C sort >"$required_file"
  awk '$1 == "INVARIANT" { print $2 }' "$cfg" | LC_ALL=C sort >"$actual_file"
  if ! diff -u "$required_file" "$actual_file" >"$WORK/${model}.invariants.diff"; then
    echo "TLA_CONFIG_STATUS=FAIL model=$model reason=invariant-mismatch" >&2
    cat "$WORK/${model}.invariants.diff" >&2
    exit 25
  fi

  required_properties "$model" | LC_ALL=C sort >"$required_properties_file"
  awk '$1 == "PROPERTY" { print $2 }' "$cfg" | LC_ALL=C sort >"$actual_properties_file"
  if ! diff -u "$required_properties_file" "$actual_properties_file" \
      >"$WORK/${model}.properties.diff"; then
    echo "TLA_CONFIG_STATUS=FAIL model=$model reason=property-mismatch" >&2
    cat "$WORK/${model}.properties.diff" >&2
    exit 25
  fi

  if ! (cd "$WORK" && "$JAVA_BIN" -cp "$JAR" tla2sany.SANY "$spec") >"$syntax_log" 2>&1; then
    echo "TLA_SYNTAX_STATUS=FAIL model=$model" >&2
    cat "$syntax_log" >&2
    exit 20
  fi
  echo "TLA_SYNTAX_STATUS=PASS model=$model"

  if ! (cd "$WORK" && "$JAVA_BIN" -jar "$JAR" \
      -workers 1 \
      -fp 0 \
      -nowarning \
      -metadir "$WORK/meta-${model}" \
      -config "$cfg" \
      "$spec") >"$tlc_log" 2>&1; then
    echo "TLA_MODEL_STATUS=FAIL model=$model" >&2
    cat "$tlc_log" >&2
    exit 30
  fi
  if ! grep -q "Model checking completed. No error has been found." "$tlc_log"; then
    echo "TLA_MODEL_STATUS=FAIL model=$model reason=completion-marker-missing" >&2
    cat "$tlc_log" >&2
    exit 30
  fi
  local invariants
  invariants="$(paste -sd, "$actual_file")"
  echo "TLA_MODEL_STATUS=PASS model=$model invariants=$invariants"
}

for model in ${TLA_MODELS:-CidxRepositorySmoke CidxResultSmoke CidxWorkspaceLifecycleSmoke}; do
  run_model "$model"
done

echo "TLA_TOOLCHAIN_STATUS=PASS version=$TOOLS_VERSION java=17"
echo "TLA_CHECK_STATUS=PASS models=${TLA_MODELS:-CidxRepositorySmoke,CidxResultSmoke,CidxWorkspaceLifecycleSmoke} workers=1 fingerprint=0"
