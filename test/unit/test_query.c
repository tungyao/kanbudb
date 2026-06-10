#include "db.h"
#include "macros.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define TEST_DB_PATH "/tmp/kanbudb_test_query"

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

static int test_lifecycle(void) {
  cleanup();

  db_t* db = NULL;
  if (db_open(TEST_DB_PATH, NULL, &db) != KANBUDB_OK) return 0;
  if (!db) return 0;

  query_builder_t* qb = db_query(db, "nonexistent");
  if (!qb) { db_close(db); return 0; }

  qb_destroy(qb);

  db_close(db);
  cleanup();
  return 1;
}

static int test_chain(void) {
  cleanup();

  db_t* db = NULL;
  if (db_open(TEST_DB_PATH, NULL, &db) != KANBUDB_OK) return 0;

  const char* col_names[] = {"id", "name", "age"};
  kanbudb_col_type_t col_types[] = {KANBUDB_INT32, KANBUDB_STRING, KANBUDB_INT32};
  if (db_create_table(db, "users", col_names, col_types, 3, "id") != KANBUDB_OK) {
    db_close(db); return 0;
  }

  query_builder_t* qb = db_query(db, "users");
  if (!qb) { db_close(db); return 0; }

  if (qb_from(qb, "users") != KANBUDB_OK) { qb_destroy(qb); db_close(db); return 0; }
  if (qb_filter(qb, "age", ">", "18") != KANBUDB_OK) { qb_destroy(qb); db_close(db); return 0; }
  if (qb_sort(qb, "name", 1) != KANBUDB_OK) { qb_destroy(qb); db_close(db); return 0; }
  if (qb_limit(qb, 10) != KANBUDB_OK) { qb_destroy(qb); db_close(db); return 0; }
  if (qb_join(qb, "orders", "id", "user_id") != KANBUDB_OK) { qb_destroy(qb); db_close(db); return 0; }

  result_set_t* rs = qb_exec(qb);
  if (!rs) { qb_destroy(qb); db_close(db); return 0; }

  if (rs_num_columns(rs) != 3) { rs_close(rs); qb_destroy(qb); db_close(db); return 0; }

  if (rs_get_column_type(rs, 0) != KANBUDB_INT32) { rs_close(rs); qb_destroy(qb); db_close(db); return 0; }
  if (rs_get_column_type(rs, 1) != KANBUDB_STRING) { rs_close(rs); qb_destroy(qb); db_close(db); return 0; }
  if (rs_get_column_type(rs, 2) != KANBUDB_INT32) { rs_close(rs); qb_destroy(qb); db_close(db); return 0; }

  int count = 0;
  while (rs_next(rs)) {
    count++;
  }
  if (count != 0) { rs_close(rs); qb_destroy(qb); db_close(db); return 0; }

  rs_close(rs);
  qb_destroy(qb);

  db_close(db);
  cleanup();
  return 1;
}

static int test_no_crash(void) {
  cleanup();

  db_t* db = NULL;
  if (db_open(TEST_DB_PATH, NULL, &db) != KANBUDB_OK) return 0;

  query_builder_t* qb = db_query(db, "t");
  if (!qb) { db_close(db); return 0; }

  result_set_t* rs = qb_exec(qb);
  if (!rs) { qb_destroy(qb); db_close(db); return 0; }

  rs_close(rs);
  qb_destroy(qb);

  rs_close(NULL);
  qb_destroy(NULL);

  db_close(db);
  cleanup();
  return 1;
}

static int test_from_override(void) {
  cleanup();

  db_t* db = NULL;
  if (db_open(TEST_DB_PATH, NULL, &db) != KANBUDB_OK) return 0;

  query_builder_t* qb = db_query(db, "original");
  if (!qb) { db_close(db); return 0; }

  if (qb_from(qb, "newtable") != KANBUDB_OK) { qb_destroy(qb); db_close(db); return 0; }

  result_set_t* rs = qb_exec(qb);
  if (!rs) { qb_destroy(qb); db_close(db); return 0; }

  rs_close(rs);
  qb_destroy(qb);

  db_close(db);
  cleanup();
  return 1;
}

int main(void) {
  printf("query builder tests:\n");
  TEST(lifecycle);
  TEST(chain);
  TEST(no_crash);
  TEST(from_override);
  printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
  return tests_failed > 0 ? 1 : 0;
}
