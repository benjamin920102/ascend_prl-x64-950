#!/usr/bin/env bash
set -euo pipefail
REPO="${REPO:-arabel1a/ascend_prl}"
ARCH="${ARCH:-$(uname -m)}"
NPU="${NPU:-ascend950}"
ASSET="${ASSET:-build-$ARCH-$NPU.tar.gz}"
cd "$(dirname "$0")/.."
# latest excludes prereleases. Set RELEASE_TAG for CI prerelease downloads.
if [[ -n "${RELEASE_TAG:-}" ]]; then
  api="https://api.github.com/repos/$REPO/releases/tags/$RELEASE_TAG"
else
  api="https://api.github.com/repos/$REPO/releases/latest"
fi
metadata=$(mktemp)
archive=$(mktemp)
trap 'rm -f "$metadata" "$archive"' EXIT
curl -fsSL "$api" -o "$metadata"
url=$(python3 - "$metadata" "$ASSET" <<'PYURL'
import json,sys
assets=json.load(open(sys.argv[1])).get('assets',[])
matches=[a['browser_download_url'] for a in assets if a['name']==sys.argv[2]]
if len(matches)!=1: sys.exit('Expected exactly one release asset: '+sys.argv[2])
print(matches[0])
PYURL
)
curl -fL "$url" -o "$archive"
mkdir -p build
# CI archives entries directly under ./; do not strip their first component.
tar xzf "$archive" -C build
echo "done -> ./build/ ; run with scripts/launch.sh"
