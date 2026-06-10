#include "db.h"
#include "macros.h"
#include "core/db.h"
#include "query/query_builder.h"
#include "lsm.h"
#include "btree.h"
#include "wal.h"
#include "sstable.h"
#include "compaction.h"
#include "fts/index.h"
#include "fts/tokenizer.h"
#include "fts/parser.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define KANBUDB_SYSTEM_TABLE_SUFFIX ".system"
#define KANBUDB_COMPACTION_THRESHOLD 4

static const db_config_t default_config = {
  KANBUDB_FSYNC_NONE,
  65536,
  65536,
  1
};

struct sstable_flush_ctx {
  sstable_writer_t* w;
  int had_error;
};

static int flush_write_cb(const lsm_entry_t* e, void* ctx) {
  struct sstable_flush_ctx* fc = (struct sstable_flush_ctx*)ctx;
  uint8_t flag = e->deleted ? KANBUDB_SSTABLE_FLAG_TOMBSTONE
                            : KANBUDB_SSTABLE_FLAG_ALIVE;
  int rc = sstable_writer_add(fc->w, e->key, e->key_len,
                               e->value, e->val_len, flag);
  if (rc != KANBUDB_OK) fc->had_error = 1;
  return rc;
}

static int wal_replay_cb(int op, uint64_t table_id,
                          const void* key, size_t key_len,
                          const void* value, size_t val_len,
                          void* ctx) {
  struct kanbudb_db* db = (struct kanbudb_db*)ctx;
  (void)table_id;
  if (op == KANBUDB_WAL_PUT) {
    return lsm_put(db->lsm, table_id, key, key_len, value, val_len);
  } else {
    return lsm_delete(db->lsm, table_id, key, key_len);
  }
}

static int sstable_load_cb(const void* key, size_t key_len,
                            const void* value, size_t val_len,
                            uint8_t flag, void* ctx) {
  struct kanbudb_db* db = (struct kanbudb_db*)ctx;
  if (flag & KANBUDB_SSTABLE_FLAG_TOMBSTONE) {
    btree_delete(db->btree, key, key_len);
    return KANBUDB_OK;
  }
  return btree_put(db->btree, key, key_len, value, val_len);
}

/* ── Schema persistence ──────────────────────────────────────
 * System SSTable stores table definitions as:
 *   key = "table:N:name"  value = JSON-like schema string
 *   key = "tables_count"  value = count as string
 */

static int schema_save_cb(const void* key, size_t key_len,
                           const void* value, size_t val_len,
                           uint8_t flag, void* ctx) {
  (void)flag;
  sstable_writer_t* w = (sstable_writer_t*)ctx;
  return sstable_writer_add(w, key, key_len, value, val_len,
                             KANBUDB_SSTABLE_FLAG_ALIVE);
}

static int db_save_schema(struct kanbudb_db* db) {
  char sys_path[512];
  char tmp_path[512];
  snprintf(sys_path, sizeof(sys_path), "%s%s", db->path, KANBUDB_SYSTEM_TABLE_SUFFIX);
  snprintf(tmp_path, sizeof(tmp_path), "%s%s.tmp", db->path, KANBUDB_SYSTEM_TABLE_SUFFIX);

  sstable_writer_t* w = sstable_writer_create(tmp_path, sys_path, 0);
  if (!w) return KANBUDB_ERR_OOM;

  /* Write count */
  char count_str[16];
  snprintf(count_str, sizeof(count_str), "%d", db->num_tables);
  sstable_writer_add(w, "tables_count", 13, count_str, strlen(count_str) + 1,
                      KANBUDB_SSTABLE_FLAG_ALIVE);

  for (int i = 0; i < db->num_tables; i++) {
    kanbudb_table_t* t = &db->tables[i];
    /* key = "table:NNN:name" */
    char key[128];
    snprintf(key, sizeof(key), "table:%d:%s", i, t->name);

    /* value = "num_cols|pk_idx|col1_type:col1_name|col2_type:col2_name|..." */
    char val[2048];
    int pos = snprintf(val, sizeof(val), "%d|%d", t->num_cols, t->primary_key_idx);
    for (int j = 0; j < t->num_cols; j++) {
      pos += snprintf(val + pos, sizeof(val) - (size_t)pos,
                       "|%d:%s", (int)t->col_types[j], t->col_names[j]);
    }
    sstable_writer_add(w, key, strlen(key) + 1, val, strlen(val) + 1,
                        KANBUDB_SSTABLE_FLAG_ALIVE);
  }

  int rc = sstable_writer_finish(w);
  sstable_writer_destroy(w);
  return rc;
}

/* Callback for loading schema from system SSTable */
typedef struct {
  struct kanbudb_db* db;
  int error;
} schema_load_ctx_t;

static int schema_load_cb(const void* key, size_t key_len,
                           const void* value, size_t val_len,
                           uint8_t flag, void* ctx) {
  schema_load_ctx_t* lc = (schema_load_ctx_t*)ctx;
  if (flag & KANBUDB_SSTABLE_FLAG_TOMBSTONE) return KANBUDB_OK;

  const char* k = (const char*)key;
  const char* v = (const char*)value;

  if (strcmp(k, "tables_count") == 0) {
    return KANBUDB_OK; /* handled after full scan */
  }

  if (strncmp(k, "table:", 6) != 0) return KANBUDB_OK;

  /* Parse: table:IDX:NAME */
  const char* name_start = k + 6;
  const char* name_colon = strchr(name_start, ':');
  if (!name_colon) return KANBUDB_OK;
  const char* table_name = name_colon + 1;

  /* Parse value: num_cols|pk_idx|type:name|type:name|... */
  int num_cols, pk_idx;
  if (sscanf(v, "%d|%d", &num_cols, &pk_idx) < 2) return KANBUDB_OK;

  if (lc->db->num_tables >= KANBUDB_MAX_TABLES) return KANBUDB_ERR_OOM;
  kanbudb_table_t* t = &lc->db->tables[lc->db->num_tables];
  memset(t, 0, sizeof(*t));

  size_t nlen = strlen(table_name);
  if (nlen >= sizeof(t->name)) nlen = sizeof(t->name) - 1;
  memcpy(t->name, table_name, nlen);
  t->name[nlen] = '\0';
  t->num_cols = num_cols;
  t->primary_key_idx = pk_idx;

  t->col_types = (kanbudb_col_type_t*)malloc((size_t)num_cols * sizeof(kanbudb_col_type_t));
  t->col_names = (char**)malloc((size_t)num_cols * sizeof(char*));
  if (!t->col_types || !t->col_names) { lc->error = 1; return KANBUDB_ERR_OOM; }

  /* Parse each type:name pair */
  const char* p = v;
  int consumed = 0;
  sscanf(p, "%d|%d%n", &num_cols, &pk_idx, &consumed);
  p += consumed;

  for (int j = 0; j < num_cols; j++) {
    int col_type;
    char col_name[256];
    if (sscanf(p, "|%d:%255s%n", &col_type, col_name, &consumed) < 2) break;
    p += consumed;
    t->col_types[j] = (kanbudb_col_type_t)col_type;
    t->col_names[j] = strdup(col_name);
    if (!t->col_names[j]) { lc->error = 1; return KANBUDB_ERR_OOM; }
  }

  t->id = (uint64_t)(lc->db->num_tables + 1);
  lc->db->num_tables++;
  return KANBUDB_OK;
}

/* ── Checkpoint: dump B-tree to SSTable ───────────────────── */

static int btree_dump_cb(const void* key, size_t key_len,
                          const void* value, size_t val_len,
                          uint8_t flag, void* ctx) {
  (void)flag;
  sstable_writer_t* w = (sstable_writer_t*)ctx;
  return sstable_writer_add(w, key, key_len, value, val_len,
                             KANBUDB_SSTABLE_FLAG_ALIVE);
}

static int db_checkpoint(struct kanbudb_db* db) {
  /* Dump B-tree contents to a checkpoint SSTable:
   * {dbpath}.ckpt.{seq} */
  uint64_t seq = lsm_next_seq(db->lsm);

  char ckpt_path[512], tmp_path[512];
  snprintf(ckpt_path, sizeof(ckpt_path), "%s.ckpt.%llu",
           db->path, (unsigned long long)seq);
  snprintf(tmp_path, sizeof(tmp_path), "%s.ckpt.%llu.tmp",
           db->path, (unsigned long long)seq);

  sstable_writer_t* w = sstable_writer_create(tmp_path, ckpt_path, seq);
  if (!w) return KANBUDB_ERR_OOM;

  /* Iterate B-tree via cursor */
  btree_cursor_t* cur = btree_cursor_create(db->btree);
  if (!cur) { sstable_writer_destroy(w); return KANBUDB_ERR_OOM; }

  int rc = btree_cursor_seek(cur, NULL, 0);
  if (rc == KANBUDB_OK) {
    btree_kv_t kv;
    while (btree_cursor_next(cur, &kv) == KANBUDB_OK) {
      rc = sstable_writer_add(w, kv.key, kv.key_len,
                               kv.value, kv.val_len,
                               KANBUDB_SSTABLE_FLAG_ALIVE);
      if (rc != KANBUDB_OK) break;
    }
  }

  btree_cursor_destroy(cur);

  if (rc == KANBUDB_OK) {
    rc = sstable_writer_finish(w);
  }
  sstable_writer_destroy(w);
  return rc;
}

/* ── Auto compaction ──────────────────────────────────────── */

static int db_compact_sstables(struct kanbudb_db* db) {
  char* paths[256];
  int num = sstable_scan_dir(db->path, paths, 256, NULL);
  if (num < 2) {
    for (int i = 0; i < num; i++) free(paths[i]);
    return KANBUDB_OK;
  }

  uint64_t out_seq = lsm_next_seq(db->lsm);
  char out_path[512];
  snprintf(out_path, sizeof(out_path), "%s.sst.1.%llu",
           db->path, (unsigned long long)out_seq);

  kanbudb_compactor_t* c = compactor_create();
  int rc = KANBUDB_ERR_OOM;
  if (c) {
    rc = compactor_merge_sstables(c, (const char**)paths, num, out_path, out_seq);
    compactor_destroy(c);
  }

  /* Remove old SSTables if merge succeeded */
  if (rc == KANBUDB_OK) {
    for (int i = 0; i < num; i++) {
      unlink(paths[i]);
    }
    /* Load merged SSTable into B-tree */
    sstable_reader_t* sr = sstable_reader_open(out_path);
    if (sr) {
      sstable_reader_scan(sr, &sstable_load_cb, db);
      sstable_reader_close(sr);
    }
  }

  for (int i = 0; i < num; i++) free(paths[i]);
  return rc;
}

/* ── SSTable path comparison (sort by seq number) ─────────── */

static int sst_path_cmp(const void* a, const void* b) {
  const char* pa = *(const char**)a;
  const char* pb = *(const char**)b;
  /* Extract last numeric field after the final '.' */
  const char* da = strrchr(pa, '.');
  const char* db = strrchr(pb, '.');
  uint64_t sa = da ? (uint64_t)atoll(da + 1) : 0;
  uint64_t sb = db ? (uint64_t)atoll(db + 1) : 0;
  if (sa < sb) return -1;
  if (sa > sb) return 1;
  return 0;
}

/* ── Persistent sequence counter for SSTable naming ───────── */

static uint64_t db_persistent_seq(const char* db_path) {
  char seq_path[512];
  snprintf(seq_path, sizeof(seq_path), "%s.seq", db_path);
  
  uint64_t seq = 0;
  FILE* f = fopen(seq_path, "rb");
  if (f) {
    if (fread(&seq, sizeof(seq), 1, f) != 1) seq = 0;
    fclose(f);
  }
  
  seq++;
  f = fopen(seq_path, "wb");
  if (f) {
    fwrite(&seq, sizeof(seq), 1, f);
    fclose(f);
  }
  
  return seq;
}

/* ── Flush memtable to SSTable, truncate WAL ──────────────── */

static int db_flush_memtable(struct kanbudb_db* db) {
  if (!db) return KANBUDB_ERR_INVAL;

  int rc = lsm_flush(db->lsm);
  if (rc != KANBUDB_OK) return rc;

  /* Use a persistent monotonic sequence for SSTable naming */
  uint64_t seq = db_persistent_seq(db->path);

  char sst_path[512], tmp_path[512];
  snprintf(sst_path, sizeof(sst_path), "%s.sst.0.%llu",
           db->path, (unsigned long long)seq);
  snprintf(tmp_path, sizeof(tmp_path), "%s.sst.0.%llu.tmp",
           db->path, (unsigned long long)seq);

  sstable_writer_t* w = sstable_writer_create(tmp_path, sst_path, seq);
  if (!w) { lsm_destroy_flushing(db->lsm); return KANBUDB_ERR_OOM; }

  struct sstable_flush_ctx fctx;
  fctx.w = w;
  fctx.had_error = 0;

  rc = lsm_iterate_flushing(db->lsm, &flush_write_cb, &fctx);
  if (rc != KANBUDB_OK || fctx.had_error) {
    sstable_writer_destroy(w);
    lsm_destroy_flushing(db->lsm);
    return KANBUDB_ERR_IO;
  }

  rc = sstable_writer_finish(w);
  sstable_writer_destroy(w);
  if (rc != KANBUDB_OK) { lsm_destroy_flushing(db->lsm); return rc; }

  /* Load into B-tree */
  sstable_reader_t* sr = sstable_reader_open(sst_path);
  if (sr) {
    sstable_reader_scan(sr, &sstable_load_cb, db);
    sstable_reader_close(sr);
  }

  lsm_destroy_flushing(db->lsm);

  /* Truncate WAL — all flushed entries are now durable in SSTable+B-tree */
  wal_truncate(db->wal);

  return KANBUDB_OK;
}

static int find_table(struct kanbudb_db* db, const char* name) {
  for (int i = 0; i < db->num_tables; i++) {
    if (strcmp(db->tables[i].name, name) == 0)
      return i;
  }
  return -1;
}

/* ── db_open ───────────────────────────────────────────────── */

int db_open(const char* path, const db_config_t* config, db_t** out) {
  if (!path || !out) return KANBUDB_ERR_INVAL;

  struct kanbudb_db* db = (struct kanbudb_db*)calloc(1, sizeof(*db));
  if (!db) return KANBUDB_ERR_OOM;

  db->path = strdup(path);
  if (!db->path) { free(db); return KANBUDB_ERR_OOM; }

  db->config = config ? *config : default_config;

  char wal_path[512];
  snprintf(wal_path, sizeof(wal_path), "%s.wal", path);
  db->wal = wal_create(wal_path, db->config.fsync_mode);
  if (!db->wal) { free(db->path); free(db); return KANBUDB_ERR_IO; }

  db->lsm = lsm_create(path, db->config.memtable_size);
  if (!db->lsm) { wal_destroy(db->wal); free(db->path); free(db); return KANBUDB_ERR_OOM; }

  db->btree = btree_create();
  if (!db->btree) { lsm_destroy(db->lsm); wal_destroy(db->wal); free(db->path); free(db); return KANBUDB_ERR_OOM; }

  db->fts_index = fts_index_create();
  if (!db->fts_index) { btree_destroy(db->btree); lsm_destroy(db->lsm); wal_destroy(db->wal); free(db->path); free(db); return KANBUDB_ERR_OOM; }

  db->num_tables = 0;
  db->last_error = KANBUDB_OK;

  /* Phase 1: load system table (schema) */
  {
    char sys_path[512];
    snprintf(sys_path, sizeof(sys_path), "%s%s", path, KANBUDB_SYSTEM_TABLE_SUFFIX);
    sstable_reader_t* sr = sstable_reader_open(sys_path);
    if (sr) {
      schema_load_ctx_t lc;
      memset(&lc, 0, sizeof(lc));
      lc.db = db;
      sstable_reader_scan(sr, &schema_load_cb, &lc);
      sstable_reader_close(sr);
    }
  }

  /* Phase 2: load data SSTables into B-tree (sorted by seq) */
  {
    char* sst_paths[256];
    int num_sst = sstable_scan_dir(path, sst_paths, 256, NULL);

    /* Sort by sequence number so newer data overwrites older */
    if (num_sst > 1) {
      qsort(sst_paths, (size_t)num_sst, sizeof(char*), &sst_path_cmp);
    }

    (void)0; /* debug placeholder */

    for (int i = 0; i < num_sst; i++) {
      sstable_reader_t* sr = sstable_reader_open(sst_paths[i]);
      if (sr) {
        sstable_reader_scan(sr, &sstable_load_cb, db);
        sstable_reader_close(sr);
      }
      free(sst_paths[i]);
    }
  }

  /* Phase 3: [disabled] auto compaction — seq not yet unique across sessions */

  /* Phase 4: replay WAL for any unflushed writes */
  {
    wal_replay(db->wal, &wal_replay_cb, db);
  }

  *out = db;
  return KANBUDB_OK;
}

/* ── db_close ──────────────────────────────────────────────── */

int db_close(db_t* db) {
  if (!db) return KANBUDB_ERR_INVAL;
  struct kanbudb_db* internal = (struct kanbudb_db*)db;

  /* Flush any remaining data in memtable */
  if (lsm_has_data(internal->lsm)) {
    db_flush_memtable(internal);
  }

  /* Save schema to system table */
  if (internal->num_tables > 0) {
    db_save_schema(internal);
  }

  /* Take a checkpoint of the B-tree */
  db_checkpoint(internal);

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
    t->col_names[i] = strdup(col_names[i]);
    if (!t->col_names[i]) {
      for (int j = 0; j < i; j++) free(t->col_names[j]);
      free(t->col_names); free(t->col_types);
      internal->last_error = KANBUDB_ERR_OOM; return KANBUDB_ERR_OOM;
    }
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

  /* Persist schema immediately */
  db_save_schema(internal);

  internal->last_error = KANBUDB_OK;
  return KANBUDB_OK;
}

int db_put(db_t* db, const char* table, const char* key, size_t key_len,
           const void* value, size_t value_len) {
  if (!db || !table || !key) return KANBUDB_ERR_INVAL;

  struct kanbudb_db* internal = (struct kanbudb_db*)db;
  int idx = find_table(internal, table);
  if (idx < 0) { internal->last_error = KANBUDB_ERR_NOTFOUND; return KANBUDB_ERR_NOTFOUND; }

  int rc = wal_append(internal->wal, KANBUDB_WAL_PUT,
                       internal->tables[idx].id,
                       key, key_len, value, value_len);
  if (rc != KANBUDB_OK) { internal->last_error = rc; return rc; }

  rc = lsm_put(internal->lsm, internal->tables[idx].id,
                key, key_len, value, value_len);
  if (rc != KANBUDB_OK) { internal->last_error = rc; return rc; }

  if (lsm_is_full(internal->lsm)) {
    int frc = db_flush_memtable(internal);
    if (frc != KANBUDB_OK) { internal->last_error = frc; return frc; }
  }

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

  int rc = wal_append(internal->wal, KANBUDB_WAL_DELETE,
                       internal->tables[idx].id,
                       key, key_len, NULL, 0);
  if (rc != KANBUDB_OK) { internal->last_error = rc; return rc; }

  rc = lsm_delete(internal->lsm, internal->tables[idx].id, key, key_len);
  if (rc != KANBUDB_OK) { internal->last_error = rc; return rc; }

  if (lsm_is_full(internal->lsm)) {
    int frc = db_flush_memtable(internal);
    if (frc != KANBUDB_OK) { internal->last_error = frc; return frc; }
  }

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
      free(rs->doc_ids); free(rs->scores); free(rs);
      internal->last_error = KANBUDB_ERR_OOM; return KANBUDB_ERR_OOM;
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
