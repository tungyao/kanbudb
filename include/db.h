#ifndef KANBUDB_DB_H
#define KANBUDB_DB_H

#include <stddef.h>
#include <stdint.h>

#include "macros.h"
#include "vector.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque types */
typedef struct kanbudb_db db_t;
typedef struct query_builder_t query_builder_t;
typedef struct result_set_t result_set_t;
typedef struct qb_condition_t qb_condition_t;
/* Column types */
typedef enum {
  KANBUDB_INT32,
  KANBUDB_INT64,
  KANBUDB_FLOAT,
  KANBUDB_DOUBLE,
  KANBUDB_STRING,
  KANBUDB_BLOB,
  KANBUDB_BOOL
} kanbudb_col_type_t;

/* Fsync modes */
typedef enum {
  KANBUDB_FSYNC_NONE,
  KANBUDB_FSYNC_PERIODIC,
  KANBUDB_FSYNC_ALWAYS
} kanbudb_fsync_mode_t;

/* Database configuration */
typedef struct db_config_t {
  kanbudb_fsync_mode_t fsync_mode;
  size_t cache_size;
  size_t memtable_size;
  int compaction_threads;
  int multi_process;      /* enable multi-process shared mode */
} db_config_t;

/* FTS options */
typedef struct {
  int enable_stemming;
  int enable_stop_words;
  const char *language;
} fts_options_t;

/* Database lifecycle */
int db_open(const char *path, const db_config_t *config, db_t **out);
int db_close(db_t *db);
int db_last_error(db_t *db);
const char *db_error_string(int err);

/* Schema */
int db_create_table(db_t *db, const char *table_name,
                    const char **col_names, const kanbudb_col_type_t *col_types,
                    int num_columns, const char *primary_key);

/* CRUD */
int db_put(db_t *db, const char *table, const char *key, size_t key_len,
           const void *value, size_t value_len);
int db_get(db_t *db, const char *table, const char *key, size_t key_len,
           void **value, size_t *value_len);
int db_delete(db_t *db, const char *table, const char *key, size_t key_len);

/* Query builder */
query_builder_t *db_query(db_t *db, const char *table);
int qb_from(query_builder_t *qb, const char *table);
int qb_filter(query_builder_t *qb, const char *column,
              const char *op, const void *value);
int qb_sort(query_builder_t *qb, const char *column, int ascending);
int qb_limit(query_builder_t *qb, int limit);
int qb_join(query_builder_t *qb, const char *table,
            const char *on_local, const char *on_foreign);
result_set_t *qb_exec(query_builder_t *qb);
void qb_destroy(query_builder_t *qb);

/* Condition tree for multi-condition filters */
qb_condition_t *qb_cond(query_builder_t *qb, const char *column,
                         const char *op, const void *value);
qb_condition_t *qb_cond_and(query_builder_t *qb,
                            qb_condition_t *left, qb_condition_t *right);
qb_condition_t *qb_cond_or(query_builder_t *qb,
                           qb_condition_t *left, qb_condition_t *right);
qb_condition_t *qb_cond_not(query_builder_t *qb, qb_condition_t *child);
int qb_where(query_builder_t *qb, qb_condition_t *cond);

/* Result set */
int rs_next(result_set_t *rs);
int rs_get_column(result_set_t *rs, int col, void **data, size_t *len);
kanbudb_col_type_t rs_get_column_type(result_set_t *rs, int col);
int rs_num_columns(result_set_t *rs);
void rs_close(result_set_t *rs);

/* Full-text search */
int db_fts_search(db_t *db, const char *table, const char *column,
                  const char *query, result_set_t **out);
int db_fts_create_index(db_t *db, const char *table, const char *column,
                        const fts_options_t *opts);
int db_fts_drop_index(db_t *db, const char *table, const char *column);

/* Vector index + built-in embedding */
typedef struct kanbudb_vec_index kanbudb_vec_index_t;
typedef struct kanbudb_embed kanbudb_embed_t;

typedef struct {
    uint32_t dimension;
    uint32_t ngram_size;
    int      enable_hnsw;
    uint32_t hnsw_m;
    uint32_t hnsw_ef_construction;
} db_vec_options_t;

#define KANBUDB_VEC_OPTIONS_DEFAULT \
    { .dimension = 128, .ngram_size = 3, .enable_hnsw = 0, \
      .hnsw_m = 16, .hnsw_ef_construction = 200 }

int db_vec_create_index(db_t *db, const db_vec_options_t *opts);
int db_vec_destroy_index(db_t *db);
int db_vec_insert_text(db_t *db, uint64_t id,
                       const char *text, size_t text_len);
int db_vec_insert_batch(db_t *db, uint32_t count,
                        const uint64_t *ids,
                        const char **texts, const size_t *text_lens);
int db_vec_insert_vector(db_t *db, uint64_t id, const float *vector);
int db_vec_search_text(db_t *db, const char *text, size_t text_len,
                       uint32_t k, kanbudb_vec_result_t *results);
int db_vec_search(db_t *db, const float *query,
                  uint32_t k, kanbudb_vec_result_t *results);
int db_vec_delete(db_t *db, uint64_t id);
int db_vec_count(db_t *db);
int db_vec_flush(db_t *db);
int db_vec_set_embed(db_t *db, kanbudb_embed_t *embed);

/* ── Filtered vector search ─────────────────────────────────── */
typedef int (*db_vec_filter_fn)(uint64_t id, void *ctx);

int db_vec_search_filtered(db_t *db, const float *query, uint32_t k,
                           db_vec_filter_fn filter, void *filter_ctx,
                           kanbudb_vec_result_t *results);

int db_vec_search_text_filtered(db_t *db, const char *text, size_t text_len,
                                uint32_t k, db_vec_filter_fn filter,
                                void *filter_ctx,
                                kanbudb_vec_result_t *results);

/* ── Hybrid search (vector + FTS fusion) ────────────────────── */
typedef enum {
    KANBUDB_HYBRID_RRF = 0,
    KANBUDB_HYBRID_WEIGHTED = 1
} kanbudb_hybrid_mode_t;

typedef struct {
    kanbudb_hybrid_mode_t mode;
    double vec_weight;
    double fts_weight;
    uint32_t k;
} kanbudb_hybrid_params_t;

#define KANBUDB_HYBRID_PARAMS_DEFAULT \
    { .mode = KANBUDB_HYBRID_RRF, .vec_weight = 0.5, .fts_weight = 0.5, .k = 10 }

typedef struct {
    uint64_t id;
    double   score;
    double   vec_distance;
    double   fts_score;
} kanbudb_hybrid_result_t;

int db_hybrid_search(db_t *db, const char *table, const char *column,
                     const char *fts_query, const float *vec_query,
                     const kanbudb_hybrid_params_t *params,
                     kanbudb_hybrid_result_t *results, int max_results);

/* ── Vector quantization ────────────────────────────────────── */
typedef struct kanbudb_quantizer kanbudb_quantizer_t;

typedef enum {
    KANBUDB_QUANT_NONE = 0,
    KANBUDB_QUANT_SQ8 = 1,
    KANBUDB_QUANT_PQ = 2
} kanbudb_quant_type_t;

typedef struct {
    kanbudb_quant_type_t type;
    uint32_t dimension;
    uint32_t pq_subspaces;
} kanbudb_quant_params_t;

int  db_vec_quant_create(db_t *db, const kanbudb_quant_params_t *params);
void db_vec_quant_destroy(db_t *db);
int  db_vec_quant_train(db_t *db, const float *vectors, uint32_t count);
int  db_vec_quant_insert(db_t *db, uint64_t id, const float *vector);
int  db_vec_quant_search(db_t *db, const float *query, uint32_t k,
                         kanbudb_vec_result_t *results);
int  db_vec_quant_decode(db_t *db, uint64_t id, float *out_vector);

#ifdef __cplusplus
}
#endif

#endif /* KANBUDB_DB_H */
