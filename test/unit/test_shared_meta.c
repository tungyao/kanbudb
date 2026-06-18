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

  meta.flushed_seq = 100;
  meta.wal_version = 2;
  rc = kanbudb_shared_meta_write(db, &meta);
  assert(rc == 0);

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
