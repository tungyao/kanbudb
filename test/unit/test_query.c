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
  snprintf(path, sizeof(path), "%s.seq", TEST_DB_PATH);
  unlink(path);
  for (int i = 0; i < 256; i++) {
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

/* Encode a row for schema: id(int32) + name(string) + age(int32) */
static int encode_row(i32 id, const char* name, i32 age, u8* buf, i32 buf_size) {
  (void)buf_size;
  i32* id_out = (i32*)buf; *id_out = id;
  u32 name_len = (u32)strlen(name);
  memcpy(buf + 4, &name_len, 4);
  memcpy(buf + 8, name, name_len);
  i32* age_out = (i32*)(buf + 8 + name_len); *age_out = age;
  return 8 + (i32)name_len + 4;
}

static int test_range(void) {
  cleanup();

  db_t* db = NULL;
  if (db_open(TEST_DB_PATH, NULL, &db) != KANBUDB_OK) return 0;

  const char* col_names[] = {"id", "name", "age"};
  kanbudb_col_type_t col_types[] = {KANBUDB_INT32, KANBUDB_STRING, KANBUDB_INT32};
  if (db_create_table(db, "users", col_names, col_types, 3, "id") != KANBUDB_OK) {
    db_close(db); return 0;
  }

  u8 buf[256];
  for (i32 i = 0; i < 5; i++) {
    char name[32]; snprintf(name, sizeof(name), "user_%d", (int)i);
    i32 age = (i + 1) * 10;
    i32 len = encode_row(i + 1, name, age, buf, sizeof(buf));
    char key[16]; snprintf(key, sizeof(key), "key_%d", (int)(i + 1));
    if (db_put(db, "users", key, strlen(key) + 1, buf, (size_t)len) != KANBUDB_OK) {
      db_close(db); return 0;
    }
  }

  /* Test: age > 25 (expect ages 30, 40, 50 = 3 rows) */
  query_builder_t* qb = db_query(db, "users");
  if (qb_filter(qb, "age", ">", "25") != KANBUDB_OK) { qb_destroy(qb); db_close(db); return 0; }
  result_set_t* rs = qb_exec(qb);
  if (!rs) { qb_destroy(qb); db_close(db); return 0; }
  int count = 0;
  while (rs_next(rs)) {
    void* age_data; size_t age_len;
    rs_get_column(rs, 2, &age_data, &age_len);
    if (age_len != 4 || *(const i32*)age_data <= 25) { rs_close(rs); qb_destroy(qb); db_close(db); return 0; }
    count++;
  }
  if (count != 3) { rs_close(rs); qb_destroy(qb); db_close(db); return 0; }
  rs_close(rs);
  qb_destroy(qb);

  /* Test: age <= 20 (expect ages 10, 20 = 2 rows) */
  qb = db_query(db, "users");
  qb_filter(qb, "age", "<=", "20");
  rs = qb_exec(qb);
  count = 0;
  while (rs_next(rs)) { count++; }
  if (count != 2) { rs_close(rs); qb_destroy(qb); db_close(db); return 0; }
  rs_close(rs);
  qb_destroy(qb);

  db_close(db);
  cleanup();
  return 1;
}

static int test_sort(void) {
  cleanup();

  db_t* db = NULL;
  db_open(TEST_DB_PATH, NULL, &db);

  const char* col_names[] = {"id", "name", "age"};
  kanbudb_col_type_t col_types[] = {KANBUDB_INT32, KANBUDB_STRING, KANBUDB_INT32};
  db_create_table(db, "users", col_names, col_types, 3, "id");

  u8 buf[256];
  i32 ages[] = {50, 10, 40, 20, 30};
  for (int i = 0; i < 5; i++) {
    char name[16]; snprintf(name, sizeof(name), "u%d", i);
    i32 len = encode_row((i32)(i + 1), name, ages[i], buf, sizeof(buf));
    char key[16]; snprintf(key, sizeof(key), "k%d", i);
    db_put(db, "users", key, strlen(key) + 1, buf, (size_t)len);
  }

  /* Sort ascending by age */
  query_builder_t* qb = db_query(db, "users");
  qb_sort(qb, "age", 1);
  result_set_t* rs = qb_exec(qb);
  if (!rs) { qb_destroy(qb); db_close(db); return 0; }
  i32 prev = -1;
  int count = 0;
  while (rs_next(rs)) {
    void* d; size_t l;
    rs_get_column(rs, 2, &d, &l);
    i32 age = *(const i32*)d;
    if (age < prev) { rs_close(rs); qb_destroy(qb); db_close(db); cleanup(); return 0; }
    prev = age;
    count++;
  }
  if (count != 5) { rs_close(rs); qb_destroy(qb); db_close(db); cleanup(); return 0; }
  rs_close(rs);
  qb_destroy(qb);

  /* Sort descending by age */
  qb = db_query(db, "users");
  qb_sort(qb, "age", 0);
  rs = qb_exec(qb);
  prev = 999;
  while (rs_next(rs)) {
    void* d; size_t l;
    rs_get_column(rs, 2, &d, &l);
    i32 age = *(const i32*)d;
    if (age > prev) { rs_close(rs); qb_destroy(qb); db_close(db); cleanup(); return 0; }
    prev = age;
  }
  rs_close(rs);
  qb_destroy(qb);

  db_close(db);
  cleanup();
  return 1;
}

static int test_limit(void) {
  cleanup();
  db_t* db = NULL;
  db_open(TEST_DB_PATH, NULL, &db);
  const char* cols[] = {"id", "name", "age"};
  kanbudb_col_type_t ctypes[] = {KANBUDB_INT32, KANBUDB_STRING, KANBUDB_INT32};
  db_create_table(db, "users", cols, ctypes, 3, "id");

  u8 buf[256];
  for (int i = 0; i < 20; i++) {
    char name[16]; snprintf(name, sizeof(name), "u%d", i);
    i32 len = encode_row((i32)i, name, (i32)(i * 10), buf, sizeof(buf));
    char key[16]; snprintf(key, sizeof(key), "k%d", i);
    db_put(db, "users", key, strlen(key) + 1, buf, (size_t)len);
  }

  query_builder_t* qb = db_query(db, "users");
  qb_limit(qb, 5);
  result_set_t* rs = qb_exec(qb);
  int count = 0;
  while (rs_next(rs)) count++;
  if (count != 5) { rs_close(rs); qb_destroy(qb); db_close(db); cleanup(); return 0; }
  rs_close(rs);
  qb_destroy(qb);

  db_close(db);
  cleanup();
  return 1;
}

static int test_limit_sort(void) {
  cleanup();
  db_t* db = NULL;
  db_open(TEST_DB_PATH, NULL, &db);
  const char* cols[] = {"id", "name", "age"};
  kanbudb_col_type_t ctypes[] = {KANBUDB_INT32, KANBUDB_STRING, KANBUDB_INT32};
  db_create_table(db, "users", cols, ctypes, 3, "id");

  u8 buf[256];
  for (int i = 0; i < 10; i++) {
    char name[16]; snprintf(name, sizeof(name), "u%d", i);
    i32 len = encode_row((i32)i, name, (i32)(i * 10), buf, sizeof(buf));
    char key[16]; snprintf(key, sizeof(key), "k%d", i);
    db_put(db, "users", key, strlen(key) + 1, buf, (size_t)len);
  }

  query_builder_t* qb = db_query(db, "users");
  qb_sort(qb, "age", 0);
  qb_limit(qb, 3);
  result_set_t* rs = qb_exec(qb);
  i32 prev = 999;
  int count = 0;
  while (rs_next(rs)) {
    void* d; size_t l;
    rs_get_column(rs, 2, &d, &l);
    i32 age = *(const i32*)d;
    if (age > prev) { rs_close(rs); qb_destroy(qb); db_close(db); cleanup(); return 0; }
    prev = age;
    count++;
  }
  if (count != 3) { rs_close(rs); qb_destroy(qb); db_close(db); cleanup(); return 0; }
  rs_close(rs);
  qb_destroy(qb);

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

/* Helper: create a "users" table with id(int32), name(string), age(int32) and insert test data */
static db_t* setup_users_table(int num_users) {
  db_t* db = NULL;
  if (db_open(TEST_DB_PATH, NULL, &db) != KANBUDB_OK) return NULL;

  const char* col_names[] = {"id", "name", "age"};
  kanbudb_col_type_t col_types[] = {KANBUDB_INT32, KANBUDB_STRING, KANBUDB_INT32};
  if (db_create_table(db, "users", col_names, col_types, 3, "id") != KANBUDB_OK) {
    db_close(db); return NULL;
  }

  /* Users: (1, "alice", 25), (2, "bob", 17), (3, "charlie", 30),
     (4, "diana", 12), (5, "eve", 22) */
  const char* names[] = {"alice", "bob", "charlie", "diana", "eve"};
  i32 ages[] = {25, 17, 30, 12, 22};
  int n = num_users < 5 ? num_users : 5;

  u8 buf[256];
  for (int i = 0; i < n; i++) {
    i32 len = encode_row((i32)(i + 1), names[i], ages[i], buf, sizeof(buf));
    char key[16]; snprintf(key, sizeof(key), "k%d", i + 1);
    if (db_put(db, "users", key, strlen(key) + 1, buf, (size_t)len) != KANBUDB_OK) {
      db_close(db); return NULL;
    }
  }
  return db;
}

static int count_rows(result_set_t* rs) {
  int count = 0;
  while (rs_next(rs)) count++;
  return count;
}

static int test_multi_and(void) {
  cleanup();
  db_t* db = setup_users_table(5);
  if (!db) return 0;

  /* WHERE age > 18 AND age < 28 => alice(25), eve(22) = 2 rows */
  query_builder_t* qb = db_query(db, "users");
  qb_filter(qb, "age", ">", "18");
  qb_filter(qb, "age", "<", "28");
  result_set_t* rs = qb_exec(qb);
  if (!rs) { qb_destroy(qb); db_close(db); cleanup(); return 0; }
  int count = count_rows(rs);
  rs_close(rs);
  qb_destroy(qb);
  db_close(db); cleanup();
  return count == 2;
}

static int test_multi_or(void) {
  cleanup();
  db_t* db = setup_users_table(5);
  if (!db) return 0;

  /* WHERE age < 15 OR age > 28 => diana(12), charlie(30) = 2 rows */
  query_builder_t* qb = db_query(db, "users");
  qb_condition_t* c1 = qb_cond(qb, "age", "<", "15");
  qb_condition_t* c2 = qb_cond(qb, "age", ">", "28");
  qb_condition_t* root = qb_cond_or(qb, c1, c2);
  qb_where(qb, root);
  result_set_t* rs = qb_exec(qb);
  if (!rs) { qb_destroy(qb); db_close(db); cleanup(); return 0; }
  int count = count_rows(rs);
  rs_close(rs);
  qb_destroy(qb);
  db_close(db); cleanup();
  return count == 2;
}

static int test_nested_and_or(void) {
  cleanup();
  db_t* db = setup_users_table(5);
  if (!db) return 0;

  /* WHERE (age > 20 AND age < 28) OR age < 15
     => alice(25), eve(22), diana(12) = 3 rows */
  query_builder_t* qb = db_query(db, "users");
  qb_condition_t* a = qb_cond(qb, "age", ">", "20");
  qb_condition_t* b = qb_cond(qb, "age", "<", "28");
  qb_condition_t* left = qb_cond_and(qb, a, b);
  qb_condition_t* right = qb_cond(qb, "age", "<", "15");
  qb_condition_t* root = qb_cond_or(qb, left, right);
  qb_where(qb, root);
  result_set_t* rs = qb_exec(qb);
  if (!rs) { qb_destroy(qb); db_close(db); cleanup(); return 0; }
  int count = count_rows(rs);
  rs_close(rs);
  qb_destroy(qb);
  db_close(db); cleanup();
  return count == 3;
}

static int test_not(void) {
  cleanup();
  db_t* db = setup_users_table(5);
  if (!db) return 0;

  /* WHERE NOT (age > 20) => bob(17), diana(12) = 2 rows */
  query_builder_t* qb = db_query(db, "users");
  qb_condition_t* inner = qb_cond(qb, "age", ">", "20");
  qb_condition_t* root = qb_cond_not(qb, inner);
  qb_where(qb, root);
  result_set_t* rs = qb_exec(qb);
  if (!rs) { qb_destroy(qb); db_close(db); cleanup(); return 0; }
  int count = count_rows(rs);
  rs_close(rs);
  qb_destroy(qb);
  db_close(db); cleanup();
  return count == 2;
}

static int test_deep_nesting(void) {
  cleanup();
  db_t* db = setup_users_table(5);
  if (!db) return 0;

  /* WHERE NOT (age > 18 AND (age < 20 OR name = "eve"))
     age > 18 AND (age < 20 OR name = "eve")
     => (25: F), (17: F), (30: F), (12: F), (22: name=eve -> T) => only eve matches inner
     NOT => alice(25), bob(17), charlie(30), diana(12) = 4 rows */
  query_builder_t* qb = db_query(db, "users");
  qb_condition_t* age_gt18 = qb_cond(qb, "age", ">", "18");
  qb_condition_t* age_lt20 = qb_cond(qb, "age", "<", "20");
  qb_condition_t* name_eve = qb_cond(qb, "name", "=", "eve");
  qb_condition_t* or_node = qb_cond_or(qb, age_lt20, name_eve);
  qb_condition_t* and_node = qb_cond_and(qb, age_gt18, or_node);
  qb_condition_t* root = qb_cond_not(qb, and_node);
  qb_where(qb, root);
  result_set_t* rs = qb_exec(qb);
  if (!rs) { qb_destroy(qb); db_close(db); cleanup(); return 0; }
  int count = count_rows(rs);
  rs_close(rs);
  qb_destroy(qb);
  db_close(db); cleanup();
  return count == 4;
}

static int test_backward_compat(void) {
  cleanup();
  db_t* db = setup_users_table(5);
  if (!db) return 0;

  /* Old-style qb_filter still works: age > 25 => charlie(30) = 1 row */
  query_builder_t* qb = db_query(db, "users");
  qb_filter(qb, "age", ">", "25");
  result_set_t* rs = qb_exec(qb);
  if (!rs) { qb_destroy(qb); db_close(db); cleanup(); return 0; }
  int count = count_rows(rs);
  rs_close(rs);
  qb_destroy(qb);
  db_close(db); cleanup();
  return count == 1;
}

int main(void) {
  printf("query builder tests:\n");
  TEST(lifecycle);
  TEST(chain);
  TEST(no_crash);
  TEST(from_override);
  TEST(range);
  TEST(sort);
  TEST(limit);
  TEST(limit_sort);
  TEST(multi_and);
  TEST(multi_or);
  TEST(nested_and_or);
  TEST(not);
  TEST(deep_nesting);
  TEST(backward_compat);
  printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
  return tests_failed > 0 ? 1 : 0;
}
