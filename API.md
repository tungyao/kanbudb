# KanbuDB Embedded Database — AI API Reference

## Project Info

- Language: C99
- Build: CMake
- Header: `include/db.h`
- Single-file dist: `dist/kanbudb.h` + `dist/kanbudb.c` (via `make amalgamate`)
- Link: `-lkanbudb_static` or `-lkanbudb_shared`
- License: MIT

---

## Types

```c
// Opaque handles
typedef struct kanbudb_db     db_t;             // Database instance
typedef struct query_builder_t query_builder_t; // Query builder
typedef struct result_set_t    result_set_t;    // Query/FTS results

// Column types for table schema
typedef enum {
  KANBUDB_INT32,  KANBUDB_INT64,  KANBUDB_FLOAT,
  KANBUDB_DOUBLE, KANBUDB_STRING, KANBUDB_BLOB,  KANBUDB_BOOL
} kanbudb_col_type_t;

// Fsync durability modes
typedef enum {
  KANBUDB_FSYNC_NONE,      // No fflush, fastest
  KANBUDB_FSYNC_PERIODIC,  // fflush periodically
  KANBUDB_FSYNC_ALWAYS     // fflush every write
} kanbudb_fsync_mode_t;

// Database config (pass NULL for defaults)
typedef struct {
  kanbudb_fsync_mode_t fsync_mode;       // default: PERIODIC
  size_t              cache_size;        // 0 = auto
  size_t              memtable_size;     // default: 4MB
  int                 compaction_threads; // default: 1
} db_config_t;

// FTS index options
typedef struct {
  int         enable_stemming;
  int         enable_stop_words;
  const char *language;                  // "english" etc
} fts_options_t;
```

**Error codes** (all functions return int):

| Constant                | Value | Meaning          |
|-------------------------|-------|------------------|
| `KANBUDB_OK`             | 0     | Success          |
| `KANBUDB_ERR_OOM`        | -1    | Out of memory    |
| `KANBUDB_ERR_NOTFOUND`   | -2    | Key not found    |
| `KANBUDB_ERR_EXISTS`     | -3    | Already exists   |
| `KANBUDB_ERR_CORRUPT`    | -4    | Corrupt data     |
| `KANBUDB_ERR_IO`         | -5    | I/O error        |
| `KANBUDB_ERR_INVAL`      | -6    | Invalid argument |
| `KANBUDB_ERR_BUSY`       | -7    | Busy             |

---

## Functions

### Lifecycle

```c
// Open or create a database. config may be NULL for defaults.
// Creates <path>.wal and <path>.lsm files.
int db_open(const char *path, const db_config_t *config, db_t **out);

// Close database, flush all data, free resources.
int db_close(db_t *db);

// Return the last error code set by any operation.
int db_last_error(db_t *db);

// Convert error code to human-readable string.
const char *db_error_string(int err);
```

### Schema

```c
// Create a table. primary_key is the column name used as key.
// num_columns is the count of col_names/col_types arrays.
int db_create_table(db_t *db, const char *table_name,
                    const char **col_names, const kanbudb_col_type_t *col_types,
                    int num_columns, const char *primary_key);
```

### CRUD

```c
// Insert or update a key-value pair under the given table.
// key is a byte string; value is arbitrary binary data.
int db_put(db_t *db, const char *table,
           const char *key, size_t key_len,
           const void *value, size_t value_len);

// Retrieve a value by key. *value points to internal memory —
// copy immediately, invalidated by next write/flush.
int db_get(db_t *db, const char *table,
           const char *key, size_t key_len,
           void **value, size_t *value_len);

// Delete a key. Returns KANBUDB_ERR_NOTFOUND if missing.
int db_delete(db_t *db, const char *table,
              const char *key, size_t key_len);
```

### Query Builder (fluent API)

```c
// Create a new query builder for the given table.
// Returns NULL on OOM.
query_builder_t *db_query(db_t *db, const char *table);

// Add filter condition: column op value.
// op is a string like "=", ">", "<", ">=", "<=", "!=".
int qb_filter(query_builder_t *qb, const char *column,
              const char *op, const void *value);

// Set sort column and direction. ascending: 0=desc, 1=asc.
int qb_sort(query_builder_t *qb, const char *column, int ascending);

// Limit max rows returned.
int qb_limit(query_builder_t *qb, int limit);

// Add join. on_local and on_foreign are column names.
int qb_join(query_builder_t *qb, const char *table,
            const char *on_local, const char *on_foreign);

// Execute query. Returns result set or NULL.
result_set_t *qb_exec(query_builder_t *qb);

// Free query builder (does not free result set).
void qb_destroy(query_builder_t *qb);
```

### Result Set

```c
// Advance to next row. Returns 1 if valid, 0 if done.
int rs_next(result_set_t *rs);

// Get column data at 0-based index.
// *data points to internal memory — copy immediately.
int rs_get_column(result_set_t *rs, int col, void **data, size_t *len);

// Get column type at 0-based index.
kanbudb_col_type_t rs_get_column_type(result_set_t *rs, int col);

// Return number of columns.
int rs_num_columns(result_set_t *rs);

// Close and free result set.
void rs_close(result_set_t *rs);
```

### Full-Text Search

```c
// Create FTS index on a table column. opts may be NULL for defaults.
int db_fts_create_index(db_t *db, const char *table, const char *column,
                        const fts_options_t *opts);

// Search indexed column. query is Lucene-like syntax:
//   "hello world"      — phrase match
//   hello~2            — fuzzy (edit distance 2)
//   hello AND world    — boolean AND
//   hello OR world     — boolean OR
//   hello NOT world    — boolean NOT
//   title:hello        — field-specific
//   hello^2.5          — boost
// Returns result set with columns: doc_id (INT64), score (DOUBLE).
// *out is NULL on error or no results.
int db_fts_search(db_t *db, const char *table, const char *column,
                  const char *query, result_set_t **out);

// Drop FTS index on a column.
int db_fts_drop_index(db_t *db, const char *table, const char *column);
```

---

## Architecture (data flow)

```
db_put() → WAL (append) → LSM memtable (skip list)
                               │
                      memtable full?
                               │
                          flush → SSTable
                               │
                     compaction → B+tree (hot data)

db_get() → LSM memtable (fast) → B+tree (fallback)

db_fts_search() → query parser → FST inverted index → BM25 ranker → result set
```

## Internal modules

| Module | File(s) | Role |
|--------|---------|------|
| Arena allocator | `src/util/arena.{h,c}` | Block-list allocator, O(1) alloc, batch free |
| Page cache | `src/util/page_cache.{h,c}` | Hash + LRU, 4KB pages, dirty tracking |
| FST dictionary | `src/util/fst.{h,c}` | 256-ary trie, prefix + Levenshtein fuzzy search |
| WAL | `src/storage/wal.{h,c}` | Binary log, magic/version header, replay callback |
| LSM | `src/storage/lsm.{h,c}` | Skip-list memtable (level 12), auto-flush |
| B+tree | `src/storage/btree.{h,c}` | Order 16, split-on-full, leaf-linked cursor |
| Compaction | `src/storage/compaction.{h,c}` | LSM→B+tree merge (v1 stub) |
| Tokenizer | `src/fts/tokenizer.{h,c}` | Unicode split, Snowball-like stemmer, stopwords |
| FTS Index | `src/fts/index.{h,c}` | Inverted index wrapping FST |
| FTS Parser | `src/fts/parser.{h,c}` | Lucene-like query syntax parser |
| FTS Ranker | `src/fts/ranker.{h,c}` | BM25 scoring |
| Query Builder | `src/query/query_builder.{h,c}` | Fluent query API + result set |
| Core DB | `src/core/db.{c,h}` | Orchestration, table metadata |

## Test commands

```bash
# Build
cmake -B build && cmake --build build

# Run all tests
cd build && ctest --output-on-failure

# Run single test
make test_db && ./test_db

# Run benchmark
make bench_basic && ./bench_basic

# Generate single-file distribution
make amalgamate
# → dist/kanbudb.h + dist/kanbudb.c
```

## Constraints

- **Single-writer, multi-reader**: No concurrent write locking (v1)
- **Eventual consistency**: Async writes via WAL + LSM flush
- **No ACID transactions**: No rollback, no cross-op atomicity
- **No SQL**: Custom C API only
- **Value lifetime**: Pointers from `db_get` / `rs_get_column` are valid until next mutable operation. Copy immediately for long-lived references.
