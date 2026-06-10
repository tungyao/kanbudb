#include "db.h"
#include "macros.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define TEST_DB_PATH "/tmp/kanbudb_test_db"

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
}

static int test_open_close(void) {
  cleanup();

  db_t* db = NULL;
  int rc = db_open(TEST_DB_PATH, NULL, &db);
  if (rc != KANBUDB_OK) return 0;
  if (!db) return 0;

  rc = db_close(db);
  if (rc != KANBUDB_OK) return 0;

  char path[256];
  snprintf(path, sizeof(path), "%s.wal", TEST_DB_PATH);
  FILE* f = fopen(path, "rb");
  if (!f) return 0;
  fclose(f);

  cleanup();
  return 1;
}

static int test_create_table(void) {
  cleanup();

  db_t* db = NULL;
  if (db_open(TEST_DB_PATH, NULL, &db) != KANBUDB_OK) return 0;

  const char* col_names[] = {"id", "name", "age"};
  kanbudb_col_type_t col_types[] = {KANBUDB_INT32, KANBUDB_STRING, KANBUDB_INT32};
  int rc = db_create_table(db, "users", col_names, col_types, 3, "id");
  if (rc != KANBUDB_OK) { db_close(db); return 0; }

  rc = db_close(db);
  cleanup();
  return rc == KANBUDB_OK ? 1 : 0;
}

static int test_put_get(void) {
  cleanup();

  db_t* db = NULL;
  if (db_open(TEST_DB_PATH, NULL, &db) != KANBUDB_OK) return 0;

  const char* col_names[] = {"id", "name"};
  kanbudb_col_type_t col_types[] = {KANBUDB_INT32, KANBUDB_STRING};
  if (db_create_table(db, "test", col_names, col_types, 2, "id") != KANBUDB_OK) {
    db_close(db); return 0;
  }

  const char* key = "mykey";
  const char* val = "myvalue";
  int rc = db_put(db, "test", key, strlen(key) + 1, val, strlen(val) + 1);
  if (rc != KANBUDB_OK) { db_close(db); return 0; }

  void* out_val = NULL;
  size_t out_len = 0;
  rc = db_get(db, "test", key, strlen(key) + 1, &out_val, &out_len);
  if (rc != KANBUDB_OK) { db_close(db); return 0; }
  if (out_len != strlen(val) + 1) { db_close(db); return 0; }
  if (memcmp(out_val, val, out_len) != 0) { db_close(db); return 0; }

  rc = db_close(db);
  cleanup();
  return rc == KANBUDB_OK ? 1 : 0;
}

static int test_put_delete_get(void) {
  cleanup();

  db_t* db = NULL;
  if (db_open(TEST_DB_PATH, NULL, &db) != KANBUDB_OK) return 0;

  const char* col_names[] = {"id"};
  kanbudb_col_type_t col_types[] = {KANBUDB_INT32};
  if (db_create_table(db, "del", col_names, col_types, 1, "id") != KANBUDB_OK) {
    db_close(db); return 0;
  }

  const char* key = "delete_me";
  const char* val = "temporary";
  if (db_put(db, "del", key, strlen(key) + 1, val, strlen(val) + 1) != KANBUDB_OK) {
    db_close(db); return 0;
  }

  if (db_delete(db, "del", key, strlen(key) + 1) != KANBUDB_OK) {
    db_close(db); return 0;
  }

  void* out_val = NULL;
  size_t out_len = 0;
  int rc = db_get(db, "del", key, strlen(key) + 1, &out_val, &out_len);
  if (rc != KANBUDB_ERR_NOTFOUND) { db_close(db); return 0; }

  rc = db_close(db);
  cleanup();
  return rc == KANBUDB_OK ? 1 : 0;
}

static int test_get_nonexistent(void) {
  cleanup();

  db_t* db = NULL;
  if (db_open(TEST_DB_PATH, NULL, &db) != KANBUDB_OK) return 0;

  const char* col_names[] = {"id"};
  kanbudb_col_type_t col_types[] = {KANBUDB_INT32};
  if (db_create_table(db, "empty", col_names, col_types, 1, "id") != KANBUDB_OK) {
    db_close(db); return 0;
  }

  void* out_val = NULL;
  size_t out_len = 0;
  int rc = db_get(db, "empty", "nobody", 7, &out_val, &out_len);
  if (rc != KANBUDB_ERR_NOTFOUND) { db_close(db); return 0; }

  rc = db_close(db);
  cleanup();
  return rc == KANBUDB_OK ? 1 : 0;
}

static int test_create_table_duplicate(void) {
  cleanup();

  db_t* db = NULL;
  if (db_open(TEST_DB_PATH, NULL, &db) != KANBUDB_OK) return 0;

  const char* col_names[] = {"id"};
  kanbudb_col_type_t col_types[] = {KANBUDB_INT32};
  if (db_create_table(db, "dup", col_names, col_types, 1, "id") != KANBUDB_OK) {
    db_close(db); return 0;
  }

  int rc = db_create_table(db, "dup", col_names, col_types, 1, "id");
  if (rc != KANBUDB_ERR_EXISTS) { db_close(db); return 0; }

  rc = db_close(db);
  cleanup();
  return rc == KANBUDB_OK ? 1 : 0;
}

static int test_error_string(void) {
  const char* s = db_error_string(KANBUDB_OK);
  if (!s) return 0;
  s = db_error_string(KANBUDB_ERR_OOM);
  if (!s) return 0;
  s = db_error_string(KANBUDB_ERR_NOTFOUND);
  if (!s) return 0;
  s = db_error_string(KANBUDB_ERR_EXISTS);
  if (!s) return 0;
  s = db_error_string(-999);
  if (!s) return 0;
  return 1;
}

int main(void) {
  printf("db tests:\n");
  TEST(open_close);
  TEST(create_table);
  TEST(put_get);
  TEST(put_delete_get);
  TEST(get_nonexistent);
  TEST(create_table_duplicate);
  TEST(error_string);
  printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
  return tests_failed > 0 ? 1 : 0;
}
