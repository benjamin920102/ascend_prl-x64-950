#ifndef PEARL_PREP_H
#define PEARL_PREP_H

#include <stddef.h>
#include <stdint.h>

/* One cache per matrix side. Both bundles may share it when prep is serialized. */
typedef struct {
    size_t bytes;
    uint8_t key[32];
    size_t depth;
    uint8_t siblings[8 * sizeof(size_t)][32];
} prep_root_cache_t;

/* The owner must reset this state if data is replaced or modified outside prep. */
typedef struct {
    int8_t *data;
    size_t zero_tail_bytes;
    prep_root_cache_t *root_cache;
} prep_input_t;

int prep_random_cached(uint64_t seed, prep_input_t *A, prep_input_t *B, const uint8_t *key,
                       int64_t m, int64_t n, int64_t k, int rank,
                       int8_t *An, int8_t *Bt, int8_t *EAL, int8_t *EBR,
                       uint8_t *rA, uint8_t *rB, uint8_t *cA, uint8_t *cB,
                       int nt, int cert_version, int packed);
int prep_b_side_cached(uint64_t seed, prep_input_t *B, const uint8_t *key,
                       int64_t n, int64_t k, int rank, int8_t *bt_packed,
                       int8_t *EBR, uint8_t *rootB, uint8_t *commitB,
                       int nt, int cert_version);
int prep_a_side_cached(uint64_t seed, prep_input_t *A, const uint8_t *key, const uint8_t *commitB,
                       int64_t m, int64_t k, int rank, int8_t *An, int8_t *EAL,
                       uint8_t *rootA, uint8_t *commitA, int nt, int cert_version);

#endif
