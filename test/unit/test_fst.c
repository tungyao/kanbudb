#include "fst.h"
#include <stdio.h>
#include <string.h>

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) do { \
  printf("  TEST: %s ... ", #name); \
  if (test_##name()) { printf("PASS\n"); tests_passed++; } \
  else { printf("FAIL\n"); tests_failed++; } \
} while(0)

static int test_create_destroy(void) {
  kanbudb_fst_t* fst = fst_create();
  if (!fst) return 0;
  if (fst_size(fst) != 0) { fst_destroy(fst); return 0; }
  if (fst_memory_used(fst) == 0) { fst_destroy(fst); return 0; }
  fst_destroy(fst);
  return 1;
}

static int test_insert_get(void) {
  kanbudb_fst_t* fst = fst_create();
  if (!fst) return 0;

  int rc;
  rc = fst_insert(fst, "hello", 1);
  if (rc != KANBUDB_OK) { fst_destroy(fst); return 0; }

  rc = fst_insert(fst, "world", 2);
  if (rc != KANBUDB_OK) { fst_destroy(fst); return 0; }

  uint64_t val;
  rc = fst_get(fst, "hello", &val);
  if (rc != KANBUDB_OK || val != 1) { fst_destroy(fst); return 0; }

  rc = fst_get(fst, "world", &val);
  if (rc != KANBUDB_OK || val != 2) { fst_destroy(fst); return 0; }

  rc = fst_get(fst, "nonexistent", &val);
  if (rc != KANBUDB_ERR_NOTFOUND) { fst_destroy(fst); return 0; }

  if (fst_size(fst) != 2) { fst_destroy(fst); return 0; }

  fst_destroy(fst);
  return 1;
}

static int test_insert_duplicate(void) {
  kanbudb_fst_t* fst = fst_create();
  if (!fst) return 0;

  fst_insert(fst, "key", 10);
  fst_insert(fst, "key", 20);

  uint64_t val;
  fst_get(fst, "key", &val);

  int ok = (val == 20 && fst_size(fst) == 1);
  fst_destroy(fst);
  return ok;
}

static int test_prefix_search(void) {
  kanbudb_fst_t* fst = fst_create();
  if (!fst) return 0;

  fst_insert(fst, "cat", 1);
  fst_insert(fst, "car", 2);
  fst_insert(fst, "dog", 3);

  uint64_t results[10];
  int n = fst_prefix_search(fst, "ca", results, 10);

  int ok = (n == 2);
  ok = ok && ((results[0] == 1 && results[1] == 2) ||
              (results[0] == 2 && results[1] == 1));

  fst_destroy(fst);
  return ok;
}

static int test_prefix_search_none(void) {
  kanbudb_fst_t* fst = fst_create();
  if (!fst) return 0;

  fst_insert(fst, "cat", 1);
  uint64_t results[10];
  int n = fst_prefix_search(fst, "zz", results, 10);

  int ok = (n == 0);
  fst_destroy(fst);
  return ok;
}

static int test_fuzzy_search(void) {
  kanbudb_fst_t* fst = fst_create();
  if (!fst) return 0;

  fst_insert(fst, "cat", 1);
  fst_insert(fst, "car", 2);
  fst_insert(fst, "bat", 3);

  uint64_t results[10];
  int n = fst_fuzzy_search(fst, "cat", 1, results, 10);

  int ok = (n >= 2);
  int found_cat = 0, found_bat = 0;
  for (int i = 0; i < n; i++) {
    if (results[i] == 1) found_cat = 1;
    if (results[i] == 3) found_bat = 1;
  }
  ok = ok && found_cat && found_bat;

  fst_destroy(fst);
  return ok;
}

static int test_fuzzy_search_exact(void) {
  kanbudb_fst_t* fst = fst_create();
  if (!fst) return 0;

  fst_insert(fst, "hello", 42);
  fst_insert(fst, "world", 99);
  fst_insert(fst, "help", 7);

  uint64_t results[10];
  int n = fst_fuzzy_search(fst, "hello", 0, results, 10);

  int ok = (n == 1 && results[0] == 42);
  fst_destroy(fst);
  return ok;
}

static int test_destroy_null(void) {
  fst_destroy(NULL);
  return 1;
}

static int test_get_null_key(void) {
  kanbudb_fst_t* fst = fst_create();
  if (!fst) return 0;
  uint64_t val;
  int rc = fst_get(fst, NULL, &val);
  int ok = (rc == KANBUDB_ERR_INVAL);
  fst_destroy(fst);
  return ok;
}

static int test_long_key(void) {
  kanbudb_fst_t* fst = fst_create();
  if (!fst) return 0;

  char key[300];
  for (int i = 0; i < 250; i++) key[i] = 'a' + (i % 26);
  key[250] = '\0';

  int rc = fst_insert(fst, key, 42);
  if (rc != KANBUDB_OK) { fst_destroy(fst); return 0; }

  uint64_t val;
  rc = fst_get(fst, key, &val);
  int ok = (rc == KANBUDB_OK && val == 42);
  fst_destroy(fst);
  return ok;
}

static int test_empty_string(void) {
  kanbudb_fst_t* fst = fst_create();
  if (!fst) return 0;

  fst_insert(fst, "", 999);
  uint64_t val;
  int rc = fst_get(fst, "", &val);

  int ok = (rc == KANBUDB_OK && val == 999 && fst_size(fst) == 1);
  fst_destroy(fst);
  return ok;
}

int main(void) {
  printf("fst tests:\n");
  TEST(create_destroy);
  TEST(insert_get);
  TEST(insert_duplicate);
  TEST(prefix_search);
  TEST(prefix_search_none);
  TEST(fuzzy_search);
  TEST(fuzzy_search_exact);
  TEST(empty_string);
  TEST(destroy_null);
  TEST(get_null_key);
  TEST(long_key);
  printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
  return tests_failed > 0 ? 1 : 0;
}
