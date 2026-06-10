# MK+Tree Range Query Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement range query, sort, limit, and join in the query builder, replacing the current v1 stub.

**Architecture:** Compact binary row encoding based on table schema. B-tree cursor + LSM memtable iterate feed a merge pipeline with predicate pushdown. Sort uses qsort (or heap for top-N). Join uses nested-loop via db_get.

**Tech Stack:** C99, existing CMake build system, existing test framework (assert-based C tests)

**Spec:** `docs/superpowers/specs/2026-06-10-range-query-design.md`

---

### Task 1: Row binary format (row_format.h/c)

**Files:**
- Create: `src/query/row_format.h`
- Create: `src/query/row_format.c`
- Modify: `CMakeLists.txt` (add row_format.c to KANBUDB_SOURCES)

- [ ] **Step 1: Write row_format.h**

```c
#ifndef KANBUDB_ROW_FORMAT_H
#define KANBUDB_ROW_FORMAT_H

#include "macros.h"
#include "db.h"

/* Precomputed column offset info for a table schema.
 * For fixed-width columns (INT32/INT64/FLOAT/DOUBLE/BOOL), offset is constant.
 * For variable-width columns (STRING/BLOB), offset_base is the offset before
 * the 4-byte length prefix; actual data starts at offset_base + 4. */
typedef struct {
  int   num_cols;
  i32   fixed_offsets[KANBUDB_MAX_COLS];  /* -1 for variable-width cols */
  i32   fixed_sizes[KANBUDB_MAX_COLS];    /* type size (4/8/1) or 0 for var */
} row_schema_t;

/* Compute column offsets from a table schema. */
void row_schema_init(row_schema_t* rs, const kanbudb_col_type_t* types, int num_cols);

/* Extract a pointer to column col's data within a row value.
 * For fixed-width types: returns pointer, *out_len set to type size.
 * For STRING/BLOB: returns pointer to data bytes, *out_len set to length.
 * Returns NULL on error. */
const void* row_extract_column(const row_schema_t* rs, int col,
                                const void* row_data, i32 row_len, i32* out_len);

/* Encode a column value into the row buffer at the position indicated by row_schema.
 * buf must have enough space (row_fixed_size + sum of string/blob lengths + 4 per string/blob).
 * Returns number of bytes written, or -1 on error. */
i32 row_encode_column(u8* buf, i32 offset, kanbudb_col_type_t type,
                       const void* data, i32 len);

/* Total encoded size for a row given column types and individual column lengths.
 * Call this to allocate buffer before encoding. */
i32 row_encoded_size(const kanbudb_col_type_t* types, int num_cols,
                      const i32* col_lens);

#endif
```

- [ ] **Step 2: Write row_format.c**

```c
#include "row_format.h"
#include <string.h>

void row_schema_init(row_schema_t* rs, const kanbudb_col_type_t* types, int num_cols) {
  rs->num_cols = num_cols;
  i32 offset = 0;
  for (int i = 0; i < num_cols && i < KANBUDB_MAX_COLS; i++) {
    switch (types[i]) {
      case KANBUDB_INT32:  rs->fixed_offsets[i] = offset; rs->fixed_sizes[i] = 4;  offset += 4; break;
      case KANBUDB_INT64:  rs->fixed_offsets[i] = offset; rs->fixed_sizes[i] = 8;  offset += 8; break;
      case KANBUDB_FLOAT:  rs->fixed_offsets[i] = offset; rs->fixed_sizes[i] = 4;  offset += 4; break;
      case KANBUDB_DOUBLE: rs->fixed_offsets[i] = offset; rs->fixed_sizes[i] = 8;  offset += 8; break;
      case KANBUDB_BOOL:   rs->fixed_offsets[i] = offset; rs->fixed_sizes[i] = 1;  offset += 1; break;
      default:
        rs->fixed_offsets[i] = -1; rs->fixed_sizes[i] = 0;
        offset += 4; /* length prefix */
        break;
    }
  }
}

const void* row_extract_column(const row_schema_t* rs, int col,
                                const void* row_data, i32 row_len, i32* out_len) {
  if (!rs || col < 0 || col >= rs->num_cols || !row_data || !out_len) return NULL;
  (void)row_len;

  const u8* data = (const u8*)row_data;
  i32 offset = 0;

  /* Walk through all columns before `col` to find its offset */
  for (int i = 0; i < col; i++) {
    if (rs->fixed_sizes[i] > 0) {
      offset += rs->fixed_sizes[i];
    } else {
      u32 slen;
      memcpy(&slen, data + offset, 4);
      offset += 4 + (i32)slen;
    }
  }

  if (rs->fixed_sizes[col] > 0) {
    *out_len = rs->fixed_sizes[col];
    return data + offset;
  }

  u32 slen;
  memcpy(&slen, data + offset, 4);
  *out_len = (i32)slen;
  return data + offset + 4;
}

i32 row_encode_column(u8* buf, i32 offset, kanbudb_col_type_t type,
                       const void* data, i32 len) {
  (void)type;
  memcpy(buf + offset, data, (size_t)len);
  return len;
}

i32 row_encoded_size(const kanbudb_col_type_t* types, int num_cols,
                      const i32* col_lens) {
  i32 total = 0;
  for (int i = 0; i < num_cols; i++) {
    switch (types[i]) {
      case KANBUDB_INT32:  total += 4; break;
      case KANBUDB_INT64:  total += 8; break;
      case KANBUDB_FLOAT:  total += 4; break;
      case KANBUDB_DOUBLE: total += 8; break;
      case KANBUDB_BOOL:   total += 1; break;
      default: total += 4 + (col_lens ? col_lens[i] : 0); break;
    }
  }
  return total;
}
```

- [ ] **Step 3: Add row_format.c to CMakeLists.txt**

```cmake
# In the KANBUDB_SOURCES list, add:
  src/query/row_format.c
```

- [ ] **Step 4: Verify build**

Run: `cmake --build build`
Expected: compiles without errors

- [ ] **Step 5: Commit**

```bash
git add src/query/row_format.h src/query/row_format.c CMakeLists.txt
git commit -m "feat: add row binary format helpers"
```

---

### Task 2: Extend result_set_t for row storage

**Files:**
- Modify: `src/query/query_builder.h`
- Modify: `src/query/query_builder.c`

- [ ] **Step 1: Add row storage fields to result_set_t**

In `src/query/query_builder.h`, add fields after `scores`:

```c
  /* MK+Tree row storage */
  void**            row_data;
  size_t*           row_lens;
  int               row_capacity;
  int               row_count;
```

Also add the result set builder helper declaration:

```c
/* Internal: add a row to result set (copies the data) */
int rs_add_row(result_set_t* rs, const void* data, size_t len);

/* Internal: get pointer to column data from stored row */
const void* rs_get_row_column(result_set_t* rs, int row_idx, int col, size_t* out_len);
```

- [ ] **Step 2: Implement rs_add_row and rs_get_row_column**

In `query_builder.c`, add:

```c
int rs_add_row(result_set_t* rs, const void* data, size_t len) {
  if (!rs) return KANBUDB_ERR_INVAL;
  if (rs->row_count >= rs->row_capacity) {
    int new_cap = rs->row_capacity ? rs->row_capacity * 2 : 64;
    void** new_data = (void**)realloc(rs->row_data, (size_t)new_cap * sizeof(void*));
    size_t* new_lens = (size_t*)realloc(rs->row_lens, (size_t)new_cap * sizeof(size_t));
    if (!new_data || !new_lens) return KANBUDB_ERR_OOM;
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
```

Include `"row_format.h"` at the top of `query_builder.c`.

- [ ] **Step 3: Fix rs_close to free row data**

In `rs_close`:

```c
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
```

- [ ] **Step 4: Build and test**

```bash
cmake --build build
./build/test_query
```

Expected: existing tests pass, build succeeds.

- [ ] **Step 5: Commit**

```bash
git add src/query/query_builder.h src/query/query_builder.c
git commit -m "feat: extend result_set_t with row storage"
```

---

### Task 3: Implement B-tree scanner + filter in qb_exec

**Files:**
- Modify: `src/query/query_builder.c`
- Modify: `src/query/query_builder.h`

- [ ] **Step 1: Add internal helper declarations to query_builder.h**

```c
/* Internal: evaluate filter predicate */
int filter_match(const void* col_data, i32 col_len,
                  kanbudb_col_type_t type, const char* op, const char* literal);
```

- [ ] **Step 2: Implement filter_match**

In `query_builder.c`, add:

```c
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
      /* col_data points to string bytes, col_len is the length (may not be null-terminated) */
      /* literal is null-terminated */
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
```

- [ ] **Step 3: Implement B-tree scanner in qb_exec**

Add to `src/query/query_builder.c` — include the btree header:

```c
#include "btree.h"
#include "core/db.h"
```

Replace the `qb_exec` body. After column metadata setup, add B-tree scan:

```c
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

  /* ---- B-tree scan ---- */
  struct kanbudb_db* internal = qb->db;
  if (internal->btree) {
    btree_cursor_t* cur = btree_cursor_create(internal->btree);
    if (cur) {
      if (btree_cursor_seek(cur, NULL, 0) == KANBUDB_OK) {
        btree_kv_t kv;
        while (btree_cursor_next(cur, &kv) == KANBUDB_OK) {
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

  /* Set result set row count */
  rs->num_rows = rs->row_count;
```

- [ ] **Step 4: Build and test**

```bash
cmake --build build
```

Expected: compiles without errors.

- [ ] **Step 5: Commit**

```bash
git add src/query/query_builder.h src/query/query_builder.c
git commit -m "feat: implement B-tree scanner and filter evaluation"
```

---

### Task 4: Implement LSM scanner + merge dedup

**Files:**
- Modify: `src/query/query_builder.c`

- [ ] **Step 1: Add key hash set helper**

`lsm.h` is already included in `query_builder.c`. Add the hash set code:

```c
/* Simple hash set for dedup keys from LSM.
 * Stores key pointers from LSM scan to skip during B-tree scan. */
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
    void* tmp = realloc(ks->keys, (size_t)new_cap * sizeof(void*));
    if (!tmp) return KANBUDB_ERR_OOM;
    ks->keys = (const void**)tmp;
    tmp = realloc(ks->key_lens, (size_t)new_cap * sizeof(size_t));
    if (!tmp) return KANBUDB_ERR_OOM;
    ks->key_lens = (size_t*)tmp;
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
```

- [ ] **Step 2: Add LSM scan callback struct**

```c
typedef struct {
  key_set_t*      seen_keys;
  query_builder_t* qb;
  row_schema_t*   row_schema;
  int             filter_col;
  kanbudb_col_type_t filter_type;
  result_set_t*   rs;
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
```

- [ ] **Step 3: Add LSM accessor functions to lsm.h/lsm.c**

Add to `lsm.h`:

```c
/* Expose memtable access for query scanning */
kanbudb_memtable_t* lsm_get_active(kanbudb_lsm_t* lsm);
kanbudb_memtable_t* lsm_get_flushing(kanbudb_lsm_t* lsm);
```

In `lsm.c`:

```c
kanbudb_memtable_t* lsm_get_active(kanbudb_lsm_t* lsm) {
  return lsm ? lsm->active : NULL;
}
kanbudb_memtable_t* lsm_get_flushing(kanbudb_lsm_t* lsm) {
  return lsm ? lsm->flushing : NULL;
}
```

- [ ] **Step 4: Modify qb_exec to add LSM scan before B-tree scan**

In `query_builder.c`, replace the existing B-tree-only qb_exec body. Insert LSM scan before B-tree scan, using `key_set_t` for dedup:

```c
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

  /* Set result set row count */
  rs->num_rows = rs->row_count;
```

- [ ] **Step 5: Build and test**

```bash
cmake --build build
```

Expected: compiles without errors.

- [ ] **Step 6: Commit**

```bash
git add src/query/query_builder.c src/storage/lsm.h src/storage/lsm.c
git commit -m "feat: implement LSM scanner and merge dedup"
```

---

### Task 5: Implement Sort and Limit

**Files:**
- Modify: `src/query/query_builder.c`

- [ ] **Step 1: Add sort + limit logic after scan**

After both scans complete and before setting `rs->num_rows`, add:

```c
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
      /* Build sort index */
      sort_entry_t* entries = (sort_entry_t*)malloc((size_t)rs->row_count * sizeof(sort_entry_t));
      if (entries) {
        row_schema_t sort_schema;
        row_schema_init(&sort_schema, rs->col_types, rs->num_cols);

        for (int i = 0; i < rs->row_count; i++) {
          i32 col_len;
          const void* col_data = row_extract_column(&sort_schema, sort_col,
                                                      rs->row_data[i], (i32)rs->row_lens[i], &col_len);
          if (col_data && col_len > 0) {
            kanbudb_col_type_t st = rs->col_types[sort_col];
            if (st == KANBUDB_STRING) {
              /* For strings, use first 8 bytes as a rough sort key (will be refined) */
              char buf[9] = {0};
              memcpy(buf, col_data, col_len > 8 ? 8 : (i32)col_len);
              double h = 0;
              for (int j = 0; j < 8 && buf[j]; j++) h = h * 31 + buf[j];
              entries[i].key = h;
            } else if (st == KANBUDB_INT32 || st == KANBUDB_INT64 || st == KANBUDB_BOOL) {
              entries[i].key = (f64)(st == KANBUDB_INT32 ? *(const i32*)col_data :
                                      st == KANBUDB_INT64 ? (f64)*(const i64*)col_data :
                                      (f64)*(const u8*)col_data);
            } else {
              entries[i].key = st == KANBUDB_FLOAT ? (f64)*(const f32*)col_data : *(const f64*)col_data;
            }
          } else {
            entries[i].key = 0;
          }
          entries[i].idx = i;
        }

        /* Sort */
        if (qb->sort_ascending) {
          qsort(entries, (size_t)rs->row_count, sizeof(sort_entry_t),
                (int (*)(const void*, const void*))sort_entry_cmp_asc);
        } else {
          qsort(entries, (size_t)rs->row_count, sizeof(sort_entry_t),
                (int (*)(const void*, const void*))sort_entry_cmp_desc);
        }

        /* Rebuild row_data in sorted order (in-place reorder) */
        void** sorted_data = (void**)malloc((size_t)rs->row_count * sizeof(void*));
        size_t* sorted_lens = (size_t*)malloc((size_t)rs->row_count * sizeof(size_t));
        if (sorted_data && sorted_lens) {
          for (int i = 0; i < rs->row_count; i++) {
            sorted_data[i] = rs->row_data[entries[i].idx];
            sorted_lens[i] = rs->row_lens[entries[i].idx];
          }
          /* Swap arrays */
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

  /* Set result set row count */
  rs->num_rows = rs->row_count;
```

- [ ] **Step 2: Add sort_entry_t typedef at file scope**

Add to `query_builder.c` (before `qb_exec`):

```c
typedef struct {
  f64  key;
  int  idx;
} sort_entry_t;
```

- [ ] **Step 3: Add sort comparator functions**

Add before `qb_exec` (after the typedef):

```c
static int sort_entry_cmp_asc(const void* a, const void* b) {
  f64 ka = ((const sort_entry_t*)a)->key;
  f64 kb = ((const sort_entry_t*)b)->key;
  if (ka < kb) return -1;
  if (ka > kb) return 1;
  return 0;
}
static int sort_entry_cmp_desc(const void* a, const void* b) {
  f64 ka = ((const sort_entry_t*)a)->key;
  f64 kb = ((const sort_entry_t*)b)->key;
  if (ka > kb) return -1;
  if (ka < kb) return 1;
  return 0;
}
```

- [ ] **Step 4: Build and test**

```bash
cmake --build build
```

Expected: compiles without errors.

- [ ] **Step 5: Commit**

```bash
git add src/query/query_builder.c
git commit -m "feat: implement sort and limit"
```

---

### Task 6: Implement Join

**Files:**
- Modify: `src/query/query_builder.c`

- [ ] **Step 1: Add join logic after sort/limit**

After the limit section and before `rs->num_rows = rs->row_count;`, insert the join logic:

```c
  /* ---- Apply join ---- */
  if (qb->has_join && rs->row_count > 0) {
    /* Find join column indices */
    int local_col = -1;
    for (int i = 0; i < rs->num_cols; i++) {
      if (strcmp(rs->col_names[i], qb->join_on_local) == 0) {
        local_col = i;
        break;
      }
    }
    if (local_col < 0) {
      /* No matching column, skip join */
    } else {
      /* Look up foreign table schema */
      int foreign_table_idx = -1;
      for (int i = 0; i < internal->num_tables; i++) {
        if (strcmp(internal->tables[i].name, qb->join_table) == 0) {
          foreign_table_idx = i;
          break;
        }
      }
      if (foreign_table_idx >= 0) {
        kanbudb_table_t* ft = &internal->tables[foreign_table_idx];
        /* Find foreign join column */
        int foreign_col = -1;
        for (int i = 0; i < ft->num_cols; i++) {
          if (strcmp(ft->col_names[i], qb->join_on_foreign) == 0) {
            foreign_col = i;
            break;
          }
        }
        if (foreign_col >= 0) {
          /* Build result with combined schema */
          int new_num_cols = rs->num_cols + ft->num_cols;
          /* Avoid duplicate join key column in output */
          kanbudb_col_type_t new_types[KANBUDB_MAX_COLS];
          char* new_names[KANBUDB_MAX_COLS];
          int nidx = 0;
          for (int i = 0; i < rs->num_cols; i++) {
            new_types[nidx] = rs->col_types[i];
            new_names[nidx] = rs->col_names[i];
            nidx++;
          }
          int foreign_join_idx = -1;
          for (int i = 0; i < ft->num_cols; i++) {
            /* Skip the join key column in foreign (it matches local) */
            if (i == foreign_col) {
              foreign_join_idx = nidx;
              continue;
            }
            new_types[nidx] = ft->col_types[i];
            new_names[nidx] = ft->col_names[i];
            nidx++;
          }
          if (foreign_join_idx < 0) foreign_join_idx = nidx;
          new_num_cols = nidx;

          row_schema_t ft_schema;
          row_schema_init(&ft_schema, ft->col_types, ft->num_cols);

          /* For each row, lookup foreign row and merge */
          for (int i = 0; i < rs->row_count; i++) {
            /* Extract local join column value (as key) */
            i32 join_len;
            const void* join_data = row_extract_column(&row_schema, local_col,
                                                        rs->row_data[i], (i32)rs->row_lens[i], &join_len);
            if (!join_data) continue;

            /* Convert key to string for db_get */
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

            /* Lookup foreign row */
            void* fval = NULL;
            size_t flen = 0;
            int rc = db_get((db_t*)internal, qb->join_table,
                             key_str, (size_t)key_len + 1, &fval, &flen);
            if (rc != KANBUDB_OK || !fval) continue;

            /* Build combined row */
            i32 combined_size = (i32)rs->row_lens[i];
            /* Add foreign columns (skip the join key column data) */
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

            /* Copy local row */
            memcpy(combined, rs->row_data[i], rs->row_lens[i]);
            i32 off = (i32)rs->row_lens[i];

            /* Append foreign columns (skip join key) */
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

            /* Replace the row with combined row */
            free(rs->row_data[i]);
            rs->row_data[i] = combined;
            rs->row_lens[i] = (size_t)combined_size;
          }

          /* Update column metadata */
          rs->num_cols = new_num_cols;
          for (int i = 0; i < new_num_cols; i++) {
            rs->col_types[i] = new_types[i];
            rs->col_names[i] = new_names[i];
          }
        }
      }
    }
  }
```

- [ ] **Step 2: Build and test**

```bash
cmake --build build
```

Expected: compiles without errors.

- [ ] **Step 3: Commit**

```bash
git add src/query/query_builder.c
git commit -m "feat: implement join (nested-loop)"
```

---

### Task 7: Make rs_next / rs_get_column return real data

**Files:**
- Modify: `src/query/query_builder.c`

- [ ] **Step 1: Replace rs_next stub with working implementation**

Update `rs_next` — the stub logic is already correct (it increments `current` and checks bounds), so just remove the `/* v1 stub */` comments. The function already works: it walks through `num_rows` using `current`.

- [ ] **Step 2: Replace rs_get_column stub**

Replace the v1 stub section in `rs_get_column`:

```c
  if (!rs->is_fts) {
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
```

- [ ] **Step 3: Remove old v1 stub comments**

Clean up any remaining `/* v1 stub */` comments.

- [ ] **Step 4: Build**

```bash
cmake --build build
```

- [ ] **Step 5: Commit**

```bash
git add src/query/query_builder.c
git commit -m "feat: implement rs_next and rs_get_column for real data"
```

---

### Task 8: Unit tests for range queries

**Files:**
- Modify: `test/unit/test_query.c`

- [ ] **Step 1: Add serialization helper for building rows**

Add before tests:

```c
/* Encode a row value for db_put using the compact binary format.
 * Schema: id(int32), name(string), age(int32) */
static int encode_row_u32(const char* name, i32 age, u8* buf, i32 buf_size) {
  (void)buf_size;
  i32* id = (i32*)buf; *id = 0;              /* id = 4 bytes */
  u32 name_len = (u32)strlen(name);           /* name length prefix = 4 bytes */
  memcpy(buf + 4, &name_len, 4);
  memcpy(buf + 8, name, name_len);            /* name data */
  i32* age_out = (i32*)(buf + 8 + name_len); *age_out = age; /* age = 4 bytes */
  return 8 + (i32)name_len + 4;
}
/* Same but with specific id */
static int encode_row(i32 id, const char* name, i32 age, u8* buf, i32 buf_size) {
  (void)buf_size;
  i32* id_out = (i32*)buf; *id_out = id;
  u32 name_len = (u32)strlen(name);
  memcpy(buf + 4, &name_len, 4);
  memcpy(buf + 8, name, name_len);
  i32* age_out = (i32*)(buf + 8 + name_len); *age_out = age;
  return 8 + (i32)name_len + 4;
}
```

- [ ] **Step 2: Add test_range test**

```c
static int test_range(void) {
  cleanup();

  db_t* db = NULL;
  if (db_open(TEST_DB_PATH, NULL, &db) != KANBUDB_OK) return 0;

  const char* col_names[] = {"id", "name", "age"};
  kanbudb_col_type_t col_types[] = {KANBUDB_INT32, KANBUDB_STRING, KANBUDB_INT32};
  if (db_create_table(db, "users", col_names, col_types, 3, "id") != KANBUDB_OK) {
    db_close(db); return 0;
  }

  /* Insert 5 rows: ages 10, 20, 30, 40, 50 */
  u8 buf[256];
  for (i32 i = 0; i < 5; i++) {
    char name[32]; snprintf(name, sizeof(name), "user_%d", (int)i);
    i32 id = i + 1;
    i32 age = (i + 1) * 10;
    i32 len = encode_row(id, name, age, buf, sizeof(buf));
    char key[16]; snprintf(key, sizeof(key), "key_%d", (int)id);
    if (db_put(db, "users", key, strlen(key) + 1, buf, (size_t)len) != KANBUDB_OK) {
      db_close(db); return 0;
    }
  }

  /* Query: age > 25 (should match ages 30, 40, 50) */
  query_builder_t* qb = db_query(db, "users");
  if (!qb) { db_close(db); return 0; }
  if (qb_filter(qb, "age", ">", "25") != KANBUDB_OK) { qb_destroy(qb); db_close(db); return 0; }

  result_set_t* rs = qb_exec(qb);
  if (!rs) { qb_destroy(qb); db_close(db); return 0; }

  int count = 0;
  while (rs_next(rs)) {
    void* age_data; size_t age_len;
    if (rs_get_column(rs, 2, &age_data, &age_len) != KANBUDB_OK) { rs_close(rs); qb_destroy(qb); db_close(db); return 0; }
    if (age_len != 4) { rs_close(rs); qb_destroy(qb); db_close(db); return 0; }
    i32 age_val = *(const i32*)age_data;
    if (age_val <= 25) { rs_close(rs); qb_destroy(qb); db_close(db); return 0; }
    count++;
  }
  rs_close(rs);
  qb_destroy(qb);

  /* Also test <, >=, <= */
  qb = db_query(db, "users");
  if (qb_filter(qb, "age", "<=", "20") != KANBUDB_OK) { qb_destroy(qb); db_close(db); return 0; }
  rs = qb_exec(qb);
  count = 0;
  while (rs_next(rs)) { count++; }
  if (count != 2) { rs_close(rs); qb_destroy(qb); db_close(db); return 0; }
  rs_close(rs);
  qb_destroy(qb);

  db_close(db);
  cleanup();
  return 1;
}
```

- [ ] **Step 3: Add test_sort test**

```c
static int test_sort(void) {
  cleanup();

  db_t* db = NULL;
  if (db_open(TEST_DB_PATH, NULL, &db) != KANBUDB_OK) return 0;

  const char* col_names[] = {"id", "name", "age"};
  kanbudb_col_type_t col_types[] = {KANBUDB_INT32, KANBUDB_STRING, KANBUDB_INT32};
  if (db_create_table(db, "users", col_names, col_types, 3, "id") != KANBUDB_OK) {
    db_close(db); return 0;
  }

  u8 buf[256];
  i32 ages[] = {50, 10, 40, 20, 30};
  for (int i = 0; i < 5; i++) {
    char name[16]; snprintf(name, sizeof(name), "u%d", i);
    i32 len = encode_row((i32)(i + 1), name, ages[i], buf, sizeof(buf));
    char key[16]; snprintf(key, sizeof(key), "k%d", i);
    db_put(db, "users", key, strlen(key) + 1, buf, (size_t)len);
  }

  /* Sort ascending by age */
  query_builder_t* qb = db_query(db, "users");
  qb_sort(qb, "age", 1);
  result_set_t* rs = qb_exec(qb);
  if (!rs) { qb_destroy(qb); db_close(db); return 0; }

  i32 prev = -1;
  while (rs_next(rs)) {
    void* d; size_t l;
    rs_get_column(rs, 2, &d, &l);
    i32 age = *(const i32*)d;
    if (age < prev) { rs_close(rs); qb_destroy(qb); db_close(db); return 0; }
    prev = age;
  }
  rs_close(rs);
  qb_destroy(qb);

  /* Sort descending */
  qb = db_query(db, "users");
  qb_sort(qb, "age", 0);
  rs = qb_exec(qb);
  prev = 999;
  while (rs_next(rs)) {
    void* d; size_t l;
    rs_get_column(rs, 2, &d, &l);
    i32 age = *(const i32*)d;
    if (age > prev) { rs_close(rs); qb_destroy(qb); db_close(db); return 0; }
    prev = age;
  }
  rs_close(rs);
  qb_destroy(qb);

  db_close(db);
  cleanup();
  return 1;
}
```

- [ ] **Step 4: Add test_limit and test_limit_sort tests**

```c
static int test_limit(void) {
  cleanup();
  db_t* db = NULL;
  db_open(TEST_DB_PATH, NULL, &db);
  const char* cols[] = {"id", "name", "age"};
  kanbudb_col_type_t ctypes[] = {KANBUDB_INT32, KANBUDB_STRING, KANBUDB_INT32};
  db_create_table(db, "users", cols, ctypes, 3, "id");

  u8 buf[256];
  for (int i = 0; i < 20; i++) {
    char name[16]; snprintf(name, sizeof(name), "u%d", i);
    i32 len = encode_row((i32)i, name, (i32)(i * 10), buf, sizeof(buf));
    char key[16]; snprintf(key, sizeof(key), "k%d", i);
    db_put(db, "users", key, strlen(key) + 1, buf, (size_t)len);
  }

  query_builder_t* qb = db_query(db, "users");
  qb_limit(qb, 5);
  result_set_t* rs = qb_exec(qb);
  int count = 0;
  while (rs_next(rs)) count++;
  if (count > 5) { rs_close(rs); qb_destroy(qb); db_close(db); cleanup(); return 0; }
  rs_close(rs);
  qb_destroy(qb);

  db_close(db);
  cleanup();
  return 1;
}

static int test_limit_sort(void) {
  cleanup();
  db_t* db = NULL;
  db_open(TEST_DB_PATH, NULL, &db);
  const char* cols[] = {"id", "name", "age"};
  kanbudb_col_type_t ctypes[] = {KANBUDB_INT32, KANBUDB_STRING, KANBUDB_INT32};
  db_create_table(db, "users", cols, ctypes, 3, "id");

  u8 buf[256];
  for (int i = 0; i < 10; i++) {
    char name[16]; snprintf(name, sizeof(name), "u%d", i);
    i32 len = encode_row((i32)i, name, (i32)(i * 10), buf, sizeof(buf));
    char key[16]; snprintf(key, sizeof(key), "k%d", i);
    db_put(db, "users", key, strlen(key) + 1, buf, (size_t)len);
  }

  query_builder_t* qb = db_query(db, "users");
  qb_sort(qb, "age", 0);
  qb_limit(qb, 3);
  result_set_t* rs = qb_exec(qb);
  i32 prev = 999;
  int count = 0;
  while (rs_next(rs)) {
    void* d; size_t l;
    rs_get_column(rs, 2, &d, &l);
    i32 age = *(const i32*)d;
    if (age > prev) { rs_close(rs); qb_destroy(qb); db_close(db); cleanup(); return 0; }
    prev = age;
    count++;
  }
  if (count > 3) { rs_close(rs); qb_destroy(qb); db_close(db); cleanup(); return 0; }
  rs_close(rs);
  qb_destroy(qb);

  db_close(db);
  cleanup();
  return 1;
}
```

- [ ] **Step 5: Register new tests in main()**

```c
  TEST(range);
  TEST(sort);
  TEST(limit);
  TEST(limit_sort);
```

- [ ] **Step 6: Build and run**

```bash
cmake --build build && ./build/test_query
```

Expected: all tests pass.

- [ ] **Step 7: Commit**

```bash
git add test/unit/test_query.c
git commit -m "test: add range, sort, limit tests"
```

---

### Task 9: Integration tests

**Files:**
- Modify: `test/integration/test_e2e.c`

- [ ] **Step 1: Add e2e range + sort + limit test**

```c
static int test_e2e_queries(void) {
  cleanup();

  db_config_t config;
  config.fsync_mode = KANBUDB_FSYNC_NONE;
  config.cache_size = 4096;
  config.memtable_size = 4096;
  config.compaction_threads = 1;

  db_t* db = NULL;
  if (db_open(TEST_DB_PATH, &config, &db) != KANBUDB_OK || !db) return 0;

  const char* col_names[] = {"id", "title", "score"};
  kanbudb_col_type_t col_types[] = {KANBUDB_INT32, KANBUDB_STRING, KANBUDB_INT32};
  if (db_create_table(db, "items", col_names, col_types, 3, "id") != KANBUDB_OK) {
    db_close(db); return 0;
  }

  /* Insert 10 items with scores 10, 20, ..., 100 */
  u8 buf[256];
  for (int i = 0; i < 10; i++) {
    char title[32]; snprintf(title, sizeof(title), "item_%d", i);
    /* Encode: id(int32) + title(string len+data) + score(int32) */
    i32 id = (i32)(i + 1);
    *(i32*)buf = id;
    u32 tlen = (u32)strlen(title);
    memcpy(buf + 4, &tlen, 4);
    memcpy(buf + 8, title, tlen);
    *(i32*)(buf + 8 + tlen) = (i32)((i + 1) * 10);
    i32 row_len = 8 + (i32)tlen + 4;

    char key[16]; snprintf(key, sizeof(key), "key_%d", i);
    db_put(db, "items", key, strlen(key) + 1, buf, (size_t)row_len);
  }

  /* Range query: score > 50 */
  query_builder_t* qb = db_query(db, "items");
  qb_filter(qb, "score", ">", "50");
  qb_sort(qb, "score", 0);
  qb_limit(qb, 3);

  result_set_t* rs = qb_exec(qb);
  if (!rs) { qb_destroy(qb); db_close(db); return 0; }

  i32 expected[] = {100, 90, 80};
  int idx = 0;
  while (rs_next(rs)) {
    void* score_data; size_t score_len;
    rs_get_column(rs, 2, &score_data, &score_len);
    if (score_len != 4) { rs_close(rs); qb_destroy(qb); db_close(db); return 0; }
    i32 score = *(const i32*)score_data;
    if (idx < 3 && score != expected[idx]) { rs_close(rs); qb_destroy(qb); db_close(db); return 0; }
    idx++;
  }
  if (idx != 3) { rs_close(rs); qb_destroy(qb); db_close(db); return 0; }
  rs_close(rs);
  qb_destroy(qb);

  db_close(db);
  cleanup();
  return 1;
}
```

- [ ] **Step 2: Register in main**

```c
  TEST(e2e_queries);
```

- [ ] **Step 3: Build and run**

```bash
cmake --build build && ./build/test_e2e
```

Expected: all tests pass.

- [ ] **Step 4: Commit**

```bash
git add test/integration/test_e2e.c
git commit -m "test: add e2e query integration test"
```

---

### Task 10: Full test suite verification

- [ ] **Step 1: Run all tests**

```bash
cmake --build build && ctest --output-on-failure
```

Expected: all tests pass.

- [ ] **Step 2: Final commit with any fixes**

```bash
git add -A
git commit -m "fix: address review findings"
```
