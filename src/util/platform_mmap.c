#include "platform_mmap.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>

#ifdef _WIN32
  #include <windows.h>
#else
  #include <sys/mman.h>
  #include <sys/stat.h>
  #include <unistd.h>
#endif

int kanbudb_mmap_open(const char* path, int rw, size_t size,
                      kanbudb_mmap_region_t* region) {
  if (!region) { errno = EINVAL; return -1; }
  memset(region, 0, sizeof(*region));
  region->handle = KANBUDB_MMAP_INVALID;
  region->addr   = KANBUDB_MMAP_INVALID;
  region->fd     = -1;

#ifdef _WIN32
  DWORD access = (rw == KANBUDB_MMAP_WRITE)
                 ? (GENERIC_READ | GENERIC_WRITE)
                 : GENERIC_READ;
  DWORD share  = FILE_SHARE_READ | FILE_SHARE_WRITE;
  DWORD create = OPEN_EXISTING;
  HANDLE hFile = CreateFileA(path, access, share, NULL, create,
                             FILE_ATTRIBUTE_NORMAL, NULL);
  if (hFile == INVALID_HANDLE_VALUE) return -1;

  if (size == 0) {
    LARGE_INTEGER li;
    if (!GetFileSizeEx(hFile, &li)) { CloseHandle(hFile); return -1; }
    size = (size_t)li.QuadPart;
  }
  if (size == 0) { CloseHandle(hFile); errno = EINVAL; return -1; }

  DWORD prot  = (rw == KANBUDB_MMAP_WRITE)
                ? PAGE_READWRITE : PAGE_READONLY;
  DWORD mapAccess = (rw == KANBUDB_MMAP_WRITE)
                    ? FILE_MAP_WRITE : FILE_MAP_READ;
  HANDLE hMap = CreateFileMappingA(hFile, NULL, prot, 0, 0, NULL);
  if (!hMap) { CloseHandle(hFile); return -1; }

  void* addr = MapViewOfFile(hMap, mapAccess, 0, 0, 0);
  if (!addr) { CloseHandle(hMap); CloseHandle(hFile); return -1; }

  region->handle = hMap;
  region->addr   = addr;
  region->size   = size;
  region->fd     = (int)(intptr_t)hFile;
#else
  int flags = (rw == KANBUDB_MMAP_WRITE) ? O_RDWR : O_RDONLY;
  int fd = open(path, flags);
  if (fd < 0) return -1;

  if (size == 0) {
    struct stat st;
    if (fstat(fd, &st) != 0) { close(fd); return -1; }
    size = (size_t)st.st_size;
  }
  if (size == 0) { close(fd); errno = EINVAL; return -1; }

  int prot = PROT_READ;
  if (rw == KANBUDB_MMAP_WRITE) prot |= PROT_WRITE;

  void* addr = mmap(NULL, size, prot, MAP_SHARED, fd, 0);
  if (addr == MAP_FAILED) { close(fd); return -1; }

  region->handle = addr;
  region->addr   = addr;
  region->size   = size;
  region->fd     = fd;
#endif

  return 0;
}

int kanbudb_mmap_sync(kanbudb_mmap_region_t* region) {
  if (!region || !region->addr || region->addr == KANBUDB_MMAP_INVALID) {
    errno = EINVAL;
    return -1;
  }

#ifdef _WIN32
  if (!FlushViewOfFile(region->addr, region->size)) return -1;
  HANDLE hFile = (HANDLE)(intptr_t)region->fd;
  if (!FlushFileBuffers(hFile)) return -1;
  return 0;
#else
  return msync(region->addr, region->size, MS_SYNC);
#endif
}

int kanbudb_mmap_close(kanbudb_mmap_region_t* region) {
  if (!region) return 0;

#ifdef _WIN32
  if (region->addr && region->addr != KANBUDB_MMAP_INVALID)
    UnmapViewOfFile(region->addr);
  if (region->handle && region->handle != KANBUDB_MMAP_INVALID)
    CloseHandle(region->handle);
  if (region->fd != -1)
    CloseHandle((HANDLE)(intptr_t)region->fd);
#else
  if (region->addr && region->addr != KANBUDB_MMAP_INVALID)
    munmap(region->addr, region->size);
  if (region->fd != -1)
    close(region->fd);
#endif

  region->handle = KANBUDB_MMAP_INVALID;
  region->addr   = KANBUDB_MMAP_INVALID;
  region->size   = 0;
  region->fd     = -1;
  return 0;
}

int kanbudb_file_size(const char* path, uint64_t* out_size) {
  if (!out_size) { errno = EINVAL; return -1; }
#ifdef _WIN32
  HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  if (h == INVALID_HANDLE_VALUE) return -1;
  LARGE_INTEGER li;
  BOOL ok = GetFileSizeEx(h, &li);
  CloseHandle(h);
  if (!ok) return -1;
  *out_size = (uint64_t)li.QuadPart;
#else
  struct stat st;
  if (stat(path, &st) != 0) return -1;
  *out_size = (uint64_t)st.st_size;
#endif
  return 0;
}

void kanbudb_atomic_store_u64(uint64_t* ptr, uint64_t value) {
#ifdef _WIN32
  InterlockedExchange64((volatile LONG64*)ptr, (LONG64)value);
#elif defined(__GNUC__) || defined(__clang__)
  __atomic_store_n(ptr, value, __ATOMIC_SEQ_CST);
#else
  *ptr = value;
#endif
}

uint64_t kanbudb_atomic_load_u64(const uint64_t* ptr) {
#ifdef _WIN32
  return (uint64_t)InterlockedOr64((volatile LONG64*)ptr, 0);
#elif defined(__GNUC__) || defined(__clang__)
  return __atomic_load_n(ptr, __ATOMIC_SEQ_CST);
#else
  return *ptr;
#endif
}

void kanbudb_atomic_store_u32(uint32_t* ptr, uint32_t value) {
#ifdef _WIN32
  InterlockedExchange((volatile LONG*)ptr, (LONG)value);
#elif defined(__GNUC__) || defined(__clang__)
  __atomic_store_n(ptr, value, __ATOMIC_SEQ_CST);
#else
  *ptr = value;
#endif
}

uint32_t kanbudb_atomic_load_u32(const uint32_t* ptr) {
#ifdef _WIN32
  return (uint32_t)InterlockedOr((volatile LONG*)ptr, 0);
#elif defined(__GNUC__) || defined(__clang__)
  return __atomic_load_n(ptr, __ATOMIC_SEQ_CST);
#else
  return *ptr;
#endif
}
