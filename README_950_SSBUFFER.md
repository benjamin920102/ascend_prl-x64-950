# Ascend 950 — Cube→Vector 片上直連 (L0C→UB)

> **狀態：原始碼實作，未經 CANN 編譯、未經 Ascend 950 實機驗證，也沒有執行測試。**
> 使用前必須自行確認結果正確性與效能；此處不聲稱已能上線挖礦或保證提速。

## 真正改動的資料路徑

舊 `pearl_hlc2_mix.asc`：

```text
A/B GM → L1 → L0A/B → MMAD → L0C → GM partial ring → AIV UB → XOR / BLAKE3
```

此版預設 MIX：

```text
A/B GM → L1 → L0A/B → asc_mmad → L0C
                                    └→ asc_copy_l0c2ub (PIPE_FIX, Nz2ND, int32)
                                       → AIV0 / AIV1 私有 UB ping-pong
                                       → Accumulate / Native BitwiseXor / BLAKE3
                                       → 命中資訊 GM（僅最終結果）
```

用 Ascend 950 官方 C API `asc_copy_l0c2ub` 在同一個 `__mix__(1,2)` kernel
把每份 **64×128 INT32 = 32 KiB** partial 直接寫到對應 AIV 的 UB，
避免先把每個 band 的 32 份 partial 寫進 GM 再讀回。

每個 band：32 × 32 KiB = **1 MiB GM 寫入 + 1 MiB GM 讀取**的
中間搬運理論上可省略（不包含必要的輸入 GM 讀取和最終結果寫回）。
舊 `g_partD` 在 GRID=20、BUFS=4、NFOLD=32 時的 **80 MiB GM ring**
不再配置；Matmul 高階 API 的暫存 workspace 也不再配置。

## 同步協定與 UB 資源

- 每個 AIV 對其 32 KiB partial buffer 使用 **ping/pong 兩個槽位**；Cube 在重用同一槽之前須收到 AIV 的 ACK。
- READY：AIC 的 `CrossCoreSetFlag<4, PIPE_FIX>`，保證實際 FIX 寫到 UB 完成才通知 AIV。
- ACK：AIV 的 `CrossCoreSetFlag<4, PIPE_V>`，等 `Add(acc, partial)` 已讀取 partial 後才釋放 UB 槽位。
- AIC 發給 AIV0 的 READY ID=0、AIV1 ID=16；AIV 發回 ACK ID=1（AIC 所見 AIV1 ID=17）。
- AIC 另外加入 MTE1→MTE2、M→MTE1、M→FIX、FIX→M pipeline 相依，避免重用 L1/L0/UB 時發生讀寫競態。
- 每個 AIV 的固定 UB 配置為 148 KiB：2×32 KiB partial、32 KiB accumulator、16 KiB folding、4 KiB transcript、2×16 KiB XOR 暫存。
- 以固定 UB 地址的 `LocalTensor(TPosition, offset, elements)` 對應 C API 的 `__ubuf__` 目的位址；本直連版本**不再使用 TPipe 配置 UB**，避免與固定地址衝突。
- Vector BLAKE3 的 native `AscendC::BitwiseXor` 與原本 scalar keyed BLAKE3 保留。

## ABI 與支援範圍

- 原 host C ABI (`pearl_init`、`pearl_set_b*`、`pearl_use_b`、`pearl_strip_submit*`、`pearl_strip_wait*`、`pearl_get_hit_result` 等) 保留。
- `MIX_950=ON` 預設走新直連；`MIX_950=OFF` 保留獨立 Cube/Vector 的 GM fallback。
- 預設/支援編譯規格：`dav-3510`、`NINNER=1`、`VEC2X=2`、`RANK=128`、`BN=64`、`MM_N=128`；NFOLD 預設 32。
- AT 仍是 **[R][NFOLD × MM_K]** 的自然 ND row-major 格式，GM→L1 copy 的 row-pitch 是 `NFOLD * MM_K` **bytes**，不是 `MM_K`；保留這個 stride 才能與原 Matmul 逐 fold 結果一致。
- `PRL_NOFOLD` 仍只供 Cube 測量；命中資料不具有挖礦效力。

## 建置（未執行）

```bash
source /usr/local/Ascend/cann/set_env.sh
make kernel MIX_950=ON NPU_ARCH=dav-3510 GRID=20 VEC2X=2
make miner
```

**本 ZIP 沒有執行 build / test / benchmark**，因此未確認 CANN 9.1
對 C API `asc_init` 與 AscendC `LocalTensor/BitwiseXor/CrossCoreSetFlag` 混用的
編譯相容性、mode 4 中斷的完整有序性、正確 transcript，或實際 hash rate。

**注意**：L0C→UB 路徑省去 GM partial traffic 但每份 partial 增加細粒度
READY/ACK；新路徑不保證比原高階 Matmul 實作快。需要實機 profiling 才能衡量。

## Ascend 官方 API 文件

- [asc_copy_l0c2ub（950 L0C→UB）](https://asc.gitcode.com/api/SIMD-API/c_api/cube_datamove/cube_compute_store/asc_copy_l0c2ub.html)
- [asc_mmad（INT8×INT8→INT32）](https://asc.gitcode.com/api/SIMD-API/c_api/cube_compute/mmad_compute/asc_mmad.html)
- [跨核 SSBuffer / L0C→UB 通道](https://asc.gitcode.com/guide/programming_guide/advanced_programming/inter_core_communication/ssbuffer_inter_core_memory_feature.html)
- [LocalTensor 固定位址配置](https://asc.gitcode.com/guide/programming_guide/programming_model/ai_core_simd_programming/cpp_tensor_programming/cpp_tensor_programming_overview.html)
- [CrossCoreSetFlag (mode 4)](https://asc.gitcode.com/api/SIMD-API/basic_api/sync_control/inter_core_sync/CrossCoreSetFlag_ISASI.html)
