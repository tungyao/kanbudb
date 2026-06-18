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

  int written = wal_mmap_append(&wm, 1, 0, 1, "hello", 5, "world", 5);
  assert(written > 0);
  assert(wm.header->write_pos > KANBUDB_WAL_MMAP_HEADER_SIZE);

  written = wal_mmap_append(&wm, 2, 0, 1, "foo", 3, "bar", 3);
  assert(written > 0);

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

  wal_mmap_append(&wm, 1, 0, 1, "key1", 4, "val1", 4);
  wal_mmap_append(&wm, 2, 1, 1, "key2", 4, NULL, 0);

  kanbudb_wal_mmap_t reader;
  wal_mmap_open(path, 0, 0, &reader);

  uint64_t seq; int op; uint64_t tid;
  void* key; size_t kl; void* val; size_t vl;

  int rc = wal_mmap_read_entry(&reader, &seq, &op, &tid, &key, &kl, &val, &vl);
  assert(rc == 0);
  assert(seq == 1);
  assert(op == 0);
  assert(tid == 1);
  assert(kl == 4 && memcmp(key, "key1", 4) == 0);
  assert(vl == 4 && memcmp(val, "val1", 4) == 0);

  rc = wal_mmap_read_entry(&reader, &seq, &op, &tid, &key, &kl, &val, &vl);
  assert(rc == 0);
  assert(seq == 2);
  assert(op == 1);
  assert(kl == 4 && memcmp(key, "key2", 4) == 0);

  rc = wal_mmap_read_entry(&reader, &seq, &op, &tid, &key, &kl, &val, &vl);
  assert(rc == -1);

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
