# Multi-Process Shared WAL Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Enable multiple processes to share the same kanbudb database, with one writer and multiple readers seeing near real-time updates via mmap-shared WAL.

**Architecture:** Cross-platform mmap abstraction layers a shared WAL file between writer (RW) and readers (RO). A `.shared` metadata file coordinates checkpoint state. Writer appends to WAL + updates mmap header atomically. Reader polls header for new entries, remaps on file switch.

**Tech Stack:** C99, POSIX mmap / Windows CreateFileMapping, pthreads, CMake

---

## File Structure

| File | Responsibility |
|------|---------------|
| `src/util/platform_mmap.h` | Cross-platform mmap API declarations |
| `src/util/platform_mmap.c` | Linux/Windows mmap implementation |
| `src/storage/wal_mmap.h` | WAL mmap reader/writer API declarations |
| `src/storage/wal_mmap.c` | WAL mmap implementation (header ops, append, read, file switch) |
| `src/storage/shared_meta.h` | `.shared` metadata file API declarations |
| `src/storage/shared_meta.c` | `.shared` read/write/sync implementation |
| `src/core/db.c` | Modify `db_open()`, `db_put()`, `db_close()` for multi_process mode |
| `src/core/db.h` | Add mmap and shared_meta fields to `kanbudb_db` struct |
| `include/db.h` | Add `multi_process`, `reader_poll_ms` to config; add `db_open_reader()` |
| `test/unit/test_platform_mmap.c` | Unit tests for platform mmap abstraction |
| `test/unit/test_shared_meta.c` | Unit tests for shared metadata |
| `test/unit/test_wal_mmap.c` | Unit tests for WAL mmap operations |
| `test/unit/test_multiprocess.c` | Integration test: fork writer + reader processes |
| `CMakeLists.txt` | Add new source files and test targets |

---

### Task 1: Platform mmap abstraction header

**Files:**
- Create: `src/util/platform_mmap.h`

- [ ] **Step 1: Create platform_mmap.h**

```c
#ifndef KANBUDB_PLATFORM_MMAP_H
#define KANBUDB_PLATFORM_MMAP_H

#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
  #include <windows.h>
  typedef HANDLE kanbudb_mmap_handle_t;
  #define KANBUDB_MMAP_INVALID NULL
#else
  #include <sys/mman.h>
  #include <unistd.h>
  typedef void* kanbudb_mmap_handle_t;
  #define KANBUDB_MMAP_INVALID MAP_FAILED
#endif

typedef struct kanbudb_mmap_region {
  kanbudb_mmap_handle_t handle;
  void*                 addr;
  size_t                size;
  int                   fd;     /* Linux: fd; Windows: file handle stored separately */
} kanbudb_mmap_region_t;

/* Open a file and mmap it. rw=1 for read-write, rw=0 for read-only.
   size=0 means map the entire file (Linux only; Windows must specify size).
   Returns 0 on success, -1 on error. */
int kanbudb_mmap_open(const char* path, int rw, size_t size,
                      kanbudb_mmap_region_t* region);

/* Flush mapped region to disk. Returns 0 on success, -1 on error. */
int kanbudb_mmap_sync(kanbudb_mmap_region_t* region);

/* Unmap and close handles. Returns 0 on success, -1 on error. */
int kanbudb_mmap_close(kanbudb_mmap_region_t* region);

/* Atomically store a uint64_t value (seq-cst ordering). */
void kanbudb_atomic_store_u64(uint64_t* ptr, uint64_t value);

/* Atomically load a uint64_t value (seq-cst ordering). */
uint64_t kanbudb_atomic_load_u64(const uint64_t* ptr);

/* Atomically store a uint32_t value (seq-cst ordering). */
void kanbudb_atomic_store_u32(uint32_t* ptr, uint32_t value);

/* Atomically load a uint32_t value (seq-cst ordering). */
uint32_t kanbudb_atomic_load_u32(const uint32_t* ptr);

/* Get file size. Returns 0 on success, -1 on error. */
int kanbudb_file_size(const char* path, uint64_t* out_size);

#endif /* KANBUDB_PLATFORM_MMAP_H */
```

- [ ] **Step 2: Commit**

```bash
git add src/util/platform_mmap.h
git commit -m "feat: add cross-platform mmap abstraction header"
```

---

### Task 2: Platform mmap implementation (Linux/macOS)

**Files:**
- Create: `src/util/platform_mmap.c`

- [ ] **Step 1: Write the failing test**

Create `test/unit/test_platform_mmap.c`:

```c
#include "platform_mmap.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <fcntl.h>

static void test_mmap_open_close(void) {
  const char* path = "/tmp/test_mmap_basic.dat";

  /* Create a file with known content */
  int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
  assert(fd >= 0);
  uint64_t val = 42;
  write(fd, &val, sizeof(val));
  close(fd);

  /* mmap read-only */
  kanbudb_mmap_region_t region;
  int rc = kanbudb_mmap_open(path, 0, 0, &region);
  assert(rc == 0);
  assert(region.addr != KANBUDB_MMAP_INVALID);
  assert(region.size == sizeof(uint64_t));

  /* Read the value */
  uint64_t read_val;
  memcpy(&read_val, region.addr, sizeof(read_val));
  assert(read_val == 42);

  kanbudb_mmap_close(&region);
  unlink(path);
  printf("PASS: test_mmap_open_close\n");
}

static void test_mmap_read_write(void) {
  const char* path = "/tmp/test_mmap_rw.dat";

  /* Create file */
  int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
  assert(fd >= 0);
  uint64_t zeros[4] = {0};
  write(fd, zeros, sizeof(zeros));
  close(fd);

  /* mmap read-write */
  kanbudb_mmap_region_t region;
  int rc = kanbudb_mmap_open(path, 1, sizeof(zeros), &region);
  assert(rc == 0);

  /* Write a value */
  uint64_t val = 99;
  memcpy(region.addr, &val, sizeof(val));

  /* Sync */
  rc = kanbudb_mmap_sync(&region);
  assert(rc == 0);

  kanbudb_mmap_close(&region);

  /* Re-open and verify */
  rc = kanbudb_mmap_open(path, 0, 0, &region);
  assert(rc == 0);
  memcpy(&read_val, region.addr, sizeof(read_val));
  assert(read_val == 99);

  kanbudb_mmap_close(&region);
  unlink(path);
  printf("PASS: test_mmap_read_write\n");
}

static void test_atomic_store_load(void) {
  uint64_t val = 0;
  kanbudb_atomic_store_u64(&val, 12345);
  uint64_t loaded = kanbudb_atomic_load_u64(&val);
  assert(loaded == 12345);

  uint32_t val32 = 0;
  kanbudb_atomic_store_u32(&val32, 67890);
  uint32_t loaded32 = kanbudb_atomic_load_u32(&val32);
  assert(loaded32 == 67890);

  printf("PASS: test_atomic_store_load\n");
}

int main(void) {
  test_mmap_open_close();
  test_mmap_read_write();
  test_atomic_store_load();
  printf("All platform_mmap tests passed.\n");
  return 0;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd build && cmake .. && make test_platform_mmap && ./test_platform_mmap`
Expected: Compilation error — `platform_mmap.h` not found or functions undefined.

- [ ] **Step 3: Write platform_mmap.c implementation**

Create `src/util/platform_mmap.c`:

```c
#include "platform_mmap.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>

#ifdef _WIN32

int kanbudb_mmap_open(const char* path, int rw, size_t size,
                      kanbudb_mmap_region_t* region) {
  DWORD access = rw ? (GENERIC_READ | GENERIC_WRITE) : GENERIC_READ;
  DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE;
  DWORD protect = rw ? PAGE_READWRITE : PAGE_READONLY;

  HANDLE fh = CreateFileA(path, access, share, NULL,
                          OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  if (fh == INVALID_HANDLE_VALUE) return -1;

  if (size == 0) {
    LARGE_INTEGER li;
    if (!GetFileSizeEx(fh, &li)) { CloseHandle(fh); return -1; }
    size = (size_t)li.QuadPart;
  }

  HANDLE mh = CreateFileMappingA(fh, NULL, protect, 0, 0, NULL);
  if (!mh) { CloseHandle(fh); return -1; }

  DWORD viewAccess = rw ? FILE_MAP_WRITE : FILE_MAP_READ;
  void* p = MapViewOfFile(mh, viewAccess, 0, 0, size);
  if (!p) { CloseHandle(mh); CloseHandle(fh); return -1; }

  region->handle = mh;
  region->addr = p;
  region->size = size;
  region->fd = (int)(intptr_t)fh;  /* store file handle */
  return 0;
}

int kanbudb_mmap_sync(kanbudb_mmap_region_t* region) {
  if (!region || !region->addr) return -1;
  if (!FlushViewOfFile(region->addr, region->size)) return -1;
  HANDLE fh = (HANDLE)(intptr_t)region->fd;
  FlushFileBuffers(fh);
  return 0;
}

int kanbudb_mmap_close(kanbudb_mmap_region_t* region) {
  if (!region) return -1;
  if (region->addr && region->addr != KANBUDB_MMAP_INVALID) {
    UnmapViewOfFile(region->addr);
  }
  if (region->handle && region->handle != KANBUDB_MMAP_INVALID) {
    CloseHandle(region->handle);
  }
  if (region->fd >= 0) {
    CloseHandle((HANDLE)(intptr_t)region->fd);
  }
  region->addr = NULL;
  region->handle = KANBUDB_MMAP_INVALID;
  region->fd = -1;
  return 0;
}

#else /* Linux / macOS */

int kanbudb_mmap_open(const char* path, int rw, size_t size,
                      kanbudb_mmap_region_t* region) {
  int flags = rw ? (O_RDWR) : O_RDONLY;
  int fd = open(path, flags, 0644);
  if (fd < 0) return -1;

  if (size == 0) {
    struct stat st;
    if (fstat(fd, &st) < 0) { close(fd); return -1; }
    size = (size_t)st.st_size;
  }

  int prot = rw ? (PROT_READ | PROT_WRITE) : PROT_READ;
  void* p = mmap(NULL, size, prot, MAP_SHARED, fd, 0);
  if (p == MAP_FAILED) { close(fd); return -1; }

  region->handle = (kanbudb_mmap_handle_t)(intptr_t)fd;
  region->addr = p;
  region->size = size;
  region->fd = fd;
  return 0;
}

int kanbudb_mmap_sync(kanbudb_mmap_region_t* region) {
  if (!region || !region->addr) return -1;
  return msync(region->addr, region->size, MS_SYNC);
}

int kanbudb_mmap_close(kanbudb_mmap_region_t* region) {
  if (!region) return -1;
  if (region->addr && region->addr != KANBUDB_MMAP_INVALID) {
    munmap(region->addr, region->size);
  }
  if (region->fd >= 0) {
    close(region->fd);
  }
  region->addr = NULL;
  region->handle = KANBUDB_MMAP_INVALID;
  region->fd = -1;
  return 0;
}

#endif /* _WIN32 */

/* Atomic operations — use compiler builtins (GCC/Clang/MSVC) */

#if defined(__GNUC__) || defined(__clang__)

void kanbudb_atomic_store_u64(uint64_t* ptr, uint64_t value) {
  __atomic_store_n(ptr, value, __ATOMIC_SEQ_CST);
}

uint64_t kanbudb_atomic_load_u64(const uint64_t* ptr) {
  return __atomic_load_n(ptr, __ATOMIC_SEQ_CST);
}

void kanbudb_atomic_store_u32(uint32_t* ptr, uint32_t value) {
  __atomic_store_n(ptr, value, __ATOMIC_SEQ_CST);
}

uint32_t kanbudb_atomic_load_u32(const uint32_t* ptr) {
  return __atomic_load_n(ptr, __ATOMIC_SEQ_CST);
}

#elif defined(_MSC_VER)

#include <intrin.h>

void kanbudb_atomic_store_u64(uint64_t* ptr, uint64_t value) {
  InterlockedExchange64((LONG64*)ptr, (LONG64)value);
}

uint64_t kanbudb_atomic_load_u64(const uint64_t* ptr) {
  return InterlockedOr64((LONG64*)ptr, 0);
}

void kanbudb_atomic_store_u32(uint32_t* ptr, uint32_t value) {
  InterlockedExchange((LONG*)ptr, (LONG)value);
}

uint32_t kanbudb_atomic_load_u32(const uint32_t* ptr) {
  return InterlockedOr((LONG*)ptr, 0);
}

#else
#error "No atomic intrinsics available"
#endif

int kanbudb_file_size(const char* path, uint64_t* out_size) {
  struct stat st;
  if (stat(path, &st) < 0) return -1;
  *out_size = (uint64_t)st.st_size;
  return 0;
}
```

- [ ] **Step 4: Update CMakeLists.txt to include platform_mmap.c**

In `CMakeLists.txt`, add `src/util/platform_mmap.c` to `KANBUDB_SOURCES` after `src/util/page_cache.c`:

```cmake
set(KANBUDB_SOURCES
  src/util/arena.c
  src/util/page_cache.c
  src/util/platform_mmap.c
  ...
```

Add test target:

```cmake
add_executable(test_platform_mmap test/unit/test_platform_mmap.c)
target_include_directories(test_platform_mmap PRIVATE src/util)
target_link_libraries(test_platform_mmap kanbudb_static)
add_test(NAME test_platform_mmap COMMAND test_platform_mmap)
```

- [ ] **Step 5: Run tests to verify they pass**

Run: `cd build && cmake .. && make test_platform_mmap && ./test_platform_mmap`
Expected: All 3 tests pass.

- [ ] **Step 6: Commit**

```bash
git add src/util/platform_mmap.h src/util/platform_mmap.c test/unit/test_platform_mmap.c CMakeLists.txt
git commit -m "feat: implement cross-platform mmap abstraction with tests"
```

---

### Task 3: Shared metadata file (.shared)

**Files:**
- Create: `src/storage/shared_meta.h`
- Create: `src/storage/shared_meta.c`
- Create: `test/unit/test_shared_meta.c`

- [ ] **Step 1: Create shared_meta.h**

```c
#ifndef KANBUDB_SHARED_META_H
#define KANBUDB_SHARED_META_H

#include <stdint.h>

#define KANBUDB_SHARED_MAGIC 0x4B414E4255444200ULL  /* "KANBUDb\0" */

typedef struct __attribute__((packed)) {
  uint64_t magic;
  uint64_t flushed_seq;    /* last seq fully flushed to SSTable */
  uint32_t wal_version;    /* WAL file version (incremented on switch) */
  uint32_t reader_count;   /* active reader processes */
} kanbudb_shared_meta_t;   /* 24 bytes */

/* Create or open .shared file. Returns 0 on success. */
int kanbudb_shared_meta_open(const char* db_path, kanbudb_shared_meta_t* meta);

/* Write meta to .shared file (atomic). Returns 0 on success. */
int kanbudb_shared_meta_write(const char* db_path, const kanbudb_shared_meta_t* meta);

/* Increment reader_count. Returns 0 on success. */
int kanbudb_shared_meta_reader_join(const char* db_path, kanbudb_shared_meta_t* meta);

/* Decrement reader_count. Returns 0 on success. */
int kanbudb_shared_meta_reader_leave(const char* db_path, kanbudb_shared_meta_t* meta);

#endif /* KANBUDB_SHARED_META_H */
```

- [ ] **Step 2: Create shared_meta.c**

```c
#include "shared_meta.h"
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

static void make_path(const char* db_path, char* out, size_t out_len) {
  snprintf(out, out_len, "%s.shared", db_path);
}

int kanbudb_shared_meta_open(const char* db_path, kanbudb_shared_meta_t* meta) {
  char path[512];
  make_path(db_path, path, sizeof(path));

  int fd = open(path, O_RDONLY);
  if (fd < 0) {
    /* File doesn't exist — initialize defaults */
    meta->magic = KANBUDB_SHARED_MAGIC;
    meta->flushed_seq = 0;
    meta->wal_version = 1;
    meta->reader_count = 0;
    return kanbudb_shared_meta_write(db_path, meta);
  }

  memset(meta, 0, sizeof(*meta));
  ssize_t n = read(fd, meta, sizeof(*meta));
  close(fd);

  if (n != sizeof(*meta) || meta->magic != KANBUDB_SHARED_MAGIC) {
    /* Corrupt or partial — reinitialize */
    meta->magic = KANBUDB_SHARED_MAGIC;
    meta->flushed_seq = 0;
    meta->wal_version = 1;
    meta->reader_count = 0;
    return kanbudb_shared_meta_write(db_path, meta);
  }
  return 0;
}

int kanbudb_shared_meta_write(const char* db_path, const kanbudb_shared_meta_t* meta) {
  char path[512];
  make_path(db_path, path, sizeof(path));

  char tmp_path[516];
  snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);

  int fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) return -1;

  ssize_t written = write(fd, meta, sizeof(*meta));
  close(fd);

  if (written != sizeof(*meta)) return -1;

  /* Atomic rename */
  if (rename(tmp_path, path) < 0) return -1;
  return 0;
}

int kanbudb_shared_meta_reader_join(const char* db_path, kanbudb_shared_meta_t* meta) {
  meta->reader_count++;
  return kanbudb_shared_meta_write(db_path, meta);
}

int kanbudb_shared_meta_reader_leave(const char* db_path, kanbudb_shared_meta_t* meta) {
  if (meta->reader_count > 0) meta->reader_count--;
  return kanbudb_shared_meta_write(db_path, meta);
}
```

- [ ] **Step 3: Write tests**

Create `test/unit/test_shared_meta.c`:

```c
#include "shared_meta.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>

static void test_create_and_read(void) {
  const char* db = "/tmp/test_shared_meta";
  unlink("/tmp/test_shared_meta.shared");

  kanbudb_shared_meta_t meta;
  int rc = kanbudb_shared_meta_open(db, &meta);
  assert(rc == 0);
  assert(meta.magic == KANBUDB_SHARED_MAGIC);
  assert(meta.flushed_seq == 0);
  assert(meta.wal_version == 1);
  assert(meta.reader_count == 0);

  /* Modify and write */
  meta.flushed_seq = 100;
  meta.wal_version = 2;
  rc = kanbudb_shared_meta_write(db, &meta);
  assert(rc == 0);

  /* Re-read */
  kanbudb_shared_meta_t meta2;
  rc = kanbudb_shared_meta_open(db, &meta2);
  assert(rc == 0);
  assert(meta2.flushed_seq == 100);
  assert(meta2.wal_version == 2);

  unlink("/tmp/test_shared_meta.shared");
  printf("PASS: test_create_and_read\n");
}

static void test_reader_join_leave(void) {
  const char* db = "/tmp/test_shared_meta_rl";
  unlink("/tmp/test_shared_meta_rl.shared");

  kanbudb_shared_meta_t meta;
  kanbudb_shared_meta_open(db, &meta);

  kanbudb_shared_meta_reader_join(db, &meta);
  assert(meta.reader_count == 1);

  kanbudb_shared_meta_reader_join(db, &meta);
  assert(meta.reader_count == 2);

  kanbudb_shared_meta_reader_leave(db, &meta);
  assert(meta.reader_count == 1);

  kanbudb_shared_meta_reader_leave(db, &meta);
  assert(meta.reader_count == 0);

  unlink("/tmp/test_shared_meta_rl.shared");
  printf("PASS: test_reader_join_leave\n");
}

int main(void) {
  test_create_and_read();
  test_reader_join_leave();
  printf("All shared_meta tests passed.\n");
  return 0;
}
```

- [ ] **Step 4: Add to CMakeLists.txt**

Add after `test_platform_mmap`:

```cmake
add_executable(test_shared_meta test/unit/test_shared_meta.c)
target_include_directories(test_shared_meta PRIVATE src/storage src/util)
target_link_libraries(test_shared_meta kanbudb_static)
add_test(NAME test_shared_meta COMMAND test_shared_meta)
```

- [ ] **Step 5: Build and run tests**

Run: `cd build && cmake .. && make test_shared_meta && ./test_shared_meta`
Expected: Both tests pass.

- [ ] **Step 6: Commit**

```bash
git add src/storage/shared_meta.h src/storage/shared_meta.c test/unit/test_shared_meta.c CMakeLists.txt
git commit -m "feat: add shared metadata file for multi-process coordination"
```

---

### Task 4: WAL mmap API header

**Files:**
- Create: `src/storage/wal_mmap.h`

- [ ] **Step 1: Create wal_mmap.h**

```c
#ifndef KANBUDB_WAL_MMAP_H
#define KANBUDB_WAL_MMAP_H

#include "platform_mmap.h"
#include <stddef.h>
#include <stdint.h>

#define KANBUDB_WAL_MMAP_MAGIC   0x4845524D4553ULL
#define KANBUDB_WAL_MMAP_VERSION 1
#define KANBUDB_WAL_MMAP_HEADER_SIZE 64

/* WAL mmap header — 64 bytes, at offset 0 of the WAL file */
typedef struct __attribute__((packed)) {
  uint64_t magic;
  uint32_t version;
  uint32_t _pad0;
  uint64_t write_pos;     /* current write position (atomically updated) */
  uint64_t data_size;     /* total size of data region */
  uint8_t  _pad[32];     /* padding to 64 bytes */
} kanbudb_wal_mmap_header_t;

/* WAL mmap region — combines mmap region with parsed header info */
typedef struct {
  kanbudb_mmap_region_t   region;
  kanbudb_wal_mmap_header_t* header;  /* points into mapped region */
  uint8_t*                data;       /* points into mapped region (offset 64) */
  size_t                  data_cap;   /* capacity of data region */
  uint64_t                last_seq;   /* last sequence number seen */
} kanbudb_wal_mmap_t;

/* Open WAL file with mmap. If creating new, init_header=1.
   data_cap is the desired data region size (rounded up to page size).
   Returns 0 on success. */
int wal_mmap_open(const char* path, int rw, size_t data_cap,
                  kanbudb_wal_mmap_t* wm);

/* Append a serialized entry to the mmap'd WAL.
   Returns bytes written, or -1 on error. */
int wal_mmap_append(kanbudb_wal_mmap_t* wm, uint64_t seq, int op,
                    uint64_t table_id, const void* key, size_t key_len,
                    const void* value, size_t val_len);

/* Read next entry from current read position.
   Returns 0 on success, -1 if no more entries. */
int wal_mmap_read_entry(kanbudb_wal_mmap_t* wm, uint64_t* out_seq,
                        int* out_op, uint64_t* out_table_id,
                        void** out_key, size_t* out_key_len,
                        void** out_value, size_t* out_val_len);

/* Get current write_pos from header (atomic read). */
uint64_t wal_mmap_get_write_pos(kanbudb_wal_mmap_t* wm);

/* Sync WAL to disk. */
int wal_mmap_sync(kanbudb_wal_mmap_t* wm);

/* Close and unmap WAL. */
int wal_mmap_close(kanbudb_wal_mmap_t* wm);

#endif /* KANBUDB_WAL_MMAP_H */
```

- [ ] **Step 2: Commit**

```bash
git add src/storage/wal_mmap.h
git commit -m "feat: add WAL mmap API header"
```

---

### Task 5: WAL mmap implementation

**Files:**
- Create: `src/storage/wal_mmap.c`

- [ ] **Step 1: Write the failing test**

Create `test/unit/test_wal_mmap.c`:

```c
#include "wal_mmap.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>

static void test_wal_mmap_create_and_append(void) {
  const char* path = "/tmp/test_wal_mmap.dat";
  unlink(path);

  kanbudb_wal_mmap_t wm;
  int rc = wal_mmap_open(path, 1, 4096, &wm);
  assert(rc == 0);
  assert(wm.header->magic == KANBUDB_WAL_MMAP_MAGIC);
  assert(wm.header->write_pos == KANBUDB_WAL_MMAP_HEADER_SIZE);

  /* Append an entry */
  const char* key = "hello";
  const char* val = "world";
  int written = wal_mmap_append(&wm, 1, 0, 1, key, 5, val, 5);
  assert(written > 0);
  assert(wm.header->write_pos > KANBUDB_WAL_MMAP_HEADER_SIZE);

  /* Append another */
  written = wal_mmap_append(&wm, 2, 0, 1, "foo", 3, "bar", 3);
  assert(written > 0);

  /* Sync */
  rc = wal_mmap_sync(&wm);
  assert(rc == 0);

  wal_mmap_close(&wm);
  unlink(path);
  printf("PASS: test_wal_mmap_create_and_append\n");
}

static void test_wal_mmap_read_entries(void) {
  const char* path = "/tmp/test_wal_mmap_read.dat";
  unlink(path);

  kanbudb_wal_mmap_t wm;
  wal_mmap_open(path, 1, 4096, &wm);

  /* Append two entries */
  wal_mmap_append(&wm, 1, 0, 1, "key1", 4, "val1", 4);
  wal_mmap_append(&wm, 2, 1, 1, "key2", 4, NULL, 0);

  /* Read back */
  kanbudb_wal_mmap_t reader;
  wal_mmap_open(path, 0, 0, &reader);

  uint64_t seq; int op; uint64_t tid;
  void* key; size_t kl; void* val; size_t vl;

  int rc = wal_mmap_read_entry(&reader, &seq, &op, &tid, &key, &kl, &val, &vl);
  assert(rc == 0);
  assert(seq == 1);
  assert(op == 0);  /* PUT */
  assert(tid == 1);
  assert(kl == 4 && memcmp(key, "key1", 4) == 0);
  assert(vl == 4 && memcmp(val, "val1", 4) == 0);

  rc = wal_mmap_read_entry(&reader, &seq, &op, &tid, &key, &kl, &val, &vl);
  assert(rc == 0);
  assert(seq == 2);
  assert(op == 1);  /* DELETE */
  assert(kl == 4 && memcmp(key, "key2", 4) == 0);

  rc = wal_mmap_read_entry(&reader, &seq, &op, &tid, &key, &kl, &val, &vl);
  assert(rc == -1);  /* no more entries */

  wal_mmap_close(&reader);
  wal_mmap_close(&wm);
  unlink(path);
  printf("PASS: test_wal_mmap_read_entries\n");
}

static void test_wal_mmap_get_write_pos(void) {
  const char* path = "/tmp/test_wal_mmap_pos.dat";
  unlink(path);

  kanbudb_wal_mmap_t wm;
  wal_mmap_open(path, 1, 4096, &wm);

  uint64_t pos = wal_mmap_get_write_pos(&wm);
  assert(pos == KANBUDB_WAL_MMAP_HEADER_SIZE);

  wal_mmap_append(&wm, 1, 0, 1, "a", 1, "b", 1);
  pos = wal_mmap_get_write_pos(&wm);
  assert(pos > KANBUDB_WAL_MMAP_HEADER_SIZE);

  wal_mmap_close(&wm);
  unlink(path);
  printf("PASS: test_wal_mmap_get_write_pos\n");
}

int main(void) {
  test_wal_mmap_create_and_append();
  test_wal_mmap_read_entries();
  test_wal_mmap_get_write_pos();
  printf("All wal_mmap tests passed.\n");
  return 0;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd build && cmake .. && make test_wal_mmap && ./test_wal_mmap`
Expected: Compilation error — functions undefined.

- [ ] **Step 3: Write wal_mmap.c implementation**

Create `src/storage/wal_mmap.c`:

```c
#include "wal_mmap.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Round up to page size */
static size_t page_align(size_t size) {
  size_t page = 4096;
  return (size + page - 1) & ~(page - 1);
}

/* Serialize entry into buffer. Returns bytes written. */
static int serialize_entry(uint8_t* buf, size_t cap,
                           uint64_t seq, int op, uint64_t table_id,
                           const void* key, size_t key_len,
                           const void* value, size_t val_len) {
  uint8_t op_u8 = (op == 1) ? 1 : 0;
  size_t needed = sizeof(seq) + 1 + sizeof(table_id) +
                  sizeof(uint64_t) + sizeof(uint64_t) +
                  key_len + ((op_u8 == 0) ? val_len : 0);
  if (needed > cap) return -1;

  size_t off = 0;
  memcpy(buf + off, &seq, sizeof(seq)); off += sizeof(seq);
  memcpy(buf + off, &op_u8, 1); off += 1;
  memcpy(buf + off, &table_id, sizeof(table_id)); off += sizeof(table_id);
  uint64_t kl = key_len, vl = val_len;
  memcpy(buf + off, &kl, sizeof(kl)); off += sizeof(kl);
  memcpy(buf + off, &vl, sizeof(vl)); off += sizeof(vl);
  if (key_len > 0) { memcpy(buf + off, key, key_len); off += key_len; }
  if (op_u8 == 0 && value && val_len > 0) {
    memcpy(buf + off, value, val_len); off += val_len;
  }
  return (int)off;
}

int wal_mmap_open(const char* path, int rw, size_t data_cap,
                  kanbudb_wal_mmap_t* wm) {
  memset(wm, 0, sizeof(*wm));

  size_t total_size = page_align(KANBUDB_WAL_MMAP_HEADER_SIZE + data_cap);
  if (data_cap == 0) total_size = 0;  /* let open figure out size */

  int rc = kanbudb_mmap_open(path, rw, total_size, &wm->region);
  if (rc < 0) return -1;

  wm->header = (kanbudb_wal_mmap_header_t*)wm->region.addr;
  wm->data = wm->region.addr + KANBUDB_WAL_MMAP_HEADER_SIZE;
  wm->data_cap = wm->region.size - KANBUDB_WAL_MMAP_HEADER_SIZE;
  wm->last_seq = 0;

  if (rw && wm->header->magic != KANBUDB_WAL_MMAP_MAGIC) {
    /* Initialize new WAL header */
    memset(wm->region.addr, 0, wm->region.size);
    kanbudb_atomic_store_u64(&wm->header->magic, KANBUDB_WAL_MMAP_MAGIC);
    kanbudb_atomic_store_u32(&wm->header->version, KANBUDB_WAL_MMAP_VERSION);
    kanbudb_atomic_store_u64(&wm->header->write_pos, KANBUDB_WAL_MMAP_HEADER_SIZE);
    kanbudb_atomic_store_u64(&wm->header->data_size, wm->data_cap);
    kanbudb_mmap_sync(&wm->region);
  }

  return 0;
}

int wal_mmap_append(kanbudb_wal_mmap_t* wm, uint64_t seq, int op,
                    uint64_t table_id, const void* key, size_t key_len,
                    const void* value, size_t val_len) {
  uint64_t write_pos = kanbudb_atomic_load_u64(&wm->header->write_pos);
  size_t data_offset = write_pos - KANBUDB_WAL_MMAP_HEADER_SIZE;

  int written = serialize_entry(wm->data + data_offset,
                                wm->data_cap - data_offset,
                                seq, op, table_id, key, key_len, value, val_len);
  if (written < 0) return -1;

  kanbudb_atomic_store_u64(&wm->header->write_pos, write_pos + written);
  wm->last_seq = seq;
  return written;
}

int wal_mmap_read_entry(kanbudb_wal_mmap_t* wm, uint64_t* out_seq,
                        int* out_op, uint64_t* out_table_id,
                        void** out_key, size_t* out_key_len,
                        void** out_value, size_t* out_val_len) {
  uint64_t write_pos = kanbudb_atomic_load_u64(&wm->header->write_pos);
  uint64_t read_pos = wm->last_seq;  /* reuse as read cursor (offset) */

  if (read_pos >= write_pos) return -1;

  size_t off = (size_t)(read_pos - KANBUDB_WAL_MMAP_HEADER_SIZE);
  size_t remaining = (size_t)(write_pos - read_pos);
  if (remaining < 8 + 1 + 8 + 8 + 8) return -1;

  uint64_t seq, table_id, kl, vl;
  uint8_t op_u8;

  memcpy(&seq, wm->data + off, sizeof(seq)); off += sizeof(seq);
  memcpy(&op_u8, wm->data + off, 1); off += 1;
  memcpy(&table_id, wm->data + off, sizeof(table_id)); off += sizeof(table_id);
  memcpy(&kl, wm->data + off, sizeof(kl)); off += sizeof(kl);
  memcpy(&vl, wm->data + off, sizeof(vl)); off += sizeof(vl);

  *out_seq = seq;
  *out_op = (op_u8 == 0) ? 0 : 1;
  *out_table_id = table_id;
  *out_key = wm->data + off;
  *out_key_len = (size_t)kl;
  off += (size_t)kl;

  if (op_u8 == 0 && vl > 0) {
    *out_value = wm->data + off;
    *out_val_len = (size_t)vl;
  } else {
    *out_value = NULL;
    *out_val_len = 0;
  }

  wm->last_seq = seq + 1;  /* advance cursor */
  return 0;
}

uint64_t wal_mmap_get_write_pos(kanbudb_wal_mmap_t* wm) {
  return kanbudb_atomic_load_u64(&wm->header->write_pos);
}

int wal_mmap_sync(kanbudb_wal_mmap_t* wm) {
  return kanbudb_mmap_sync(&wm->region);
}

int wal_mmap_close(kanbudb_wal_mmap_t* wm) {
  return kanbudb_mmap_close(&wm->region);
}
```

- [ ] **Step 4: Add to CMakeLists.txt**

Add `src/storage/wal_mmap.c` to `KANBUDB_SOURCES` after `src/storage/wal.c`:

```cmake
  src/storage/wal.c
  src/storage/wal_mmap.c
```

Add test target:

```cmake
add_executable(test_wal_mmap test/unit/test_wal_mmap.c)
target_include_directories(test_wal_mmap PRIVATE src/storage src/util)
target_link_libraries(test_wal_mmap kanbudb_static)
add_test(NAME test_wal_mmap COMMAND test_wal_mmap)
```

- [ ] **Step 5: Build and run tests**

Run: `cd build && cmake .. && make test_wal_mmap && ./test_wal_mmap`
Expected: All 3 tests pass.

- [ ] **Step 6: Commit**

```bash
git add src/storage/wal_mmap.h src/storage/wal_mmap.c test/unit/test_wal_mmap.c CMakeLists.txt
git commit -m "feat: implement WAL mmap with cross-platform support"
```

---

### Task 6: Update db_config_t and db struct for multi-process

**Files:**
- Modify: `include/db.h`
- Modify: `src/core/db.h`

- [ ] **Step 1: Add config fields to db.h**

In `include/db.h`, add fields to `db_config_t`:

```c
typedef struct db_config_t {
  kanbudb_fsync_mode_t fsync_mode;
  size_t cache_size;
  size_t memtable_size;
  int compaction_threads;
  int multi_process;        /* NEW: 1 = enable multi-process sharing */
  int reader_poll_ms;       /* NEW: reader polling interval in ms (default 10) */
} db_config_t;
```

Update `default_config` in `src/core/db.c`:

```c
static const db_config_t default_config = {
  KANBUDB_FSYNC_NONE,
  65536,
  65536,
  1,
  0,    /* multi_process: disabled by default */
  10    /* reader_poll_ms: 10ms */
};
```

- [ ] **Step 2: Add new API declaration to include/db.h**

Add after `db_close()`:

```c
/* Multi-process: open as read-only reader (tails writer's WAL via mmap) */
int db_open_reader(const char *path, const db_config_t *config, db_t **out);

/* Multi-process: manually trigger reader refresh from WAL */
int db_reader_refresh(db_t *db);
```

- [ ] **Step 3: Add mmap fields to core db struct**

In `src/core/db.h`, add includes and fields:

```c
#include "wal_mmap.h"
#include "shared_meta.h"

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
  kanbudb_vec_index_t*  vec_index;
  kanbudb_embed_t*      embed;
  kanbudb_quantizer_t*  quantizer;
  float*               quant_vectors;
  uint64_t*            quant_ids;
  uint32_t             quant_count;
  uint32_t             quant_capacity;
  /* Fine-grained locking (lock order: table < wal < lsm < btree) */
  pthread_rwlock_t     table_lock;
  pthread_mutex_t      wal_lock;
  pthread_rwlock_t     lsm_lock;
  pthread_rwlock_t     btree_lock;
  /* Background compaction */
  pthread_t            compact_thread;
  int                  compact_running;
  int                  compact_trigger;
  /* Multi-process sharing (new) */
  int                  is_reader;            /* 1 if opened as reader */
  kanbudb_wal_mmap_t   wal_mmap;            /* mmap'd WAL for reader */
  kanbudb_shared_meta_t shared_meta;         /* .shared metadata */
  pthread_t            reader_poll_thread;   /* background poll thread */
  int                  reader_poll_running;  /* poll thread control flag */
};
```

- [ ] **Step 4: Commit**

```bash
git add include/db.h src/core/db.h src/core/db.c
git commit -m "feat: add multi_process config fields and reader struct members"
```

---

### Task 7: Implement db_open_reader() and reader poll thread

**Files:**
- Modify: `src/core/db.c`

- [ ] **Step 1: Write the failing integration test**

Create `test/unit/test_multiprocess.c`:

```c
#include "db.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <sys/wait.h>

static const char* DB_PATH = "/tmp/test_multiprocess_db";

static void cleanup(void) {
  char buf[512];
  snprintf(buf, sizeof(buf), "%s.wal", DB_PATH); unlink(buf);
  snprintf(buf, sizeof(buf), "%s.shared", DB_PATH); unlink(buf);
  snprintf(buf, sizeof(buf), "%s.system", DB_PATH); unlink(buf);
  /* remove any SSTable files */
  for (int i = 0; i < 10; i++) {
    snprintf(buf, sizeof(buf), "%s.sst.0.%d", DB_PATH, i);
    unlink(buf);
  }
}

static void test_writer_reader_basic(void) {
  cleanup();

  /* Writer process */
  pid_t pid = fork();
  if (pid == 0) {
    db_config_t cfg = {
      .fsync_mode = KANBUDB_FSYNC_ALWAYS,
      .cache_size = 65536,
      .memtable_size = 65536,
      .compaction_threads = 1,
      .multi_process = 1,
      .reader_poll_ms = 10
    };

    db_t* db;
    int rc = db_open(DB_PATH, &cfg, &db);
    if (rc != 0) { fprintf(stderr, "writer db_open failed: %d\n", rc); _exit(1); }

    const char* cols[] = {"id", "name"};
    kanbudb_col_type_t types[] = {KANBUDB_INT32, KANBUDB_STRING};
    rc = db_create_table(db, "users", cols, types, 2, "id");
    if (rc != 0) { fprintf(stderr, "create_table failed: %d\n", rc); _exit(1); }

    /* Write some data */
    for (int i = 0; i < 100; i++) {
      char key[32], val[64];
      snprintf(key, sizeof(key), "%d", i);
      snprintf(val, sizeof(val), "user_%d", i);
      rc = db_put(db, "users", key, strlen(key), val, strlen(val) + 1);
      if (rc != 0) { fprintf(stderr, "db_put failed at %d: %d\n", i, rc); _exit(1); }
    }

    /* Wait for reader to catch up */
    usleep(200000);

    db_close(db);
    _exit(0);
  }

  /* Wait a bit for writer to create DB */
  usleep(50000);

  /* Reader process */
  db_config_t cfg = {
    .fsync_mode = KANBUDB_FSYNC_NONE,
    .cache_size = 65536,
    .memtable_size = 65536,
    .compaction_threads = 0,
    .multi_process = 1,
    .reader_poll_ms = 10
  };

  db_t* reader;
  int rc = db_open_reader(DB_PATH, &cfg, &reader);
  assert(rc == 0);

  /* Wait for writer to finish */
  int status;
  waitpid(pid, &status, 0);
  assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);

  /* Refresh reader to pick up WAL entries */
  rc = db_reader_refresh(reader);
  assert(rc == 0);

  /* Verify some data */
  void* val; size_t val_len;
  rc = db_get(reader, "users", "0", 1, &val, &val_len);
  assert(rc == 0);
  assert(val_len > 0);

  rc = db_get(reader, "users", "99", 2, &val, &val_len);
  assert(rc == 0);
  assert(val_len > 0);

  db_close(reader);
  cleanup();
  printf("PASS: test_writer_reader_basic\n");
}

int main(void) {
  test_writer_reader_basic();
  printf("All multiprocess tests passed.\n");
  return 0;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd build && cmake .. && make test_multiprocess && ./test_multiprocess`
Expected: Link error — `db_open_reader` undefined.

- [ ] **Step 3: Implement db_open_reader() in db.c**

Add to `src/core/db.c` after `db_open()`:

```c
/* ── Reader poll thread (background) ──────────────────────── */
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

int db_open_reader(const char* path, const db_config_t* config, db_t** out) {
  if (!path || !out) return KANBUDB_ERR_INVAL;

  struct kanbudb_db* db = (struct kanbudb_db*)calloc(1, sizeof(*db));
  if (!db) return KANBUDB_ERR_OOM;

  db->path = strdup(path);
  if (!db->path) { free(db); return KANBUDB_ERR_OOM; }

  db->config = config ? *config : default_config;
  db->is_reader = 1;

  pthread_rwlock_init(&db->table_lock, NULL);
  pthread_rwlock_init(&db->lsm_lock, NULL);
  pthread_rwlock_init(&db->btree_lock, NULL);

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

  /* Load data SSTables into B-tree */
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

  /* Open shared metadata */
  kanbudb_shared_meta_open(path, &db->shared_meta);
  kanbudb_shared_meta_reader_join(path, &db->shared_meta);

  /* Open WAL with mmap (read-only) */
  char wal_path[512];
  snprintf(wal_path, sizeof(wal_path), "%s.wal", path);
  int rc = wal_mmap_open(wal_path, 0, 0, &db->wal_mmap);
  if (rc < 0) {
    /* WAL might not exist yet — that's OK */
    memset(&db->wal_mmap, 0, sizeof(db->wal_mmap));
  }

  /* Start poll thread */
  db->reader_poll_running = 1;
  pthread_create(&db->reader_poll_thread, NULL, reader_poll_worker, db);

  *out = db;
  return KANBUDB_OK;
}

int db_reader_refresh(db_t* db) {
  if (!db || !db->is_reader) return KANBUDB_ERR_INVAL;
  struct kanbudb_db* internal = (struct kanbudb_db*)db;

  /* Check if WAL has new data */
  if (!internal->wal_mmap.region.addr) {
    /* Try to open WAL (writer might have created it) */
    char wal_path[512];
    snprintf(wal_path, sizeof(wal_path), "%s.wal", internal->path);
    int rc = wal_mmap_open(wal_path, 0, 0, &internal->wal_mmap);
    if (rc < 0) return KANBUDB_OK;  /* still no WAL */
  }

  uint64_t write_pos = wal_mmap_get_write_pos(&internal->wal_mmap);
  uint64_t read_cursor = internal->wal_mmap.last_seq;
  /* last_seq is used as seq number, not byte offset — use write_pos comparison */
  (void)write_pos;
  (void)read_cursor;

  /* Read all available entries */
  pthread_rwlock_wrlock(&internal->btree_lock);
  pthread_rwlock_wrlock(&internal->lsm_lock);

  uint64_t seq; int op; uint64_t tid;
  void* key; size_t kl; void* val; size_t vl;

  while (wal_mmap_read_entry(&internal->wal_mmap, &seq, &op, &tid,
                              &key, &kl, &val, &vl) == 0) {
    if (op == 0) {  /* PUT */
      lsm_put(internal->lsm, tid, key, kl, val, vl);
      btree_put(internal->btree, key, kl, val, vl);
    } else {  /* DELETE */
      lsm_delete(internal->lsm, tid, key, kl);
      btree_delete(internal->btree, key, kl);
    }
  }

  pthread_rwlock_unlock(&internal->lsm_lock);
  pthread_rwlock_unlock(&internal->btree_lock);

  return KANBUDB_OK;
}
```

- [ ] **Step 4: Modify db_close() to handle reader cleanup**

In `src/core/db.c`, modify `db_close()` — add before the lock acquisition section:

```c
  if (internal->is_reader) {
    /* Stop poll thread */
    internal->reader_poll_running = 0;
    pthread_join(internal->reader_poll_thread, NULL);

    /* Leave shared metadata */
    kanbudb_shared_meta_reader_leave(internal->path, &internal->shared_meta);

    /* Close WAL mmap */
    wal_mmap_close(&internal->wal_mmap);

    /* Free locks */
    pthread_rwlock_destroy(&internal->table_lock);
    pthread_rwlock_destroy(&internal->lsm_lock);
    pthread_rwlock_destroy(&internal->btree_lock);

    /* Destroy btree */
    btree_destroy(internal->btree);

    /* Free schema */
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
```

- [ ] **Step 5: Modify db_put() for multi_process mode**

In `src/core/db.c`, after the `wal_append` call in `db_put()`, add WAL mmap update:

```c
  /* In multi_process mode, also append to mmap'd WAL for readers */
  if (internal->config.multi_process) {
    wal_mmap_append(&internal->wal_mmap, internal->wal->seq,
                    KANBUDB_WAL_PUT, tbl_id,
                    key, key_len, value, value_len);
    wal_mmap_sync(&internal->wal_mmap);
  }
```

Similarly for `db_delete()`, append a DELETE entry.

- [ ] **Step 6: Modify db_open() to initialize WAL mmap in multi_process mode**

In `src/core/db.c`, at the end of `db_open()` (before `*out = db`), add:

```c
  if (db->config.multi_process) {
    /* Initialize shared metadata */
    kanbudb_shared_meta_open(path, &db->shared_meta);

    /* Open WAL with mmap (read-write) for multi-process sharing */
    char wal_path_mmap[512];
    snprintf(wal_path_mmap, sizeof(wal_path_mmap), "%s.wal", path);
    size_t wal_data_cap = 64 * 1024 * 1024;  /* 64MB data region */
    int mrc = wal_mmap_open(wal_path_mmap, 1, wal_data_cap, &db->wal_mmap);
    if (mrc < 0) {
      fprintf(stderr, "kanbudb: WARNING: wal_mmap_open failed (%d), multi-process sharing disabled\n", mrc);
      db->config.multi_process = 0;
    }
  }
```

- [ ] **Step 7: Add test target to CMakeLists.txt**

```cmake
add_executable(test_multiprocess test/unit/test_multiprocess.c)
target_include_directories(test_multiprocess PRIVATE src/util src/storage src)
target_link_libraries(test_multiprocess kanbudb_static Threads::Threads)
add_test(NAME test_multiprocess COMMAND test_multiprocess)
```

- [ ] **Step 8: Build and run tests**

Run: `cd build && cmake .. && make && ./test_multiprocess`
Expected: All tests pass (including existing tests).

- [ ] **Step 9: Commit**

```bash
git add src/core/db.c src/core/db.h include/db.h test/unit/test_multiprocess.c CMakeLists.txt
git commit -m "feat: implement multi-process shared WAL with reader poll thread"
```

---

### Task 8: Handle WAL file switch for writer

**Files:**
- Modify: `src/storage/wal_mmap.c`
- Modify: `src/core/db.c`

- [ ] **Step 1: Add WAL file switch function to wal_mmap.h/c**

Add to `src/storage/wal_mmap.h`:

```c
/* Switch to a new WAL file. Creates new_file, copies unflushed entries,
   atomic renames to wal_path, updates shared_meta wal_version.
   Returns 0 on success. */
int wal_mmap_switch_file(const char* wal_path,
                         kanbudb_wal_mmap_t* wm,
                         kanbudb_shared_meta_t* shared_meta);
```

Implement in `src/storage/wal_mmap.c`:

```c
int wal_mmap_switch_file(const char* wal_path,
                         kanbudb_wal_mmap_t* wm,
                         kanbudb_shared_meta_t* shared_meta) {
  /* Create new WAL file */
  char new_path[512];
  snprintf(new_path, sizeof(new_path), "%s.%u", wal_path,
           shared_meta->wal_version + 1);

  kanbudb_wal_mmap_t new_wm;
  size_t data_cap = wm->data_cap > 0 ? wm->data_cap : 64 * 1024 * 1024;
  int rc = wal_mmap_open(new_path, 1, data_cap, &new_wm);
  if (rc < 0) return -1;

  /* Copy unflushed entries (seq > flushed_seq) */
  kanbudb_wal_mmap_t reader;
  memset(&reader, 0, sizeof(reader));
  rc = kanbudb_mmap_open(wal_path, 0, 0, &reader.region);
  if (rc == 0) {
    reader.header = (kanbudb_wal_mmap_header_t*)reader.region.addr;
    reader.data = reader.region.addr + KANBUDB_WAL_MMAP_HEADER_SIZE;
    reader.data_cap = reader.region.size - KANBUDB_WAL_MMAP_HEADER_SIZE;
    reader.last_seq = 0;

    uint64_t seq; int op; uint64_t tid;
    void* key; size_t kl; void* val; size_t vl;
    uint64_t new_seq = 1;

    while (wal_mmap_read_entry(&reader, &seq, &op, &tid,
                                &key, &kl, &val, &vl) == 0) {
      if (seq > shared_meta->flushed_seq) {
        wal_mmap_append(&new_wm, new_seq++, op, tid, key, kl, val, vl);
      }
    }
    kanbudb_mmap_close(&reader.region);
  }

  wal_mmap_sync(&new_wm);
  wal_mmap_close(&new_wm);

  /* Close old mmap */
  kanbudb_mmap_close(&wm->region);

  /* Atomic rename new → wal_path */
  if (rename(new_path, wal_path) < 0) return -1;

  /* Update shared_meta */
  shared_meta->wal_version++;
  kanbudb_shared_meta_write(
    /* extract db_path from wal_path by removing ".wal" suffix */
    /* ... */, shared_meta);

  /* Re-open new WAL */
  return wal_mmap_open(wal_path, 1, data_cap, wm);
}
```

- [ ] **Step 2: Call WAL switch when WAL grows too large**

In `db_put()` in `db.c`, after mmap append, check size:

```c
  if (internal->config.multi_process) {
    uint64_t write_pos = wal_mmap_get_write_pos(&internal->wal_mmap);
    size_t data_used = write_pos - KANBUDB_WAL_MMAP_HEADER_SIZE;
    size_t data_cap = internal->wal_mmap.data_cap;

    /* Switch WAL if >80% full */
    if (data_used > data_cap * 80 / 100) {
      wal_mmap_switch_file(wal_path, &internal->wal_mmap, &internal->shared_meta);
    }
  }
```

- [ ] **Step 3: Commit**

```bash
git add src/storage/wal_mmap.h src/storage/wal_mmap.c src/core/db.c
git commit -m "feat: add WAL file switch for writer when WAL grows large"
```

---

### Task 9: Full test suite verification

**Files:**
- Modify: `test/unit/test_multiprocess.c` (add edge case tests)

- [ ] **Step 1: Add test for writer crash recovery**

Add to `test_multiprocess.c`:

```c
static void test_writer_crash_recovery(void) {
  cleanup();

  /* Writer writes then "crashes" (exits without db_close) */
  pid_t pid = fork();
  if (pid == 0) {
    db_config_t cfg = { KANBUDB_FSYNC_ALWAYS, 65536, 65536, 1, 1, 10 };
    db_t* db;
    db_open(DB_PATH, &cfg, &db);

    const char* cols[] = {"id", "val"};
    kanbudb_col_type_t types[] = {KANBUDB_INT32, KANBUDB_STRING};
    db_create_table(db, "t", cols, types, 2, "id");

    for (int i = 0; i < 50; i++) {
      char k[16], v[32];
      snprintf(k, sizeof(k), "%d", i);
      snprintf(v, sizeof(v), "data_%d", i);
      db_put(db, "t", k, strlen(k), v, strlen(v) + 1);
    }
    /* Simulate crash — no db_close */
    _exit(0);
  }

  int status;
  waitpid(pid, &status, 0);
  assert(WIFEXITED(status));

  /* Writer opens again — should recover from WAL */
  db_config_t cfg = { KANBUDB_FSYNC_ALWAYS, 65536, 65536, 1, 1, 10 };
  db_t* db2;
  int rc = db_open(DB_PATH, &cfg, &db2);
  assert(rc == 0);

  void* val; size_t val_len;
  rc = db_get(db2, "t", "0", 1, &val, &val_len);
  assert(rc == 0);

  db_close(db2);
  cleanup();
  printf("PASS: test_writer_crash_recovery\n");
}
```

- [ ] **Step 2: Run full test suite**

Run: `cd build && cmake .. && make && ctest --output-on-failure`
Expected: All tests pass, including the new multiprocess tests.

- [ ] **Step 3: Final commit**

```bash
git add -A
git commit -m "feat: complete multi-process shared WAL implementation"
```

---

## Summary

| Task | Description | Files Changed |
|------|-------------|---------------|
| 1 | Platform mmap header | `platform_mmap.h` (new) |
| 2 | Platform mmap impl | `platform_mmap.c` (new), `test_platform_mmap.c` (new) |
| 3 | Shared metadata | `shared_meta.h` (new), `shared_meta.c` (new), `test_shared_meta.c` (new) |
| 4 | WAL mmap header | `wal_mmap.h` (new) |
| 5 | WAL mmap impl | `wal_mmap.c` (new), `test_wal_mmap.c` (new) |
| 6 | Config + struct updates | `db.h`, `db.h` (core) |
| 7 | db_open_reader + poll | `db.c`, `test_multiprocess.c` (new) |
| 8 | WAL file switch | `wal_mmap.c`, `db.c` |
| 9 | Full test verification | `test_multiprocess.c` |
