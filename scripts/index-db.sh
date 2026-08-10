#!/usr/bin/env bash
# Push/pull the checked-out semantic index (index.db) to private MinIO.
#
#   scripts/index-db.sh push [file]   upload index.db (also stamps a dated copy)
#   scripts/index-db.sh pull [file]   download the latest index.db
#   scripts/index-db.sh list          list stored versions
#
# The index is too large for GitHub, so it lives in the private `cidx-index`
# bucket on minio-api.senussi.me (Tailscale-only). Credentials are read from the
# `minio` secret in the `infrastructure` namespace unless MINIO_ACCESS_KEY and
# MINIO_SECRET_KEY are set in the environment.
set -euo pipefail

MINIO_URL=${MINIO_URL:-https://minio-api.senussi.me}
MINIO_ALIAS=${MINIO_ALIAS:-cidx-minio}
BUCKET=${CIDX_INDEX_BUCKET:-cidx-index}
REPO_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
DEFAULT_DB="$REPO_ROOT/index.db"
LATEST="$MINIO_ALIAS/$BUCKET/index.db"

die() { printf 'error: %s\n' "$*" >&2; exit 1; }

command -v mc >/dev/null || die "mc (MinIO client) not found; brew install minio/stable/mc"

configure_alias() {
  if [[ -z ${MINIO_ACCESS_KEY:-} || -z ${MINIO_SECRET_KEY:-} ]]; then
    command -v kubectl >/dev/null || die "set MINIO_ACCESS_KEY/MINIO_SECRET_KEY or install kubectl"
    MINIO_ACCESS_KEY=$(kubectl get secret minio -n infrastructure -o jsonpath='{.data.rootUser}' | base64 -d)
    MINIO_SECRET_KEY=$(kubectl get secret minio -n infrastructure -o jsonpath='{.data.rootPassword}' | base64 -d)
  fi
  mc alias set "$MINIO_ALIAS" "$MINIO_URL" "$MINIO_ACCESS_KEY" "$MINIO_SECRET_KEY" >/dev/null \
    || die "could not reach $MINIO_URL (Tailscale up?)"
  mc mb --ignore-existing "$MINIO_ALIAS/$BUCKET" >/dev/null
}

cmd=${1:-}
db=${2:-$DEFAULT_DB}

case "$cmd" in
  push)
    [[ -f $db ]] || die "no such file: $db"
    configure_alias
    stamp=$(date -u +%Y%m%dT%H%M%SZ)
    mc cp "$db" "$LATEST"
    mc cp "$LATEST" "$MINIO_ALIAS/$BUCKET/history/index-$stamp.db"
    printf 'uploaded %s -> s3://%s/index.db (history/index-%s.db)\n' "$db" "$BUCKET" "$stamp"
    ;;
  pull)
    configure_alias
    mc cp "$LATEST" "$db"
    printf 'downloaded s3://%s/index.db -> %s\n' "$BUCKET" "$db"
    ;;
  list)
    configure_alias
    mc ls --recursive "$MINIO_ALIAS/$BUCKET"
    ;;
  *)
    sed -n '2,9p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
    exit 1
    ;;
esac
