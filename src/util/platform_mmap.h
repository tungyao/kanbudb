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
  int                   fd;
} kanbudb_mmap_region_t;

int kanbudb_mmap_open(const char* path, int rw, size_t size,
                      kanbudb_mmap_region_t* region);

int kanbudb_mmap_sync(kanbudb_mmap_region_t* region);

int kanbudb_mmap_close(kanbudb_mmap_region_t* region);

void kanbudb_atomic_store_u64(uint64_t* ptr, uint64_t value);
uint64_t kanbudb_atomic_load_u64(const uint64_t* ptr);
void kanbudb_atomic_store_u32(uint32_t* ptr, uint32_t value);
uint32_t kanbudb_atomic_load_u32(const uint32_t* ptr);

int kanbudb_file_size(const char* path, uint64_t* out_size);

#endif /* KANBUDB_PLATFORM_MMAP_H */
