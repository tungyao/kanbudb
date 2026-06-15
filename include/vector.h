#ifndef KANBUDB_VECTOR_H
#define KANBUDB_VECTOR_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Error codes (extend macros.h codes) */
#define KANBUDB_VEC_OK            0
#define KANBUDB_VEC_ERR_OOM      (-1)
#define KANBUDB_VEC_ERR_NOTFOUND (-2)
#define KANBUDB_VEC_ERR_CORRUPT  (-3)
#define KANBUDB_VEC_ERR_IO       (-4)
#define KANBUDB_VEC_ERR_INVAL    (-5)

/* Index algorithm */
typedef enum {
    KANBUDB_VEC_ALGO_FLAT = 0,
    KANBUDB_VEC_ALGO_HNSW = 1
} kanbudb_vec_algo_t;

/* Distance metrics */
typedef enum {
    KANBUDB_VEC_METRIC_L2 = 0,
    KANBUDB_VEC_METRIC_COSINE = 1,
    KANBUDB_VEC_METRIC_IP = 2
} kanbudb_vec_metric_t;

/* Vector index parameters */
typedef struct {
    kanbudb_vec_algo_t   algo;
    kanbudb_vec_metric_t metric;
    uint32_t             dimension;
    uint32_t             initial_capacity;
    int                  enable_persistence;
    uint32_t             M;                 /* HNSW M param */
    uint32_t             ef_construction;   /* HNSW ef_construction */
    uint32_t             ef_search;         /* HNSW ef_search */
} kanbudb_vec_params_t;

#define KANBUDB_VEC_PARAMS_DEFAULT \
    { .algo = KANBUDB_VEC_ALGO_FLAT, .metric = KANBUDB_VEC_METRIC_L2, \
      .dimension = 0, .initial_capacity = 0, .enable_persistence = 0, \
      .M = 16, .ef_construction = 200, .ef_search = 50 }

/* Opaque handle */
typedef struct kanbudb_vec_index kanbudb_vec_index_t;

/* Search result */
typedef struct {
    uint64_t id;
    float    distance;
} kanbudb_vec_result_t;

/* Statistics */
typedef struct {
    uint64_t count;
    uint64_t capacity;
    uint64_t memory_bytes;
    uint32_t dimension;
} kanbudb_vec_stats_t;

/* Lifecycle */
int kanbudb_vec_create(const char* path, const kanbudb_vec_params_t* params,
                       kanbudb_vec_index_t** out);
int kanbudb_vec_open(const char* path, kanbudb_vec_index_t** out);
int kanbudb_vec_close(kanbudb_vec_index_t* idx);
int kanbudb_vec_flush(kanbudb_vec_index_t* idx);
int kanbudb_vec_flush_interval(kanbudb_vec_index_t* idx, int interval_ms);
int kanbudb_vec_destroy(const char* path);

/* Data operations */
int kanbudb_vec_insert(kanbudb_vec_index_t* idx, uint64_t id,
                       const float* vector);
int kanbudb_vec_insert_batch(kanbudb_vec_index_t* idx, uint32_t count,
                             const uint64_t* ids, const float* vectors);
int kanbudb_vec_delete(kanbudb_vec_index_t* idx, uint64_t id);
int kanbudb_vec_get(kanbudb_vec_index_t* idx, uint64_t id,
                    float* out_vector);

/* Search */
int kanbudb_vec_search(kanbudb_vec_index_t* idx, const float* query,
                       uint32_t k, kanbudb_vec_result_t* results);
int kanbudb_vec_search_radius(kanbudb_vec_index_t* idx,
                              const float* query, float radius,
                              kanbudb_vec_result_t* results,
                              uint32_t max_results);

/* Info */
int kanbudb_vec_count(kanbudb_vec_index_t* idx);
int kanbudb_vec_dimension(kanbudb_vec_index_t* idx);
int kanbudb_vec_stats(kanbudb_vec_index_t* idx,
                      kanbudb_vec_stats_t* stats);

/* ------------------------------------------------------------------ */
/*  Built-in text embedding (n-gram hash + random projection)          */
/* ------------------------------------------------------------------ */

typedef struct kanbudb_embed kanbudb_embed_t;

int  kanbudb_embed_create(uint32_t dimensions, uint32_t ngram_size,
                          kanbudb_embed_t** out);
void kanbudb_embed_destroy(kanbudb_embed_t* embed);
int  kanbudb_embed_text(const kanbudb_embed_t* embed,
                        const char* text, size_t text_len,
                        float* out_vector);
int  kanbudb_embed_batch(const kanbudb_embed_t* embed,
                         const char** texts, const size_t* text_lens,
                         uint32_t count, float* out_vectors);
uint32_t kanbudb_embed_dimensions(const kanbudb_embed_t* embed);

#ifdef __cplusplus
}
#endif

#endif /* KANBUDB_VECTOR_H */
