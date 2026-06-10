#include "query_builder.h"
#include "db.h"
#include "macros.h"
#include "core/db.h"
#include "lsm.h"
#include "btree.h"
#include "row_format.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct query_builder_t {
  struct kanbudb_db* db;
  char     table[64];
  char     filter_column[64];
  char     filter_op[16];
  char     filter_value[256];
  size_t   filter_value_len;
  int      has_filter;
  char     sort_column[64];
  int      sort_ascending;
  int      has_sort;
  int      limit;
  int      has_limit;
  char     join_table[64];
  char     join_on_local[64];
  char     join_on_foreign[64];
  int      has_join;
};



query_builder_t* db_query(db_t* db, const char* table) {
  if (!db || !table) return NULL;

  struct query_builder_t* qb;
  qb = (struct query_builder_t*)calloc(1, sizeof(*qb));
  if (!qb) return NULL;

  qb->db = (struct kanbudb_db*)db;

  size_t n = strlen(table);
  if (n >= sizeof(qb->table)) n = sizeof(qb->table) - 1;
  memcpy(qb->table, table, n);
  qb->table[n] = '\0';

  qb->limit = 0;
  qb->has_filter = 0;
  qb->has_sort = 0;
  qb->has_limit = 0;
  qb->has_join = 0;

  return qb;
}

int qb_from(query_builder_t* qb, const char* table) {
  if (!qb || !table) return KANBUDB_ERR_INVAL;

  size_t n = strlen(table);
  if (n >= sizeof(qb->table)) n = sizeof(qb->table) - 1;
  memcpy(qb->table, table, n);
  qb->table[n] = '\0';

  return KANBUDB_OK;
}

int qb_filter(query_builder_t* qb, const char* column,
              const char* op, const void* value) {
  if (!qb || !column || !op || !value) return KANBUDB_ERR_INVAL;

  size_t n = strlen(column);
  if (n >= sizeof(qb->filter_column)) n = sizeof(qb->filter_column) - 1;
  memcpy(qb->filter_column, column, n);
  qb->filter_column[n] = '\0';

  n = strlen(op);
  if (n >= sizeof(qb->filter_op)) n = sizeof(qb->filter_op) - 1;
  memcpy(qb->filter_op, op, n);
  qb->filter_op[n] = '\0';

  /* Store value as string for v1 simplicity */
  snprintf(qb->filter_value, sizeof(qb->filter_value), "%s", (const char*)value);
  qb->filter_value_len = strlen(qb->filter_value);
  qb->has_filter = 1;

  return KANBUDB_OK;
}

int qb_sort(query_builder_t* qb, const char* column, int ascending) {
  if (!qb || !column) return KANBUDB_ERR_INVAL;

  size_t n = strlen(column);
  if (n >= sizeof(qb->sort_column)) n = sizeof(qb->sort_column) - 1;
  memcpy(qb->sort_column, column, n);
  qb->sort_column[n] = '\0';

  qb->sort_ascending = ascending;
  qb->has_sort = 1;

  return KANBUDB_OK;
}

int qb_limit(query_builder_t* qb, int limit) {
  if (!qb || limit < 0) return KANBUDB_ERR_INVAL;

  qb->limit = limit;
  qb->has_limit = 1;

  return KANBUDB_OK;
}

int qb_join(query_builder_t* qb, const char* table,
            const char* on_local, const char* on_foreign) {
  if (!qb || !table || !on_local || !on_foreign) return KANBUDB_ERR_INVAL;

  size_t n = strlen(table);
  if (n >= sizeof(qb->join_table)) n = sizeof(qb->join_table) - 1;
  memcpy(qb->join_table, table, n);
  qb->join_table[n] = '\0';

  n = strlen(on_local);
  if (n >= sizeof(qb->join_on_local)) n = sizeof(qb->join_on_local) - 1;
  memcpy(qb->join_on_local, on_local, n);
  qb->join_on_local[n] = '\0';

  n = strlen(on_foreign);
  if (n >= sizeof(qb->join_on_foreign)) n = sizeof(qb->join_on_foreign) - 1;
  memcpy(qb->join_on_foreign, on_foreign, n);
  qb->join_on_foreign[n] = '\0';

  qb->has_join = 1;

  return KANBUDB_OK;
}

static i64 parse_int(const char* s) {
  return (i64)atoll(s);
}
static f64 parse_double(const char* s) {
  return atof(s);
}

int filter_match(const void* col_data, i32 col_len,
                  kanbudb_col_type_t type, const char* op, const char* literal) {
  if (!col_data || !op || !literal) return 0;

  int cmp = 0;
  switch (type) {
    case KANBUDB_INT32:
    case KANBUDB_INT64: {
      i64 col_val = (type == KANBUDB_INT32)
        ? (i64)*(const i32*)col_data
        : *(const i64*)col_data;
      i64 lit_val = parse_int(literal);
      if (col_val < lit_val) cmp = -1;
      else if (col_val > lit_val) cmp = 1;
      else cmp = 0;
      break;
    }
    case KANBUDB_FLOAT:
    case KANBUDB_DOUBLE: {
      f64 col_val = (type == KANBUDB_FLOAT)
        ? (f64)*(const f32*)col_data
        : *(const f64*)col_data;
      f64 lit_val = parse_double(literal);
      if (col_val < lit_val) cmp = -1;
      else if (col_val > lit_val) cmp = 1;
      else cmp = 0;
      break;
    }
    case KANBUDB_BOOL: {
      int col_val = *(const u8*)col_data ? 1 : 0;
      int lit_val = (strcmp(literal, "true") == 0 || strcmp(literal, "1") == 0) ? 1 : 0;
      if (col_val < lit_val) cmp = -1;
      else if (col_val > lit_val) cmp = 1;
      else cmp = 0;
      break;
    }
    case KANBUDB_STRING: {
      size_t lit_len = strlen(literal);
      size_t min_len = (size_t)col_len < lit_len ? (size_t)col_len : lit_len;
      cmp = memcmp(col_data, literal, min_len);
      if (cmp == 0) {
        if ((size_t)col_len < lit_len) cmp = -1;
        else if ((size_t)col_len > lit_len) cmp = 1;
      }
      break;
    }
    default:
      return 0;
  }

  if (strcmp(op, "=") == 0) return cmp == 0;
  if (strcmp(op, "!=") == 0) return cmp != 0;
  if (strcmp(op, ">") == 0) return cmp > 0;
  if (strcmp(op, "<") == 0) return cmp < 0;
  if (strcmp(op, ">=") == 0) return cmp >= 0;
  if (strcmp(op, "<=") == 0) return cmp <= 0;
  return 0;
}

/* Simple hash set for dedup keys from LSM */
typedef struct {
  const void** keys;
  size_t*      key_lens;
  int          count;
  int          capacity;
} key_set_t;

static void key_set_init(key_set_t* ks) {
  ks->keys = NULL;
  ks->key_lens = NULL;
  ks->count = 0;
  ks->capacity = 0;
}

static int key_set_add(key_set_t* ks, const void* key, size_t key_len) {
  if (ks->count >= ks->capacity) {
    int new_cap = ks->capacity ? ks->capacity * 2 : 64;
    const void** new_keys = (const void**)realloc(ks->keys, (size_t)new_cap * sizeof(void*));
    if (!new_keys) return KANBUDB_ERR_OOM;
    size_t* new_lens = (size_t*)realloc(ks->key_lens, (size_t)new_cap * sizeof(size_t));
    if (!new_lens) return KANBUDB_ERR_OOM;
    ks->keys = new_keys;
    ks->key_lens = new_lens;
    ks->capacity = new_cap;
  }
  ks->keys[ks->count] = key;
  ks->key_lens[ks->count] = key_len;
  ks->count++;
  return KANBUDB_OK;
}

static int key_set_contains(key_set_t* ks, const void* key, size_t key_len) {
  for (int i = 0; i < ks->count; i++) {
    if (ks->key_lens[i] == key_len && memcmp(ks->keys[i], key, key_len) == 0)
      return 1;
  }
  return 0;
}

static void key_set_destroy(key_set_t* ks) {
  free(ks->keys);
  free(ks->key_lens);
}

typedef struct {
  key_set_t*         seen_keys;
  query_builder_t*   qb;
  row_schema_t*      row_schema;
  int                filter_col;
  kanbudb_col_type_t filter_type;
  result_set_t*      rs;
} lsm_scan_ctx_t;

static int lsm_scan_cb(const lsm_entry_t* entry, void* ctx) {
  lsm_scan_ctx_t* c = (lsm_scan_ctx_t*)ctx;
  if (entry->deleted) return 0;

  key_set_add(c->seen_keys, entry->key, entry->key_len);

  int pass = 1;
  if (c->qb->has_filter && c->filter_col >= 0) {
    i32 col_len;
    const void* col_data = row_extract_column(c->row_schema, c->filter_col,
                                                entry->value, (i32)entry->val_len, &col_len);
    if (col_data) {
      pass = filter_match(col_data, col_len, c->filter_type,
                           c->qb->filter_op, c->qb->filter_value);
    }
  }
  if (pass) {
    rs_add_row(c->rs, entry->value, entry->val_len);
  }
  return 0;
}

typedef struct {
  int  idx;
} sort_entry_t;

/* Sort context for qsort comparator */
typedef struct {
  const void**     row_data;
  const size_t*    row_lens;
  row_schema_t     schema;
  int              col;
  kanbudb_col_type_t type;
  int              ascending;
} sort_ctx_t;

static sort_ctx_t sort_ctx;

static int sort_entry_cmp(const void* a, const void* b) {
  const sort_entry_t* ea = (const sort_entry_t*)a;
  const sort_entry_t* eb = (const sort_entry_t*)b;
  i32 len_a, len_b;
  const void* da = row_extract_column(&sort_ctx.schema, sort_ctx.col,
                                       sort_ctx.row_data[ea->idx],
                                       (i32)sort_ctx.row_lens[ea->idx], &len_a);
  const void* db = row_extract_column(&sort_ctx.schema, sort_ctx.col,
                                       sort_ctx.row_data[eb->idx],
                                       (i32)sort_ctx.row_lens[eb->idx], &len_b);
  int cmp = 0;
  if (sort_ctx.type == KANBUDB_STRING) {
    size_t min = (size_t)(len_a < len_b ? len_a : len_b);
    cmp = memcmp(da, db, min);
    if (cmp == 0) cmp = (len_a < len_b) ? -1 : (len_a > len_b) ? 1 : 0;
  } else if (sort_ctx.type == KANBUDB_INT32 || sort_ctx.type == KANBUDB_INT64 || sort_ctx.type == KANBUDB_BOOL) {
    i64 va = sort_ctx.type == KANBUDB_INT32 ? *(const i32*)da : sort_ctx.type == KANBUDB_INT64 ? *(const i64*)da : *(const u8*)da;
    i64 vb = sort_ctx.type == KANBUDB_INT32 ? *(const i32*)db : sort_ctx.type == KANBUDB_INT64 ? *(const i64*)db : *(const u8*)db;
    if (va < vb) cmp = -1; else if (va > vb) cmp = 1;
  } else {
    f64 va = sort_ctx.type == KANBUDB_FLOAT ? *(const f32*)da : *(const f64*)da;
    f64 vb = sort_ctx.type == KANBUDB_FLOAT ? *(const f32*)db : *(const f64*)db;
    if (va < vb) cmp = -1; else if (va > vb) cmp = 1;
  }
  return sort_ctx.ascending ? cmp : -cmp;
}

result_set_t* qb_exec(query_builder_t* qb) {
  if (!qb) return NULL;

  result_set_t* rs = (result_set_t*)calloc(1, sizeof(*rs));
  if (!rs) return NULL;

  rs->num_rows = 0;
  rs->current = -1;
  rs->num_cols = 0;

  /* Look up the table to populate column metadata */
  struct kanbudb_db* internal = qb->db;
  for (int i = 0; i < internal->num_tables; i++) {
    if (strcmp(internal->tables[i].name, qb->table) == 0) {
      kanbudb_table_t* t = &internal->tables[i];
      rs->num_cols = t->num_cols;
      for (int j = 0; j < t->num_cols && j < KANBUDB_MAX_COLS; j++) {
        rs->col_types[j] = t->col_types[j];
        rs->col_names[j] = t->col_names[j];
      }
      break;
    }
  }

  /* Precompute row schema for column extraction */
  row_schema_t row_schema;
  row_schema_init(&row_schema, rs->col_types, rs->num_cols);

  /* Determine filter column index */
  int filter_col = -1;
  kanbudb_col_type_t filter_type = KANBUDB_INT32;
  if (qb->has_filter) {
    for (int i = 0; i < rs->num_cols; i++) {
      if (strcmp(rs->col_names[i], qb->filter_column) == 0) {
        filter_col = i;
        filter_type = rs->col_types[i];
        break;
      }
    }
  }

  /* ---- LSM scan first (collect seen keys + matching rows) ---- */
  key_set_t seen_keys;
  key_set_init(&seen_keys);

  if (internal->lsm) {
    lsm_scan_ctx_t ctx;
    ctx.seen_keys = &seen_keys;
    ctx.qb = qb;
    ctx.row_schema = &row_schema;
    ctx.filter_col = filter_col;
    ctx.filter_type = filter_type;
    ctx.rs = rs;

    kanbudb_memtable_t* active = lsm_get_active(internal->lsm);
    kanbudb_memtable_t* flushing = lsm_get_flushing(internal->lsm);
    if (active) memtable_iterate(active, lsm_scan_cb, &ctx);
    if (flushing) memtable_iterate(flushing, lsm_scan_cb, &ctx);
  }

  /* ---- B-tree scan (skip keys already seen in LSM) ---- */
  if (internal->btree) {
    btree_cursor_t* cur = btree_cursor_create(internal->btree);
    if (cur) {
      if (btree_cursor_seek(cur, NULL, 0) == KANBUDB_OK) {
        btree_kv_t kv;
        while (btree_cursor_next(cur, &kv) == KANBUDB_OK) {
          if (key_set_contains(&seen_keys, kv.key, kv.key_len))
            continue;
          int pass = 1;
          if (qb->has_filter && filter_col >= 0) {
            i32 col_len;
            const void* col_data = row_extract_column(&row_schema, filter_col,
                                                        kv.value, (i32)kv.val_len, &col_len);
            if (col_data) {
              pass = filter_match(col_data, col_len, filter_type,
                                   qb->filter_op, qb->filter_value);
            }
          }
          if (pass) {
            rs_add_row(rs, kv.value, kv.val_len);
          }
        }
      }
      btree_cursor_destroy(cur);
    }
  }

  key_set_destroy(&seen_keys);

  /* ---- Apply sort ---- */
  if (qb->has_sort && rs->row_count > 1) {
    int sort_col = -1;
    for (int i = 0; i < rs->num_cols; i++) {
      if (strcmp(rs->col_names[i], qb->sort_column) == 0) {
        sort_col = i;
        break;
      }
    }
    if (sort_col >= 0) {
      sort_ctx.row_data = (const void**)rs->row_data;
      sort_ctx.row_lens = rs->row_lens;
      row_schema_init(&sort_ctx.schema, rs->col_types, rs->num_cols);
      sort_ctx.col = sort_col;
      sort_ctx.type = rs->col_types[sort_col];
      sort_ctx.ascending = qb->sort_ascending;

      sort_entry_t* entries = (sort_entry_t*)malloc((size_t)rs->row_count * sizeof(sort_entry_t));
      if (entries) {
        for (int i = 0; i < rs->row_count; i++) {
          entries[i].idx = i;
        }
        qsort(entries, (size_t)rs->row_count, sizeof(sort_entry_t), sort_entry_cmp);

        void** sorted_data = (void**)malloc((size_t)rs->row_count * sizeof(void*));
        size_t* sorted_lens = (size_t*)malloc((size_t)rs->row_count * sizeof(size_t));
        if (sorted_data && sorted_lens) {
          for (int i = 0; i < rs->row_count; i++) {
            sorted_data[i] = rs->row_data[entries[i].idx];
            sorted_lens[i] = rs->row_lens[entries[i].idx];
          }
          free(rs->row_data);
          free(rs->row_lens);
          rs->row_data = sorted_data;
          rs->row_lens = sorted_lens;
        } else {
          free(sorted_data);
          free(sorted_lens);
        }
        free(entries);
      }
    }
  }

  /* ---- Apply limit ---- */
  if (qb->has_limit && qb->limit < rs->row_count) {
    for (int i = qb->limit; i < rs->row_count; i++) {
      free(rs->row_data[i]);
    }
    rs->row_count = qb->limit;
  }

  /* ---- Apply join ---- */
  if (qb->has_join && rs->row_count > 0) {
    int local_col = -1;
    for (int i = 0; i < rs->num_cols; i++) {
      if (strcmp(rs->col_names[i], qb->join_on_local) == 0) {
        local_col = i;
        break;
      }
    }
    if (local_col >= 0) {
      int foreign_table_idx = -1;
      for (int i = 0; i < internal->num_tables; i++) {
        if (strcmp(internal->tables[i].name, qb->join_table) == 0) {
          foreign_table_idx = i;
          break;
        }
      }
      if (foreign_table_idx >= 0) {
        kanbudb_table_t* ft = &internal->tables[foreign_table_idx];
        int foreign_col = -1;
        for (int i = 0; i < ft->num_cols; i++) {
          if (strcmp(ft->col_names[i], qb->join_on_foreign) == 0) {
            foreign_col = i;
            break;
          }
        }
        if (foreign_col >= 0) {
          int new_num_cols = rs->num_cols + ft->num_cols;
          kanbudb_col_type_t new_types[KANBUDB_MAX_COLS];
          char* new_names[KANBUDB_MAX_COLS];
          int nidx = 0;
          for (int i = 0; i < rs->num_cols; i++) {
            new_types[nidx] = rs->col_types[i];
            new_names[nidx] = rs->col_names[i];
            nidx++;
          }
          for (int i = 0; i < ft->num_cols; i++) {
            if (i == foreign_col) continue;
            new_types[nidx] = ft->col_types[i];
            new_names[nidx] = ft->col_names[i];
            nidx++;
          }
          new_num_cols = nidx;

          row_schema_t ft_schema;
          row_schema_init(&ft_schema, ft->col_types, ft->num_cols);

          for (int i = 0; i < rs->row_count; i++) {
            i32 join_len;
            const void* join_data = row_extract_column(&row_schema, local_col,
                                                        rs->row_data[i], (i32)rs->row_lens[i], &join_len);
            if (!join_data) continue;

            char key_str[256];
            int key_len = 0;
            kanbudb_col_type_t ltype = rs->col_types[local_col];
            if (ltype == KANBUDB_INT32) {
              key_len = snprintf(key_str, sizeof(key_str), "%d", *(const i32*)join_data);
            } else if (ltype == KANBUDB_INT64) {
              key_len = snprintf(key_str, sizeof(key_str), "%lld", (long long)*(const i64*)join_data);
            } else {
              memcpy(key_str, join_data, (size_t)join_len);
              key_len = join_len;
            }
            key_str[key_len] = '\0';

            void* fval = NULL;
            size_t flen = 0;
            int rc = db_get((db_t*)internal, qb->join_table,
                             key_str, (size_t)key_len + 1, &fval, &flen);
            if (rc != KANBUDB_OK || !fval) continue;

            i32 combined_size = (i32)rs->row_lens[i];
            for (int j = 0; j < ft->num_cols; j++) {
              if (j == foreign_col) continue;
              i32 fclen;
              const void* fc = row_extract_column(&ft_schema, j, fval, (i32)flen, &fclen);
              if (fc) {
                if (ft->col_types[j] == KANBUDB_STRING || ft->col_types[j] == KANBUDB_BLOB) {
                  combined_size += 4 + fclen;
                } else {
                  combined_size += fclen;
                }
              }
            }

            u8* combined = (u8*)malloc((size_t)combined_size);
            if (!combined) continue;

            memcpy(combined, rs->row_data[i], rs->row_lens[i]);
            i32 off = (i32)rs->row_lens[i];

            for (int j = 0; j < ft->num_cols; j++) {
              if (j == foreign_col) continue;
              i32 fclen;
              const void* fc = row_extract_column(&ft_schema, j, fval, (i32)flen, &fclen);
              if (fc) {
                if (ft->col_types[j] == KANBUDB_STRING || ft->col_types[j] == KANBUDB_BLOB) {
                  u32 slen = (u32)fclen;
                  memcpy(combined + off, &slen, 4);
                  off += 4;
                  memcpy(combined + off, fc, (size_t)fclen);
                  off += fclen;
                } else {
                  memcpy(combined + off, fc, (size_t)fclen);
                  off += fclen;
                }
              }
            }

            free(rs->row_data[i]);
            rs->row_data[i] = combined;
            rs->row_lens[i] = (size_t)combined_size;
          }

          rs->num_cols = new_num_cols;
          for (int i = 0; i < new_num_cols; i++) {
            rs->col_types[i] = new_types[i];
            rs->col_names[i] = new_names[i];
          }
        }
      }
    }
  }

  /* Set result set row count */
  rs->num_rows = rs->row_count;

  return rs;
}

void qb_destroy(query_builder_t* qb) {
  free(qb);
}

int rs_add_row(result_set_t* rs, const void* data, size_t len) {
  if (!rs) return KANBUDB_ERR_INVAL;
  if (rs->row_count >= rs->row_capacity) {
    int new_cap = rs->row_capacity ? rs->row_capacity * 2 : 64;
    void** new_data = (void**)realloc(rs->row_data, (size_t)new_cap * sizeof(void*));
    if (!new_data) return KANBUDB_ERR_OOM;
    size_t* new_lens = (size_t*)realloc(rs->row_lens, (size_t)new_cap * sizeof(size_t));
    if (!new_lens) return KANBUDB_ERR_OOM;
    rs->row_data = new_data;
    rs->row_lens = new_lens;
    rs->row_capacity = new_cap;
  }
  void* copy = malloc(len);
  if (!copy) return KANBUDB_ERR_OOM;
  memcpy(copy, data, len);
  rs->row_data[rs->row_count] = copy;
  rs->row_lens[rs->row_count] = len;
  rs->row_count++;
  return KANBUDB_OK;
}

const void* rs_get_row_column(result_set_t* rs, int row_idx, int col, size_t* out_len) {
  if (!rs || row_idx < 0 || row_idx >= rs->row_count || col < 0 || col >= rs->num_cols) {
    if (out_len) *out_len = 0;
    return NULL;
  }
  row_schema_t schema;
  row_schema_init(&schema, rs->col_types, rs->num_cols);
  i32 len;
  const void* ptr = row_extract_column(&schema, col, rs->row_data[row_idx],
                                         (i32)rs->row_lens[row_idx], &len);
  if (out_len) *out_len = (size_t)len;
  return ptr;
}

int rs_next(result_set_t* rs) {
  if (!rs) return 0;

  rs->current++;
  if (rs->current >= rs->num_rows) {
    rs->current = rs->num_rows;
    return 0;
  }

  return 1;
}

int rs_get_column(result_set_t* rs, int col, void** data, size_t* len) {
  if (!rs || !data || !len) return KANBUDB_ERR_INVAL;
  if (col < 0 || col >= rs->num_cols) return KANBUDB_ERR_INVAL;

  if (rs->current < 0 || rs->current >= rs->num_rows) {
    *data = NULL;
    *len = 0;
    return KANBUDB_OK;
  }

  if (rs->is_fts) {
    if (col == 0) {
      *data = (void*)&rs->doc_ids[rs->current];
      *len = sizeof(uint64_t);
    } else if (col == 1) {
      *data = (void*)&rs->scores[rs->current];
      *len = sizeof(double);
    } else {
      *data = NULL;
      *len = 0;
    }
    return KANBUDB_OK;
  }

  size_t col_len;
  const void* col_data = rs_get_row_column(rs, rs->current, col, &col_len);
  if (col_data) {
    *data = (void*)col_data;
    *len = col_len;
  } else {
    *data = NULL;
    *len = 0;
  }
  return KANBUDB_OK;
}

kanbudb_col_type_t rs_get_column_type(result_set_t* rs, int col) {
  if (!rs || col < 0 || col >= rs->num_cols) return KANBUDB_INT32;

  return rs->col_types[col];
}

int rs_num_columns(result_set_t* rs) {
  if (!rs) return 0;

  return rs->num_cols;
}

void rs_close(result_set_t* rs) {
  if (!rs) return;
  if (rs->is_fts) {
    free(rs->doc_ids);
    free(rs->scores);
  }
  for (int i = 0; i < rs->row_count; i++) {
    free(rs->row_data[i]);
  }
  free(rs->row_data);
  free(rs->row_lens);
  free(rs);
}
