#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
[[ $(uname -m) == x86_64 ]] || { echo 'This comparison requires x86_64' >&2; exit 1; }
blk="${BLK:-$PWD/vendor/BLAKE3/c}"
[[ -f "$blk/blake3.h" ]] || { echo 'Run scripts/fetch_deps.sh or set BLK' >&2; exit 1; }
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
common=(-std=gnu11 -O3 -fopenmp -DBLAKE3_USE_TBB -DBLAKE3_USE_NEON=0 -I"$blk" tests/cpu_port.c src/blake3_join.c "$blk/blake3.c" "$blk/blake3_dispatch.c" "$blk/blake3_portable.c" -lpthread)
"${CC:-gcc}" "${common[@]}" -DBLAKE3_NO_SSE2 -DBLAKE3_NO_SSE41 -DBLAKE3_NO_AVX2 -DBLAKE3_NO_AVX512 -o "$tmp/portable"
"${CC:-gcc}" "${common[@]}" "$blk/blake3_sse2_x86-64_unix.S" "$blk/blake3_sse41_x86-64_unix.S" "$blk/blake3_avx2_x86-64_unix.S" "$blk/blake3_avx512_x86-64_unix.S" -o "$tmp/simd"
"$tmp/portable" > "$tmp/portable.txt"
"$tmp/simd" > "$tmp/simd.txt"
cmp "$tmp/portable.txt" "$tmp/simd.txt"
echo 'PASS: BLAKE3 known answer, threaded/serial equality, packed layout, and portable/x64 SIMD equality (certificate v2/v3).'
