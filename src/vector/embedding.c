#include "embedding.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

struct kanbudb_embed {
    uint32_t dimensions;
    uint32_t ngram_size;
    float*   proj_matrix;
    uint32_t hash_buckets;
};

static uint32_t fnv1a_hash(const uint8_t* data, size_t len)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; i++) {
        h ^= data[i];
        h *= 16777619u;
    }
    return h;
}

static void seeded_srand(uint32_t seed, float* buf, uint32_t count)
{
    uint32_t state = seed;
    for (uint32_t i = 0; i < count; i++) {
        state = state * 1103515245u + 12345u;
        int32_t raw = (int32_t)(state >> 16);
        buf[i] = ((float)(raw & 0x7FFF) / 16384.0f) - 1.0f;
    }
}

int kanbudb_embed_create(uint32_t dimensions, uint32_t ngram_size,
                         kanbudb_embed_t** out)
{
    if (!out || dimensions == 0) return -1;
    if (ngram_size == 0) ngram_size = 3;

    kanbudb_embed_t* e = calloc(1, sizeof(*e));
    if (!e) return -1;

    e->dimensions   = dimensions;
    e->ngram_size   = ngram_size;
    e->hash_buckets = dimensions * 256;

    e->proj_matrix = malloc((size_t)e->hash_buckets * dimensions * sizeof(float));
    if (!e->proj_matrix) { free(e); return -1; }

    seeded_srand(42, e->proj_matrix, e->hash_buckets * dimensions);

    *out = e;
    return 0;
}

void kanbudb_embed_destroy(kanbudb_embed_t* embed)
{
    if (!embed) return;
    free(embed->proj_matrix);
    free(embed);
}

int kanbudb_embed_text(const kanbudb_embed_t* embed,
                       const char* text, size_t text_len,
                       float* out_vector)
{
    if (!embed || !text || !out_vector) return -1;

    uint32_t dim = embed->dimensions;
    uint32_t ng  = embed->ngram_size;

    memset(out_vector, 0, (size_t)dim * sizeof(float));

    if (text_len < ng) {
        uint32_t h = fnv1a_hash((const uint8_t*)text, text_len);
        uint32_t bucket = h % embed->hash_buckets;
        for (uint32_t d = 0; d < dim; d++)
            out_vector[d] += embed->proj_matrix[(size_t)bucket * dim + d];
    } else {
        for (size_t i = 0; i + ng <= text_len; i++) {
            uint32_t h = fnv1a_hash((const uint8_t*)text + i, ng);
            uint32_t bucket = h % embed->hash_buckets;
            for (uint32_t d = 0; d < dim; d++)
                out_vector[d] += embed->proj_matrix[(size_t)bucket * dim + d];
        }
    }

    float norm = 0.0f;
    for (uint32_t d = 0; d < dim; d++)
        norm += out_vector[d] * out_vector[d];
    if (norm > 1e-30f) {
        norm = 1.0f / sqrtf(norm);
        for (uint32_t d = 0; d < dim; d++)
            out_vector[d] *= norm;
    }

    return 0;
}

int kanbudb_embed_batch(const kanbudb_embed_t* embed,
                        const char** texts, const size_t* text_lens,
                        uint32_t count, float* out_vectors)
{
    if (!embed || !texts || !text_lens || !out_vectors || count == 0)
        return -1;
    for (uint32_t i = 0; i < count; i++) {
        if (kanbudb_embed_text(embed, texts[i], text_lens[i],
                               out_vectors + (size_t)i * embed->dimensions) != 0)
            return -1;
    }
    return 0;
}

uint32_t kanbudb_embed_dimensions(const kanbudb_embed_t* embed)
{
    return embed ? embed->dimensions : 0;
}
