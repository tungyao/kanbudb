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

#define KANBUDB_MMAP_READ 0
#define KANBUDB_MMAP_WRITE 1

typedef struct kanbudb_mmap_region {
  kanbudb_mmap_handle_t handle;
  void*                 addr;
  size_t                size;
  int                   fd; /* POSIX-only; unused on Windows */
} kanbudb_mmap_region_t;

/**
 * Opens or creates a memory-mapped file region.
 *
 * @param path   File path to map.
 * @param rw     Access mode: KANBUDB_MMAP_READ or KANBUDB_MMAP_WRITE.
 * @param size   Size in bytes to map (must be > 0).
 * @param region Output region struct to populate.
 * @return 0 on success, -1 on error (errno set).
 */
int kanbudb_mmap_open(const char* path, int rw, size_t size,
                      kanbudb_mmap_region_t* region);

/**
 * Flushes dirty pages of the mapped region to storage.
 *
 * @param region Previously opened mmap region.
 * @return 0 on success, -1 on error (errno set).
 */
int kanbudb_mmap_sync(kanbudb_mmap_region_t* region);

/**
 * Unmaps the region and releases associated resources.
 *
 * @param region Previously opened mmap region.
 * @return 0 on success, -1 on error (errno set).
 */
int kanbudb_mmap_close(kanbudb_mmap_region_t* region);

/*
 * Atomic operations for WAL mmap layer.
 * Not directly related to mmap, but placed here for shared use.
 */
void kanbudb_atomic_store_u64(uint64_t* ptr, uint64_t value);
uint64_t kanbudb_atomic_load_u64(const uint64_t* ptr);
void kanbudb_atomic_store_u32(uint32_t* ptr, uint32_t value);
uint32_t kanbudb_atomic_load_u32(const uint32_t* ptr);

/**
 * Gets the size of a file in bytes.
 *
 * @param path     File path to query.
 * @param out_size Output size.
 * @return 0 on success, -1 on error (errno set).
 */
int kanbudb_file_size(const char* path, uint64_t* out_size);

#endif /* KANBUDB_PLATFORM_MMAP_H */
