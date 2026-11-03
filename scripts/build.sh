#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
source scripts/cann_env.sh
prl_load_cann
prl_load_asc_modules
make RANK="${RANK:-128}" NPU_ARCH="${NPU_ARCH:-dav-3510}" "$@"
