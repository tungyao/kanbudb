#include "db.h"
#include "macros.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define TEST_DB_PATH "/tmp/kanbudb_test_fts_integration"

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

static int test_create_index(void) {
  cleanup();

  db_t* db = NULL;
  if (db_open(TEST_DB_PATH, NULL, &db) != KANBUDB_OK) return 0;

  const char* col_names[] = {"id", "title", "body"};
  kanbudb_col_type_t col_types[] = {KANBUDB_INT32, KANBUDB_STRING, KANBUDB_STRING};
  if (db_create_table(db, "docs", col_names, col_types, 3, "id") != KANBUDB_OK) {
    db_close(db); return 0;
  }

  fts_options_t opts = {1, 1, "en"};
  int rc = db_fts_create_index(db, "docs", "title", &opts);
  if (rc != KANBUDB_OK) { db_close(db); return 0; }

  rc = db_fts_create_index(db, "docs", "body", &opts);
  if (rc != KANBUDB_OK) { db_close(db); return 0; }

  rc = db_close(db);
  cleanup();
  return rc == KANBUDB_OK ? 1 : 0;
}

static int test_search_empty(void) {
  cleanup();

  db_t* db = NULL;
  if (db_open(TEST_DB_PATH, NULL, &db) != KANBUDB_OK) return 0;

  const char* col_names[] = {"id", "text"};
  kanbudb_col_type_t col_types[] = {KANBUDB_INT32, KANBUDB_STRING};
  if (db_create_table(db, "articles", col_names, col_types, 2, "id") != KANBUDB_OK) {
    db_close(db); return 0;
  }

  fts_options_t opts = {0, 0, NULL};
  if (db_fts_create_index(db, "articles", "text", &opts) != KANBUDB_OK) {
    db_close(db); return 0;
  }

  result_set_t* rs = NULL;
  int rc = db_fts_search(db, "articles", "text", "hello", &rs);
  if (rc != KANBUDB_OK) { db_close(db); return 0; }
  if (!rs) { db_close(db); return 0; }

  int num_cols = rs_num_columns(rs);
  if (num_cols != 2) { rs_close(rs); db_close(db); return 0; }

  kanbudb_col_type_t t0 = rs_get_column_type(rs, 0);
  kanbudb_col_type_t t1 = rs_get_column_type(rs, 1);
  if (t0 != KANBUDB_INT64) { rs_close(rs); db_close(db); return 0; }
  if (t1 != KANBUDB_DOUBLE) { rs_close(rs); db_close(db); return 0; }

  int has_row = rs_next(rs);
  if (has_row) { rs_close(rs); db_close(db); return 0; }

  rs_close(rs);
  rc = db_close(db);
  cleanup();
  return rc == KANBUDB_OK ? 1 : 0;
}

static int test_search_nonexistent_table(void) {
  cleanup();

  db_t* db = NULL;
  if (db_open(TEST_DB_PATH, NULL, &db) != KANBUDB_OK) return 0;

  result_set_t* rs = NULL;
  int rc = db_fts_search(db, "nonexistent", "col", "test", &rs);
  if (rc != KANBUDB_ERR_NOTFOUND) { db_close(db); return 0; }

  rc = db_close(db);
  cleanup();
  return rc == KANBUDB_OK ? 1 : 0;
}

static int test_drop_index(void) {
  cleanup();

  db_t* db = NULL;
  if (db_open(TEST_DB_PATH, NULL, &db) != KANBUDB_OK) return 0;

  const char* col_names[] = {"id", "content"};
  kanbudb_col_type_t col_types[] = {KANBUDB_INT32, KANBUDB_STRING};
  if (db_create_table(db, "pages", col_names, col_types, 2, "id") != KANBUDB_OK) {
    db_close(db); return 0;
  }

  fts_options_t opts = {0, 0, NULL};
  if (db_fts_create_index(db, "pages", "content", &opts) != KANBUDB_OK) {
    db_close(db); return 0;
  }

  int rc = db_fts_drop_index(db, "pages", "content");
  if (rc != KANBUDB_OK) { db_close(db); return 0; }

  rc = db_close(db);
  cleanup();
  return rc == KANBUDB_OK ? 1 : 0;
}

static int test_rs_close_no_crash(void) {
  rs_close(NULL);
  return 1;
}

int main(void) {
  printf("fts integration tests:\n");
  TEST(create_index);
  TEST(search_empty);
  TEST(search_nonexistent_table);
  TEST(drop_index);
  TEST(rs_close_no_crash);
  printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
  return tests_failed > 0 ? 1 : 0;
}
