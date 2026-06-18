# Multi-Process Shared WAL Design

## Overview

Enable multiple processes to share the same kanbudb database file, where one process writes and other processes read in near real-time (millisecond latency). Uses mmap-based shared WAL for zero-copy data sharing between processes.

## Requirements

- **Near real-time reads**: Reader processes see writes within ~10ms
- **Multiple program instances**: Same program binary, some instances write, others read
- **Shared database file**: All processes operate on the same on-disk database
- **Cross-platform**: Linux, macOS, Windows
- **Single writer**: Maintain existing single-writer model (no concurrent writes)

## Architecture

```
Process A (Writer)                Process B (Reader)
┌──────────────────────┐        ┌──────────────────────┐
│  db_put()            │        │  db_get()            │
│    ↓                 │        │    ↓                 │
│  WAL (mmap RW)       │←──────→│  WAL (mmap RO)       │
│  [header | entries]  │  shared │  [header | entries]  │
│    ↓                 │        │    ↓                 │
│  LSM memtable        │        │  local B+tree        │
│    ↓ (flush)         │        │    ↑ (load)          │
│  SSTable files       │────────│  SSTable files       │
│    ↓                 │        │                     │
│  .shared metadata    │←──────→│  .shared metadata    │
└──────────────────────┘        └──────────────────────┘
```

Key insight: SSTables are immutable files safely shared via mmap. WAL is the only mutable file requiring coordination.

## File Formats

### WAL File (mmap-friendly)

```
Offset 0:   [HEADER]  64 bytes (fixed, page-aligned)
              magic:      u64  (0x4845524D4553)
              version:    u32  (1)
              write_pos:  u64  (atomically updated, current write offset)
              data_size:  u64  (total data region size)
              reserved:   padding to 64 bytes

Offset 64:  [DATA]    variable length, append-only
              Each entry: seq(u64) + op(u8) + table_id(u64) +
                          key_len(u64) + val_len(u64) + key + val
```

### .shared Metadata File

```c
typedef struct {
    uint64_t magic;          // 0x4B414E4255444200 ("KANBUDb\0")
    uint64_t flushed_seq;    // last seq fully flushed to SSTable
    uint32_t wal_version;    // WAL file version (detects file switches)
    uint32_t reader_count;   // number of active reader processes
} shared_meta_t;             // 16 bytes, atomically written
```

## Writer Flow

### Normal Write (db_put)
1. mmap WAL data region, append serialized entry
2. Atomically update header `write_pos` via `__atomic_store_n()` / `InterlockedExchange64()`
3. Simultaneously write to LSM memtable (unchanged)

### WAL Full → File Switch
1. Create new WAL file (`<path>.wal.N+1`)
2. Copy unflushed entries to new file
3. `rename()` (Linux) / `MoveFileExA()` (Windows) atomic replace
4. Update `.shared.wal_version++`
5. `munmap()` / `UnmapViewOfFile()` old file, mmap new file

### Flush to SSTable
1. Flush memtable → SSTable (unchanged)
2. Update `.shared.flushed_seq = current_seq`
3. Optional: create fresh WAL with only unflushed entries (compaction)

## Reader Flow

### Initial Open (db_open_reader)
1. Load all SSTables into local B+tree (unchanged)
2. Read `.shared` → get `flushed_seq` and `wal_version`
3. mmap WAL file (`PROT_READ, MAP_SHARED` / `FILE_MAP_READ`)
4. Seek to position after `flushed_seq` entries

### Background Poll Thread
Every N ms (default 10ms):
1. Atomically read WAL header `write_pos`
2. If `write_pos > last_known_pos`: read new entries, replay to local B+tree
3. Check `.shared.wal_version`: if changed, munmap + remap new WAL file

## Cross-Platform Abstraction

### Platform API Mapping

| Operation | Linux/macOS | Windows |
|-----------|------------|---------|
| File mapping | `mmap()` | `CreateFileMapping()` + `MapViewOfFile()` |
| Sync | `msync()` | `FlushViewOfFile()` + `FlushFileBuffers()` |
| Unmap | `munmap()` | `UnmapViewOfFile()` + `CloseHandle()` |
| Atomic store | `__atomic_store_n()` | `InterlockedExchange64()` |
| Atomic load | `__atomic_load_n()` | `InterlockedOr64()` |
| File rename | `rename()` | `MoveFileExA(MOVEFILE_REPLACE_EXISTING)` |
| File lock | `flock()` | `LockFileEx()` / `UnlockFileEx()` |

### Abstracted API (platform_mmap.h)

```c
typedef struct kanbudb_mmap_region {
    kanbudb_mmap_handle_t handle;
    void* addr;
    size_t size;
    int fd;              // Linux: file descriptor; Windows: file handle
} kanbudb_mmap_region_t;

int  kanbudb_mmap_open(const char* path, int rw, kanbudb_mmap_region_t* region, size_t size);
int  kanbudb_mmap_sync(kanbudb_mmap_region_t* region);
int  kanbudb_mmap_close(kanbudb_mmap_region_t* region);
```

## API Changes

### New Config Field

```c
typedef struct db_config_t {
    kanbudb_fsync_mode_t fsync_mode;
    size_t cache_size;
    size_t memtable_size;
    int compaction_threads;
    int multi_process;          // NEW: enable multi-process sharing
    int reader_poll_ms;         // NEW: reader polling interval (default 10ms)
} db_config_t;
```

### New Functions

```c
// Open database in read-only mode for reader process
int db_open_reader(const char* path, const db_config_t* config, db_t** out);

// Refresh reader's local state from WAL (called by poll thread or manually)
int db_reader_refresh(db_t* db);
```

### Modified Functions

- `db_open()`: if `config.multi_process`, mmap WAL + load `.shared` metadata
- `db_put()`: after WAL append, atomically update mmap header
- `db_close()`: if `multi_process`, decrement `.shared.reader_count`, cleanup mmap

## Data Consistency

### Atomicity Guarantees
- x86_64: 8-byte aligned `uint64_t` writes are CPU-atomic
- Compiler: `__atomic_store_n()` / `InterlockedExchange64()` prevent reordering
- Reader always sees complete `write_pos`, never partial writes

### Sequence Number Tracking
- Each WAL entry has monotonic `seq`
- `.shared.flushed_seq` marks the boundary between SSTable and WAL data
- Reader uses `seq` to avoid duplicate processing across WAL file switches

### Crash Recovery
- Writer crash: WAL provides recovery (existing mechanism)
- Reader crash: no shared state to clean up (mmap auto-released)
- `.shared.reader_count` prevents WAL compaction while readers active

## Limitations

1. **Single writer**: No concurrent write support (maintains existing model)
2. **Reader latency**: Depends on polling interval (default 10ms)
3. **WAL growth**: Needs compaction mechanism (future optimization)
4. **Platform**: Linux, macOS, Windows (no mmap on some embedded platforms)

## Files to Modify

| File | Changes |
|------|---------|
| `include/db.h` | Add `multi_process`, `reader_poll_ms` to config; add `db_open_reader()`, `db_reader_refresh()` |
| `src/core/db.h` | Add `shared_meta_t`, `wal_mmap_t` structs; reader poll thread |
| `src/core/db.c` | Modify `db_open()`, `db_put()`, `db_close()`; add `db_open_reader()`, `db_reader_refresh()` |
| `src/storage/wal.h` | Add mmap API: `wal_mmap_open()`, `wal_mmap_append()`, `wal_mmap_read()`, `wal_mmap_close()` |
| `src/storage/wal.c` | Implement mmap WAL functions with cross-platform support |
| `src/util/platform_mmap.h` | NEW: Cross-platform mmap abstraction |
| `src/util/platform_mmap.c` | NEW: Linux/Windows mmap implementation |
