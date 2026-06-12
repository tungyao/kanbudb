#ifndef KANBUDB_CORE_DB_H
#define KANBUDB_CORE_DB_H

#include <macros.h>
#include <db.h>
#include <pthread.h>
#include "fts/index.h"

#define KANBUDB_MAX_TABLES 64

typedef struct {
  char              name[64];
  kanbudb_col_type_t* col_types;
  char**            col_names;
  int               num_cols;
  int               primary_key_idx;
  uint64_t          id;
} kanbudb_table_t;

struct kanbudb_lsm;
struct kanbudb_btree;
struct kanbudb_wal;

struct kanbudb_db {
  char*             path;
  struct kanbudb_lsm*  lsm;
  struct kanbudb_btree* btree;
  struct kanbudb_wal*   wal;
  kanbudb_table_t       tables[KANBUDB_MAX_TABLES];
  int                  num_tables;
  db_config_t          config;
  int                  last_error;
  kanbudb_fts_index_t*  fts_index;
  pthread_rwlock_t     rwlock;
};

#endif /* KANBUDB_CORE_DB_H */
