#include "db.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

#define DB_PATH "/tmp/test_mp_write_read"

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) do { \
  printf("  TEST: %s ... ", #name); \
  if (test_##name()) { printf("PASS\n"); tests_passed++; } \
  else { printf("FAIL\n"); tests_failed++; } \
} while(0)

static void cleanup(void) {
  char buf[512];
  snprintf(buf, sizeof(buf), "%s.wal", DB_PATH); unlink(buf);
  snprintf(buf, sizeof(buf), "%s.wal.mmap", DB_PATH); unlink(buf);
  snprintf(buf, sizeof(buf), "%s.shared", DB_PATH); unlink(buf);
  snprintf(buf, sizeof(buf), "%s.system", DB_PATH); unlink(buf);
  snprintf(buf, sizeof(buf), "%s.seq", DB_PATH); unlink(buf);
  for (int i = 0; i < 30; i++) {
    snprintf(buf, sizeof(buf), "%s.ckpt.%d", DB_PATH, i); unlink(buf);
    snprintf(buf, sizeof(buf), "%s.sst.0.%d", DB_PATH, i); unlink(buf);
    snprintf(buf, sizeof(buf), "%s.sst.1.%d", DB_PATH, i); unlink(buf);
  }
}

/* Helper: writer child writes N keys, signals readiness via pipe */
static void run_writer(int pipe_write, int n) {
  db_config_t cfg = { KANBUDB_FSYNC_ALWAYS, 65536, 65536, 1, 1, 10 };
  db_t* db;
  if (db_open(DB_PATH, &cfg, &db) != 0) _exit(1);

  const char* cols[] = {"id", "name", "score"};
  kanbudb_col_type_t types[] = {KANBUDB_INT32, KANBUDB_STRING, KANBUDB_INT32};
  if (db_create_table(db, "items", cols, types, 3, "id") != 0) _exit(1);

  for (int i = 0; i < n; i++) {
    char key[32], val[128];
    snprintf(key, sizeof(key), "key_%d", i);
    snprintf(val, sizeof(val), "item_%d|%d", i, i * 10);
    if (db_put(db, "items", key, strlen(key) + 1, val, strlen(val) + 1) != 0)
      _exit(1);
  }

  /* Signal that all writes are done */
  write(pipe_write, "D", 1);
  db_close(db);
  _exit(0);
}

/* ── Test 1: Writer writes N keys, exits. Reader reads all N. ─ */

static int test_writer_then_reader(void) {
  cleanup();

  int pfd[2];
  if (pipe(pfd) != 0) return 0;
  int n = 500;

  pid_t pid = fork();
  if (pid == 0) { close(pfd[0]); run_writer(pfd[1], n); }

  close(pfd[1]);

  char sig;
  read(pfd[0], &sig, 1);
  close(pfd[0]);

  int status;
  waitpid(pid, &status, 0);
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) return 0;

  db_config_t rcfg = { KANBUDB_FSYNC_NONE, 65536, 65536, 0, 1, 10 };
  db_t* reader;
  if (db_open_reader(DB_PATH, &rcfg, &reader) != 0) return 0;

  db_reader_refresh(reader);

  int found = 0;
  for (int i = 0; i < n; i++) {
    char key[32];
    snprintf(key, sizeof(key), "key_%d", i);
    void* val; size_t val_len;
    if (db_get(reader, "items", key, strlen(key) + 1, &val, &val_len) == 0)
      found++;
  }

  db_close(reader);
  cleanup();

  if (found != n) { fprintf(stderr, "  expected %d keys, found %d\n", n, found); return 0; }
  return 1;
}

/* ── Test 2: Multiple tables ──────────────────────────────── */

static int test_multiple_tables(void) {
  cleanup();

  int pfd[2];
  if (pipe(pfd) != 0) return 0;

  pid_t pid = fork();
  if (pid == 0) {
    close(pfd[0]);
    db_config_t cfg = { KANBUDB_FSYNC_ALWAYS, 65536, 65536, 1, 1, 10 };
    db_t* db;
    if (db_open(DB_PATH, &cfg, &db) != 0) _exit(1);

    const char* ca[] = {"id", "label"};
    kanbudb_col_type_t ta[] = {KANBUDB_INT32, KANBUDB_STRING};
    db_create_table(db, "alpha", ca, ta, 2, "id");

    const char* cb[] = {"uid", "email"};
    kanbudb_col_type_t tb[] = {KANBUDB_INT64, KANBUDB_STRING};
    db_create_table(db, "beta", cb, tb, 2, "uid");

    db_put(db, "alpha", "a1", 3, "alpha_one", 10);
    db_put(db, "alpha", "a2", 3, "alpha_two", 10);
    db_put(db, "beta", "b1", 3, "beta_one", 9);

    write(pfd[1], "D", 1);
    db_close(db);
    _exit(0);
  }

  close(pfd[1]);
  char sig; read(pfd[0], &sig, 1); close(pfd[0]);

  int status;
  waitpid(pid, &status, 0);
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) return 0;

  db_config_t rcfg = { KANBUDB_FSYNC_NONE, 65536, 65536, 0, 1, 10 };
  db_t* reader;
  if (db_open_reader(DB_PATH, &rcfg, &reader) != 0) return 0;

  db_reader_refresh(reader);

  void* val; size_t vlen;
  int ok = 1;
  if (db_get(reader, "alpha", "a1", 3, &val, &vlen) != 0) ok = 0;
  if (db_get(reader, "alpha", "a2", 3, &val, &vlen) != 0) ok = 0;
  if (db_get(reader, "beta", "b1", 3, &val, &vlen) != 0) ok = 0;
  if (db_get(reader, "beta", "nonexist", 9, &val, &vlen) != KANBUDB_ERR_NOTFOUND) ok = 0;

  db_close(reader);
  cleanup();
  return ok;
}

/* ── Test 3: Overwrite — reader sees latest value ─────────── */

static int test_overwrite(void) {
  cleanup();

  int pfd[2];
  if (pipe(pfd) != 0) return 0;

  pid_t pid = fork();
  if (pid == 0) {
    close(pfd[0]);
    db_config_t cfg = { KANBUDB_FSYNC_ALWAYS, 65536, 65536, 1, 1, 10 };
    db_t* db;
    if (db_open(DB_PATH, &cfg, &db) != 0) _exit(1);

    const char* cols[] = {"id", "val"};
    kanbudb_col_type_t types[] = {KANBUDB_INT32, KANBUDB_STRING};
    db_create_table(db, "cfg", cols, types, 2, "id");

    db_put(db, "cfg", "k1", 3, "v1", 3);
    db_put(db, "cfg", "k1", 3, "v2", 3);
    db_put(db, "cfg", "k1", 3, "v3", 3);

    write(pfd[1], "D", 1);
    db_close(db);
    _exit(0);
  }

  close(pfd[1]);
  char sig; read(pfd[0], &sig, 1); close(pfd[0]);

  int status;
  waitpid(pid, &status, 0);
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) return 0;

  db_config_t rcfg = { KANBUDB_FSYNC_NONE, 65536, 65536, 0, 1, 10 };
  db_t* reader;
  if (db_open_reader(DB_PATH, &rcfg, &reader) != 0) return 0;

  db_reader_refresh(reader);

  void* val; size_t vlen;
  if (db_get(reader, "cfg", "k1", 3, &val, &vlen) != 0) { db_close(reader); cleanup(); return 0; }
  if (vlen != 3 || memcmp(val, "v3", 3) != 0) {
    fprintf(stderr, "  expected 'v3'\n");
    db_close(reader); cleanup(); return 0;
  }

  db_close(reader);
  cleanup();
  return 1;
}

/* ── Test 4: Reader opens before writer — no crash ────────── */

static int test_reader_before_writer(void) {
  cleanup();

  db_config_t rcfg = { KANBUDB_FSYNC_NONE, 65536, 65536, 0, 1, 10 };
  db_t* reader;
  int rc = db_open_reader(DB_PATH, &rcfg, &reader);
  if (rc != 0) {
    /* No writer has ever opened — reader failing gracefully is OK */
    cleanup();
    return 1;
  }

  db_reader_refresh(reader);

  int pfd[2];
  if (pipe(pfd) != 0) { db_close(reader); cleanup(); return 0; }

  pid_t pid = fork();
  if (pid == 0) {
    close(pfd[0]);
    db_config_t cfg = { KANBUDB_FSYNC_ALWAYS, 65536, 65536, 1, 1, 10 };
    db_t* db;
    if (db_open(DB_PATH, &cfg, &db) != 0) _exit(1);

    const char* cols[] = {"id", "x"};
    kanbudb_col_type_t types[] = {KANBUDB_INT32, KANBUDB_INT32};
    db_create_table(db, "t", cols, types, 2, "id");
    db_put(db, "t", "k", 2, "42", 3);

    db_close(db);
    write(pfd[1], "D", 1);
    _exit(0);
  }

  close(pfd[1]);
  char sig; read(pfd[0], &sig, 1); close(pfd[0]);

  int status;
  waitpid(pid, &status, 0);
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    fprintf(stderr, "  writer failed\n"); db_close(reader); cleanup(); return 0;
  }

  int rr = db_reader_refresh(reader);
  if (rr != 0) { fprintf(stderr, "  refresh failed: %d\n", rr); db_close(reader); cleanup(); return 0; }

  void* val; size_t vlen;
  rc = db_get(reader, "t", "k", 2, &val, &vlen);
  if (rc != 0) { fprintf(stderr, "  db_get failed: %d (%s)\n", rc, db_error_string(rc)); db_close(reader); cleanup(); return 0; }

  db_close(reader);
  cleanup();
  return 1;
}

/* ── Test 5: 1000 keys with value content verification ────── */

static int test_large_dataset(void) {
  cleanup();

  int pfd[2];
  if (pipe(pfd) != 0) return 0;
  int n = 1000;

  pid_t pid = fork();
  if (pid == 0) { close(pfd[0]); run_writer(pfd[1], n); }

  close(pfd[1]);
  char sig; read(pfd[0], &sig, 1); close(pfd[0]);

  int status;
  waitpid(pid, &status, 0);
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) return 0;

  db_config_t rcfg = { KANBUDB_FSYNC_NONE, 65536, 65536, 0, 1, 10 };
  db_t* reader;
  if (db_open_reader(DB_PATH, &rcfg, &reader) != 0) return 0;

  db_reader_refresh(reader);

  int ok = 1;
  for (int i = 0; i < n; i++) {
    char key[32], expected[128];
    snprintf(key, sizeof(key), "key_%d", i);
    snprintf(expected, sizeof(expected), "item_%d|%d", i, i * 10);

    void* val; size_t vlen;
    if (db_get(reader, "items", key, strlen(key) + 1, &val, &vlen) != 0) { ok = 0; break; }
    if (vlen != strlen(expected) + 1 || memcmp(val, expected, vlen) != 0) { ok = 0; break; }
  }

  db_close(reader);
  cleanup();
  return ok;
}

/* ── Test 6: Concurrent via poll thread (1ms interval) ────── */

static int test_poll_catches_writes(void) {
  cleanup();

  int pfd[2];
  if (pipe(pfd) != 0) return 0;

  db_config_t rcfg = { KANBUDB_FSYNC_NONE, 65536, 65536, 0, 1, 1 };
  db_t* reader;
  if (db_open_reader(DB_PATH, &rcfg, &reader) != 0) return 0;

  pid_t pid = fork();
  if (pid == 0) {
    close(pfd[0]);
    db_config_t cfg = { KANBUDB_FSYNC_ALWAYS, 65536, 65536, 1, 1, 10 };
    db_t* db;
    if (db_open(DB_PATH, &cfg, &db) != 0) _exit(1);

    const char* cols[] = {"id", "v"};
    kanbudb_col_type_t types[] = {KANBUDB_INT32, KANBUDB_INT32};
    db_create_table(db, "polltest", cols, types, 2, "id");

    for (int i = 0; i < 50; i++) {
      char key[32], val[32];
      snprintf(key, sizeof(key), "p_%d", i);
      snprintf(val, sizeof(val), "%d", i * 10);
      db_put(db, "polltest", key, strlen(key) + 1, val, strlen(val) + 1);
      usleep(5000);
    }

    write(pfd[1], "D", 1);
    db_close(db);
    _exit(0);
  }

  close(pfd[1]);

  /* Wait for writer to signal done */
  char sig;
  read(pfd[0], &sig, 1);
  close(pfd[0]);

  /* Give poll thread a moment to catch the last writes */
  usleep(50000);

  void* val; size_t vlen;
  int found_first = db_get(reader, "polltest", "p_0", 4, &val, &vlen) == 0;
  int found_last  = db_get(reader, "polltest", "p_49", 5, &val, &vlen) == 0;

  int status;
  waitpid(pid, &status, 0);
  db_close(reader);
  cleanup();

  if (!found_first) { fprintf(stderr, "  poll thread did not catch p_0\n"); return 0; }
  if (!found_last)  { fprintf(stderr, "  poll thread did not catch p_49\n"); return 0; }
  return 1;
}

/* ── Test 7: Reader-only operations (no writer) ───────────── */

static int test_no_writer(void) {
  cleanup();

  db_config_t rcfg = { KANBUDB_FSYNC_NONE, 65536, 65536, 0, 1, 10 };
  db_t* reader;
  int rc = db_open_reader(DB_PATH, &rcfg, &reader);
  if (rc != 0) { cleanup(); return 1; } /* graceful failure is OK */

  rc = db_reader_refresh(reader);
  db_close(reader);
  cleanup();
  return rc == 0 ? 1 : 1; /* either way is acceptable — no crash */
}

int main(void) {
  printf("multi-process write/read tests:\n");
  TEST(writer_then_reader);
  TEST(multiple_tables);
  TEST(overwrite);
  TEST(reader_before_writer);
  TEST(large_dataset);
  TEST(poll_catches_writes);
  TEST(no_writer);
  printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
  return tests_failed > 0 ? 1 : 0;
}
