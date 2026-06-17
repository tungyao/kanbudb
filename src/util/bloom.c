#include "bloom.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ── Hash helpers: split the key into two 64-bit seeds ──── */

static inline uint64_t hash64(const uint8_t* data, size_t len, uint64_t seed)
{
    /* MurmurHash64A finalizer — fast for short/medium keys */
    uint64_t h = seed ^ (len * 0x9e3779b97f4a7c15ULL);
    const uint64_t* p = (const uint64_t*)data;
    size_t n8 = len / 8;
    for (size_t i = 0; i < n8; i++) {
        uint64_t k = p[i];
        k *= 0x87c37b91114253d5ULL;
        k ^= (k >> 31);
        k *= 0x4cf5ad432745937fULL;
        h ^= k;
        h  = (h << 27) | (h >> 37);
        h  = h * 5 + 0x52dce729;
    }
    const uint8_t* tail = data + n8 * 8;
    uint64_t k = 0;
    switch (len & 7) {
        case 7: k ^= (uint64_t)tail[6] << 48;  /* fallthrough */
        case 6: k ^= (uint64_t)tail[5] << 40;  /* fallthrough */
        case 5: k ^= (uint64_t)tail[4] << 32;  /* fallthrough */
        case 4: k ^= (uint64_t)tail[3] << 24;  /* fallthrough */
        case 3: k ^= (uint64_t)tail[2] << 16;  /* fallthrough */
        case 2: k ^= (uint64_t)tail[1] << 8;   /* fallthrough */
        case 1: k ^= tail[0];
                k *= 0x87c37b91114253d5ULL;
                k ^= (k >> 31);
                k *= 0x4cf5ad432745937fULL;
                h ^= k;
    }
    h ^= (h >> 37);
    h *= 0x52dce729;
    h ^= (h >> 32);
    return h;
}

/* Compute h1 (first hash) and h2 (second hash) from key */
#define BLOOM_HASH_SEED1 0x5a4f9a3b1c6d8e2fULL
#define BLOOM_HASH_SEED2 0xc8d3e1f4a5b6c7d8ULL

static inline void bloom_hash(const uint8_t* key, size_t key_len,
                              uint64_t* h1, uint64_t* h2)
{
    *h1 = hash64(key, key_len, BLOOM_HASH_SEED1);
    *h2 = hash64(key, key_len, BLOOM_HASH_SEED2);
}

/* ── Optimal k computation ────────────────────────────────── */

static uint32_t optimal_k(uint32_t num_bits, uint32_t num_keys)
{
    if (num_keys == 0) return 1;
    /* k = (m/n) * ln(2) */
    double ratio = (double)num_bits / (double)num_keys;
    uint32_t k = (uint32_t)(ratio * 0.69314718 + 0.5);
    if (k < 1) k = 1;
    return k;
}

/* ── Core API ─────────────────────────────────────────────── */

kanbudb_bloom_t* kanbudb_bloom_create(uint32_t num_keys, uint8_t bits_per_key)
{
    if (num_keys == 0 || bits_per_key == 0) return NULL;

    uint32_t num_bits = num_keys * bits_per_key;
    /* Clamp to reasonable range */
    if (num_bits < 64) num_bits = 64;
    /* Round up to multiple of 64 for aligned access */
    num_bits = (num_bits + 63) & ~63U;

    uint32_t num_hashes = optimal_k(num_bits, num_keys);
    uint32_t byte_count = (num_bits + 7) / 8;

    kanbudb_bloom_t* bf = (kanbudb_bloom_t*)calloc(1, sizeof(*bf));
    if (!bf) return NULL;

    bf->bits = (uint8_t*)calloc(1, byte_count);
    if (!bf->bits) { free(bf); return NULL; }

    bf->num_bits   = num_bits;
    bf->num_hashes = num_hashes;
    bf->num_keys   = num_keys;
    return bf;
}

kanbudb_bloom_t* kanbudb_bloom_load(const uint8_t* buf, size_t buf_size)
{
    if (!buf) return NULL;
    if (buf_size < 12) return NULL; /* need at least 3 × uint32 */

    uint32_t nb, nh, nk;
    memcpy(&nb, buf, 4);
    memcpy(&nh, buf + 4, 4);
    memcpy(&nk, buf + 8, 4);

    uint32_t byte_count = (nb + 7) / 8;
    if (buf_size < (size_t)(12 + byte_count)) return NULL;

    /* Allocate a private copy so the bloom fully owns its data */
    uint8_t* bits_copy = (uint8_t*)malloc(byte_count);
    if (!bits_copy) return NULL;
    memcpy(bits_copy, buf + 12, byte_count);

    kanbudb_bloom_t* bf = (kanbudb_bloom_t*)calloc(1, sizeof(*bf));
    if (!bf) { free(bits_copy); return NULL; }

    bf->num_bits   = nb;
    bf->num_hashes = nh;
    bf->num_keys   = nk;
    bf->bits       = bits_copy;
    return bf;
}

void kanbudb_bloom_destroy(kanbudb_bloom_t* bf)
{
    if (!bf) return;
    free(bf->bits);
    free(bf);
}

void kanbudb_bloom_unload(kanbudb_bloom_t* bf)
{
    /* Bits were allocated by load (private copy) — free them, then free struct */
    if (!bf) return;
    free(bf->bits);
    free(bf);
}

void kanbudb_bloom_insert(kanbudb_bloom_t* bf,
                          const void* key, size_t key_len)
{
    if (!bf || !key) return;

    uint64_t h1, h2;
    bloom_hash((const uint8_t*)key, key_len, &h1, &h2);

    for (uint32_t i = 0; i < bf->num_hashes; i++) {
        uint64_t bit = (h1 + i * h2) % bf->num_bits;
        bf->bits[bit / 8] |= (uint8_t)(1U << (bit % 8));
    }
}

int kanbudb_bloom_maybe(const kanbudb_bloom_t* bf,
                        const void* key, size_t key_len)
{
    if (!bf || !key) return 0;

    uint64_t h1, h2;
    bloom_hash((const uint8_t*)key, key_len, &h1, &h2);

    for (uint32_t i = 0; i < bf->num_hashes; i++) {
        uint64_t bit = (h1 + i * h2) % bf->num_bits;
        if (!(bf->bits[bit / 8] & (uint8_t)(1U << (bit % 8))))
            return 0; /* definitely not in set */
    }
    return 1; /* maybe in set */
}

void kanbudb_bloom_reset(kanbudb_bloom_t* bf)
{
    if (!bf) return;
    uint32_t byte_count = (bf->num_bits + 7) / 8;
    memset(bf->bits, 0, byte_count);
}

uint32_t kanbudb_bloom_serialized_size(const kanbudb_bloom_t* bf)
{
    if (!bf) return 0;
    return 12 + (bf->num_bits + 7) / 8;
}

void kanbudb_bloom_serialize(const kanbudb_bloom_t* bf, uint8_t* buf)
{
    if (!bf || !buf) return;
    uint32_t byte_count = (bf->num_bits + 7) / 8;
    memcpy(buf,      &bf->num_bits,   4);
    memcpy(buf + 4,  &bf->num_hashes, 4);
    memcpy(buf + 8,  &bf->num_keys,   4);
    memcpy(buf + 12, bf->bits,        byte_count);
}
