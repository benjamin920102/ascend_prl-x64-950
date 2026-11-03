#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
: "${PEARL_REF:?Set PEARL_REF to the Pearl revision compatible with your pool/proof format}"
BLAKE3_REF="${BLAKE3_REF:-df610ddc3b93841ffc59a87e3da659a15910eb46}"
fetch() {
  local url=$1 dest=$2 ref=$3
  if [[ -e "$dest" ]]; then
    echo "Refusing to overwrite $dest; reuse it or move it aside explicitly" >&2
    return 1
  fi
  git init -q "$dest"
  git -C "$dest" remote add origin "$url"
  git -C "$dest" fetch --depth 1 origin "$ref"
  git -C "$dest" checkout --detach FETCH_HEAD
}
mkdir -p vendor
fetch https://github.com/pearl-research-labs/pearl.git vendor/pearl "$PEARL_REF"
fetch https://github.com/BLAKE3-team/BLAKE3.git vendor/BLAKE3 "$BLAKE3_REF"
{
  printf 'pearl '; git -C vendor/pearl rev-parse HEAD
  printf 'BLAKE3 '; git -C vendor/BLAKE3 rev-parse HEAD
} > vendor/DEPENDENCY_REVISIONS.txt
