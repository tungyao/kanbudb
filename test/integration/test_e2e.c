#include "db.h"
#include "macros.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define TEST_DB_PATH "/tmp/kanbudb_e2e_test"

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) do { \
  printf("  TEST: %s ... ", #name); \
  if (test_##name()) { printf("PASS\n"); tests_passed++; } \
  else { printf("FAIL\n"); tests_failed++; } \
} while(0)

static void cleanup(void) {
  char path[256];
  snprintf(path, sizeof(path), "%s.wal", TEST_DB_PATH);
  unlink(path);
  snprintf(path, sizeof(path), "%s.lsm", TEST_DB_PATH);
  unlink(path);
  snprintf(path, sizeof(path), "%s.system", TEST_DB_PATH);
  unlink(path);
  for (int i = 0; i < 10; i++) {
    snprintf(path, sizeof(path), "%s.sst.0.%d", TEST_DB_PATH, i);
    unlink(path);
    snprintf(path, sizeof(path), "%s.ckpt.%d", TEST_DB_PATH, i);
    unlink(path);
  }
}

static int test_e2e_full_workflow(void) {
  cleanup();

  db_config_t config;
  config.fsync_mode = KANBUDB_FSYNC_NONE;
  config.cache_size = 4096;
  config.memtable_size = 1024;
  config.compaction_threads = 1;

  db_t* db = NULL;
  int rc = db_open(TEST_DB_PATH, &config, &db);
  if (rc != KANBUDB_OK || !db) return 0;

  const char* col_names[] = {"id", "title", "body"};
  kanbudb_col_type_t col_types[] = {KANBUDB_INT32, KANBUDB_STRING, KANBUDB_STRING};
  rc = db_create_table(db, "articles", col_names, col_types, 3, "id");
  if (rc != KANBUDB_OK) { db_close(db); return 0; }

  int n = 10;
  char keys[10][32];
  char values[10][128];
  for (int i = 0; i < n; i++) {
    snprintf(keys[i], sizeof(keys[i]), "article_%d", i);
    snprintf(values[i], sizeof(values[i]),
             "{\"id\":%d,\"title\":\"Title %d\",\"body\":\"Body content %d\"}", i, i, i);
    rc = db_put(db, "articles", keys[i], strlen(keys[i]) + 1,
                values[i], strlen(values[i]) + 1);
    if (rc != KANBUDB_OK) { db_close(db); return 0; }
  }

  for (int i = 0; i < n; i++) {
    void* out_val = NULL;
    size_t out_len = 0;
    rc = db_get(db, "articles", keys[i], strlen(keys[i]) + 1, &out_val, &out_len);
    if (rc != KANBUDB_OK) { db_close(db); return 0; }
    if (out_len != strlen(values[i]) + 1) { db_close(db); return 0; }
    if (memcmp(out_val, values[i], out_len) != 0) { db_close(db); return 0; }
  }

  rc = db_close(db);
  cleanup();
  return rc == KANBUDB_OK ? 1 : 0;
}

int main(void) {
  printf("e2e integration tests:\n");
  TEST(e2e_full_workflow);
  printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
  return tests_failed > 0 ? 1 : 0;
}
