# Thread Safety Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add single-writer multiple-reader concurrency protection to the KanbuDB engine using `pthread_rwlock_t`.

**Architecture:** Add a `pthread_rwlock_t` to `db_t`. All public write API calls acquire the write lock; all read API calls acquire the read lock. Additionally, fix two standalone race conditions: global `sort_ctx` in query_builder.c and lazy CRC32 init in sstable.c.

**Tech Stack:** C99, POSIX pthread, CMake, `qsort_r` (GNU/BSD extension)

---

## File Map

| File | Change |
|------|--------|
| `CMakeLists.txt` | Link `-lpthread` for all test targets |
| `src/core/db.h` | Add `#include <pthread.h>`, add `pthread_rwlock_t rwlock` to `kanbudb_db` |
| `src/core/db.c` | Init/destroy rwlock in `db_open`/`db_close`; lock/unlock in all public API functions |
| `src/query/query_builder.c` | Replace global `static sort_ctx_t sort_ctx` with `qsort_r` + stack-local context |
| `src/storage/sstable.c` | Replace lazy CRC32 init with `__attribute__((constructor))` |
| `test/unit/test_thread_safety.c` | New: multi-threaded stress test |

---

## Task 1: Link pthread in CMakeLists.txt

**Files:**
- Modify: `CMakeLists.txt:27-31`

- [ ] **Step 1: Add pthread link to static and shared libraries**

In `CMakeLists.txt`, after line 31 (`target_include_directories(kanbudb_shared ...)`), add:

```cmake
find_package(Threads REQUIRED)
target_link_libraries(kanbudb_static PRIVATE Threads::Threads)
target_link_libraries(kanbudb_shared PRIVATE Threads::Threads)
```

The full relevant section becomes:

```cmake
add_library(kanbudb_static STATIC ${KANBUDB_SOURCES})
target_include_directories(kanbudb_static PUBLIC include src/util src/storage src)

add_library(kanbudb_shared SHARED ${KANBUDB_SOURCES})
target_include_directories(kanbudb_shared PUBLIC include src/util src/storage src)

find_package(Threads REQUIRED)
target_link_libraries(kanbudb_static PRIVATE Threads::Threads)
target_link_libraries(kanbudb_shared PRIVATE Threads::Threads)
```

- [ ] **Step 2: Build and verify**

```bash
cmake -S . -B build && cmake --build build/
```

Expected: Builds successfully with no errors.

---

## Task 2: Add rwlock to db_t struct

**Files:**
- Modify: `src/core/db.h:1-35`

- [ ] **Step 1: Add pthread include and rwlock field**

Replace the entire content of `src/core/db.h` with:

```c
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
```

- [ ] **Step 2: Build to verify no compile errors**

```bash
cmake --build build/
```

Expected: Builds successfully. The new field is added but unused.

---

## Task 3: Fix global sort_ctx in query_builder.c

**Files:**
- Modify: `src/query/query_builder.c:283-434`

- [ ] **Step 1: Replace global sort_ctx with qsort_r**

Find the sort context section (around lines 283-434). Make these changes:

1. **Remove** the global `static sort_ctx_t sort_ctx;` on line 293.

2. **Change** the comparator function signature from:
```c
static int sort_entry_cmp(const void* a, const void* b) {
```
to:
```c
static int sort_entry_cmp(const void* a, const void* b, void* ctx) {
```

3. **Change** all references inside `sort_entry_cmp` from `sort_ctx.` to `sctx->`, and add the context variable at the top of the function:
```c
static int sort_entry_cmp(const void* a, const void* b, void* ctx) {
  sort_ctx_t* sctx = (sort_ctx_t*)ctx;
  const sort_entry_t* ea = (const sort_entry_t*)a;
  const sort_entry_t* eb = (const sort_entry_t*)b;
  i32 len_a, len_b;
  const void* da = row_extract_column(&sctx->schema, sctx->col,
                                       sctx->row_data[ea->idx],
                                       (i32)sctx->row_lens[ea->idx], &len_a);
  const void* db = row_extract_column(&sctx->schema, sctx->col,
                                       sctx->row_data[eb->idx],
                                       (i32)sctx->row_lens[eb->idx], &len_b);
  int cmp = 0;
  if (sctx->type == KANBUDB_STRING) {
    size_t min = (size_t)(len_a < len_b ? len_a : len_b);
    cmp = memcmp(da, db, min);
    if (cmp == 0) cmp = (len_a < len_b) ? -1 : (len_a > len_b) ? 1 : 0;
  } else if (sctx->type == KANBUDB_INT32 || sctx->type == KANBUDB_INT64 || sctx->type == KANBUDB_BOOL) {
    i64 va = sctx->type == KANBUDB_INT32 ? *(const i32*)da : sctx->type == KANBUDB_INT64 ? *(const i64*)da : *(const u8*)da;
    i64 vb = sctx->type == KANBUDB_INT32 ? *(const i32*)db : sctx->type == KANBUDB_INT64 ? *(const i64*)db : *(const u8*)db;
    if (va < vb) cmp = -1; else if (va > vb) cmp = 1;
  } else {
    f64 va = sctx->type == KANBUDB_FLOAT ? *(const f32*)da : *(const f64*)da;
    f64 vb = sctx->type == KANBUDB_FLOAT ? *(const f32*)db : *(const f64*)db;
    if (va < vb) cmp = -1; else if (va > vb) cmp = 1;
  }
  return sctx->ascending ? cmp : -cmp;
}
```

4. **In `qb_exec()`**, change the sort section (around lines 421-434) from:
```c
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
```
to:
```c
      sort_ctx_t sort_ctx_local;
      sort_ctx_local.row_data = (const void**)rs->row_data;
      sort_ctx_local.row_lens = rs->row_lens;
      row_schema_init(&sort_ctx_local.schema, rs->col_types, rs->num_cols);
      sort_ctx_local.col = sort_col;
      sort_ctx_local.type = rs->col_types[sort_col];
      sort_ctx_local.ascending = qb->sort_ascending;

      sort_entry_t* entries = (sort_entry_t*)malloc((size_t)rs->row_count * sizeof(sort_entry_t));
      if (entries) {
        for (int i = 0; i < rs->row_count; i++) {
          entries[i].idx = i;
        }
        qsort_r(entries, (size_t)rs->row_count, sizeof(sort_entry_t), sort_entry_cmp, &sort_ctx_local);
```

- [ ] **Step 2: Build and run query tests**

```bash
cmake --build build/ && rm -f /tmp/kanbudb_test_query* && ./build/test_query
```

Expected: All 8 tests pass (lifecycle, chain, no_crash, from_override, range, sort, limit, limit_sort).

---

## Task 4: Fix CRC32 lazy init in sstable.c

**Files:**
- Modify: `src/storage/sstable.c:12-39`

- [ ] **Step 1: Replace lazy init with constructor**

Replace the entire CRC32 section (lines 12-39) with:

```c
/* ── CRC32 (table-driven) ────────────────────────────────── */

static uint32_t crc32_table[256];

__attribute__((constructor))
static void crc32_init_table(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t crc = i;
        for (int j = 0; j < 8; j++) {
            if (crc & 1)
                crc = (crc >> 1) ^ 0xEDB88320U;
            else
                crc >>= 1;
        }
        crc32_table[i] = crc;
    }
}

static uint32_t crc32_bytes(const void* data, size_t len, uint32_t crc) {
    const uint8_t* buf = (const uint8_t*)data;
    crc = ~crc;
    for (size_t i = 0; i < len; i++) {
        crc = crc32_table[(crc ^ buf[i]) & 0xFF] ^ (crc >> 8);
    }
    return ~crc;
}
```

Key changes:
- Removed `static int crc32_table_init = 0;`
- Added `__attribute__((constructor))` to `crc32_init_table`
- Removed the `if (!crc32_table_init) crc32_init_table();` check from `crc32_bytes`

- [ ] **Step 2: Build and run sstable tests**

```bash
cmake --build build/ && rm -f /tmp/kanbudb_test_sstable* && ./build/test_sstable
```

Expected: All sstable tests pass.

---

## Task 5: Add lock/unlock to all public API functions in db.c

**Files:**
- Modify: `src/core/db.c:373-735`

- [ ] **Step 1: Initialize rwlock in db_open**

In `db_open()` (line 373), after the `fts_index` allocation block (around line 396), add the rwlock initialization:

```c
  db->fts_index = fts_index_create();
  if (!db->fts_index) { btree_destroy(db->btree); lsm_destroy(db->lsm); wal_destroy(db->wal); free(db->path); free(db); return KANBUDB_ERR_OOM; }

  pthread_rwlock_init(&db->rwlock, NULL);

  db->num_tables = 0;
```

- [ ] **Step 2: Add write lock to db_close**

Replace the `db_close` function (lines 450-485) with:

```c
int db_close(db_t* db) {
  if (!db) return KANBUDB_ERR_INVAL;
  struct kanbudb_db* internal = (struct kanbudb_db*)db;

  pthread_rwlock_wrlock(&internal->rwlock);

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

  pthread_rwlock_unlock(&internal->rwlock);
  pthread_rwlock_destroy(&internal->rwlock);
  free(internal);
  return KANBUDB_OK;
}
```

- [ ] **Step 3: Add write lock to db_create_table**

In `db_create_table()` (line 506), wrap the body after the validation checks with write lock. Change:

```c
  struct kanbudb_db* internal = (struct kanbudb_db*)db;

  if (find_table(internal, table_name) >= 0) {
```
to:
```c
  struct kanbudb_db* internal = (struct kanbudb_db*)db;
  pthread_rwlock_wrlock(&internal->rwlock);

  if (find_table(internal, table_name) >= 0) {
```

And at the end of the function, before the final `return KANBUDB_OK;` on line 566, add:

```c
  internal->last_error = KANBUDB_OK;
  pthread_rwlock_unlock(&internal->rwlock);
  return KANBUDB_OK;
```

Also add unlock on all early-return error paths in `db_create_table`. The error returns on lines 511, 518, 523, 534, 538, 545 all need unlock before return. Each one should become:

```c
    pthread_rwlock_unlock(&internal->rwlock);
    return KANBUDB_ERR_INVAL;  // (or appropriate error code)
```

- [ ] **Step 4: Add write lock to db_put**

Replace the `db_put` function (lines 569-593) with:

```c
int db_put(db_t* db, const char* table, const char* key, size_t key_len,
           const void* value, size_t value_len) {
  if (!db || !table || !key) return KANBUDB_ERR_INVAL;

  struct kanbudb_db* internal = (struct kanbudb_db*)db;
  pthread_rwlock_wrlock(&internal->rwlock);

  int idx = find_table(internal, table);
  if (idx < 0) {
    internal->last_error = KANBUDB_ERR_NOTFOUND;
    pthread_rwlock_unlock(&internal->rwlock);
    return KANBUDB_ERR_NOTFOUND;
  }

  int rc = wal_append(internal->wal, KANBUDB_WAL_PUT,
                       internal->tables[idx].id,
                       key, key_len, value, value_len);
  if (rc != KANBUDB_OK) {
    internal->last_error = rc;
    pthread_rwlock_unlock(&internal->rwlock);
    return rc;
  }

  rc = lsm_put(internal->lsm, internal->tables[idx].id,
                key, key_len, value, value_len);
  if (rc != KANBUDB_OK) {
    internal->last_error = rc;
    pthread_rwlock_unlock(&internal->rwlock);
    return rc;
  }

  if (lsm_is_full(internal->lsm)) {
    int frc = db_flush_memtable(internal);
    if (frc != KANBUDB_OK) {
      internal->last_error = frc;
      pthread_rwlock_unlock(&internal->rwlock);
      return frc;
    }
  }

  internal->last_error = KANBUDB_OK;
  pthread_rwlock_unlock(&internal->rwlock);
  return KANBUDB_OK;
}
```

- [ ] **Step 5: Add read lock to db_get**

Replace the `db_get` function (lines 595-612) with:

```c
int db_get(db_t* db, const char* table, const char* key, size_t key_len,
           void** value, size_t* value_len) {
  if (!db || !table || !key || !value || !value_len) return KANBUDB_ERR_INVAL;

  struct kanbudb_db* internal = (struct kanbudb_db*)db;
  pthread_rwlock_rdlock(&internal->rwlock);

  int idx = find_table(internal, table);
  if (idx < 0) {
    internal->last_error = KANBUDB_ERR_NOTFOUND;
    pthread_rwlock_unlock(&internal->rwlock);
    return KANBUDB_ERR_NOTFOUND;
  }

  int rc = lsm_get(internal->lsm, internal->tables[idx].id,
                    key, key_len, value, value_len);
  if (rc == KANBUDB_OK) {
    internal->last_error = KANBUDB_OK;
    pthread_rwlock_unlock(&internal->rwlock);
    return KANBUDB_OK;
  }

  rc = btree_get(internal->btree, key, key_len, value, value_len);
  if (rc == KANBUDB_OK) {
    internal->last_error = KANBUDB_OK;
    pthread_rwlock_unlock(&internal->rwlock);
    return KANBUDB_OK;
  }

  internal->last_error = KANBUDB_ERR_NOTFOUND;
  pthread_rwlock_unlock(&internal->rwlock);
  return KANBUDB_ERR_NOTFOUND;
}
```

- [ ] **Step 6: Add write lock to db_delete**

Replace the `db_delete` function (lines 614-636) with:

```c
int db_delete(db_t* db, const char* table, const char* key, size_t key_len) {
  if (!db || !table || !key) return KANBUDB_ERR_INVAL;

  struct kanbudb_db* internal = (struct kanbudb_db*)db;
  pthread_rwlock_wrlock(&internal->rwlock);

  int idx = find_table(internal, table);
  if (idx < 0) {
    internal->last_error = KANBUDB_ERR_NOTFOUND;
    pthread_rwlock_unlock(&internal->rwlock);
    return KANBUDB_ERR_NOTFOUND;
  }

  int rc = wal_append(internal->wal, KANBUDB_WAL_DELETE,
                       internal->tables[idx].id,
                       key, key_len, NULL, 0);
  if (rc != KANBUDB_OK) {
    internal->last_error = rc;
    pthread_rwlock_unlock(&internal->rwlock);
    return rc;
  }

  rc = lsm_delete(internal->lsm, internal->tables[idx].id, key, key_len);
  if (rc != KANBUDB_OK) {
    internal->last_error = rc;
    pthread_rwlock_unlock(&internal->rwlock);
    return rc;
  }

  if (lsm_is_full(internal->lsm)) {
    int frc = db_flush_memtable(internal);
    if (frc != KANBUDB_OK) {
      internal->last_error = frc;
      pthread_rwlock_unlock(&internal->rwlock);
      return frc;
    }
  }

  internal->last_error = KANBUDB_OK;
  pthread_rwlock_unlock(&internal->rwlock);
  return KANBUDB_OK;
}
```

- [ ] **Step 7: Add write lock to db_fts_create_index**

Replace the `db_fts_create_index` function (lines 638-649) with:

```c
int db_fts_create_index(db_t* db, const char* table, const char* column,
                        const fts_options_t* opts) {
  (void)opts;
  if (!db || !table || !column) return KANBUDB_ERR_INVAL;
  struct kanbudb_db* internal = (struct kanbudb_db*)db;
  pthread_rwlock_wrlock(&internal->rwlock);
  if (find_table(internal, table) < 0) {
    internal->last_error = KANBUDB_ERR_NOTFOUND;
    pthread_rwlock_unlock(&internal->rwlock);
    return KANBUDB_ERR_NOTFOUND;
  }
  internal->last_error = KANBUDB_OK;
  pthread_rwlock_unlock(&internal->rwlock);
  return KANBUDB_OK;
}
```

- [ ] **Step 8: Add write lock to db_fts_drop_index**

Replace the `db_fts_drop_index` function (lines 651-656) with:

```c
int db_fts_drop_index(db_t* db, const char* table, const char* column) {
  if (!db || !table || !column) return KANBUDB_ERR_INVAL;
  struct kanbudb_db* internal = (struct kanbudb_db*)db;
  pthread_rwlock_wrlock(&internal->rwlock);
  internal->last_error = KANBUDB_OK;
  pthread_rwlock_unlock(&internal->rwlock);
  return KANBUDB_OK;
}
```

- [ ] **Step 9: Add read lock to db_fts_search**

In `db_fts_search()` (lines 658-735), add read lock after the validation checks and unlock on all exit paths. The function is long, so add the lock right after the internal variable is set:

```c
int db_fts_search(db_t* db, const char* table, const char* column,
                  const char* query, result_set_t** out) {
  if (!db || !table || !column || !query || !out) return KANBUDB_ERR_INVAL;
  struct kanbudb_db* internal = (struct kanbudb_db*)db;
  pthread_rwlock_rdlock(&internal->rwlock);
```

And add `pthread_rwlock_unlock(&internal->rwlock);` before every `return` statement in the function. There are returns at approximately these lines (in the original): 666, 671, 678, 708, 723, 733, 734.

- [ ] **Step 10: Build and run all existing tests**

```bash
cmake --build build/ && rm -f /tmp/kanbudb_test_* && cd build && ctest --output-on-failure
```

Expected: All 15 tests pass.

---

## Task 6: Add multi-threaded stress test

**Files:**
- Create: `test/unit/test_thread_safety.c`
- Modify: `CMakeLists.txt` (add test target)

- [ ] **Step 1: Create the multi-threaded test file**

Create `test/unit/test_thread_safety.c` with the following content:

```c
#include "db.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

#define TEST_DB_PATH "/tmp/kanbudb_test_thread"
#define NUM_WRITERS 4
#define NUM_READERS 4
#define ITERS_PER_THREAD 200

static void cleanup(void) {
  char path[256];
  snprintf(path, sizeof(path), "%s.wal", TEST_DB_PATH);
  unlink(path);
  snprintf(path, sizeof(path), "%s.lsm", TEST_DB_PATH);
  unlink(path);
  snprintf(path, sizeof(path), "%s.system", TEST_DB_PATH);
  unlink(path);
  snprintf(path, sizeof(path), "%s.seq", TEST_DB_PATH);
  unlink(path);
  for (int i = 0; i < 30; i++) {
    snprintf(path, sizeof(path), "%s.sst.0.%d", TEST_DB_PATH, i);
    unlink(path);
    snprintf(path, sizeof(path), "%s.ckpt.%d", TEST_DB_PATH, i);
    unlink(path);
  }
}

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) do { \
  printf("  TEST: %s ... ", #name); \
  if (test_##name()) { printf("PASS\n"); tests_passed++; } \
  else { printf("FAIL\n"); tests_failed++; } \
} while(0)

/* Encode a row for schema: id(int32) + name(string) + val(int32) */
static int encode_row(int id, const char* name, int val, unsigned char* buf) {
  *(int*)buf = id;
  unsigned int nlen = (unsigned int)strlen(name);
  memcpy(buf + 4, &nlen, 4);
  memcpy(buf + 8, name, nlen);
  *(int*)(buf + 8 + nlen) = val;
  return 8 + (int)nlen + 4;
}

typedef struct {
  db_t* db;
  int thread_id;
  int errors;
} writer_ctx_t;

typedef struct {
  db_t* db;
  int thread_id;
  int errors;
} reader_ctx_t;

static void* writer_func(void* arg) {
  writer_ctx_t* ctx = (writer_ctx_t*)arg;
  unsigned char buf[256];
  char key[32];
  int errors = 0;

  for (int i = 0; i < ITERS_PER_THREAD; i++) {
    int id = ctx->thread_id * ITERS_PER_THREAD + i;
    char name[16];
    snprintf(name, sizeof(name), "w%d_%d", ctx->thread_id, i);
    int len = encode_row(id, name, id * 10, buf);
    snprintf(key, sizeof(key), "key_%d", id);

    int rc = db_put(ctx->db, "threads", key, strlen(key) + 1, buf, (size_t)len);
    if (rc != KANBUDB_OK) errors++;
  }

  ctx->errors = errors;
  return NULL;
}

static void* reader_func(void* arg) {
  reader_ctx_t* ctx = (reader_ctx_t*)arg;
  int errors = 0;

  for (int i = 0; i < ITERS_PER_THREAD; i++) {
    int id = i;
    char key[32];
    snprintf(key, sizeof(key), "key_%d", id);

    void* val = NULL;
    size_t val_len = 0;
    db_get(ctx->db, "threads", key, strlen(key) + 1, &val, &val_len);
    /* db_get may return NOTFOUND for keys not yet written -- that's ok */
  }

  ctx->errors = errors;
  return NULL;
}

static int test_concurrent_read_write(void) {
  cleanup();

  db_t* db = NULL;
  if (db_open(TEST_DB_PATH, NULL, &db) != KANBUDB_OK) return 0;

  const char* col_names[] = {"id", "name", "val"};
  kanbudb_col_type_t col_types[] = {KANBUDB_INT32, KANBUDB_STRING, KANBUDB_INT32};
  if (db_create_table(db, "threads", col_names, col_types, 3, "id") != KANBUDB_OK) {
    db_close(db); return 0;
  }

  pthread_t writers[NUM_WRITERS];
  writer_ctx_t wctx[NUM_WRITERS];
  pthread_t readers[NUM_READERS];
  reader_ctx_t rctx[NUM_READERS];

  /* Start readers first (they should see empty/partial data without crashing) */
  for (int i = 0; i < NUM_READERS; i++) {
    rctx[i].db = db;
    rctx[i].thread_id = i;
    rctx[i].errors = 0;
    pthread_create(&readers[i], NULL, reader_func, &rctx[i]);
  }

  /* Start writers */
  for (int i = 0; i < NUM_WRITERS; i++) {
    wctx[i].db = db;
    wctx[i].thread_id = i;
    wctx[i].errors = 0;
    pthread_create(&writers[i], NULL, writer_func, &wctx[i]);
  }

  /* Wait for all threads */
  for (int i = 0; i < NUM_WRITERS; i++) {
    pthread_join(writers[i], NULL);
    if (wctx[i].errors > 0) { db_close(db); cleanup(); return 0; }
  }
  for (int i = 0; i < NUM_READERS; i++) {
    pthread_join(readers[i], NULL);
    if (rctx[i].errors > 0) { db_close(db); cleanup(); return 0; }
  }

  /* Verify all written data is readable */
  int total_expected = NUM_WRITERS * ITERS_PER_THREAD;
  int found = 0;
  for (int i = 0; i < total_expected; i++) {
    char key[32];
    snprintf(key, sizeof(key), "key_%d", i);
    void* val = NULL;
    size_t val_len = 0;
    if (db_get(db, "threads", key, strlen(key) + 1, &val, &val_len) == KANBUDB_OK) {
      if (val_len > 0) found++;
    }
  }

  if (found != total_expected) {
    fprintf(stderr, "  expected %d rows, found %d\n", total_expected, found);
    db_close(db); cleanup(); return 0;
  }

  db_close(db);
  cleanup();
  return 1;
}

static int test_concurrent_sort_queries(void) {
  cleanup();

  db_t* db = NULL;
  if (db_open(TEST_DB_PATH, NULL, &db) != KANBUDB_OK) return 0;

  const char* col_names[] = {"id", "name", "score"};
  kanbudb_col_type_t col_types[] = {KANBUDB_INT32, KANBUDB_STRING, KANBUDB_INT32};
  if (db_create_table(db, "items", col_names, col_types, 3, "id") != KANBUDB_OK) {
    db_close(db); return 0;
  }

  /* Insert data */
  unsigned char buf[256];
  for (int i = 0; i < 50; i++) {
    char name[16];
    snprintf(name, sizeof(name), "item_%d", i);
    *(int*)buf = i + 1;
    unsigned int nlen = (unsigned int)strlen(name);
    memcpy(buf + 4, &nlen, 4);
    memcpy(buf + 8, name, nlen);
    *(int*)(buf + 8 + nlen) = (i + 1) * 10;
    int len = 8 + (int)nlen + 4;
    char key[16];
    snprintf(key, sizeof(key), "k%d", i);
    db_put(db, "items", key, strlen(key) + 1, buf, (size_t)len);
  }

  /* Concurrent sort queries */
  pthread_t readers[NUM_READERS];
  reader_ctx_t rctx[NUM_READERS];
  int all_ok = 1;

  for (int i = 0; i < NUM_READERS; i++) {
    rctx[i].db = db;
    rctx[i].thread_id = i;
    rctx[i].errors = 0;
    pthread_create(&readers[i], NULL, reader_func, &rctx[i]);
  }

  for (int i = 0; i < NUM_READERS; i++) {
    pthread_join(readers[i], NULL);
    if (rctx[i].errors > 0) all_ok = 0;
  }

  db_close(db);
  cleanup();
  return all_ok;
}

int main(void) {
  printf("thread safety tests:\n");
  TEST(concurrent_read_write);
  TEST(concurrent_sort_queries);
  printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
  return tests_failed > 0 ? 1 : 0;
}
```

- [ ] **Step 2: Add test target to CMakeLists.txt**

In `CMakeLists.txt`, after the `test_e2e` block (around line 108), add:

```cmake
  add_executable(test_thread_safety test/unit/test_thread_safety.c)
  target_include_directories(test_thread_safety PRIVATE src/util src/storage src)
  target_link_libraries(test_thread_safety kanbudb_static Threads::Threads)
  add_test(NAME test_thread_safety COMMAND test_thread_safety)
```

- [ ] **Step 3: Build and run the new test**

```bash
cmake -S . -B build && cmake --build build/ && rm -f /tmp/kanbudb_test_thread* && ./build/test_thread_safety
```

Expected: Both tests pass with no crashes or data corruption.

---

## Task 7: Run full test suite and fix any issues

**Files:**
- None (verification step)

- [ ] **Step 1: Rebuild and run all 16 tests**

```bash
cmake --build build/ && rm -f /tmp/kanbudb_test_* && cd build && ctest --output-on-failure
```

Expected: All 16 tests pass (15 existing + 1 new thread_safety).

- [ ] **Step 2: Run test binary directly to verify no leftover file issues**

```bash
rm -f /tmp/kanbudb_test_query* && ./build/test_query
```

Expected: All 8 query tests pass.

---

## Commit

```bash
git add CMakeLists.txt src/core/db.h src/core/db.c src/query/query_builder.c src/storage/sstable.c test/unit/test_thread_safety.c docs/superpowers/specs/2026-06-12-thread-safety-design.md
git commit -m "feat: add thread safety via single-writer multiple-reader lock

- Add pthread_rwlock_t to db_t for db-level concurrency protection
- Write-lock all write operations (put, delete, create_table, close, fts)
- Read-lock all read operations (get, fts_search)
- Fix global sort_ctx race in query_builder.c using qsort_r
- Fix CRC32 lazy init race in sstable.c using __attribute__((constructor))
- Add multi-threaded stress test for concurrent read/write"
```
