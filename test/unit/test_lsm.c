#include "lsm.h"
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

static int test_memtable_put_get(void) {
  kanbudb_memtable_t* mt = memtable_create(65536);
  if (!mt) return 0;

  const char* key = "hello";
  const char* val = "world";
  int rc = memtable_put(mt, 1, key, strlen(key) + 1, val, strlen(val) + 1);
  if (rc != KANBUDB_OK) { memtable_destroy(mt); return 0; }

  void* out_val = NULL;
  size_t out_len = 0;
  int deleted = 0;
  rc = memtable_get(mt, key, strlen(key) + 1, &out_val, &out_len, &deleted);
  if (rc != KANBUDB_OK) { memtable_destroy(mt); return 0; }
  if (deleted) { memtable_destroy(mt); return 0; }
  if (out_len != strlen(val) + 1) { memtable_destroy(mt); return 0; }
  if (memcmp(out_val, val, out_len) != 0) { memtable_destroy(mt); return 0; }

  memtable_destroy(mt);
  return 1;
}

static int test_memtable_delete(void) {
  kanbudb_memtable_t* mt = memtable_create(65536);
  if (!mt) return 0;

  const char* key = "todelete";
  const char* val = "someval";
  memtable_put(mt, 1, key, strlen(key) + 1, val, strlen(val) + 1);

  int rc = memtable_delete(mt, 2, key, strlen(key) + 1);
  if (rc != KANBUDB_OK) { memtable_destroy(mt); return 0; }

  void* out_val = NULL;
  size_t out_len = 0;
  int deleted = 0;
  rc = memtable_get(mt, key, strlen(key) + 1, &out_val, &out_len, &deleted);
  if (rc != KANBUDB_ERR_NOTFOUND) { memtable_destroy(mt); return 0; }
  if (!deleted) { memtable_destroy(mt); return 0; }

  memtable_destroy(mt);
  return 1;
}

static int test_memtable_is_full(void) {
  kanbudb_memtable_t* mt = memtable_create(128);
  if (!mt) return 0;

  if (memtable_is_full(mt)) { memtable_destroy(mt); return 0; }

  for (int i = 0; i < 50; i++) {
    char k[8], v[8];
    snprintf(k, sizeof(k), "k%d", i);
    snprintf(v, sizeof(v), "v%d", i);
    memtable_put(mt, i, k, strlen(k) + 1, v, strlen(v) + 1);
  }

  if (!memtable_is_full(mt)) { memtable_destroy(mt); return 0; }

  memtable_destroy(mt);
  return 1;
}

typedef struct {
  int count;
  char keys[16][32];
  size_t key_lens[16];
  int deleted[16];
} iter_ctx_t;

static int iter_cb(const lsm_entry_t* entry, void* ctx) {
  iter_ctx_t* c = (iter_ctx_t*)ctx;
  if (c->count >= 16) return -1;
  size_t klen = entry->key_len < 32 ? entry->key_len : 31;
  memcpy(c->keys[c->count], entry->key, klen);
  c->keys[c->count][klen] = '\0';
  c->key_lens[c->count] = entry->key_len;
  c->deleted[c->count] = entry->deleted;
  c->count++;
  return 0;
}

static int test_memtable_iterate(void) {
  kanbudb_memtable_t* mt = memtable_create(65536);
  if (!mt) return 0;

  memtable_put(mt, 1, "b", 2, "bb", 3);
  memtable_put(mt, 2, "a", 2, "aa", 3);
  memtable_put(mt, 3, "c", 2, "cc", 3);
  memtable_delete(mt, 4, "b", 2);

  iter_ctx_t ctx;
  memset(&ctx, 0, sizeof(ctx));
  int rc = memtable_iterate(mt, iter_cb, &ctx);
  if (rc != KANBUDB_OK) { memtable_destroy(mt); return 0; }
  if (ctx.count != 3) { memtable_destroy(mt); return 0; }

  int ok = (strcmp(ctx.keys[0], "a") == 0);
  ok = ok && (strcmp(ctx.keys[1], "b") == 0);
  ok = ok && (ctx.deleted[1] == 1);
  ok = ok && (strcmp(ctx.keys[2], "c") == 0);

  memtable_destroy(mt);
  return ok;
}

static int test_lsm_put_get(void) {
  kanbudb_lsm_t* lsm = lsm_create("/tmp/kanbudb_test_lsm", 65536);
  if (!lsm) return 0;

  const char* key = "lsmkey";
  const char* val = "lsmval";
  int rc = lsm_put(lsm, 1, key, strlen(key) + 1, val, strlen(val) + 1);
  if (rc != KANBUDB_OK) { lsm_destroy(lsm); return 0; }

  void* out_val = NULL;
  size_t out_len = 0;
  rc = lsm_get(lsm, 1, key, strlen(key) + 1, &out_val, &out_len);
  if (rc != KANBUDB_OK) { lsm_destroy(lsm); return 0; }
  if (out_len != strlen(val) + 1) { lsm_destroy(lsm); return 0; }
  if (memcmp(out_val, val, out_len) != 0) { lsm_destroy(lsm); return 0; }

  lsm_destroy(lsm);
  return 1;
}

static int test_lsm_delete(void) {
  kanbudb_lsm_t* lsm = lsm_create("/tmp/kanbudb_test_lsm", 65536);
  if (!lsm) return 0;

  const char* key = "delkey";
  const char* val = "delval";
  lsm_put(lsm, 1, key, strlen(key) + 1, val, strlen(val) + 1);

  int rc = lsm_delete(lsm, 1, key, strlen(key) + 1);
  if (rc != KANBUDB_OK) { lsm_destroy(lsm); return 0; }

  void* out_val = NULL;
  size_t out_len = 0;
  rc = lsm_get(lsm, 1, key, strlen(key) + 1, &out_val, &out_len);
  if (rc != KANBUDB_ERR_NOTFOUND) { lsm_destroy(lsm); return 0; }

  lsm_destroy(lsm);
  return 1;
}

static int test_lsm_flush(void) {
  kanbudb_lsm_t* lsm = lsm_create("/tmp/kanbudb_test_lsm", 128);
  if (!lsm) return 0;

  const char* key1 = "flush1";
  const char* val1 = "value1";
  lsm_put(lsm, 1, key1, strlen(key1) + 1, val1, strlen(val1) + 1);

  int rc = lsm_flush(lsm);
  if (rc != KANBUDB_OK) { lsm_destroy(lsm); return 0; }

  const char* key2 = "flush2";
  const char* val2 = "value2";
  lsm_put(lsm, 1, key2, strlen(key2) + 1, val2, strlen(val2) + 1);

  void* out_val = NULL;
  size_t out_len = 0;
  rc = lsm_get(lsm, 1, key1, strlen(key1) + 1, &out_val, &out_len);
  if (rc != KANBUDB_OK) { lsm_destroy(lsm); return 0; }
  if (memcmp(out_val, val1, out_len) != 0) { lsm_destroy(lsm); return 0; }

  rc = lsm_get(lsm, 1, key2, strlen(key2) + 1, &out_val, &out_len);
  if (rc != KANBUDB_OK) { lsm_destroy(lsm); return 0; }
  if (memcmp(out_val, val2, out_len) != 0) { lsm_destroy(lsm); return 0; }

  lsm_destroy(lsm);
  return 1;
}

int main(void) {
  srand(0);
  printf("lsm tests:\n");
  TEST(memtable_put_get);
  TEST(memtable_delete);
  TEST(memtable_is_full);
  TEST(memtable_iterate);
  TEST(lsm_put_get);
  TEST(lsm_delete);
  TEST(lsm_flush);
  printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
  return tests_failed > 0 ? 1 : 0;
}
