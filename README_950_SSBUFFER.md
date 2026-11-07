# Ascend PRL 950 — MIX + SSBuffer source revision

> **Status: source implementation + offline static checks; NOT CANN-compiled or 950 device-validated.**
> Do not deploy for actual mining before comparing transcripts and hit results against the original/reference. No Ascend toolkit or 950 hardware is present in the build environment used to prepare this ZIP.

## Scope of changes

- Added `kernel/pearl_hlc2_mix.asc` and made this the **default** target (`PEARL_950_MIX=ON`) for `dav-3510`.
- One `__global__ __mix__(1, 2)` launch per strip replaces independent `__cube__` and `__vector__` kernels and cross-stream GM-flag polling.
- The AIC writes **32-byte** SSBuffer descriptors containing `(sequence, ring slot, band, NFOLD)` for each band, occupying `BUFS * 32B`, starting at SSBuffer offset **2048B**. This is a user-defined layout; CANN does **not** guarantee the rest of SSBuffer is free. Audit Matmul internals for each CANN build.
- `CrossCoreSetFlag<4, PIPE_S>`/`CrossCoreWaitFlag<4, PIPE_S>` perform AIC→AIV READY and AIV→AIC ACK. For AIV0, AIC uses IDs 0 (READY) and 1 (ACK). For AIV1, AIC uses 16 (READY) and 17 (ACK). Each AIV uses 0 for READY and 1 for ACK.
- `IterateAll<true>` completes all 32 GM matrix partials before AIC publishes READY. AIV acknowledges its slot after the last GM→UB copy is synchronized via MTE2_S; hashing proceeds independently.
- The GM partial ring, exact Folding/XOR/BLAKE3 helper implementations, transcript layout, result ABI and B double buffering are retained. Removed **only** the new default path's GM producer/consumer flag allocations, flag resets, and `dcci`-based hot polling. Result writeback still calls `dcci`.
- Removed speculative next-band prefetch: a point-to-point hardware READY cannot be safely peeked without consuming it.
- All strip launches now run on **one device stream**. This avoids cross-stream reorder hazards but also removes old cross-strip Cube(i+1)/Vector(i) overlap; benchmark both.
- Unmodified reference files `pearl_hlc2_cube.asc` and `pearl_hlc2_vec.asc` remain in the ZIP; compile with `MIX_950=OFF` to get the original implementation.

## Build

Load the actual CANN 9.1+ toolkit for 950PR/950DT (`dav-3510`), then:

```bash
source /usr/local/Ascend/cann/set_env.sh  # adjust to your installation
make kernel MIX_950=ON NPU_ARCH=dav-3510 GRID=20 VEC2X=2
make miner
```

The CMake target sets `-DENABLE_CV_COMM_VIA_SSBUF=true` only for the MIX variant. `VEC2X=2` is mandatory. `BUFS` must be even, with its descriptor ring fitting in the final 1KB of SSBuffer. The Makefile retains `BUFS=4`.

For a comparison run with the original two-kernel version:

```bash
make kernel MIX_950=OFF NPU_ARCH=dav-3510 GRID=20 VEC2X=2
make miner
```

## Offline checks performed

```bash
python3 scripts/check_950_mix_static.py
```

The static checker verifies required source/compile options, bit-for-bit equality of the original SIMD/BLAKE3 helper bodies (with comments/whitespace ignored), and symbolic AIC/AIV ring slot/credit pairing for 5 grid sizes × 259 band counts. **This does not compile Ascend C, measure TH/s, or prove NPU memory ordering.**

## Required NPU verification before use

1. **Compile:** confirm CANN 9.1 toolchain accepts `__mix__`, `__kfc_workspace__`, `ENABLE_CV_COMM_VIA_SSBUF`, the Matmul high-level API with `ASCENDC_CUBE_ONLY`, 950's SIMD compatibility, and `__ssbuf__` record accesses.
2. **SSBuffer layout:** confirm **offset 2048–2175** (BUFS=4) does not overlap implicit library/KFC SSBuffer allocation. This is **not vendor-guaranteed** and may need changing.
3. **Event ordering:** validate GM output visibility after `IterateAll<true>`, SSBuffer descriptor-before-READY ordering, and ACK-after-MTE2 completion; use profiling or instrumented diagnostic builds to prove no stale descriptor reads.
4. **Edge cases:** nbands = 1, 2, 3, 4, 19, 20, 21, 40, 41, 80, 81, 160, 161, 256; n_hi=1,2,64; several consecutive strips and scan resets. The idle AIV must exit without waiting for phantom READYs.
5. **Bit correctness:** compare full transcript bytes and hit index/Boolean against the original 910C/reference on the **same seed/key/target**, including nonhit and forced-hit cases. Check top-level proof verification against CPU.
6. **Performance:** compare old and new sustained TH/s with equal B prep and host CPU affinity; profile AIC cycles, AIV cycles, D2H, and whole-strip wall time. Be prepared for a regression due to reduced cross-strip overlap despite faster per-band signaling.

## Restrictions and caveats

- Ascend 910C has no SSBuffer: this MIX target is 950-only. Building the old fallback on `dav-3510` is **not** a correctness proof for the new target.
- This is genuine explicit SSBuffer use **for 32-byte control messages**, **not** L0C→UB direct matrix data transfer. GM partials are kept unchanged.
- Mode 4 flag namespace and SSBuffer placement must be checked against the exact Matmul implementation included in your CANN version. Matmul may reserve synchronization resources internally. This version intentionally uses Mode 4 rather than Matmul's Mode 2 counters, but collision-free operation is not guaranteed without compile/runtime review.
- On 950, 32-byte alignment/access requirements apply to SSBuffer. The source uses 32B `alignas(32)` message records; whether compiler-generated accesses meet the strict instruction requirements must be verified in generated device code.
- CANN 9.1 device and lib version must match; an incompatible driver/toolkit or a 910C node will not execute this kernel.

## Official documentation

- [Ascend 950 SSBuffer memory and inter-core communication](https://asc.gitcode.com/guide/programming_guide/advanced_programming/inter_core_communication/ssbuffer_inter_core_memory_feature.html)
- [3510 new features and SSBuffer details](https://asc.gitcode.com/guide/cross_gen_migration_guide/instructions_for_new_features/3510_new_features.html)
- [CrossCoreSetFlag / WaitFlag](https://www.hiascend.com/document/detail/zh/CANNCommunityEdition/910/API/ascendcopapi/docs/api/SIMD-API/%E5%9F%BA%E7%A1%80API/%E7%BC%93%E5%AD%98%E6%8E%A7%E5%88%B6/DataCacheCleanAndInvalid.md)
- [GetSsbufBaseAddr](https://www.hiascend.com/document/detail/en/CANNCommunityEdition/910/API/ascendcopapi/docs/en/api/SIMD-API/basic_api/tool_interface/system_resources_and_variables/GetSsbufBaseAddr.md)
- [MIX kernel keyword and `__kfc_workspace__`](https://asc.gitcode.com/guide/programming_guide/language_extension/simd_builtin_keywords.html)
