#include "db.h"
#include "macros.h"
#include "core/db.h"
#include "query/query_builder.h"
#include "lsm.h"
#include "btree.h"
#include "wal.h"
#include "fts/index.h"
#include "fts/tokenizer.h"
#include "fts/parser.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const db_config_t default_config = {
  KANBUDB_FSYNC_NONE,
  65536,
  65536,
  1
};

static int find_table(struct kanbudb_db* db, const char* name) {
  for (int i = 0; i < db->num_tables; i++) {
    if (strcmp(db->tables[i].name, name) == 0)
      return i;
  }
  return -1;
}

int db_open(const char* path, const db_config_t* config, db_t** out) {
  if (!path || !out) return KANBUDB_ERR_INVAL;

  struct kanbudb_db* db = (struct kanbudb_db*)calloc(1, sizeof(*db));
  if (!db) return KANBUDB_ERR_OOM;

  db->path = (char*)malloc(strlen(path) + 1);
  if (!db->path) { free(db); return KANBUDB_ERR_OOM; }
  strcpy(db->path, path);

  if (config) {
    db->config = *config;
  } else {
    db->config = default_config;
  }

  char wal_path[512];
  snprintf(wal_path, sizeof(wal_path), "%s.wal", path);

  db->wal = wal_create(wal_path, db->config.fsync_mode);
  if (!db->wal) { free(db->path); free(db); return KANBUDB_ERR_IO; }

  char lsm_path[512];
  snprintf(lsm_path, sizeof(lsm_path), "%s.lsm", path);

  db->lsm = lsm_create(lsm_path, db->config.memtable_size);
  if (!db->lsm) {
    wal_destroy(db->wal);
    free(db->path);
    free(db);
    return KANBUDB_ERR_OOM;
  }

  db->btree = btree_create();
  if (!db->btree) {
    lsm_destroy(db->lsm);
    wal_destroy(db->wal);
    free(db->path);
    free(db);
    return KANBUDB_ERR_OOM;
  }

  db->fts_index = fts_index_create();
  if (!db->fts_index) {
    btree_destroy(db->btree);
    lsm_destroy(db->lsm);
    wal_destroy(db->wal);
    free(db->path);
    free(db);
    return KANBUDB_ERR_OOM;
  }

  db->num_tables = 0;
  db->last_error = KANBUDB_OK;

  *out = db;
  return KANBUDB_OK;
}

int db_close(db_t* db) {
  if (!db) return KANBUDB_ERR_INVAL;
  struct kanbudb_db* internal = (struct kanbudb_db*)db;

  for (int i = 0; i < internal->num_tables; i++) {
    kanbudb_table_t* t = &internal->tables[i];
    if (t->col_names) {
      for (int j = 0; j < t->num_cols; j++) {
        free(t->col_names[j]);
      }
      free(t->col_names);
    }
    free(t->col_types);
  }

  if (internal->fts_index) fts_index_destroy(internal->fts_index);
  btree_destroy(internal->btree);
  lsm_destroy(internal->lsm);
  wal_destroy(internal->wal);
  free(internal->path);
  free(internal);
  return KANBUDB_OK;
}

int db_last_error(db_t* db) {
  if (!db) return KANBUDB_ERR_INVAL;
  return ((struct kanbudb_db*)db)->last_error;
}

const char* db_error_string(int err) {
  switch (err) {
    case KANBUDB_OK:         return "success";
    case KANBUDB_ERR_OOM:    return "out of memory";
    case KANBUDB_ERR_NOTFOUND: return "not found";
    case KANBUDB_ERR_EXISTS: return "already exists";
    case KANBUDB_ERR_CORRUPT: return "corrupt data";
    case KANBUDB_ERR_IO:     return "I/O error";
    case KANBUDB_ERR_INVAL:  return "invalid argument";
    case KANBUDB_ERR_BUSY:   return "busy";
    default:                return "unknown error";
  }
}

int db_create_table(db_t* db, const char* table_name,
                    const char** col_names, const kanbudb_col_type_t* col_types,
                    int num_columns, const char* primary_key) {
  if (!db || !table_name || !col_names || !col_types || num_columns <= 0) {
    return KANBUDB_ERR_INVAL;
  }

  struct kanbudb_db* internal = (struct kanbudb_db*)db;

  if (find_table(internal, table_name) >= 0) {
    internal->last_error = KANBUDB_ERR_EXISTS;
    return KANBUDB_ERR_EXISTS;
  }

  if (internal->num_tables >= KANBUDB_MAX_TABLES) {
    internal->last_error = KANBUDB_ERR_OOM;
    return KANBUDB_ERR_OOM;
  }

  kanbudb_table_t* t = &internal->tables[internal->num_tables];

  size_t nlen = strlen(table_name);
  if (nlen >= sizeof(t->name)) nlen = sizeof(t->name) - 1;
  memcpy(t->name, table_name, nlen);
  t->name[nlen] = '\0';

  t->num_cols = num_columns;

  t->col_types = (kanbudb_col_type_t*)malloc((size_t)num_columns * sizeof(kanbudb_col_type_t));
  if (!t->col_types) { internal->last_error = KANBUDB_ERR_OOM; return KANBUDB_ERR_OOM; }
  memcpy(t->col_types, col_types, (size_t)num_columns * sizeof(kanbudb_col_type_t));

  t->col_names = (char**)malloc((size_t)num_columns * sizeof(char*));
  if (!t->col_names) { free(t->col_types); internal->last_error = KANBUDB_ERR_OOM; return KANBUDB_ERR_OOM; }

  for (int i = 0; i < num_columns; i++) {
    t->col_names[i] = (char*)malloc(strlen(col_names[i]) + 1);
    if (!t->col_names[i]) {
      for (int j = 0; j < i; j++) free(t->col_names[j]);
      free(t->col_names);
      free(t->col_types);
      internal->last_error = KANBUDB_ERR_OOM;
      return KANBUDB_ERR_OOM;
    }
    strcpy(t->col_names[i], col_names[i]);
  }

  t->primary_key_idx = -1;
  if (primary_key) {
    for (int i = 0; i < num_columns; i++) {
      if (strcmp(col_names[i], primary_key) == 0) {
        t->primary_key_idx = i;
        break;
      }
    }
  }

  t->id = (uint64_t)(internal->num_tables + 1);
  internal->num_tables++;

  internal->last_error = KANBUDB_OK;
  return KANBUDB_OK;
}

int db_put(db_t* db, const char* table, const char* key, size_t key_len,
           const void* value, size_t value_len) {
  if (!db || !table || !key) return KANBUDB_ERR_INVAL;

  struct kanbudb_db* internal = (struct kanbudb_db*)db;
  int idx = find_table(internal, table);
  if (idx < 0) { internal->last_error = KANBUDB_ERR_NOTFOUND; return KANBUDB_ERR_NOTFOUND; }

  uint64_t table_id = internal->tables[idx].id;

  int rc = wal_append(internal->wal, KANBUDB_WAL_PUT, table_id,
                       key, key_len, value, value_len);
  if (rc != KANBUDB_OK) { internal->last_error = rc; return rc; }

  rc = lsm_put(internal->lsm, table_id, key, key_len, value, value_len);
  if (rc != KANBUDB_OK) { internal->last_error = rc; return rc; }

  internal->last_error = KANBUDB_OK;
  return KANBUDB_OK;
}

int db_get(db_t* db, const char* table, const char* key, size_t key_len,
           void** value, size_t* value_len) {
  if (!db || !table || !key || !value || !value_len) return KANBUDB_ERR_INVAL;

  struct kanbudb_db* internal = (struct kanbudb_db*)db;
  int idx = find_table(internal, table);
  if (idx < 0) { internal->last_error = KANBUDB_ERR_NOTFOUND; return KANBUDB_ERR_NOTFOUND; }

  int rc = lsm_get(internal->lsm, internal->tables[idx].id,
                    key, key_len, value, value_len);
  if (rc == KANBUDB_OK) { internal->last_error = KANBUDB_OK; return KANBUDB_OK; }

  rc = btree_get(internal->btree, key, key_len, value, value_len);
  if (rc == KANBUDB_OK) { internal->last_error = KANBUDB_OK; return KANBUDB_OK; }

  internal->last_error = KANBUDB_ERR_NOTFOUND;
  return KANBUDB_ERR_NOTFOUND;
}

int db_delete(db_t* db, const char* table, const char* key, size_t key_len) {
  if (!db || !table || !key) return KANBUDB_ERR_INVAL;

  struct kanbudb_db* internal = (struct kanbudb_db*)db;
  int idx = find_table(internal, table);
  if (idx < 0) { internal->last_error = KANBUDB_ERR_NOTFOUND; return KANBUDB_ERR_NOTFOUND; }

  uint64_t table_id = internal->tables[idx].id;

  int rc = wal_append(internal->wal, KANBUDB_WAL_DELETE, table_id,
                       key, key_len, NULL, 0);
  if (rc != KANBUDB_OK) { internal->last_error = rc; return rc; }

  rc = lsm_delete(internal->lsm, table_id, key, key_len);
  if (rc != KANBUDB_OK) { internal->last_error = rc; return rc; }

  internal->last_error = KANBUDB_OK;
  return KANBUDB_OK;
}

int db_fts_create_index(db_t* db, const char* table, const char* column,
                        const fts_options_t* opts) {
  (void)opts;
  if (!db || !table || !column) return KANBUDB_ERR_INVAL;
  struct kanbudb_db* internal = (struct kanbudb_db*)db;
  if (find_table(internal, table) < 0) {
    internal->last_error = KANBUDB_ERR_NOTFOUND;
    return KANBUDB_ERR_NOTFOUND;
  }
  internal->last_error = KANBUDB_OK;
  return KANBUDB_OK;
}

int db_fts_drop_index(db_t* db, const char* table, const char* column) {
  if (!db || !table || !column) return KANBUDB_ERR_INVAL;
  struct kanbudb_db* internal = (struct kanbudb_db*)db;
  internal->last_error = KANBUDB_OK;
  return KANBUDB_OK;
}

int db_fts_search(db_t* db, const char* table, const char* column,
                  const char* query, result_set_t** out) {
  if (!db || !table || !column || !query || !out) return KANBUDB_ERR_INVAL;
  struct kanbudb_db* internal = (struct kanbudb_db*)db;

  if (find_table(internal, table) < 0) {
    internal->last_error = KANBUDB_ERR_NOTFOUND;
    return KANBUDB_ERR_NOTFOUND;
  }

  if (!internal->fts_index) {
    internal->last_error = KANBUDB_ERR_INVAL;
    return KANBUDB_ERR_INVAL;
  }

  fts_query_node_t nodes[64];
  int num_nodes = fts_query_parse(query, nodes, 64);
  if (num_nodes <= 0) {
    internal->last_error = KANBUDB_ERR_INVAL;
    return KANBUDB_ERR_INVAL;
  }

  uint64_t doc_id_buf[1024];
  int num_doc_ids = 0;

  for (int i = 0; i < num_nodes; i++) {
    if (nodes[i].op == FTS_TERM || nodes[i].op == FTS_FUZZY) {
      uint64_t results[256];
      int n;
      if (nodes[i].op == FTS_FUZZY) {
        int max_edits = nodes[i].fuzzy_distance > 0 ? nodes[i].fuzzy_distance : 1;
        n = fts_index_search_fuzzy(internal->fts_index, nodes[i].text,
                                    max_edits, results, 256);
      } else {
        n = fts_index_search(internal->fts_index, nodes[i].text,
                              results, 256);
      }
      for (int j = 0; j < n && num_doc_ids < 1024; j++) {
        int already = 0;
        for (int k = 0; k < num_doc_ids; k++) {
          if (doc_id_buf[k] == results[j]) { already = 1; break; }
        }
        if (!already) {
          doc_id_buf[num_doc_ids++] = results[j];
        }
      }
    }
  }

  result_set_t* rs = (result_set_t*)calloc(1, sizeof(*rs));
  if (!rs) { internal->last_error = KANBUDB_ERR_OOM; return KANBUDB_ERR_OOM; }

  rs->is_fts = 1;
  rs->num_rows = num_doc_ids;
  rs->num_cols = 2;
  rs->col_types[0] = KANBUDB_INT64;
  rs->col_types[1] = KANBUDB_DOUBLE;
  rs->col_names[0] = (char*)"doc_id";
  rs->col_names[1] = (char*)"score";

  if (num_doc_ids > 0) {
    rs->doc_ids = (uint64_t*)malloc((size_t)num_doc_ids * sizeof(uint64_t));
    rs->scores = (double*)calloc((size_t)num_doc_ids, sizeof(double));
    if (!rs->doc_ids || !rs->scores) {
      free(rs->doc_ids);
      free(rs->scores);
      free(rs);
      internal->last_error = KANBUDB_ERR_OOM;
      return KANBUDB_ERR_OOM;
    }
    memcpy(rs->doc_ids, doc_id_buf, (size_t)num_doc_ids * sizeof(uint64_t));
  } else {
    rs->doc_ids = NULL;
    rs->scores = NULL;
  }

  rs->current = -1;

  *out = rs;
  internal->last_error = KANBUDB_OK;
  return KANBUDB_OK;
}
