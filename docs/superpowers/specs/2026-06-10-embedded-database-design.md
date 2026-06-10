# Embedded Database Design Specification

**Date**: 2026-06-10  
**Project**: KanbuDB Embedded Database  
**Language**: C/C++ (single-header library)  
**Target**: Small-scale (<100MB), single-process embedded use

---

## 1. Overview

A lightweight, single-process embedded database library written in C with a custom C API. Features hybrid LSM-tree (writes) + B+tree (reads) storage, advanced full-text search with Lucene-like query syntax, and zero-dependency core with optional ICU/zstd/Roaring integrations.

**Primary Use Case**: Structured data with relations + full-text search for embedded/edge applications.

---

## 2. Architecture

### 2.1 Module Breakdown

| Module | Responsibility |
|--------|----------------|
| **Core** | `db_open`, `db_close`, configuration, lifecycle |
| **Storage** | LSM (memtable, SSTable, compaction), B+tree, WAL |
| **Query** | Query builder API, executor, optimizer |
| **FTS** | Tokenizer pipeline, inverted index, parser, ranker |
| **Util** | Arena allocator, page cache, Roaring bitmap, FST |

### 2.2 Deployment Model

- Single-header C library (amalgamation build)
- Linked directly into host process (Approach A)
- No separate server process, no IPC
- Static/shared library + header-only distribution

---

## 3. Data Model

- **Tables** with typed columns: `int32`, `int64`, `float`, `double`, `string`, `blob`, `bool`
- **Primary key** (required) + **secondary indexes**
- **Foreign keys** for join support
- **Schema** defined at table creation (not schemaless)

---

## 4. C API (snake_case, opaque pointers)

```c
// Lifecycle
db_t* db_open(const char* path, const db_config_t* config);
void db_close(db_t* db);

// Key-Value
int db_put(db_t* db, const char* table, const void* key, size_t key_len, const void* value, size_t val_len);
int db_get(db_t* db, const char* table, const void* key, size_t key_len, void** out_value, size_t* out_len);
int db_delete(db_t* db, const char* table, const void* key, size_t key_len);

// Query Builder
query_builder_t* db_query(db_t* db);
query_builder_t* qb_from(query_builder_t* qb, const char* table);
query_builder_t* qb_filter(query_builder_t* qb, const char* expr);
query_builder_t* qb_sort(query_builder_t* qb, const char* field, int desc);
query_builder_t* qb_limit(query_builder_t* qb, size_t limit);
query_builder_t* qb_join(query_builder_t* qb, const char* table, const char* on);
result_set_t* qb_exec(query_builder_t* qb);

// Full-Text Search
result_set_t* db_fts_search(db_t* db, const char* table, const char* column, const char* query, const fts_options_t* opts);

// Result Set (cursor-based, zero-copy)
int rs_next(result_set_t* rs);
const void* rs_get_column(result_set_t* rs, int idx, size_t* out_len);
int rs_get_column_type(result_set_t* rs, int idx);
void rs_close(result_set_t* rs);
```

**Error Handling**: Return codes (`0` = success, negative = error). Extended error info via `db_last_error(db)`.

---

## 5. Storage Engine (Hybrid LSM + B+tree)

### 5.1 Write Path (LSM)
- **Memtable**: Skip list, ~4MB threshold
- **SSTable**: Sorted, blocked (4KB), bloom filter, checksums
- **Flush**: Background thread, non-blocking
- **WAL**: Append-only, fsync modes (none/periodic/always), crash recovery replays WAL

### 5.2 Read Path (B+tree)
- **Pages**: 4KB, copy-on-write for crash safety
- **Point queries**: O(log n)
- **Range scans**: Sequential page reads
- **Hot data promotion**: Compaction promotes frequently accessed data to B+tree

### 5.3 Compaction
- **Strategy**: Hybrid tiered + leveled
- **Triggers**: Size ratio, write amplification, schedule
- **Background**: Dedicated compaction thread

### 5.4 Page Cache
- **LRU with sharding** (~10-20% of DB size)
- **Zero-copy reads** via `mmap` where available

---

## 6. Full-Text Search

### 6.1 Tokenizer Pipeline
1. Unicode normalization (NFC)
2. Language detection (fast heuristic)
3. Script-specific tokenization (ICU or custom fallback)
4. Stemming (Snowball algorithms)
5. Stopword removal (configurable per-language)
6. Synonym expansion (configurable dictionary)

### 6.2 Index Structure
- **Inverted index**: `term → [doc_id, positions[]]` (positions enable phrase queries)
- **Roaring bitmaps** for doc_id sets (fast AND/OR/NOT)
- **FST** (Finite State Transducer) for term dictionary (prefix, autocomplete)
- **Levenshtein automaton** for fuzzy matching (max edit distance 2)

### 6.3 Query Parser (Lucene-like syntax)
- `field:"exact phrase"` — phrase query
- `term~2` — fuzzy (edit distance 2)
- `term^boost` — term boost
- `AND` / `OR` / `NOT` — boolean
- `field:[a TO z]` — range

### 6.4 Ranking
- BM25 + field-length normalization
- Position proximity scoring
- Custom per-field/document boosts

---

## 7. Transactions & Consistency

- **Model**: Eventual consistency (async writes, best effort)
- **Durability**: Configurable WAL fsync (none/periodic/always)
- **Isolation**: Snapshot reads from B+tree; writes go to LSM
- **No ACID**: No cross-operation transactions, no rollback

---

## 8. Build System & Project Structure

```
src/
  core/        # db_open, db_close, config
  storage/     # lsm, btree, wal, compaction
  query/       # query_builder, executor, optimizer
  fts/         # tokenizer, index, parser, ranker
  util/        # arena, page_cache, bitmap, fst
include/
  db.h         # public API (single header for distribution)
test/
  unit/        # per-module tests
  integration/ # end-to-end tests
  bench/       # microbenchmarks
```

- **Build**: CMake + amalgamation (single-header distribution)
- **Targets**: Static lib, shared lib, header-only
- **Dependencies**: Zero required. Optional: ICU, zstd, Roaring
- **CI**: GitHub Actions (Linux/macOS/Windows, sanitizers, fuzzing)

---

## 9. Scope & Non-Goals

### In Scope
- Core KV + query + FTS
- Hybrid LSM/B+tree storage
- Advanced tokenizer + query parser
- Single-header C distribution

### Out of Scope (v1)
- Distributed/replicated mode
- SQL parser/execution
- Backup/restore tooling
- Admin UI/metrics endpoint
- Encryption at rest
- Multiple concurrent writers (single-writer, multi-reader)

---

## 10. Success Criteria

1. **Compile** on Linux/macOS/Windows with CMake
2. **Basic KV ops** < 10μs p99 (100MB dataset)
3. **FTS query** < 50ms p99 (1M docs, 100MB index)
4. **Memory** < 50MB RSS for 100MB dataset
5. **Zero crashes** under fuzzing (libFuzzer, 24h)
6. **Single header** < 500KB amalgamated