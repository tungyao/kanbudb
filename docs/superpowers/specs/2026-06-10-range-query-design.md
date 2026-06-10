# MK+Tree: Range Query Engine Design

**Date**: 2026-06-10
**Project**: KanbuDB
**Language**: C11
**Status**: Design v1

---

## 1. Overview

MK+Tree (Monkey-Kanbu-Tree) is a column-aware predicate pushdown query engine built on top of kanbudb's existing hybrid LSM + B-tree storage. It enables `qb_filter` with range operators (`>`, `<`, `>=`, `<=`, `!=`), `qb_sort`, `qb_limit`, and `qb_join` to actually execute and return real rows.

The design follows a **one-pass merge + late materialization** model: data flows from both B-tree cursor and LSM memtable iterate through a single merge pipeline, with predicate pushdown and column projection applied before materialization.

---

## 2. Architecture

### 2.1 Data Flow

```
┌─────────────────────────────────────────────────────────┐
│  Query Plan (qb_exec)                                    │
│  1. Resolve table schema                                 │
│  2. Identify filter column index + type                  │
│  3. Determine scan strategy                              │
└────────────────────┬────────────────────────────────────┘
                     │
         ┌───────────┴───────────┐
         │                       │
   ┌─────▼──────┐        ┌──────▼──────┐
   │ B-tree Scan │        │ LSM Scan    │
   │ (cursor    )│        │ (memtable   │
   │  seek+next )│        │  iterate)   │
   └─────┬──────┘        └──────┬──────┘
         │                      │
         └──────────┬───────────┘
                    │
            ┌───────▼────────┐
            │ Merge + Dedup  │  ← by key
            │ + Predicate    │  ← filter pushdown
            │   Pushdown     │
            └───────┬────────┘
                    │
            ┌───────▼────────┐
            │ Late           │  ← project only needed columns
            │ Materialization│
            └───────┬────────┘
                    │
            ┌───────▼────────┐
            │ Sort (Top-N)   │  ← qb_sort
            └───────┬────────┘
                    │
            ┌───────▼────────┐
            │ Limit          │  ← qb_limit
            └───────┬────────┘
                    │
            ┌───────▼────────┐
            │ Join           │  ← nested-loop lookup
            │ (if has_join)  │
            └───────┬────────┘
                    │
             result_set_t
```

### 2.2 Row Format (Compact Binary Encoding)

Row values are tightly-packed column data in schema order. Since the schema defines column types, no type tags are needed:

```
for each column (in schema order):
  [data: N bytes]    ← INT32=4, INT64=8, FLOAT=4, DOUBLE=8, BOOL=1
  STRING / BLOB:
    [len: 4 bytes]   ← little-endian uint32
    [data: len bytes]
```

Column offsets are computed from the schema once; all rows in the same table share the same offset map. This changes the contract of `db_put`: values written to tables with a schema must use this encoding.

This enables:
- Late materialization: skip parsing columns not in filter/SELECT
- Constant-time column offset lookup (precomputed from schema)
- SIMD-friendly: contiguous int32 array

---

## 3. Component Design

### 3.1 Column Value Comparator

```
compare_column(col_data, col_type, op, literal_value, literal_type)
  → returns 1 (pass), 0 (fail)
```

Supports all `kanbudb_col_type_t` types. For `STRING`, uses `strcmp`. For numeric types, casts to `int64_t`/`double` for comparison. Operators: `=`, `>`, `<`, `>=`, `<=`, `!=`.

### 3.2 Row Parser

```
row_column_offset(schema, col_index)
  → byte offset of column in row value blob
```

Precompute column offsets from schema: iterate columns accumulating fixed sizes (4/8/1 bytes), for STRING/BLOB add 4 bytes (the length prefix) and note the offset is variable. For fixed-width columns, the offset is constant. For variable-width columns, offsets are determined per-row by walking past preceding variable-length columns.

```
row_extract(raw_value, val_len, col_index, col_type)
  → pointer to column data within raw_value
  → (for STRING/BLOB also returns length)
```

Zero-copy: returns a pointer into the raw value buffer. For fixed-width types this is a constant offset. For STRING/BLOB, reads the 4-byte length prefix and returns pointer to data bytes.

### 3.3 B-tree Scanner

Uses existing `btree_cursor_create`/`btree_cursor_seek`/`btree_cursor_next`:

1. `btree_cursor_seek(cur, NULL, 0)` → position at first key
2. Loop `btree_cursor_next(cur, &kv)` → get each KV pair
3. Parse value → check filter predicate → emit to result buffer

If no filter on primary key, this is a full table scan. Cursor is O(log n) to position + O(k) to iterate k rows.

### 3.4 LSM Scanner

Uses existing `memtable_iterate` (no cursor API on memtable):

1. Check active memtable + flushing memtable
2. `memtable_iterate(mt, cb, ctx)` → callback for each entry
3. Skip deleted entries
4. Parse value → check filter → emit

Since LSM and B-tree may overlap (same key in both), the merge step deduplicates by keeping the entry with the highest seq (or LSM preference for in-memory freshness).

Actually simpler: LSM has newest data, B-tree has flushed data. For each key, prefer LSM value. This is the same precedence as `db_get`.

### 3.5 Merge + Dedup

Simple approach: scan LSM first, build a hash set of emitted keys (or mark keys pending), then scan B-tree, skipping any key already seen. This avoids O(n²) merge.

Optimized approach for sorted output: two-pointer merge (B-tree cursor is naturally sorted; LSM iterate is also skip-list order → sorted). But since we don't have table-scoped keys, just use hash set.

### 3.6 Sort (Top-N Heap Sort)

If `has_sort`:
1. Collect all matching rows into a buffer (struct { row_data, parsed_sort_col })
2. `qsort` with comparator on sort column (ascending/descending)
3. If `has_limit`, use a bounded min-heap/max-heap of size N instead of full sort

For `has_limit` + `has_sort`: use partial sort (heap) → O(n log N) instead of O(n log n).

### 3.7 Join (Nested Loop)

If `has_join`:
1. For each row in the primary result set
2. Extract `on_local` column value (convert to string)
3. Call `db_get(internal, join_table, key, key_len, &val, &val_len)` using the foreign table
4. Merge column values into a combined row
5. The combined row schema = primary cols + foreign cols (excluding join key dedup)

For performance, batch lookups into a single LSM/B-tree probe. `db_get` already does LSM → B-tree fallback.

### 3.8 Limit Pushdown

Without sort: stop scanning after `limit` rows pass filter.
With sort: use bounded heap, stop after `limit` rows in heap.

---

## 4. Data Structures

### 4.1 result_set_t Extension

```c
struct result_set_t {
  int               num_rows;
  int               current;
  int               num_cols;
  kanbudb_col_type_t col_types[KANBUDB_MAX_COLS];
  char*             col_names[KANBUDB_MAX_COLS];
  int               is_fts;
  uint64_t*         doc_ids;
  double*           scores;
  /* MK+Tree extensions */
  void**            row_data;     /* array of row value blobs */
  size_t*           row_lens;     /* length of each row blob */
  int               capacity;     /* allocated capacity */
};
```

### 4.2 Internal Row Metadata

```c
typedef struct {
  size_t col_offsets[KANBUDB_MAX_COLS]; /* byte offset of each column in raw value */
  int    num_cols;
} row_meta_t;
```

Computed once per distinct row shape, not per row (all rows in a table share the same column layout, so column offsets are the same).

### 4.3 Row Buffer (for Sort)

```c
typedef struct {
  void*  data;
  size_t len;
  double sort_key; /* pre-extracted sort column value as double */
  int    row_idx;  /* index into result_set row arrays */
} sort_row_t;
```

---

## 5. Query Execution Plan (qb_exec)

```
1. Validate qb, lookup table schema
2. Determine filter column index + type
3. Allocate result_set_t
4. Set col_names/col_types from schema
5. If has_filter:
   a. B-tree scan: cursor seek → while next → parse → eval filter → emit
   b. LSM scan: memtable iterate → skip deleted → parse → eval filter → emit
   c. Dedup by key (hash set of keys seen)
6. If no filter (full table scan): same scan but skip predicate eval
7. If has_sort: sort or heap-sort collected rows
8. If has_limit: truncate or bounded during collection
9. If has_join: for each collected row, lookup foreign table, build combined row
10. Set rs->num_rows, rs->current = -1
11. Return rs
```

---

## 6. API Compatibility

No changes to public API signatures. The following already-defined functions become functional:

| Function | Current | After |
|----------|---------|-------|
| `qb_filter(qb, col, op, val)` | stores params only | stores params only (no change) |
| `qb_sort(qb, col, asc)` | stores params only | stores params only (no change) |
| `qb_limit(qb, n)` | stores params only | stores params only (no change) |
| `qb_join(qb, tbl, local, foreign)` | stores params only | stores params only (no change) |
| `qb_exec(qb)` | returns empty set | executes full pipeline |
| `rs_next(rs)` | stub | walks row_data array |
| `rs_get_column(rs, col, &data, &len)` | stub | returns column from parsed row |

---

## 7. Error Handling

| Scenario | Behavior |
|----------|----------|
| Filter column not found | qb_exec returns empty set (no error) |
| Join table not found | KANBUDB_ERR_NOTFOUND |
| Join column not found | KANBUDB_ERR_NOTFOUND |
| OOM during row collection | return KANBUDB_ERR_OOM |
| Corrupt row value | skip row, continue |
| Sort column not found | treat as no sort |

---

## 8. Testing Strategy

### 8.1 Unit Tests

| Test | Description |
|------|-------------|
| `range_int32` | `age > 18`, `age < 65`, `age >= 18`, `age <= 65` |
| `range_float` | `price > 10.5` |
| `range_string` | `name > "m"` (lexicographic) |
| `eq_filter` | `status = "active"` |
| `neq_filter` | `status != "deleted"` |
| `sort_asc` | `qb_sort("age", 1)` → ascending |
| `sort_desc` | `qb_sort("age", 0)` → descending |
| `limit` | `qb_limit(10)` → at most 10 rows |
| `limit_sort` | `qb_limit(5) + qb_sort("score", 0)` → top 5 |
| `join` | `qb_join(qb, "orders", "id", "user_id")` → combined rows |
| `no_filter` | `qb_exec` → all rows in table |
| `empty_table` | no data → empty result set |
| `no_match` | filter matches nothing → empty result set |

### 8.2 Integration Tests

| Test | Description |
|------|-------------|
| `e2e_range` | put 100 rows, range query, verify ordering + count |
| `e2e_join` | two tables with FK relationship, join query |
| `e2e_sort_limit_join` | combined query with all features |
| `large_result` | 10000 rows, sort + limit |

---

## 9. Implementation Order

1. **Row serialization** (`db_put` serialization + `qb_exec` deserialization)
2. **B-tree scanner** (cursor iterate → parse → collect)
3. **Filter predicate evaluation** (=, >, <, >=, <=, !=)
4. **LSM scanner** (memtable iterate → hook into pipeline)
5. **Merge + dedup** (hash set)
6. **Sort** (qsort / heap sort)
7. **Limit** (bounded collection / pushdown)
8. **Join** (nested loop lookup)
9. **Result set iteration** (rs_next / rs_get_column returns real data)
10. **Tests**

Items 1–5 are the minimum viable range query. Items 6–10 can be parallelized.

---

## 10. Performance Characteristics

| Operation | Complexity | Notes |
|-----------|------------|-------|
| Full table scan | O(n) | N = total rows in B-tree + LSM |
| Range filter | O(n) | no index, must scan all |
| Range filter + limit | O(k) where k=min(n, limit) | early stop |
| Sort | O(n log n) / O(n log k) | full sort / top-N heap |
| Join | O(n × lookup) | nested loop, each lookup is O(log n) |
| Dedup | O(n) | hash set |

---

## 11. Non-Goals (v1)

- Secondary indexes for fast range seek
- Multi-column filters (only one filter column)
- OR/NOT filter logic (only implicit AND via single op)
- Query optimizer cost model
- Parallel scan
- Mutable result set (read-only cursor)
