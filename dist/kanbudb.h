/* KanbuDB Embedded Database - Single Header */
#ifndef KANBUDB_DB_H
#define KANBUDB_DB_H

#include <stddef.h>
#include <stdint.h>

#include "macros.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque types */
typedef struct kanbudb_db db_t;
typedef struct query_builder_t query_builder_t;
typedef struct result_set_t result_set_t;
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

#ifdef __cplusplus
}
#endif

#endif /* KANBUDB_DB_H */
