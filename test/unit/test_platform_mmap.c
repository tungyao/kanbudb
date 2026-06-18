#include "platform_mmap.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <fcntl.h>

static void test_mmap_open_close(void) {
  const char* path = "/tmp/test_mmap_basic.dat";
  int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
  assert(fd >= 0);
  uint64_t val = 42;
  write(fd, &val, sizeof(val));
  close(fd);

  kanbudb_mmap_region_t region;
  int rc = kanbudb_mmap_open(path, KANBUDB_MMAP_READ, 0, &region);
  assert(rc == 0);
  assert(region.addr != KANBUDB_MMAP_INVALID);
  assert(region.size == sizeof(uint64_t));

  uint64_t read_val;
  memcpy(&read_val, region.addr, sizeof(read_val));
  assert(read_val == 42);

  kanbudb_mmap_close(&region);
  unlink(path);
  printf("PASS: test_mmap_open_close\n");
}

static void test_mmap_read_write(void) {
  const char* path = "/tmp/test_mmap_rw.dat";
  int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
  assert(fd >= 0);
  uint64_t zeros[4] = {0};
  write(fd, zeros, sizeof(zeros));
  close(fd);

  kanbudb_mmap_region_t region;
  int rc = kanbudb_mmap_open(path, KANBUDB_MMAP_WRITE, sizeof(zeros), &region);
  assert(rc == 0);

  uint64_t val = 99;
  memcpy(region.addr, &val, sizeof(val));

  rc = kanbudb_mmap_sync(&region);
  assert(rc == 0);

  kanbudb_mmap_close(&region);

  rc = kanbudb_mmap_open(path, KANBUDB_MMAP_READ, 0, &region);
  assert(rc == 0);
  uint64_t read_val;
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
