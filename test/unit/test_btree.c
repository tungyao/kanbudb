#include "btree.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) do { \
  printf("  TEST: %s ... ", #name); \
  if (test_##name()) { printf("PASS\n"); tests_passed++; } \
  else { printf("FAIL\n"); tests_failed++; } \
} while(0)

static int test_create_destroy(void) {
  kanbudb_btree_t* bt = btree_create();
  if (!bt) return 0;
  btree_destroy(bt);
  return 1;
}

static int test_put_get_single(void) {
  kanbudb_btree_t* bt = btree_create();
  if (!bt) return 0;

  const char* key = "hello";
  const char* val = "world";
  int rc = btree_put(bt, key, strlen(key) + 1, val, strlen(val) + 1);
  if (rc != KANBUDB_OK) { btree_destroy(bt); return 0; }

  void* out_val = NULL;
  size_t out_len = 0;
  rc = btree_get(bt, key, strlen(key) + 1, &out_val, &out_len);
  if (rc != KANBUDB_OK) { btree_destroy(bt); return 0; }
  if (out_len != strlen(val) + 1) { btree_destroy(bt); return 0; }
  if (memcmp(out_val, val, out_len) != 0) { btree_destroy(bt); return 0; }

  btree_destroy(bt);
  return 1;
}

static int test_get_nonexistent(void) {
  kanbudb_btree_t* bt = btree_create();
  if (!bt) return 0;

  const char* key = "nope";
  void* out_val = NULL;
  size_t out_len = 0;
  int rc = btree_get(bt, key, strlen(key) + 1, &out_val, &out_len);
  if (rc != KANBUDB_ERR_NOTFOUND) { btree_destroy(bt); return 0; }

  btree_destroy(bt);
  return 1;
}

static int test_put_get_many(void) {
  kanbudb_btree_t* bt = btree_create();
  if (!bt) return 0;

  int n = 100;
  char keys[100][16];
  char vals[100][16];

  for (int i = 0; i < n; i++) {
    snprintf(keys[i], sizeof(keys[i]), "key%d", i);
    snprintf(vals[i], sizeof(vals[i]), "val%d", i);
    int rc = btree_put(bt, keys[i], strlen(keys[i]) + 1,
                       vals[i], strlen(vals[i]) + 1);
    if (rc != KANBUDB_OK) { btree_destroy(bt); return 0; }
  }

  for (int i = 0; i < n; i++) {
    void* out_val = NULL;
    size_t out_len = 0;
    int rc = btree_get(bt, keys[i], strlen(keys[i]) + 1, &out_val, &out_len);
    if (rc != KANBUDB_OK) { btree_destroy(bt); return 0; }
    if (out_len != strlen(vals[i]) + 1) { btree_destroy(bt); return 0; }
    if (memcmp(out_val, vals[i], out_len) != 0) { btree_destroy(bt); return 0; }
  }

  btree_destroy(bt);
  return 1;
}

static int test_cursor_iteration(void) {
  kanbudb_btree_t* bt = btree_create();
  if (!bt) return 0;

  int n = 50;
  char keys[50][16];
  char vals[50][16];

  for (int i = 0; i < n; i++) {
    snprintf(keys[i], sizeof(keys[i]), "k%03d", i);
    snprintf(vals[i], sizeof(vals[i]), "v%03d", i);
    int rc = btree_put(bt, keys[i], strlen(keys[i]) + 1,
                       vals[i], strlen(vals[i]) + 1);
    if (rc != KANBUDB_OK) { btree_destroy(bt); return 0; }
  }

  btree_cursor_t* cur = btree_cursor_create(bt);
  if (!cur) { btree_destroy(bt); return 0; }

  int count = 0;
  int rc = btree_cursor_seek(cur, NULL, 0);
  if (rc != KANBUDB_OK) { btree_cursor_destroy(cur); btree_destroy(bt); return 0; }

  btree_kv_t kv;
  while (btree_cursor_next(cur, &kv) == KANBUDB_OK) {
    if (count >= n) { btree_cursor_destroy(cur); btree_destroy(bt); return 0; }
    if (kv.key_len != strlen(keys[count]) + 1) { btree_cursor_destroy(cur); btree_destroy(bt); return 0; }
    if (memcmp(kv.key, keys[count], kv.key_len) != 0) { btree_cursor_destroy(cur); btree_destroy(bt); return 0; }
    if (kv.val_len != strlen(vals[count]) + 1) { btree_cursor_destroy(cur); btree_destroy(bt); return 0; }
    if (memcmp(kv.value, vals[count], kv.val_len) != 0) { btree_cursor_destroy(cur); btree_destroy(bt); return 0; }
    count++;
  }

  if (count != n) { btree_cursor_destroy(cur); btree_destroy(bt); return 0; }

  btree_cursor_destroy(cur);
  btree_destroy(bt);
  return 1;
}

int main(void) {
  printf("btree tests:\n");
  TEST(create_destroy);
  TEST(put_get_single);
  TEST(get_nonexistent);
  TEST(put_get_many);
  TEST(cursor_iteration);
  printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
  return tests_failed > 0 ? 1 : 0;
}
