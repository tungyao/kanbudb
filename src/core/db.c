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
#include "fts/ranker.h"
#include "vector.h"
#include "embedding.h"
#include "quantize_internal.h"
#include "vec_filter.h"
#include <unistd.h>

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
  1,
  0,    /* multi_process */
  10    /* reader_poll_ms */
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
    if (sscanf(p, "|%d:%255[^|]%n", &col_type, col_name, &consumed) < 2) break;
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

/* ── Background compaction worker ────────────────────────────── */

static void* compact_worker(void* arg) {
  struct kanbudb_db* db = (struct kanbudb_db*)arg;
  while (db->compact_running) {
    /* Sleep 5 seconds between compaction checks */
    for (int i = 0; i < 50 && db->compact_running; i++) {
      usleep(100000); /* 100ms ticks, check running flag */
    }
    if (!db->compact_running) break;

    pthread_rwlock_wrlock(&db->table_lock);
    pthread_rwlock_wrlock(&db->lsm_lock);
    pthread_rwlock_wrlock(&db->btree_lock);
    
    kanbudb_level_t levels[KANBUDB_MAX_LEVELS];
    compaction_scan_levels(db->path, levels, KANBUDB_MAX_LEVELS);
    
    int src_level = compaction_pick_level(levels, KANBUDB_MAX_LEVELS);
    if (src_level >= 0 && levels[src_level].num_files > 0) {
      int dst_level = src_level + 1;
      uint64_t out_seq = lsm_next_seq(db->lsm);
      
      char out_path[512];
      compaction_make_path(db->path, dst_level, out_seq, out_path, sizeof(out_path));
      
      int rc = compaction_merge_to_level(
          (const char**)levels[src_level].file_paths,
          levels[src_level].num_files,
          out_path, out_seq);
      
      if (rc == KANBUDB_OK) {
        /* Delete old files */
        for (int i = 0; i < levels[src_level].num_files; i++) {
          unlink(levels[src_level].file_paths[i]);
        }
        /* Load merged result into B-tree */
        sstable_reader_t* sr = sstable_reader_open(out_path);
        if (sr) {
          sstable_reader_scan(sr, &sstable_load_cb, db);
          sstable_reader_close(sr);
        }
      }
    }
    
    compaction_free_levels(levels, KANBUDB_MAX_LEVELS);
    pthread_rwlock_unlock(&db->btree_lock);
    pthread_rwlock_unlock(&db->lsm_lock);
    pthread_rwlock_unlock(&db->table_lock);
  }
  return NULL;
}

/* ── Reader refresh: apply mmap'd WAL entries to local B-tree ── */

int db_reader_refresh(db_t* db) {
  if (!db) return KANBUDB_ERR_INVAL;
  struct kanbudb_db* internal = (struct kanbudb_db*)db;

  /* If WAL not opened yet, try to open it */
  if (!internal->wal_mmap.region.addr) {
    char wal_path_mmap[512];
    snprintf(wal_path_mmap, sizeof(wal_path_mmap), "%s.wal.mmap", internal->path);
    size_t wal_data_cap = 64 * 1024 * 1024;  /* 64MB */
    int mrc = wal_mmap_open(wal_path_mmap, 0, wal_data_cap, &internal->wal_mmap);
    if (mrc < 0) return KANBUDB_ERR_IO;
  }

  /* Re-read schema from system SSTable if not loaded yet */
  if (internal->num_tables == 0) {
    char sys_path[512];
    snprintf(sys_path, sizeof(sys_path), "%s%s", internal->path, KANBUDB_SYSTEM_TABLE_SUFFIX);
    sstable_reader_t* sr = sstable_reader_open(sys_path);
    if (sr) {
      schema_load_ctx_t lc;
      memset(&lc, 0, sizeof(lc));
      lc.db = internal;
      sstable_reader_scan(sr, &schema_load_cb, &lc);
      sstable_reader_close(sr);
    }
  }

  /* Read all available entries and apply to local B-tree */
  pthread_rwlock_wrlock(&internal->btree_lock);
  for (;;) {
    uint64_t seq;
    int op;
    uint64_t table_id;
    void* key; size_t key_len;
    void* value; size_t val_len;

    int rc = wal_mmap_read_entry(&internal->wal_mmap, &seq, &op, &table_id,
                                  &key, &key_len, &value, &val_len);
    if (rc != KANBUDB_OK) break;  /* no more entries or error */

    if (op == KANBUDB_WAL_PUT) {
      btree_put(internal->btree, key, key_len, value, val_len);
    } else {
      btree_delete(internal->btree, key, key_len);
    }
  }
  pthread_rwlock_unlock(&internal->btree_lock);
  return KANBUDB_OK;
}

static void* reader_poll_worker(void* arg) {
  struct kanbudb_db* db = (struct kanbudb_db*)arg;
  int poll_ms = db->config.reader_poll_ms;
  if (poll_ms <= 0) poll_ms = 10;

  while (db->reader_poll_running) {
    usleep((useconds_t)poll_ms * 1000);
    if (!db->reader_poll_running) break;
    db_reader_refresh((db_t*)db);
  }
  return NULL;
}

/* ── db_open_reader ──────────────────────────────────────── */

int db_open_reader(const char* path, const db_config_t* config, db_t** out) {
  if (!path || !out) return KANBUDB_ERR_INVAL;

  struct kanbudb_db* db = (struct kanbudb_db*)calloc(1, sizeof(*db));
  if (!db) return KANBUDB_ERR_OOM;

  db->path = strdup(path);
  if (!db->path) { free(db); return KANBUDB_ERR_OOM; }

  db->config = config ? *config : default_config;
  db->is_reader = 1;

  db->btree = btree_create();
  if (!db->btree) { free(db->path); free(db); return KANBUDB_ERR_OOM; }

  pthread_rwlock_init(&db->table_lock, NULL);
  pthread_rwlock_init(&db->lsm_lock, NULL);
  pthread_rwlock_init(&db->btree_lock, NULL);

  db->num_tables = 0;
  db->last_error = KANBUDB_OK;

  /* Load schema from .system SSTable */
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

  /* Load data SSTables into B-tree (leveled order) */
  {
    kanbudb_level_t levels[KANBUDB_MAX_LEVELS];
    compaction_scan_levels(path, levels, KANBUDB_MAX_LEVELS);
    for (int lvl = 0; lvl < KANBUDB_MAX_LEVELS; lvl++) {
      for (int i = 0; i < levels[lvl].num_files; i++) {
        sstable_reader_t* sr = sstable_reader_open(levels[lvl].file_paths[i]);
        if (sr) {
          sstable_reader_scan(sr, &sstable_load_cb, db);
          sstable_reader_close(sr);
        }
      }
    }
    compaction_free_levels(levels, KANBUDB_MAX_LEVELS);
  }

  /* Join shared metadata, increment reader_count */
  kanbudb_shared_meta_reader_join(path, &db->shared_meta);

  /* Open WAL with mmap (read-only) */
  {
    char wal_path_mmap[512];
    snprintf(wal_path_mmap, sizeof(wal_path_mmap), "%s.wal.mmap", path);
    size_t wal_data_cap = 64 * 1024 * 1024;  /* 64MB */
    int mrc = wal_mmap_open(wal_path_mmap, 0, wal_data_cap, &db->wal_mmap);
    if (mrc < 0) {
      /* Non-fatal: reader can still read SSTables */
    }
  }

  /* Start poll thread */
  db->reader_poll_running = 1;
  pthread_create(&db->reader_poll_thread, NULL, reader_poll_worker, db);

  /* Do initial refresh */
  db_reader_refresh((db_t*)db);

  *out = db;
  return KANBUDB_OK;
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

  pthread_rwlock_init(&db->table_lock, NULL);
  pthread_mutex_init(&db->wal_lock, NULL);
  pthread_rwlock_init(&db->lsm_lock, NULL);
  pthread_rwlock_init(&db->btree_lock, NULL);

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

  /* Phase 2: load data SSTables into B-tree (leveled order) */
  {
    kanbudb_level_t levels[KANBUDB_MAX_LEVELS];
    compaction_scan_levels(path, levels, KANBUDB_MAX_LEVELS);

    /* Load from L0 → L1 → ... → L7 so higher-level (more compacted) data wins */
    for (int lvl = 0; lvl < KANBUDB_MAX_LEVELS; lvl++) {
      for (int i = 0; i < levels[lvl].num_files; i++) {
        sstable_reader_t* sr = sstable_reader_open(levels[lvl].file_paths[i]);
        if (sr) {
          sstable_reader_scan(sr, &sstable_load_cb, db);
          sstable_reader_close(sr);
        }
      }
    }
    compaction_free_levels(levels, KANBUDB_MAX_LEVELS);
  }

  /* Phase 3: start background compaction thread */
  {
    db->compact_running = 1;
    db->compact_trigger = 0;
    pthread_create(&db->compact_thread, NULL, compact_worker, db);
  }

  /* Phase 4: replay WAL for any unflushed writes */
  {
    wal_replay(db->wal, &wal_replay_cb, db);
  }

  /* Phase 5: multi-process WAL mmap initialization */
  if (db->config.multi_process) {
    kanbudb_shared_meta_open(path, &db->shared_meta);
    char wal_path_mmap[512];
    snprintf(wal_path_mmap, sizeof(wal_path_mmap), "%s.wal.mmap", path);
    size_t wal_data_cap = 64 * 1024 * 1024;  /* 64MB */
    int mrc = wal_mmap_open(wal_path_mmap, 1, wal_data_cap, &db->wal_mmap);
    if (mrc < 0) {
      db->config.multi_process = 0;
    }
  }

  *out = db;
  return KANBUDB_OK;
}

/* ── db_close ──────────────────────────────────────────────── */

int db_close(db_t* db) {
  if (!db) return KANBUDB_ERR_INVAL;
  struct kanbudb_db* internal = (struct kanbudb_db*)db;

  /* Reader cleanup: skip writer-specific teardown */
  if (internal->is_reader) {
    internal->reader_poll_running = 0;
    pthread_join(internal->reader_poll_thread, NULL);
    kanbudb_shared_meta_reader_leave(internal->path, &internal->shared_meta);
    wal_mmap_close(&internal->wal_mmap);
    pthread_rwlock_destroy(&internal->table_lock);
    pthread_rwlock_destroy(&internal->lsm_lock);
    pthread_rwlock_destroy(&internal->btree_lock);
    btree_destroy(internal->btree);
    for (int i = 0; i < internal->num_tables; i++) {
      for (int j = 0; j < internal->tables[i].num_cols; j++) {
        free(internal->tables[i].col_names[j]);
      }
      free(internal->tables[i].col_types);
      free(internal->tables[i].col_names);
    }
    free(internal->path);
    free(internal);
    return KANBUDB_OK;
  }

  /* Acquire all locks in order */
  pthread_rwlock_wrlock(&internal->table_lock);
  pthread_mutex_lock(&internal->wal_lock);
  pthread_rwlock_wrlock(&internal->lsm_lock);
  pthread_rwlock_wrlock(&internal->btree_lock);

  /* Stop background compaction */
  internal->compact_running = 0;
  pthread_join(internal->compact_thread, NULL);

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
  if (internal->vec_index) kanbudb_vec_close(internal->vec_index);
  if (internal->embed) kanbudb_embed_destroy(internal->embed);
  if (internal->quantizer) kanbudb_quant_destroy(internal->quantizer);
  free(internal->quant_vectors);
  free(internal->quant_ids);
  btree_destroy(internal->btree);
  lsm_destroy(internal->lsm);
  wal_destroy(internal->wal);
  if (internal->wal_mmap.region.addr) {
    wal_mmap_close(&internal->wal_mmap);
  }
  free(internal->path);

  pthread_rwlock_unlock(&internal->table_lock);
  pthread_mutex_unlock(&internal->wal_lock);
  pthread_rwlock_unlock(&internal->lsm_lock);
  pthread_rwlock_unlock(&internal->btree_lock);
  pthread_rwlock_destroy(&internal->table_lock);
  pthread_mutex_destroy(&internal->wal_lock);
  pthread_rwlock_destroy(&internal->lsm_lock);
  pthread_rwlock_destroy(&internal->btree_lock);
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
  pthread_rwlock_wrlock(&internal->table_lock);

  if (find_table(internal, table_name) >= 0) {
    internal->last_error = KANBUDB_ERR_EXISTS;
    pthread_rwlock_unlock(&internal->table_lock);
    return KANBUDB_ERR_EXISTS;
  }

  if (internal->num_tables >= KANBUDB_MAX_TABLES) {
    internal->last_error = KANBUDB_ERR_OOM;
    pthread_rwlock_unlock(&internal->table_lock);
    return KANBUDB_ERR_OOM;
  }

  kanbudb_table_t* t = &internal->tables[internal->num_tables];

  size_t nlen = strlen(table_name);
  if (nlen >= sizeof(t->name)) nlen = sizeof(t->name) - 1;
  memcpy(t->name, table_name, nlen);
  t->name[nlen] = '\0';
  t->num_cols = num_columns;

  t->col_types = (kanbudb_col_type_t*)malloc((size_t)num_columns * sizeof(kanbudb_col_type_t));
  if (!t->col_types) { internal->last_error = KANBUDB_ERR_OOM; pthread_rwlock_unlock(&internal->table_lock); return KANBUDB_ERR_OOM; }
  memcpy(t->col_types, col_types, (size_t)num_columns * sizeof(kanbudb_col_type_t));

  t->col_names = (char**)malloc((size_t)num_columns * sizeof(char*));
  if (!t->col_names) { free(t->col_types); internal->last_error = KANBUDB_ERR_OOM; pthread_rwlock_unlock(&internal->table_lock); return KANBUDB_ERR_OOM; }

  for (int i = 0; i < num_columns; i++) {
    t->col_names[i] = strdup(col_names[i]);
    if (!t->col_names[i]) {
      for (int j = 0; j < i; j++) free(t->col_names[j]);
      free(t->col_names); free(t->col_types);
      internal->last_error = KANBUDB_ERR_OOM; pthread_rwlock_unlock(&internal->table_lock); return KANBUDB_ERR_OOM;
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
  pthread_rwlock_unlock(&internal->table_lock);
  return KANBUDB_OK;
}

int db_put(db_t* db, const char* table, const char* key, size_t key_len,
           const void* value, size_t value_len) {
  if (!db || !table || !key) return KANBUDB_ERR_INVAL;

  struct kanbudb_db* internal = (struct kanbudb_db*)db;
  pthread_rwlock_rdlock(&internal->table_lock);

  int idx = find_table(internal, table);
  if (idx < 0) {
    internal->last_error = KANBUDB_ERR_NOTFOUND;
    pthread_rwlock_unlock(&internal->table_lock);
    return KANBUDB_ERR_NOTFOUND;
  }
  uint64_t tbl_id = internal->tables[idx].id;
  pthread_rwlock_unlock(&internal->table_lock);

  pthread_mutex_lock(&internal->wal_lock);
  int rc = wal_append(internal->wal, KANBUDB_WAL_PUT,
                       tbl_id,
                       key, key_len, value, value_len);
  pthread_mutex_unlock(&internal->wal_lock);
  if (rc != KANBUDB_OK) {
    internal->last_error = rc;
    return rc;
  }

  /* In multi_process mode, also append to mmap'd WAL for readers */
  if (internal->config.multi_process && internal->wal_mmap.region.addr) {
    wal_mmap_append(&internal->wal_mmap, wal_last_seq(internal->wal),
                    0 /* PUT */, tbl_id,
                    key, key_len, value, value_len);
    wal_mmap_sync(&internal->wal_mmap);

    /* Check if WAL is getting full and needs a switch */
    uint64_t write_pos = wal_mmap_get_write_pos(&internal->wal_mmap);
    size_t data_used = (size_t)(write_pos - KANBUDB_WAL_MMAP_HEADER_SIZE);
    size_t data_cap = internal->wal_mmap.data_cap;

    if (data_cap > 0 && data_used > data_cap * 80 / 100) {
      char wal_path_switch[512];
      snprintf(wal_path_switch, sizeof(wal_path_switch), "%s.wal.mmap", internal->path);
      wal_mmap_switch_file(wal_path_switch, &internal->wal_mmap);
    }
  }

  pthread_rwlock_wrlock(&internal->lsm_lock);
  rc = lsm_put(internal->lsm, tbl_id,
                key, key_len, value, value_len);
  if (rc != KANBUDB_OK) {
    pthread_rwlock_unlock(&internal->lsm_lock);
    internal->last_error = rc;
    return rc;
  }

  int need_flush = lsm_is_full(internal->lsm);
  if (need_flush) {
    /* Upgrade to btree lock for flush, then flush */
    pthread_rwlock_wrlock(&internal->btree_lock);
    int frc = db_flush_memtable(internal);
    pthread_rwlock_unlock(&internal->btree_lock);
    pthread_rwlock_unlock(&internal->lsm_lock);
    if (frc != KANBUDB_OK) {
      internal->last_error = frc;
      return frc;
    }
  } else {
    pthread_rwlock_unlock(&internal->lsm_lock);
  }

  internal->last_error = KANBUDB_OK;
  return KANBUDB_OK;
}

int db_get(db_t* db, const char* table, const char* key, size_t key_len,
           void** value, size_t* value_len) {
  if (!db || !table || !key || !value || !value_len) return KANBUDB_ERR_INVAL;

  struct kanbudb_db* internal = (struct kanbudb_db*)db;
  pthread_rwlock_rdlock(&internal->table_lock);

  int idx = find_table(internal, table);
  if (idx < 0) {
    internal->last_error = KANBUDB_ERR_NOTFOUND;
    pthread_rwlock_unlock(&internal->table_lock);
    return KANBUDB_ERR_NOTFOUND;
  }
  uint64_t tbl_id = internal->tables[idx].id;
  pthread_rwlock_unlock(&internal->table_lock);

  if (internal->lsm) {
    pthread_rwlock_rdlock(&internal->lsm_lock);
    int lsm_rc = lsm_get(internal->lsm, tbl_id,
                      key, key_len, value, value_len);
    pthread_rwlock_unlock(&internal->lsm_lock);
    if (lsm_rc == KANBUDB_OK) {
      internal->last_error = KANBUDB_OK;
      return KANBUDB_OK;
    }
  }

  pthread_rwlock_rdlock(&internal->btree_lock);
  int rc = btree_get(internal->btree, key, key_len, value, value_len);
  pthread_rwlock_unlock(&internal->btree_lock);
  if (rc == KANBUDB_OK) {
    internal->last_error = KANBUDB_OK;
    return KANBUDB_OK;
  }

  internal->last_error = KANBUDB_ERR_NOTFOUND;
  return KANBUDB_ERR_NOTFOUND;
}

int db_delete(db_t* db, const char* table, const char* key, size_t key_len) {
  if (!db || !table || !key) return KANBUDB_ERR_INVAL;

  struct kanbudb_db* internal = (struct kanbudb_db*)db;
  pthread_rwlock_rdlock(&internal->table_lock);

  int idx = find_table(internal, table);
  if (idx < 0) {
    internal->last_error = KANBUDB_ERR_NOTFOUND;
    pthread_rwlock_unlock(&internal->table_lock);
    return KANBUDB_ERR_NOTFOUND;
  }
  uint64_t tbl_id = internal->tables[idx].id;
  pthread_rwlock_unlock(&internal->table_lock);

  pthread_mutex_lock(&internal->wal_lock);
  int rc = wal_append(internal->wal, KANBUDB_WAL_DELETE,
                       tbl_id,
                       key, key_len, NULL, 0);
  pthread_mutex_unlock(&internal->wal_lock);
  if (rc != KANBUDB_OK) {
    internal->last_error = rc;
    return rc;
  }

  pthread_rwlock_wrlock(&internal->lsm_lock);
  rc = lsm_delete(internal->lsm, tbl_id, key, key_len);
  if (rc != KANBUDB_OK) {
    pthread_rwlock_unlock(&internal->lsm_lock);
    internal->last_error = rc;
    return rc;
  }

  int need_flush = lsm_is_full(internal->lsm);
  if (need_flush) {
    pthread_rwlock_wrlock(&internal->btree_lock);
    int frc = db_flush_memtable(internal);
    pthread_rwlock_unlock(&internal->btree_lock);
    pthread_rwlock_unlock(&internal->lsm_lock);
    if (frc != KANBUDB_OK) {
      internal->last_error = frc;
      return frc;
    }
  } else {
    pthread_rwlock_unlock(&internal->lsm_lock);
  }

  internal->last_error = KANBUDB_OK;
  return KANBUDB_OK;
}

int db_fts_create_index(db_t* db, const char* table, const char* column,
                        const fts_options_t* opts) {
  (void)opts;
  if (!db || !table || !column) return KANBUDB_ERR_INVAL;
  struct kanbudb_db* internal = (struct kanbudb_db*)db;
  pthread_rwlock_wrlock(&internal->table_lock);
  if (find_table(internal, table) < 0) {
    internal->last_error = KANBUDB_ERR_NOTFOUND;
    pthread_rwlock_unlock(&internal->table_lock);
    return KANBUDB_ERR_NOTFOUND;
  }
  internal->last_error = KANBUDB_OK;
  pthread_rwlock_unlock(&internal->table_lock);
  return KANBUDB_OK;
}

int db_fts_drop_index(db_t* db, const char* table, const char* column) {
  if (!db || !table || !column) return KANBUDB_ERR_INVAL;
  struct kanbudb_db* internal = (struct kanbudb_db*)db;
  pthread_rwlock_wrlock(&internal->table_lock);
  internal->last_error = KANBUDB_OK;
  pthread_rwlock_unlock(&internal->table_lock);
  return KANBUDB_OK;
}

int db_fts_search(db_t* db, const char* table, const char* column,
                  const char* query, result_set_t** out) {
  if (!db || !table || !column || !query || !out) return KANBUDB_ERR_INVAL;
  struct kanbudb_db* internal = (struct kanbudb_db*)db;
  pthread_rwlock_rdlock(&internal->table_lock);

  if (find_table(internal, table) < 0) {
    internal->last_error = KANBUDB_ERR_NOTFOUND;
    pthread_rwlock_unlock(&internal->table_lock);
    return KANBUDB_ERR_NOTFOUND;
  }

  if (!internal->fts_index) {
    internal->last_error = KANBUDB_ERR_INVAL;
    pthread_rwlock_unlock(&internal->table_lock);
    return KANBUDB_ERR_INVAL;
  }

  fts_query_node_t nodes[64];
  int num_nodes = fts_query_parse(query, nodes, 64);
  if (num_nodes <= 0) {
    internal->last_error = KANBUDB_ERR_INVAL;
    pthread_rwlock_unlock(&internal->table_lock);
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
  if (!rs) { internal->last_error = KANBUDB_ERR_OOM; pthread_rwlock_unlock(&internal->table_lock); return KANBUDB_ERR_OOM; }

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
      internal->last_error = KANBUDB_ERR_OOM; pthread_rwlock_unlock(&internal->table_lock); return KANBUDB_ERR_OOM;
    }
    memcpy(rs->doc_ids, doc_id_buf, (size_t)num_doc_ids * sizeof(uint64_t));
  } else {
    rs->doc_ids = NULL;
    rs->scores = NULL;
  }

  rs->current = -1;
  *out = rs;
  internal->last_error = KANBUDB_OK;
  pthread_rwlock_unlock(&internal->table_lock);
  return KANBUDB_OK;
}

/* ── Vector index + built-in embedding ─────────────────────── */

int db_vec_create_index(db_t* db, const db_vec_options_t* opts)
{
    if (!db || !opts || opts->dimension == 0) return KANBUDB_ERR_INVAL;
    struct kanbudb_db* internal = (struct kanbudb_db*)db;
    pthread_rwlock_wrlock(&internal->table_lock);

    if (internal->vec_index) {
        kanbudb_vec_close(internal->vec_index);
        internal->vec_index = NULL;
    }
    if (internal->embed) {
        kanbudb_embed_destroy(internal->embed);
        internal->embed = NULL;
    }

    kanbudb_vec_params_t vparams = KANBUDB_VEC_PARAMS_DEFAULT;
    vparams.dimension = opts->dimension;
    if (opts->enable_hnsw) {
        vparams.algo = KANBUDB_VEC_ALGO_HNSW;
        vparams.M = opts->hnsw_m;
        vparams.ef_construction = opts->hnsw_ef_construction;
    }

    char vec_path[512];
    snprintf(vec_path, sizeof(vec_path), "%s.vec", internal->path);
    int rc = kanbudb_vec_create(vec_path, &vparams, &internal->vec_index);
    if (rc != KANBUDB_VEC_OK) {
        pthread_rwlock_unlock(&internal->table_lock);
        return rc;
    }

    rc = kanbudb_embed_create(opts->dimension,
                              opts->ngram_size ? opts->ngram_size : 3,
                              &internal->embed);

    internal->last_error = KANBUDB_OK;
    pthread_rwlock_unlock(&internal->table_lock);
    return rc;
}

int db_vec_destroy_index(db_t* db)
{
    if (!db) return KANBUDB_ERR_INVAL;
    struct kanbudb_db* internal = (struct kanbudb_db*)db;
    pthread_rwlock_wrlock(&internal->table_lock);

    if (internal->vec_index) {
        kanbudb_vec_close(internal->vec_index);
        internal->vec_index = NULL;
    }
    if (internal->embed) {
        kanbudb_embed_destroy(internal->embed);
        internal->embed = NULL;
    }

    internal->last_error = KANBUDB_OK;
    pthread_rwlock_unlock(&internal->table_lock);
    return KANBUDB_OK;
}

int db_vec_insert_text(db_t* db, uint64_t id,
                       const char* text, size_t text_len)
{
    if (!db || !text) return KANBUDB_ERR_INVAL;
    struct kanbudb_db* internal = (struct kanbudb_db*)db;
    pthread_rwlock_rdlock(&internal->table_lock);
    if (!internal->vec_index || !internal->embed) {
        pthread_rwlock_unlock(&internal->table_lock);
        return KANBUDB_ERR_INVAL;
    }
    uint32_t dim = kanbudb_embed_dimensions(internal->embed);
    float* vec = (float*)malloc((size_t)dim * sizeof(float));
    if (!vec) { pthread_rwlock_unlock(&internal->table_lock); return KANBUDB_ERR_OOM; }
    int rc = kanbudb_embed_text(internal->embed, text, text_len, vec);
    if (rc == 0)
        rc = kanbudb_vec_insert(internal->vec_index, id, vec);
    free(vec);
    pthread_rwlock_unlock(&internal->table_lock);
    return rc;
}

int db_vec_insert_batch(db_t* db, uint32_t count,
                        const uint64_t* ids,
                        const char** texts, const size_t* text_lens)
{
    if (!db || !ids || !texts || !text_lens || count == 0)
        return KANBUDB_ERR_INVAL;
    struct kanbudb_db* internal = (struct kanbudb_db*)db;
    pthread_rwlock_rdlock(&internal->table_lock);
    if (!internal->vec_index || !internal->embed) {
        pthread_rwlock_unlock(&internal->table_lock);
        return KANBUDB_ERR_INVAL;
    }
    uint32_t dim = kanbudb_embed_dimensions(internal->embed);
    float* vecs = (float*)malloc((size_t)count * dim * sizeof(float));
    if (!vecs) { pthread_rwlock_unlock(&internal->table_lock); return KANBUDB_ERR_OOM; }
    int rc = kanbudb_embed_batch(internal->embed, texts, text_lens, count, vecs);
    if (rc == 0)
        rc = kanbudb_vec_insert_batch(internal->vec_index, count, ids, vecs);
    free(vecs);
    pthread_rwlock_unlock(&internal->table_lock);
    return rc;
}

int db_vec_insert_vector(db_t* db, uint64_t id, const float* vector)
{
    if (!db || !vector) return KANBUDB_ERR_INVAL;
    struct kanbudb_db* internal = (struct kanbudb_db*)db;
    pthread_rwlock_rdlock(&internal->table_lock);
    if (!internal->vec_index) {
        pthread_rwlock_unlock(&internal->table_lock);
        return KANBUDB_ERR_INVAL;
    }
    int rc = kanbudb_vec_insert(internal->vec_index, id, vector);
    pthread_rwlock_unlock(&internal->table_lock);
    return rc;
}

int db_vec_search_text(db_t* db, const char* text, size_t text_len,
                       uint32_t k, kanbudb_vec_result_t* results)
{
    if (!db || !text || !results || k == 0) return KANBUDB_ERR_INVAL;
    struct kanbudb_db* internal = (struct kanbudb_db*)db;
    pthread_rwlock_rdlock(&internal->table_lock);
    if (!internal->vec_index || !internal->embed) {
        pthread_rwlock_unlock(&internal->table_lock);
        return KANBUDB_ERR_INVAL;
    }
    uint32_t dim = kanbudb_embed_dimensions(internal->embed);
    float* qvec = (float*)malloc((size_t)dim * sizeof(float));
    if (!qvec) { pthread_rwlock_unlock(&internal->table_lock); return KANBUDB_ERR_OOM; }
    int rc = kanbudb_embed_text(internal->embed, text, text_len, qvec);
    if (rc == 0)
        rc = kanbudb_vec_search(internal->vec_index, qvec, k, results);
    free(qvec);
    pthread_rwlock_unlock(&internal->table_lock);
    return rc;
}

int db_vec_search(db_t* db, const float* query,
                  uint32_t k, kanbudb_vec_result_t* results)
{
    if (!db || !query || !results || k == 0) return KANBUDB_ERR_INVAL;
    struct kanbudb_db* internal = (struct kanbudb_db*)db;
    pthread_rwlock_rdlock(&internal->table_lock);
    if (!internal->vec_index) {
        pthread_rwlock_unlock(&internal->table_lock);
        return KANBUDB_ERR_INVAL;
    }
    int rc = kanbudb_vec_search(internal->vec_index, query, k, results);
    pthread_rwlock_unlock(&internal->table_lock);
    return rc;
}

int db_vec_delete(db_t* db, uint64_t id)
{
    if (!db) return KANBUDB_ERR_INVAL;
    struct kanbudb_db* internal = (struct kanbudb_db*)db;
    pthread_rwlock_rdlock(&internal->table_lock);
    if (!internal->vec_index) {
        pthread_rwlock_unlock(&internal->table_lock);
        return KANBUDB_ERR_INVAL;
    }
    int rc = kanbudb_vec_delete(internal->vec_index, id);
    pthread_rwlock_unlock(&internal->table_lock);
    return rc;
}

int db_vec_count(db_t* db)
{
    if (!db) return 0;
    struct kanbudb_db* internal = (struct kanbudb_db*)db;
    pthread_rwlock_rdlock(&internal->table_lock);
    int c = internal->vec_index ? kanbudb_vec_count(internal->vec_index) : 0;
    pthread_rwlock_unlock(&internal->table_lock);
    return c;
}

int db_vec_flush(db_t* db)
{
    if (!db) return KANBUDB_ERR_INVAL;
    struct kanbudb_db* internal = (struct kanbudb_db*)db;
    pthread_rwlock_rdlock(&internal->table_lock);
    if (!internal->vec_index) {
        pthread_rwlock_unlock(&internal->table_lock);
        return KANBUDB_ERR_INVAL;
    }
    int rc = kanbudb_vec_flush(internal->vec_index);
    pthread_rwlock_unlock(&internal->table_lock);
    return rc;
}

int db_vec_set_embed(db_t* db, kanbudb_embed_t* embed)
{
    if (!db || !embed) return KANBUDB_ERR_INVAL;
    struct kanbudb_db* internal = (struct kanbudb_db*)db;
    pthread_rwlock_wrlock(&internal->table_lock);
    if (internal->embed) kanbudb_embed_destroy(internal->embed);
    internal->embed = embed;
    internal->last_error = KANBUDB_OK;
    pthread_rwlock_unlock(&internal->table_lock);
    return KANBUDB_OK;
}

/* ── Filtered vector search ─────────────────────────────────── */

int db_vec_search_filtered(db_t* db, const float* query, uint32_t k,
                           db_vec_filter_fn filter, void* filter_ctx,
                           kanbudb_vec_result_t* results)
{
    if (!db || !query || !results || k == 0) return KANBUDB_ERR_INVAL;
    struct kanbudb_db* internal = (struct kanbudb_db*)db;
    pthread_rwlock_rdlock(&internal->table_lock);
    if (!internal->vec_index) {
        pthread_rwlock_unlock(&internal->table_lock);
        return KANBUDB_ERR_INVAL;
    }
    int rc = kanbudb_vec_search_filtered(internal->vec_index, query, k,
                                         filter, filter_ctx, results);
    pthread_rwlock_unlock(&internal->table_lock);
    return rc;
}

int db_vec_search_text_filtered(db_t* db, const char* text, size_t text_len,
                                uint32_t k, db_vec_filter_fn filter,
                                void* filter_ctx,
                                kanbudb_vec_result_t* results)
{
    if (!db || !text || !results || k == 0) return KANBUDB_ERR_INVAL;
    struct kanbudb_db* internal = (struct kanbudb_db*)db;
    pthread_rwlock_rdlock(&internal->table_lock);
    if (!internal->vec_index || !internal->embed) {
        pthread_rwlock_unlock(&internal->table_lock);
        return KANBUDB_ERR_INVAL;
    }
    uint32_t dim = kanbudb_embed_dimensions(internal->embed);
    float* qvec = (float*)malloc((size_t)dim * sizeof(float));
    if (!qvec) { pthread_rwlock_unlock(&internal->table_lock); return KANBUDB_ERR_OOM; }
    int rc = kanbudb_embed_text(internal->embed, text, text_len, qvec);
    if (rc == 0)
        rc = kanbudb_vec_search_filtered(internal->vec_index, qvec, k,
                                         filter, filter_ctx, results);
    free(qvec);
    pthread_rwlock_unlock(&internal->table_lock);
    return rc;
}

/* ── Hybrid search (vector + FTS fusion) ────────────────────── */

int db_hybrid_search(db_t* db, const char* table, const char* column,
                     const char* fts_query, const float* vec_query,
                     const kanbudb_hybrid_params_t* params,
                     kanbudb_hybrid_result_t* results, int max_results)
{
    if (!db || !table || !column || !fts_query || !vec_query ||
        !params || !results || max_results <= 0)
        return KANBUDB_ERR_INVAL;

    struct kanbudb_db* internal = (struct kanbudb_db*)db;
    pthread_rwlock_rdlock(&internal->table_lock);

    if (!internal->vec_index || !internal->fts_index) {
        pthread_rwlock_unlock(&internal->table_lock);
        return KANBUDB_ERR_INVAL;
    }

    /* 1. Vector search */
    uint32_t vec_k = params->k > 0 ? params->k : 10;
    kanbudb_vec_result_t* vec_results = NULL;
    int vec_count = 0;
    if (vec_k > 0) {
        vec_results = (kanbudb_vec_result_t*)malloc(vec_k * sizeof(kanbudb_vec_result_t));
        if (vec_results) {
            vec_count = kanbudb_vec_search(internal->vec_index, vec_query,
                                           vec_k, vec_results);
            if (vec_count < 0) vec_count = 0;
        }
    }

    /* 2. FTS search */
    fts_query_node_t nodes[64];
    int num_nodes = fts_query_parse(fts_query, nodes, 64);

    uint64_t fts_ids_buf[1024];
    double fts_scores_buf[1024];
    int fts_count = 0;

    for (int i = 0; i < num_nodes && fts_count < 1024; i++) {
        if (nodes[i].op == FTS_TERM || nodes[i].op == FTS_FUZZY) {
            uint64_t doc_results[256];
            int n;
            if (nodes[i].op == FTS_FUZZY) {
                int max_edits = nodes[i].fuzzy_distance > 0 ? nodes[i].fuzzy_distance : 1;
                n = fts_index_search_fuzzy(internal->fts_index, nodes[i].text,
                                           max_edits, doc_results, 256);
            } else {
                n = fts_index_search(internal->fts_index, nodes[i].text,
                                     doc_results, 256);
            }
            for (int j = 0; j < n && fts_count < 1024; j++) {
                int already = 0;
                for (int m = 0; m < fts_count; m++) {
                    if (fts_ids_buf[m] == doc_results[j]) { already = 1; break; }
                }
                if (!already) {
                    fts_ids_buf[fts_count] = doc_results[j];
                    fts_scores_buf[fts_count] = bm25_score(1.0, 10.0, 10.0,
                        (double)internal->fts_index->total_docs,
                        1.0, 1.2, 0.75);
                    fts_count++;
                }
            }
        }
    }

    /* 3. Merge results with RRF */
    kanbudb_hybrid_result_t* merged = NULL;
    int merged_count = 0;

    if (vec_count > 0 || fts_count > 0) {
        merged = (kanbudb_hybrid_result_t*)malloc(
            (size_t)(vec_count + fts_count) * sizeof(kanbudb_hybrid_result_t));
        if (!merged) {
            free(vec_results);
            pthread_rwlock_unlock(&internal->table_lock);
            return KANBUDB_ERR_OOM;
        }

        const double k = 60.0;
        /* Add vector results */
        for (int i = 0; i < vec_count; i++) {
            merged[merged_count].id = vec_results[i].id;
            merged[merged_count].vec_distance = (double)vec_results[i].distance;
            merged[merged_count].fts_score = 0.0;
            merged[merged_count].score = params->vec_weight * (1.0 / (k + (double)(i + 1)));
            merged_count++;
        }

        /* Add FTS results */
        for (int i = 0; i < fts_count; i++) {
            int found = -1;
            for (int j = 0; j < merged_count; j++) {
                if (merged[j].id == fts_ids_buf[i]) { found = j; break; }
            }
            if (found >= 0) {
                /* Boost existing entry */
                merged[found].fts_score = fts_scores_buf[i];
                merged[found].score += params->fts_weight * (1.0 / (k + (double)(i + 1)));
            } else {
                merged[merged_count].id = fts_ids_buf[i];
                merged[merged_count].vec_distance = 0.0;
                merged[merged_count].fts_score = fts_scores_buf[i];
                merged[merged_count].score = params->fts_weight * (1.0 / (k + (double)(i + 1)));
                merged_count++;
            }
        }
    }

    /* Sort by score descending */
    /* Simple selection sort for small result sets */
    for (int i = 0; i < merged_count - 1; i++) {
        int best = i;
        for (int j = i + 1; j < merged_count; j++) {
            if (merged[j].score > merged[best].score) best = j;
        }
        if (best != i) {
            kanbudb_hybrid_result_t tmp = merged[i];
            merged[i] = merged[best];
            merged[best] = tmp;
        }
    }

    /* Output top results */
    int out_count = merged_count < max_results ? merged_count : max_results;
    for (int i = 0; i < out_count; i++) {
        results[i] = merged[i];
    }

    free(merged);
    free(vec_results);
    pthread_rwlock_unlock(&internal->table_lock);
    return out_count;
}

/* ── Vector quantization ────────────────────────────────────── */

int db_vec_quant_create(db_t* db, const kanbudb_quant_params_t* params)
{
    if (!db || !params) return KANBUDB_ERR_INVAL;
    struct kanbudb_db* internal = (struct kanbudb_db*)db;
    pthread_rwlock_wrlock(&internal->table_lock);

    if (internal->quantizer) kanbudb_quant_destroy(internal->quantizer);
    internal->quantizer = NULL;

    kanbudb_quant_params_t qparams = *params;
    if (qparams.dimension == 0 && internal->vec_index)
        qparams.dimension = (uint32_t)kanbudb_vec_dimension(internal->vec_index);

    int rc = kanbudb_quant_create(&qparams, &internal->quantizer);
    if (rc != 0) {
        pthread_rwlock_unlock(&internal->table_lock);
        return KANBUDB_ERR_OOM;
    }

    internal->quant_count = 0;
    internal->quant_capacity = 4096;
    internal->quant_vectors = (float*)calloc(internal->quant_capacity,
        qparams.dimension * sizeof(float));
    internal->quant_ids = (uint64_t*)calloc(internal->quant_capacity, sizeof(uint64_t));

    internal->last_error = KANBUDB_OK;
    pthread_rwlock_unlock(&internal->table_lock);
    return KANBUDB_OK;
}

void db_vec_quant_destroy(db_t* db)
{
    if (!db) return;
    struct kanbudb_db* internal = (struct kanbudb_db*)db;
    pthread_rwlock_wrlock(&internal->table_lock);
    if (internal->quantizer) {
        kanbudb_quant_destroy(internal->quantizer);
        internal->quantizer = NULL;
    }
    free(internal->quant_vectors);
    free(internal->quant_ids);
    internal->quant_vectors = NULL;
    internal->quant_ids = NULL;
    internal->quant_count = 0;
    internal->quant_capacity = 0;
    pthread_rwlock_unlock(&internal->table_lock);
}

int db_vec_quant_train(db_t* db, const float* vectors, uint32_t count)
{
    if (!db || !vectors || count == 0) return KANBUDB_ERR_INVAL;
    struct kanbudb_db* internal = (struct kanbudb_db*)db;
    pthread_rwlock_wrlock(&internal->table_lock);
    if (!internal->quantizer) {
        pthread_rwlock_unlock(&internal->table_lock);
        return KANBUDB_ERR_INVAL;
    }
    int rc = kanbudb_quant_train(internal->quantizer, vectors, count);
    pthread_rwlock_unlock(&internal->table_lock);
    return rc == 0 ? KANBUDB_OK : KANBUDB_ERR_IO;
}

int db_vec_quant_insert(db_t* db, uint64_t id, const float* vector)
{
    if (!db || !vector) return KANBUDB_ERR_INVAL;
    struct kanbudb_db* internal = (struct kanbudb_db*)db;
    pthread_rwlock_wrlock(&internal->table_lock);
    if (!internal->quantizer) {
        pthread_rwlock_unlock(&internal->table_lock);
        return KANBUDB_ERR_INVAL;
    }

    uint32_t dim = internal->quantizer->params.dimension;
    if (internal->quant_count >= internal->quant_capacity) {
        uint32_t new_cap = internal->quant_capacity * 2;
        float* new_vecs = (float*)realloc(internal->quant_vectors,
            (size_t)new_cap * dim * sizeof(float));
        uint64_t* new_ids = (uint64_t*)realloc(internal->quant_ids,
            (size_t)new_cap * sizeof(uint64_t));
        if (!new_vecs || !new_ids) {
            free(new_vecs); free(new_ids);
            pthread_rwlock_unlock(&internal->table_lock);
            return KANBUDB_ERR_OOM;
        }
        internal->quant_vectors = new_vecs;
        internal->quant_ids = new_ids;
        internal->quant_capacity = new_cap;
    }

    memcpy(internal->quant_vectors + (size_t)internal->quant_count * dim,
           vector, dim * sizeof(float));
    internal->quant_ids[internal->quant_count] = id;
    internal->quant_count++;

    /* Also insert into the raw vector index if available */
    if (internal->vec_index) {
        kanbudb_vec_insert(internal->vec_index, id, vector);
    }

    pthread_rwlock_unlock(&internal->table_lock);
    return KANBUDB_OK;
}

int db_vec_quant_search(db_t* db, const float* query, uint32_t k,
                        kanbudb_vec_result_t* results)
{
    if (!db || !query || !results || k == 0) return KANBUDB_ERR_INVAL;
    struct kanbudb_db* internal = (struct kanbudb_db*)db;
    pthread_rwlock_rdlock(&internal->table_lock);
    if (!internal->quantizer) {
        pthread_rwlock_unlock(&internal->table_lock);
        return KANBUDB_ERR_INVAL;
    }

    uint32_t dim = internal->quantizer->params.dimension;
    uint32_t code_size = kanbudb_quant_code_size(internal->quantizer);

    /* Encode query */
    uint8_t* qcode = (uint8_t*)malloc(code_size);
    if (!qcode) { pthread_rwlock_unlock(&internal->table_lock); return KANBUDB_ERR_OOM; }
    kanbudb_quant_encode(internal->quantizer, query, qcode, NULL);

    /* Scan all quantized vectors, compute approximate distances */
    uint32_t n = internal->quant_count;
    if (k > n) k = n;
    if (n == 0) { free(qcode); pthread_rwlock_unlock(&internal->table_lock); return 0; }

    /* Simple scan with approximate distance */
    kanbudb_vec_result_t* tmp = (kanbudb_vec_result_t*)malloc(n * sizeof(kanbudb_vec_result_t));
    if (!tmp) { free(qcode); pthread_rwlock_unlock(&internal->table_lock); return KANBUDB_ERR_OOM; }

    for (uint32_t i = 0; i < n; i++) {
        uint8_t* vcode = (uint8_t*)malloc(code_size);
        if (!vcode) continue;
        kanbudb_quant_encode(internal->quantizer,
                             internal->quant_vectors + (size_t)i * dim,
                             vcode, NULL);
        tmp[i].id = internal->quant_ids[i];
        tmp[i].distance = kanbudb_quant_distance(internal->quantizer,
                                                  qcode, code_size,
                                                  vcode, code_size);
        free(vcode);
    }

    /* Partial sort for top-K */
    for (uint32_t i = 0; i < k; i++) {
        uint32_t best = i;
        for (uint32_t j = i + 1; j < n; j++) {
            if (tmp[j].distance < tmp[best].distance) best = j;
        }
        if (best != i) {
            kanbudb_vec_result_t t = tmp[i]; tmp[i] = tmp[best]; tmp[best] = t;
        }
    }

    for (uint32_t i = 0; i < k; i++) results[i] = tmp[i];

    free(tmp);
    free(qcode);
    pthread_rwlock_unlock(&internal->table_lock);
    return (int)k;
}

int db_vec_quant_decode(db_t* db, uint64_t id, float* out_vector)
{
    if (!db || !out_vector) return KANBUDB_ERR_INVAL;
    struct kanbudb_db* internal = (struct kanbudb_db*)db;
    pthread_rwlock_rdlock(&internal->table_lock);
    if (!internal->quantizer) {
        pthread_rwlock_unlock(&internal->table_lock);
        return KANBUDB_ERR_INVAL;
    }

    /* Find the ID */
    int idx = -1;
    for (uint32_t i = 0; i < internal->quant_count; i++) {
        if (internal->quant_ids[i] == id) { idx = (int)i; break; }
    }
    if (idx < 0) {
        pthread_rwlock_unlock(&internal->table_lock);
        return KANBUDB_ERR_NOTFOUND;
    }

    uint32_t dim = internal->quantizer->params.dimension;
    uint32_t code_size = kanbudb_quant_code_size(internal->quantizer);

    uint8_t* code = (uint8_t*)malloc(code_size);
    if (!code) { pthread_rwlock_unlock(&internal->table_lock); return KANBUDB_ERR_OOM; }

    kanbudb_quant_encode(internal->quantizer,
                         internal->quant_vectors + (size_t)idx * dim,
                         code, NULL);
    kanbudb_quant_decode(internal->quantizer, code, code_size, out_vector);

    free(code);
    pthread_rwlock_unlock(&internal->table_lock);
    return KANBUDB_OK;
}
