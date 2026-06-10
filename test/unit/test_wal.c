#include "wal.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define TEST_WAL_PATH "/tmp/kanbudb_test_wal"

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) do { \
  printf("  TEST: %s ... ", #name); \
  if (test_##name()) { printf("PASS\n"); tests_passed++; } \
  else { printf("FAIL\n"); tests_failed++; } \
} while(0)

typedef struct {
  int count;
  int ops[8];
  uint64_t table_ids[8];
  char keys[8][64];
  char values[8][64];
  size_t key_lens[8];
  size_t val_lens[8];
} replay_ctx_t;

static int replay_callback(int op, uint64_t table_id,
                            const void* key, size_t key_len,
                            const void* value, size_t val_len,
                            void* ctx) {
  replay_ctx_t* c = (replay_ctx_t*)ctx;
  c->ops[c->count] = op;
  c->table_ids[c->count] = table_id;
  memcpy(c->keys[c->count], key, key_len < 64 ? key_len : 63);
  c->keys[c->count][key_len < 64 ? key_len : 63] = '\0';
  c->key_lens[c->count] = key_len;
  if (value && val_len > 0) {
    memcpy(c->values[c->count], value, val_len < 64 ? val_len : 63);
    c->values[c->count][val_len < 64 ? val_len : 63] = '\0';
    c->val_lens[c->count] = val_len;
  } else {
    c->values[c->count][0] = '\0';
    c->val_lens[c->count] = 0;
  }
  c->count++;
  return 0;
}

static int test_create_destroy(void) {
  remove(TEST_WAL_PATH);

  kanbudb_wal_t* wal = wal_create(TEST_WAL_PATH, 0);
  if (!wal) return 0;

  FILE* f = fopen(TEST_WAL_PATH, "rb");
  if (!f) { wal_destroy(wal); return 0; }

  uint64_t magic;
  uint32_t version;
  uint64_t seq;
  int ok = (fread(&magic, sizeof(magic), 1, f) == 1) &&
           (fread(&version, sizeof(version), 1, f) == 1) &&
           (fread(&seq, sizeof(seq), 1, f) == 1);
  fclose(f);

  if (!ok) { wal_destroy(wal); return 0; }

  wal_destroy(wal);
  unlink(TEST_WAL_PATH);
  return 1;
}

static int test_append_replay(void) {
  remove(TEST_WAL_PATH);

  kanbudb_wal_t* wal = wal_create(TEST_WAL_PATH, 0);
  if (!wal) return 0;

  const char* key1 = "hello";
  const char* val1 = "world";
  if (wal_append(wal, KANBUDB_WAL_PUT, 1, key1, strlen(key1) + 1,
                 val1, strlen(val1) + 1) != 0) {
    wal_destroy(wal); return 0;
  }
  if (wal_last_seq(wal) != 1) { wal_destroy(wal); return 0; }

  const char* key2 = "foo";
  if (wal_append(wal, KANBUDB_WAL_DELETE, 2, key2, strlen(key2) + 1,
                 NULL, 0) != 0) {
    wal_destroy(wal); return 0;
  }
  if (wal_last_seq(wal) != 2) { wal_destroy(wal); return 0; }

  wal_destroy(wal);

  wal = wal_create(TEST_WAL_PATH, 0);
  if (!wal) return 0;

  replay_ctx_t ctx;
  memset(&ctx, 0, sizeof(ctx));
  if (wal_replay(wal, replay_callback, &ctx) != 0) {
    wal_destroy(wal); return 0;
  }

  if (wal_last_seq(wal) != 2) { wal_destroy(wal); return 0; }

  int ok = (ctx.count == 2);
  ok = ok && (ctx.ops[0] == KANBUDB_WAL_PUT);
  ok = ok && (ctx.table_ids[0] == 1);
  ok = ok && (strcmp(ctx.keys[0], "hello") == 0);
  ok = ok && (strcmp(ctx.values[0], "world") == 0);
  ok = ok && (ctx.key_lens[0] == strlen("hello") + 1);
  ok = ok && (ctx.val_lens[0] == strlen("world") + 1);

  ok = ok && (ctx.ops[1] == KANBUDB_WAL_DELETE);
  ok = ok && (ctx.table_ids[1] == 2);
  ok = ok && (strcmp(ctx.keys[1], "foo") == 0);
  ok = ok && (ctx.val_lens[1] == 0);

  wal_destroy(wal);
  unlink(TEST_WAL_PATH);
  return ok;
}

static int test_crash_recovery(void) {
  remove(TEST_WAL_PATH);

  kanbudb_wal_t* wal = wal_create(TEST_WAL_PATH, 0);
  if (!wal) return 0;

  const char* key1 = "persist";
  const char* val1 = "data";
  wal_append(wal, KANBUDB_WAL_PUT, 42, key1, strlen(key1) + 1,
             val1, strlen(val1) + 1);

  wal_destroy(wal);

  wal = wal_create(TEST_WAL_PATH, 0);
  if (!wal) return 0;

  replay_ctx_t ctx;
  memset(&ctx, 0, sizeof(ctx));
  int rc = wal_replay(wal, replay_callback, &ctx);
  if (rc != 0) { wal_destroy(wal); return 0; }

  int ok = (ctx.count == 1);
  ok = ok && (strcmp(ctx.keys[0], "persist") == 0);
  ok = ok && (strcmp(ctx.values[0], "data") == 0);

  wal_destroy(wal);
  unlink(TEST_WAL_PATH);
  return ok;
}

static int test_empty_replay(void) {
  unlink(TEST_WAL_PATH);

  kanbudb_wal_t* wal = wal_create(TEST_WAL_PATH, 0);
  if (!wal) return 0;
  wal_destroy(wal);

  wal = wal_create(TEST_WAL_PATH, 0);
  if (!wal) return 0;

  int rc = wal_replay(wal, NULL, NULL);
  if (rc != 0) { wal_destroy(wal); return 0; }

  int ok = (wal_last_seq(wal) == 0);
  wal_destroy(wal);
  unlink(TEST_WAL_PATH);
  return ok;
}

int main(void) {
  printf("wal tests:\n");
  TEST(create_destroy);
  TEST(append_replay);
  TEST(crash_recovery);
  TEST(empty_replay);
  printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
  return tests_failed > 0 ? 1 : 0;
}
