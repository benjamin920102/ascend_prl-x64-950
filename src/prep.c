#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <omp.h>
#include "blake3_impl.h"
#include "prep.h"

#if defined(__x86_64__)
#include <immintrin.h>
#endif

#if defined(__ARM_NEON) || defined(__aarch64__)
#include <arm_neon.h>
#define HAVE_NEON 1
#endif

#define DIGEST 32
#define ZERO_POINT 32
#define RANGE_MASK 63

/* Certificate v3 binds each Merkle root to its matrix dimension before deriving
 * noise seeds. These constants must match the A/B domain salts in zk_pow.
 */
static const uint8_t SEED_SALT_A[32] = {
    0x82, 0x49, 0x40, 0x6c, 0xa0, 0xed, 0x15, 0x16, 0x96, 0x16, 0xf6, 0x92, 0xfc, 0xf0, 0x76, 0xf8,
    0x92, 0xdb, 0xdb, 0x2a, 0x70, 0x23, 0xb8, 0x52, 0xf0, 0xd4, 0x77, 0x19, 0xc3, 0x90, 0x01, 0x7b,
};
static const uint8_t SEED_SALT_B[32] = {
    0x11, 0x30, 0x06, 0x32, 0xec, 0x63, 0x01, 0xca, 0x2b, 0xe2, 0xaf, 0x71, 0x8b, 0x3f, 0x4d, 0x4f,
    0x1a, 0xe9, 0xc6, 0x39, 0x88, 0xe8, 0xcc, 0x04, 0x48, 0x44, 0x30, 0x1d, 0x71, 0xb8, 0x9a, 0xa9,
};

// Hash a fixed 64-byte block: root || dimension (LE u32) || 28 zero bytes.
static void bind_root(const uint8_t root[32], uint32_t dim, const uint8_t salt[32], uint8_t out[32]) {
    uint8_t msg[64] = {0};
    memcpy(msg, root, 32);
    memcpy(msg + 32, &dim, 4);
    blake3_hasher h;
    blake3_hasher_init_keyed(&h, salt);
    blake3_hasher_update(&h, msg, 64);
    blake3_hasher_finalize(&h, out, DIGEST);
}

#define CERT_LEGACY 0
#define CERT_SALTED 1

static void salt_roots(int cert_mode, const uint8_t rootA[32], const uint8_t rootB[32],
                        uint32_t m, uint32_t n, uint8_t outA[32], uint8_t outB[32]) {
    if (cert_mode == CERT_SALTED) {
        bind_root(rootA, m, SEED_SALT_A, outA);
        bind_root(rootB, n, SEED_SALT_B, outB);
    } else {
        memcpy(outA, rootA, 32);
        memcpy(outB, rootB, 32);
    }
}

typedef struct { uint64_t s[4]; } rng_t;

static uint64_t splitmix(uint64_t *x) {
    uint64_t z = (*x += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

static void rng_seed(rng_t *r, uint64_t seed) {
    for (int i = 0; i < 4; i++) r->s[i] = splitmix(&seed);
}

static inline uint64_t rotl64(uint64_t x, int k) {
    return (x << k) | (x >> (64 - k));
}

static inline uint64_t rng_next(rng_t *r) {
    uint64_t *s = r->s, res = rotl64(s[1] * 5, 7) * 9, t = s[1] << 17;
    s[2] ^= s[0]; s[3] ^= s[1]; s[1] ^= s[2]; s[0] ^= s[3]; s[2] ^= t; s[3] = rotl64(s[3], 45);
    return res;
}

static void draw_hash(uint32_t idx, const uint8_t *seed, const uint8_t *key,
                      int slot, uint8_t out[DIGEST]) {
    uint8_t msg[64] = {0};
    uint32_t v = idx + 1;
    memcpy(msg + slot * 4, &v, 4);
    memcpy(msg + 32, seed, 32);
    blake3_hasher h;
    blake3_hasher_init_keyed(&h, key);
    blake3_hasher_update(&h, msg, 64);
    blake3_hasher_finalize(&h, out, DIGEST);
}

/*
 * Default: zero-fill + 8-byte seed stamp (gzip-friendly; ~10-100x compression on proofs).
 * PRL_ZERO_SEED=0: legacy uniform random fill (incompressible).
 */
static void fill_buf(prep_input_t *input, int64_t total, uint64_t seed, int nt) {
    int8_t *buf = input->data;
    const char *zs = getenv("PRL_ZERO_SEED");
    if (zs && zs[0] == '0' && zs[1] == '\0') {
        input->zero_tail_bytes = 0;
#pragma omp parallel num_threads(nt)
        {
            int t = omp_get_thread_num();
            int num_t = omp_get_num_threads();
            int64_t lo = total * t / num_t, hi = total * (t + 1) / num_t;
            rng_t r;
            rng_seed(&r, seed + 0x9E37 * (uint64_t)(t + 1));
            int64_t i = lo;
            while (i < hi) {
                uint64_t v = rng_next(&r);
                for (int b = 0; b < 8 && i < hi; b++, v >>= 8)
                    buf[i++] = (int8_t)((uint8_t)v % 127) - 63;
            }
        }
        return;
    }
    if (input->zero_tail_bytes != (size_t)total) {
        memset(buf, 0, (size_t)total);
        input->zero_tail_bytes = (size_t)total;
    }
    if (total >= 8) {
        uint8_t *stamp = (uint8_t *)buf;
        store32(stamp, (uint32_t)seed);
        store32(stamp + 4, (uint32_t)(seed >> 32));
    }
}

static void calc_mk(const int8_t *buf, size_t len, const uint8_t *key, uint8_t *root) {
    blake3_hasher h;
    blake3_hasher_init_keyed(&h, key);
#if defined(BLAKE3_USE_TBB)
    blake3_hasher_update_tbb(&h, buf, (size_t)len);
#else
    blake3_hasher_update(&h, buf, (size_t)len);
#endif
    blake3_hasher_finalize(&h, root, DIGEST);
}

static void parent_cv(const uint32_t key[8], const uint8_t left[DIGEST],
                      const uint8_t right[DIGEST], uint8_t flags, uint8_t out[DIGEST]) {
    uint8_t block[BLAKE3_BLOCK_LEN];
    uint32_t cv[8];
    memcpy(block, left, DIGEST);
    memcpy(block + DIGEST, right, DIGEST);
    memcpy(cv, key, sizeof cv);
    blake3_compress_in_place(cv, block, sizeof block, 0, KEYED_HASH | PARENT | flags);
    store_cv_words(out, cv);
}

static void zero_subtree_cv(const uint8_t *buf, size_t chunks, size_t start,
                            const uint32_t key[8], uint8_t out[DIGEST]) {
    uint8_t cvs[MAX_SIMD_DEGREE_OR_2 * DIGEST];
    size_t count = blake3_compress_subtree_wide(buf, chunks * BLAKE3_CHUNK_LEN,
                                               key, start, KEYED_HASH, cvs, false);
    while (count > 1) {
        size_t parents = count / 2;
        for (size_t i = 0; i < parents; ++i)
            parent_cv(key, cvs + i * 2 * DIGEST, cvs + (i * 2 + 1) * DIGEST, 0, cvs + i * DIGEST);
        if (count % 2)
            memcpy(cvs + parents * DIGEST, cvs + (count - 1) * DIGEST, DIGEST);
        count = parents + count % 2;
    }
    memcpy(out, cvs, DIGEST);
}

static void calc_input_root(const int8_t *buf, size_t bytes, const uint8_t key[DIGEST],
                            const prep_input_t *input, uint8_t root[DIGEST]) {
    prep_root_cache_t *cache = input ? input->root_cache : NULL;
    if (!cache || input->zero_tail_bytes != bytes || bytes < 2 * BLAKE3_CHUNK_LEN ||
        bytes % BLAKE3_CHUNK_LEN) {
        calc_mk(buf, bytes, key, root);
        return;
    }

    uint32_t words[8];
    load_key_words(key, words);
    if (cache->bytes != bytes || memcmp(cache->key, key, DIGEST)) {
        const uint8_t *data = (const uint8_t *)buf;
        size_t chunks = bytes / BLAKE3_CHUNK_LEN;
        cache->depth = 0;
        // Walk from the root to chunk 0 and save the right subtree at each split.
        while (chunks > 1) {
            size_t left = round_down_to_power_of_2(chunks - 1);
            zero_subtree_cv(data + left * BLAKE3_CHUNK_LEN, chunks - left, left,
                            words, cache->siblings[cache->depth++]);
            chunks = left;
        }
        memcpy(cache->key, key, DIGEST);
        cache->bytes = bytes;
    }

    const uint8_t *first = (const uint8_t *)buf;
    blake3_hash_many(&first, 1, BLAKE3_CHUNK_LEN / BLAKE3_BLOCK_LEN, words, 0, false,
                     KEYED_HASH, CHUNK_START, CHUNK_END, root);
    for (size_t depth = cache->depth; depth > 0; --depth)
        parent_cv(words, root, cache->siblings[depth - 1], depth == 1 ? ROOT : 0, root);
}

static void unif_int8(const uint8_t *seed, const uint8_t *key, int64_t nbytes, int8_t *out, int nt) {
    const int64_t draws = (nbytes + DIGEST - 1) / DIGEST;
    const int64_t width = (int64_t)blake3_simd_degree();
    uint32_t words[8];
    load_key_words(key, words);
#pragma omp parallel num_threads(nt)
    {
        uint8_t blocks[MAX_SIMD_DEGREE][BLAKE3_BLOCK_LEN] = {{0}};
        uint8_t digests[MAX_SIMD_DEGREE * DIGEST];
        const uint8_t *inputs[MAX_SIMD_DEGREE];
        for (int64_t i = 0; i < width; ++i) {
            memcpy(blocks[i] + 32, seed, 32);
            inputs[i] = blocks[i];
        }

#pragma omp for schedule(static)
        for (int64_t i = 0; i < draws; i += width) {
            int64_t count = draws - i < width ? draws - i : width;
            for (int64_t j = 0; j < count; ++j)
                store32(blocks[j], (uint32_t)(i + j) + 1u);
            // Independent keyed, single-block root hashes. The draw index is
            // in the message, so the BLAKE3 compression counter is always zero.
            blake3_hash_many(inputs, (size_t)count, 1, words, 0, false,
                             KEYED_HASH | ROOT, CHUNK_START, CHUNK_END, digests);
            int64_t offset = i * DIGEST;
            int64_t bytes = nbytes - offset < count * DIGEST ? nbytes - offset : count * DIGEST;
            for (int64_t j = 0; j < bytes; ++j)
                out[offset + j] = (int8_t)((digests[j] & RANGE_MASK) - ZERO_POINT);
        }
    }
}

// Each index pair is deterministically derived from the commitment; XOR ensures
// the second rank index differs from the first without a rejection loop.
static void perm_pairs(const uint8_t *seed, const uint8_t *key, int64_t lines, int rank,
                       uint16_t *first, uint16_t *second) {
    int64_t draws = (lines * 4 + DIGEST - 1) / DIGEST;
    for (int64_t i = 0; i < draws; i++) {
        uint8_t d[DIGEST];
        draw_hash((uint32_t)i, seed, key, 1, d);
        for (int j = 0; j < 8; j++) {
            int64_t line = i * 8 + j;
            if (line >= lines) break;
            uint32_t u;
            memcpy(&u, d + j * 4, 4);
            uint32_t f = u & (uint32_t)(rank - 1);
            uint32_t s = f ^ (1u + (uint32_t)(((uint64_t)(rank - 1) * u) >> 32));
            first[line] = (uint16_t)f;
            second[line] = (uint16_t)s;
        }
    }
}

/* AArch64 TBL gathers 16 noise values directly from register tables.
 * Keep the uint16_t permutation representation for ranks > 256 and use the
 * low byte only in this rank-128 specialization (all indices are < 128).
 * Two disjoint 64-byte lookups return zero outside their respective ranges.
 */
#if defined(__aarch64__)
typedef struct { int8x16x4_t lo, hi; } noise128_t;

static inline noise128_t noise128_load(const int8_t *e) {
    noise128_t t;
    for (int i = 0; i < 4; ++i) {
        t.lo.val[i] = vld1q_s8(e + i * 16);
        t.hi.val[i] = vld1q_s8(e + 64 + i * 16);
    }
    return t;
}

static inline uint8x16_t indices16(const uint16_t *p) {
    return vcombine_u8(vmovn_u16(vld1q_u16(p)),
                       vmovn_u16(vld1q_u16(p + 8)));
}

static inline int8x16_t noise128_gather(noise128_t t, uint8x16_t idx) {
    return vorrq_s8(vqtbl4q_s8(t.lo, idx),
                    vqtbl4q_s8(t.hi, vsubq_u8(idx, vdupq_n_u8(64))));
}

static inline void noise128_apply(noise128_t t, const int8_t *src, int8_t *dst,
                                  const uint16_t *f, const uint16_t *s,
                                  int64_t count) {
    int64_t j = 0;
    for (; j + 16 <= count; j += 16) {
        int8x16_t vf = noise128_gather(t, indices16(f + j));
        int8x16_t vs = noise128_gather(t, indices16(s + j));
        /* Non-saturating arithmetic preserves the original int8_t wrap. */
        vst1q_s8(dst + j, vsubq_s8(vaddq_s8(vld1q_s8(src + j), vf), vs));
    }
    /* Callers use multiples of 16; retain a correct tail for other shapes. */
    for (; j < count; ++j) {
        uint8x16_t fi = vdupq_n_u8((uint8_t)f[j]);
        uint8x16_t si = vdupq_n_u8((uint8_t)s[j]);
        dst[j] = (int8_t)(src[j] + vgetq_lane_s8(noise128_gather(t, fi), 0)
                                 - vgetq_lane_s8(noise128_gather(t, si), 0));
    }
}
#endif

#if defined(__x86_64__)
static int use_vbmi_noise(int rank, int64_t count) {
    return rank == 128 && count >= 64 && __builtin_cpu_supports("avx512vbmi")
           && __builtin_cpu_supports("avx512bw");
}

static void narrow_indices(const uint16_t *src, uint8_t *out, int64_t count) {
    for (int64_t i = 0; i < count; ++i)
        out[i] = (uint8_t)src[i];
}

__attribute__((target("avx512vbmi,avx512bw")))
static void noise128_vbmi(const int8_t *noise, const int8_t *src, int8_t *dst,
                          const uint8_t *first, const uint8_t *second, int64_t count) {
    __m512i lo = _mm512_loadu_si512(noise);
    __m512i hi = _mm512_loadu_si512(noise + 64);
    int64_t j = 0;
    for (; j + 64 <= count; j += 64) {
        __m512i f = _mm512_permutex2var_epi8(lo, _mm512_loadu_si512(first + j), hi);
        __m512i s = _mm512_permutex2var_epi8(lo, _mm512_loadu_si512(second + j), hi);
        __m512i v = _mm512_loadu_si512(src + j);
        _mm512_storeu_si512(dst + j, _mm512_sub_epi8(_mm512_add_epi8(v, f), s));
    }
    for (; j < count; ++j)
        dst[j] = (int8_t)(src[j] + noise[first[j]] - noise[second[j]]);
}
#endif

static void compute_an(const int8_t *A, const int8_t *EAL, int8_t *out,
                       const uint16_t *f, const uint16_t *s,
                       int64_t m, int64_t k, int R, int nt) {
#if defined(__x86_64__)
    if (use_vbmi_noise(R, k)) {
        uint8_t first[k], second[k];
        narrow_indices(f, first, k);
        narrow_indices(s, second, k);
#pragma omp parallel for schedule(static) num_threads(nt)
        for (int64_t r = 0; r < m; ++r)
            noise128_vbmi(EAL + r * R, A + r * k, out + r * k, first, second, k);
        return;
    }
#endif
#pragma omp parallel for schedule(static) num_threads(nt)
    for (int64_t r = 0; r < m; r++) {
        const int8_t *ar = A + r * k, *el = EAL + r * R;
        int8_t *o = out + r * k;
#if defined(__aarch64__)
        noise128_apply(noise128_load(el), ar, o, f, s, k);
#else
        int64_t j = 0;
#ifdef HAVE_NEON
        int8_t efb[16], esb[16];
        for (; j + 16 <= k; j += 16) {
            for (int t = 0; t < 16; t++) {
                efb[t] = el[f[j + t]];
                esb[t] = el[s[j + t]];
            }
            int8x16_t va = vld1q_s8(ar + j);
            int8x16_t vf = vld1q_s8(efb);
            int8x16_t vs = vld1q_s8(esb);
            int8x16_t vo = vsubq_s8(vaddq_s8(va, vf), vs);
            vst1q_s8(o + j, vo);
        }
#endif
        for (; j < k; j++)
            o[j] = (int8_t)(ar[j] + el[f[j]] - el[s[j]]);
#endif
    }
}

#define TB 64
static void compute_bn(const int8_t *B, const int8_t *EBR, int8_t *out,
                       const uint16_t *f, const uint16_t *s,
                       int64_t k, int64_t n, int R, int nt) {
    int64_t kb = (k + TB - 1) / TB;
#if defined(__x86_64__)
    int vbmi = use_vbmi_noise(R, TB);
    uint8_t first[vbmi ? k : 1], second[vbmi ? k : 1];
    if (vbmi) {
        narrow_indices(f, first, k);
        narrow_indices(s, second, k);
    }
#endif
#pragma omp parallel for schedule(static) num_threads(nt)
    for (int64_t ib = 0; ib < kb; ib++) {
        int64_t i0 = ib * TB, ni = k - i0 < TB ? k - i0 : TB;
        int8_t tile[TB][TB];
        for (int64_t j0 = 0; j0 < n; j0 += TB) {
            int64_t nj = n - j0 < TB ? n - j0 : TB;
            for (int64_t j = 0; j < nj; ++j) {
                const int8_t *bj = B + (j0 + j) * k + i0;
                const int8_t *ej = EBR + (j0 + j) * R;
#if defined(__aarch64__)
                noise128_apply(noise128_load(ej), bj, tile[j], f + i0, s + i0, ni);
#else
#if defined(__x86_64__)
                if (vbmi) {
                    noise128_vbmi(ej, bj, tile[j], first + i0, second + i0, ni);
                    continue;
                }
#endif
                for (int64_t i = 0; i < ni; ++i)
                    tile[j][i] = (int8_t)(bj[i] + ej[f[i0 + i]] - ej[s[i0 + i]]);
#endif
            }
            /* Transpose in a bounded per-thread tile; GM stores now follow
             * adjacent columns instead of repeatedly writing n bytes apart. */
            for (int64_t i = 0; i < ni; ++i) {
                int8_t *dst = out + (i0 + i) * n + j0;
                for (int64_t j = 0; j < nj; ++j) dst[j] = tile[j][i];
            }
        }
    }
}

#define BN_PACK 64
// Pack the noised B matrix by 64-column band and rank-sized fold, matching the
// NPU cube kernel's [band][fold][column][k] access order.
static void compute_bn_pack(const int8_t *B, const int8_t *EBR, int8_t *bt,
                            const uint16_t *f, const uint16_t *s,
                            int64_t k, int64_t n, int R, int nt) {
    int64_t nbands = n / BN_PACK, NFOLD = k / R;
#if defined(__x86_64__)
    int vbmi = use_vbmi_noise(R, R);
    uint8_t first[vbmi ? k : 1], second[vbmi ? k : 1];
    if (vbmi) {
        narrow_indices(f, first, k);
        narrow_indices(s, second, k);
    }
#endif
#pragma omp parallel for schedule(static) num_threads(nt)
    for (int64_t jp = 0; jp < nbands; jp++) {
        /* One column at a time reuses its noise row across all folds and
         * reads its source B column sequentially. The packed offset is unchanged. */
        for (int64_t col_in_band = 0; col_in_band < BN_PACK; col_in_band++) {
            int64_t col = jp * BN_PACK + col_in_band;
            const int8_t *bcol = B + col * k;
            const int8_t *ecol = EBR + col * R;
#if defined(__aarch64__)
            noise128_t t = noise128_load(ecol);
#endif
            for (int64_t p = 0; p < NFOLD; ++p) {
                int64_t row = p * R;
                int8_t *dst = bt + ((jp * NFOLD + p) * BN_PACK + col_in_band) * R;
#if defined(__aarch64__)
                noise128_apply(t, bcol + row, dst, f + row, s + row, R);
#else
#if defined(__x86_64__)
                if (vbmi) {
                    noise128_vbmi(ecol, bcol + row, dst, first + row, second + row, R);
                    continue;
                }
#endif
                for (int64_t ki = 0; ki < R; ++ki)
                    dst[ki] = (int8_t)(bcol[row + ki] + ecol[f[row + ki]] - ecol[s[row + ki]]);
#endif
            }
        }
    }
}

static int prep_from_ab_impl(const int8_t *A, const int8_t *B, const uint8_t *key,
                 int64_t m, int64_t n, int64_t k, int R,
                 int8_t *A_noised, int8_t *Bt_out,
                 int8_t *EAL, int8_t *EBR,
                 uint8_t *rootA, uint8_t *rootB,
                 uint8_t *commitA, uint8_t *commitB,
                 int nt, int pack, int cert_mode,
                 const prep_input_t *inputA, const prep_input_t *inputB) {
    if ((m * k) % 1024 || (n * k) % 1024) return -1;

#pragma omp parallel sections num_threads(2)
    {
#pragma omp section
        calc_input_root(A, m * k, key, inputA, rootA);
#pragma omp section
        calc_input_root(B, n * k, key, inputB, rootB);
    }

    uint8_t saltedA[32], saltedB[32];
    salt_roots(cert_mode, rootA, rootB, (uint32_t)m, (uint32_t)n, saltedA, saltedB);

    // B commits first; A commits to B, so the two noise seeds are linked.
    blake3_hasher h;
    blake3_hasher_init(&h); blake3_hasher_update(&h, key, 32);
    blake3_hasher_update(&h, saltedB, 32); blake3_hasher_finalize(&h, commitB, 32);
    blake3_hasher_init(&h); blake3_hasher_update(&h, commitB, 32);
    blake3_hasher_update(&h, saltedA, 32); blake3_hasher_finalize(&h, commitA, 32);

    static const uint8_t seedA[32] = "A_tensor", seedB[32] = "B_tensor";
    unif_int8(seedA, commitA, m * R, EAL, nt);
    unif_int8(seedB, commitB, n * R, EBR, nt);

    if (k > (1 << 16)) return -2;
    uint16_t *pairs = malloc((size_t)k * 4 * sizeof(*pairs));
    if (!pairs) return -3;
    uint16_t *fA = pairs, *sA = fA + k, *fB = sA + k, *sB = fB + k;
    perm_pairs(seedA, commitA, k, R, fA, sA);
    perm_pairs(seedB, commitB, k, R, fB, sB);

    compute_an(A, EAL, A_noised, fA, sA, m, k, R, nt);
    if (pack) {
        compute_bn_pack(B, EBR, Bt_out, fB, sB, k, n, R, nt);
    } else {
        compute_bn(B, EBR, Bt_out, fB, sB, k, n, R, nt);
    }
    free(pairs);
    return 0;
}

int prep_from_ab(const int8_t *A, const int8_t *B, const uint8_t *key,
                 int64_t m, int64_t n, int64_t k, int R,
                 int8_t *A_noised, int8_t *Bt_noised, int8_t *EAL, int8_t *EBR,
                 uint8_t *rootA, uint8_t *rootB, uint8_t *commitA, uint8_t *commitB,
                 int nt, int cert_version) {
    int cert_mode = cert_version >= 3 ? CERT_SALTED : CERT_LEGACY;
    return prep_from_ab_impl(A, B, key, m, n, k, R, A_noised, Bt_noised,
                             EAL, EBR, rootA, rootB, commitA, commitB, nt, 0, cert_mode, NULL, NULL);
}

int prep_random_cached(uint64_t seed, prep_input_t *A, prep_input_t *B, const uint8_t *key,
                       int64_t m, int64_t n, int64_t k, int R,
                       int8_t *A_noised, int8_t *Bt_out, int8_t *EAL, int8_t *EBR,
                       uint8_t *rootA, uint8_t *rootB, uint8_t *commitA, uint8_t *commitB,
                       int nt, int cert_version, int packed) {
    fill_buf(A, m * k, seed, nt);
    fill_buf(B, n * k, seed ^ 0xB0B0B0B0ULL, nt);
    int cert_mode = cert_version >= 3 ? CERT_SALTED : CERT_LEGACY;
    return prep_from_ab_impl(A->data, B->data, key, m, n, k, R, A_noised, Bt_out,
                             EAL, EBR, rootA, rootB, commitA, commitB, nt, packed, cert_mode, A, B);
}

int prep_random(uint64_t seed, int8_t *A, int8_t *B, const uint8_t *key,
                int64_t m, int64_t n, int64_t k, int R,
                int8_t *A_noised, int8_t *Bt_noised, int8_t *EAL, int8_t *EBR,
                uint8_t *rootA, uint8_t *rootB, uint8_t *commitA, uint8_t *commitB,
                int nt, int cert_version) {
    prep_input_t inputA = { .data = A }, inputB = { .data = B };
    return prep_random_cached(seed, &inputA, &inputB, key, m, n, k, R, A_noised, Bt_noised,
                              EAL, EBR, rootA, rootB, commitA, commitB, nt, cert_version, 0);
}

int prep_random_bt(uint64_t seed, int8_t *A, int8_t *B, const uint8_t *key,
                int64_t m, int64_t n, int64_t k, int R,
                int8_t *A_noised, int8_t *bt_packed, int8_t *EAL, int8_t *EBR,
                uint8_t *rootA, uint8_t *rootB, uint8_t *commitA, uint8_t *commitB,
                int nt, int cert_version) {
    prep_input_t inputA = { .data = A }, inputB = { .data = B };
    return prep_random_cached(seed, &inputA, &inputB, key, m, n, k, R, A_noised, bt_packed,
                              EAL, EBR, rootA, rootB, commitA, commitB, nt, cert_version, 1);
}

int prep_b_side_cached(uint64_t seed, prep_input_t *B, const uint8_t *key,
                int64_t n, int64_t k, int R,
                int8_t *bt_packed, int8_t *EBR, uint8_t *rootB, uint8_t *commitB,
                int nt, int cert_version) {
    if ((n * k) % 1024) return -1;
    if (k > (1 << 16)) return -2;

    fill_buf(B, n * k, seed, nt);
    calc_input_root(B->data, n * k, key, B, rootB);

    int cert_mode = cert_version >= 3 ? CERT_SALTED : CERT_LEGACY;
    uint8_t saltedB[32];
    if (cert_mode == CERT_SALTED) bind_root(rootB, (uint32_t)n, SEED_SALT_B, saltedB);
    else memcpy(saltedB, rootB, 32);

    blake3_hasher h;
    blake3_hasher_init(&h); blake3_hasher_update(&h, key, 32);
    blake3_hasher_update(&h, saltedB, 32); blake3_hasher_finalize(&h, commitB, 32);

    static const uint8_t seedB[32] = "B_tensor";
    unif_int8(seedB, commitB, n * R, EBR, nt);

    uint16_t *fB = malloc((size_t)k * 2 * sizeof(*fB));
    if (!fB) return -3;
    uint16_t *sB = fB + k;
    perm_pairs(seedB, commitB, k, R, fB, sB);

    compute_bn_pack(B->data, EBR, bt_packed, fB, sB, k, n, R, nt);
    free(fB);
    return 0;
}

int prep_b_side(uint64_t seed, int8_t *B, const uint8_t *key,
                int64_t n, int64_t k, int R, int8_t *bt_packed, int8_t *EBR,
                uint8_t *rootB, uint8_t *commitB, int nt, int cert_version) {
    prep_input_t input = { .data = B };
    return prep_b_side_cached(seed, &input, key, n, k, R, bt_packed, EBR, rootB, commitB, nt, cert_version);
}

int prep_a_side_cached(uint64_t seed, prep_input_t *A, const uint8_t *key, const uint8_t *commitB,
                int64_t m, int64_t k, int R,
                int8_t *A_noised, int8_t *EAL, uint8_t *rootA, uint8_t *commitA,
                int nt, int cert_version) {
    if ((m * k) % 1024) return -1;
    if (k > (1 << 16)) return -2;

    fill_buf(A, m * k, seed, nt);
    calc_input_root(A->data, m * k, key, A, rootA);

    int cert_mode = cert_version >= 3 ? CERT_SALTED : CERT_LEGACY;
    uint8_t saltedA[32];
    if (cert_mode == CERT_SALTED) bind_root(rootA, (uint32_t)m, SEED_SALT_A, saltedA);
    else memcpy(saltedA, rootA, 32);

    blake3_hasher h;
    blake3_hasher_init(&h); blake3_hasher_update(&h, commitB, 32);
    blake3_hasher_update(&h, saltedA, 32); blake3_hasher_finalize(&h, commitA, 32);

    static const uint8_t seedA[32] = "A_tensor";
    unif_int8(seedA, commitA, m * R, EAL, nt);

    uint16_t *fA = malloc((size_t)k * 2 * sizeof(*fA));
    if (!fA) return -3;
    uint16_t *sA = fA + k;
    perm_pairs(seedA, commitA, k, R, fA, sA);

    compute_an(A->data, EAL, A_noised, fA, sA, m, k, R, nt);
    free(fA);
    return 0;
}


int prep_a_side(uint64_t seed, int8_t *A, const uint8_t *key, const uint8_t *commitB,
                int64_t m, int64_t k, int R, int8_t *A_noised, int8_t *EAL,
                uint8_t *rootA, uint8_t *commitA, int nt, int cert_version) {
    prep_input_t input = { .data = A };
    return prep_a_side_cached(seed, &input, key, commitB, m, k, R, A_noised,
                              EAL, rootA, commitA, nt, cert_version);
}
