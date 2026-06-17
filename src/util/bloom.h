#ifndef KANBUDB_BLOOM_H
#define KANBUDB_BLOOM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Bloom Filter ────────────────────────────────────────────
 *
 * Standard bloom filter with double-hashing (Kirsch-Mitzenmacher).
 * False positive rate ≈ (1 - e^{-kn/m})^k
 *
 * Recommended bits-per-key (b):
 *   b=10 → ~1% FPR,  b=14 → ~0.1%,  b=20 → ~0.02%
 * We default to b=10 (compact) with k = floor(0.693 * m/n) ≈ 7.
 */

typedef struct {
    uint8_t* bits;        /* bit array */
    uint32_t  num_bits;   /* m — total bits */
    uint32_t  num_hashes; /* k — number of hash functions */
    uint32_t  num_keys;   /* expected insertions */
} kanbudb_bloom_t;

/* Create a bloom filter for ~num_keys insertions at bits_per_key FPR.
 * bits_per_key=10 → ~1% FPR. Returns NULL on OOM. */
kanbudb_bloom_t* kanbudb_bloom_create(uint32_t num_keys, uint8_t bits_per_key);

/* Load an existing bloom filter from a serialized byte array.
 * buf points to raw bloom data: [num_bits(4)][num_hashes(4)][num_keys(4)][bit_array(num_bits/8)].
 * The buffer is NOT copied — the bloom struct points into it.
 * Returns NULL if buf is NULL or buf_size < minimum. */
kanbudb_bloom_t* kanbudb_bloom_load(const uint8_t* buf, size_t buf_size);

/* Destroy a bloom filter created with kanbudb_bloom_create (frees bits).
 * For kanbudb_bloom_load, call kanbudb_bloom_unload instead (no free). */
void             kanbudb_bloom_destroy(kanbudb_bloom_t* bf);

/* Release a loaded bloom filter (no free of bits — caller owns buf). */
void             kanbudb_bloom_unload(kanbudb_bloom_t* bf);

/* Insert a key into the bloom filter. key can be arbitrary bytes. */
void             kanbudb_bloom_insert(kanbudb_bloom_t* bf,
                                      const void* key, size_t key_len);

/* Check if a key MAY be in the set. Returns 1 = maybe, 0 = definitely not. */
int              kanbudb_bloom_maybe(const kanbudb_bloom_t* bf,
                                     const void* key, size_t key_len);

/* Reset the bloom filter (zero all bits, keep capacity). */
void             kanbudb_bloom_reset(kanbudb_bloom_t* bf);

/* Return the serialized size in bytes needed for this bloom filter. */
uint32_t         kanbudb_bloom_serialized_size(const kanbudb_bloom_t* bf);

/* Serialize the bloom filter into a byte buffer (buf must be >= serialized_size). */
void             kanbudb_bloom_serialize(const kanbudb_bloom_t* bf, uint8_t* buf);

#ifdef __cplusplus
}
#endif

#endif /* KANBUDB_BLOOM_H */
