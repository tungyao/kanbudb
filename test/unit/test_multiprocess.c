#include "db.h"
#include "wal_mmap.h"
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
  snprintf(buf, sizeof(buf), "%s.wal.mmap", DB_PATH); unlink(buf);
  snprintf(buf, sizeof(buf), "%s.shared", DB_PATH); unlink(buf);
  snprintf(buf, sizeof(buf), "%s.system", DB_PATH); unlink(buf);
  snprintf(buf, sizeof(buf), "%s.seq", DB_PATH); unlink(buf);
  for (int i = 0; i < 10; i++) {
    snprintf(buf, sizeof(buf), "%s.sst.0.%d", DB_PATH, i); unlink(buf);
  }
}

static void test_writer_reader_basic(void) {
  cleanup();

  pid_t pid = fork();
  if (pid == 0) {
    db_config_t cfg = { KANBUDB_FSYNC_ALWAYS, 65536, 65536, 1, 1, 10 };
    db_t* db;
    int rc = db_open(DB_PATH, &cfg, &db);
    if (rc != 0) { fprintf(stderr, "writer db_open failed: %d\n", rc); _exit(1); }

    const char* cols[] = {"id", "name"};
    kanbudb_col_type_t types[] = {KANBUDB_INT32, KANBUDB_STRING};
    rc = db_create_table(db, "users", cols, types, 2, "id");

    for (int i = 0; i < 100; i++) {
      char key[32], val[64];
      snprintf(key, sizeof(key), "%d", i);
      snprintf(val, sizeof(val), "user_%d", i);
      rc = db_put(db, "users", key, strlen(key), val, strlen(val) + 1);
    }

    usleep(200000);
    db_close(db);
    _exit(0);
  }

  usleep(50000);

  db_config_t cfg = { KANBUDB_FSYNC_NONE, 65536, 65536, 0, 1, 10 };
  db_t* reader;
  int rc = db_open_reader(DB_PATH, &cfg, &reader);
  assert(rc == 0);

  int status;
  waitpid(pid, &status, 0);
  assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);

  rc = db_reader_refresh(reader);
  assert(rc == 0);

  void* val; size_t val_len;
  rc = db_get(reader, "users", "0", 1, &val, &val_len);
  assert(rc == 0);

  db_close(reader);
  cleanup();
  printf("PASS: test_writer_reader_basic\n");
}

int main(void) {
  test_writer_reader_basic();
  printf("All multiprocess tests passed.\n");
  return 0;
}
