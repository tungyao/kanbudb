# Thread Safety: Single-Writer Multiple-Reader Concurrency Model

**Date:** 2026-06-12
**Status:** Approved
**Scope:** Engine layer thread safety for KanbuDB

## Problem

KanbuDB has zero thread synchronization primitives. Concurrent access from multiple threads causes data corruption in:

- **Global `sort_ctx`** (`query_builder.c:293`): Two concurrent `qb_exec()` calls overwrite each other's sort context, producing garbled results.
- **CRC32 lazy init** (`sstable.c:14-32`): Two threads both see `crc32_table_init == 0` and concurrently write to `crc32_table[]`, producing corrupt checksums.
- **WAL** (`wal.c:89-113`): `wal_append()` does non-atomic `seq++` + interleaved `fwrite()`. Two concurrent writes corrupt the WAL.
- **LSM memtable** (`lsm.c:101-179`): Concurrent `memtable_put()` corrupts skip list pointers.
- **LSM flush vs. write** (`lsm.c:325-331`): `lsm_flush()` destroys memtable while another thread writes to it.
- **db_put/db_get** (`db.c:569-612`): No lock protection. Concurrent `db_put()` and `db_get()` see inconsistent state.
- **db_close** (`db.c:450-485`): Destroys resources while concurrent operations use them (use-after-free).

## Solution: db-level Read-Write Lock

Add a `pthread_rwlock_t` to `db_t`. Write operations take the write lock; read operations take the read lock. This provides:
- Read-read concurrency
- Write serialization
- Read-write exclusion

### Lock Acquisition Map

| Function | Lock Type | Rationale |
|----------|-----------|-----------|
| `db_open()` | None | Initialization phase, single-threaded |
| `db_put()` | Write | Modifies WAL + LSM |
| `db_get()` | Read | Only reads LSM + B-tree |
| `db_delete()` | Write | Modifies WAL + LSM |
| `db_create_table()` | Write | Modifies schema |
| `db_fts_create_index()` | Write | Modifies FTS index |
| `db_fts_drop_index()` | Write | Modifies FTS index |
| `db_fts_search()` | Read | Only reads FTS index |
| `db_close()` | Write | Destroys all resources |
| `qb_exec()` | Read | Scans LSM + B-tree, sorts results |

Internal functions (`db_flush_memtable`, `db_compact_sstables`, `db_checkpoint`) do not need independent locking because they are only called from contexts that already hold the write lock.

## Changes

### 1. `src/core/db.h` — Add rwlock to db_t

```c
#include <pthread.h>

struct kanbudb_db {
  char*                path;
  struct kanbudb_lsm*  lsm;
  struct kanbudb_btree* btree;
  struct kanbudb_wal*   wal;
  kanbudb_table_t      tables[KANBUDB_MAX_TABLES];
  int                  num_tables;
  db_config_t          config;
  int                  last_error;
  kanbudb_fts_index_t* fts_index;
  pthread_rwlock_t     rwlock;          // NEW
};
```

### 2. `src/core/db.c` — Lock/unlock in public API

```c
// db_open: initialize rwlock
pthread_rwlock_init(&db->rwlock, NULL);

// db_put: write lock
int db_put(...) {
  pthread_rwlock_wrlock(&internal->rwlock);
  // ... existing logic ...
  pthread_rwlock_unlock(&internal->rwlock);
  return rc;
}

// db_get: read lock
int db_get(...) {
  pthread_rwlock_rdlock(&internal->rwlock);
  // ... existing logic ...
  pthread_rwlock_unlock(&internal->rwlock);
  return rc;
}

// db_close: write lock, then destroy
int db_close(db_t* db) {
  pthread_rwlock_wrlock(&internal->rwlock);
  // ... flush, save, checkpoint, destroy resources ...
  pthread_rwlock_unlock(&internal->rwlock);
  pthread_rwlock_destroy(&internal->rwlock);
  free(internal);
}
```

### 3. `src/query/query_builder.c` — Eliminate global sort_ctx

Replace the file-scope global `static sort_ctx_t sort_ctx` with a thread-safe approach using `qsort_r()` (POSIX extension, available on Linux and macOS).

```c
// Before (UNSAFE):
static sort_ctx_t sort_ctx;
static int sort_entry_cmp(const void* a, const void* b) {
  // reads from global sort_ctx
}

// After (SAFE):
static int sort_entry_cmp(const void* a, const void* b, void* ctx) {
  sort_ctx_t* sctx = (sort_ctx_t*)ctx;
  // reads from sctx instead of global
}

// In qb_exec():
sort_ctx_t sort_ctx_local;  // stack-local, not global
sort_ctx_local.row_data = ...;
sort_ctx_local.row_lens = ...;
// ...
qsort_r(entries, count, sizeof(sort_entry_t), sort_entry_cmp, &sort_ctx_local);
```

### 4. `src/storage/sstable.c` — Eager CRC32 initialization

Replace lazy initialization with `__attribute__((constructor))` to ensure the CRC32 table is populated before any thread can use it.

```c
// Before (RACY):
static int crc32_table_init = 0;
static void crc32_init_table(void) { ... crc32_table_init = 1; }
static uint32_t crc32_bytes(...) {
  if (!crc32_table_init) crc32_init_table();  // RACE
  ...
}

// After (SAFE):
__attribute__((constructor))
static void crc32_init_table(void) { ... }
// Remove the crc32_table_init flag and the check in crc32_bytes
static uint32_t crc32_bytes(...) {
  // Table is always initialized; no check needed
  ...
}
```

### 5. `CMakeLists.txt` — Link pthread

Add `-lpthread` to the shared and static library link flags.

## Thread Safety Guarantees

After this change:
- Multiple threads can call `db_get()` concurrently (read-read concurrency).
- Multiple threads can call `qb_exec()` concurrently (read-read concurrency).
- `db_put()` and `db_delete()` are serialized with each other and with reads.
- `db_close()` waits for all readers to finish before destroying resources.
- `sort_ctx` race is eliminated (stack-local per query).
- CRC32 lazy init race is eliminated (eager initialization at load time).

## What This Does NOT Guarantee

- **No transaction isolation**: Reads see a snapshot at the time the read lock is acquired, but there is no MVCC or snapshot isolation.
- **Value pointer lifetime**: `db_get()` still returns pointers to internal memory that may be invalidated by a subsequent write. A future `db_get_copy()` can address this.
- **No deadlock protection**: The API is designed so that each call acquires at most one lock. Nested locking is not possible through the public API.

## Testing

1. Existing C tests (15 tests) must continue to pass.
2. Add a multi-threaded stress test: N writer threads + M reader threads running concurrently for 1000 iterations. Verify no crashes, no data corruption (all written values can be read back correctly).
3. Verify `sort_ctx` fix: two concurrent `qb_exec()` with different sort columns produce correct results.

## Build Compatibility

- `pthread_rwlock_t` is POSIX. Available on Linux (glibc), macOS, FreeBSD.
- `qsort_r()` is GNU/BSD extension. Available on Linux (glibc) and macOS. Not available on musl libc (use `qsort` + mutex fallback if needed).
- `__attribute__((constructor))` is GCC/Clang. Not available on MSVC (use `DllMain` or `#pragma init_seg` for Windows).
- Windows support is out of scope for this change.
