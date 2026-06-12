#include "db.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

#define TEST_DB_PATH "/tmp/kanbudb_test_thread"
#define NUM_WRITERS 4
#define NUM_READERS 4
#define ITERS_PER_THREAD 200

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
  for (int i = 0; i < 30; i++) {
    snprintf(path, sizeof(path), "%s.sst.0.%d", TEST_DB_PATH, i);
    unlink(path);
    snprintf(path, sizeof(path), "%s.ckpt.%d", TEST_DB_PATH, i);
    unlink(path);
  }
}

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) do { \
  printf("  TEST: %s ... ", #name); \
  if (test_##name()) { printf("PASS\n"); tests_passed++; } \
  else { printf("FAIL\n"); tests_failed++; } \
} while(0)

static int encode_row(int id, const char* name, int val, unsigned char* buf) {
  *(int*)buf = id;
  unsigned int nlen = (unsigned int)strlen(name);
  memcpy(buf + 4, &nlen, 4);
  memcpy(buf + 8, name, nlen);
  *(int*)(buf + 8 + nlen) = val;
  return 8 + (int)nlen + 4;
}

typedef struct {
  db_t* db;
  int thread_id;
  int errors;
} writer_ctx_t;

typedef struct {
  db_t* db;
  int thread_id;
  int errors;
} reader_ctx_t;

static void* writer_func(void* arg) {
  writer_ctx_t* ctx = (writer_ctx_t*)arg;
  unsigned char buf[256];
  char key[32];
  int errors = 0;

  for (int i = 0; i < ITERS_PER_THREAD; i++) {
    int id = ctx->thread_id * ITERS_PER_THREAD + i;
    char name[16];
    snprintf(name, sizeof(name), "w%d_%d", ctx->thread_id, i);
    int len = encode_row(id, name, id * 10, buf);
    snprintf(key, sizeof(key), "key_%d", id);

    int rc = db_put(ctx->db, "threads", key, strlen(key) + 1, buf, (size_t)len);
    if (rc != KANBUDB_OK) errors++;
  }

  ctx->errors = errors;
  return NULL;
}

static void* reader_func(void* arg) {
  reader_ctx_t* ctx = (reader_ctx_t*)arg;
  int errors = 0;

  for (int i = 0; i < ITERS_PER_THREAD; i++) {
    int id = i;
    char key[32];
    snprintf(key, sizeof(key), "key_%d", id);

    void* val = NULL;
    size_t val_len = 0;
    db_get(ctx->db, "threads", key, strlen(key) + 1, &val, &val_len);
  }

  ctx->errors = errors;
  return NULL;
}

static int test_concurrent_read_write(void) {
  cleanup();

  db_t* db = NULL;
  if (db_open(TEST_DB_PATH, NULL, &db) != KANBUDB_OK) return 0;

  const char* col_names[] = {"id", "name", "val"};
  kanbudb_col_type_t col_types[] = {KANBUDB_INT32, KANBUDB_STRING, KANBUDB_INT32};
  if (db_create_table(db, "threads", col_names, col_types, 3, "id") != KANBUDB_OK) {
    db_close(db); return 0;
  }

  pthread_t writers[NUM_WRITERS];
  writer_ctx_t wctx[NUM_WRITERS];
  pthread_t readers[NUM_READERS];
  reader_ctx_t rctx[NUM_READERS];

  for (int i = 0; i < NUM_READERS; i++) {
    rctx[i].db = db;
    rctx[i].thread_id = i;
    rctx[i].errors = 0;
    pthread_create(&readers[i], NULL, reader_func, &rctx[i]);
  }

  for (int i = 0; i < NUM_WRITERS; i++) {
    wctx[i].db = db;
    wctx[i].thread_id = i;
    wctx[i].errors = 0;
    pthread_create(&writers[i], NULL, writer_func, &wctx[i]);
  }

  for (int i = 0; i < NUM_WRITERS; i++) {
    pthread_join(writers[i], NULL);
    if (wctx[i].errors > 0) { db_close(db); cleanup(); return 0; }
  }
  for (int i = 0; i < NUM_READERS; i++) {
    pthread_join(readers[i], NULL);
    if (rctx[i].errors > 0) { db_close(db); cleanup(); return 0; }
  }

  int total_expected = NUM_WRITERS * ITERS_PER_THREAD;
  int found = 0;
  for (int i = 0; i < total_expected; i++) {
    char key[32];
    snprintf(key, sizeof(key), "key_%d", i);
    void* val = NULL;
    size_t val_len = 0;
    if (db_get(db, "threads", key, strlen(key) + 1, &val, &val_len) == KANBUDB_OK) {
      if (val_len > 0) found++;
    }
  }

  if (found != total_expected) {
    fprintf(stderr, "  expected %d rows, found %d\n", total_expected, found);
    db_close(db); cleanup(); return 0;
  }

  db_close(db);
  cleanup();
  return 1;
}

static int test_concurrent_sort_queries(void) {
  cleanup();

  db_t* db = NULL;
  if (db_open(TEST_DB_PATH, NULL, &db) != KANBUDB_OK) return 0;

  const char* col_names[] = {"id", "name", "score"};
  kanbudb_col_type_t col_types[] = {KANBUDB_INT32, KANBUDB_STRING, KANBUDB_INT32};
  if (db_create_table(db, "items", col_names, col_types, 3, "id") != KANBUDB_OK) {
    db_close(db); return 0;
  }

  unsigned char buf[256];
  for (int i = 0; i < 50; i++) {
    char name[16];
    snprintf(name, sizeof(name), "item_%d", i);
    *(int*)buf = i + 1;
    unsigned int nlen = (unsigned int)strlen(name);
    memcpy(buf + 4, &nlen, 4);
    memcpy(buf + 8, name, nlen);
    *(int*)(buf + 8 + nlen) = (i + 1) * 10;
    int len = 8 + (int)nlen + 4;
    char key[16];
    snprintf(key, sizeof(key), "k%d", i);
    db_put(db, "items", key, strlen(key) + 1, buf, (size_t)len);
  }

  pthread_t readers[NUM_READERS];
  reader_ctx_t rctx[NUM_READERS];
  int all_ok = 1;

  for (int i = 0; i < NUM_READERS; i++) {
    rctx[i].db = db;
    rctx[i].thread_id = i;
    rctx[i].errors = 0;
    pthread_create(&readers[i], NULL, reader_func, &rctx[i]);
  }

  for (int i = 0; i < NUM_READERS; i++) {
    pthread_join(readers[i], NULL);
    if (rctx[i].errors > 0) all_ok = 0;
  }

  db_close(db);
  cleanup();
  return all_ok;
}

int main(void) {
  printf("thread safety tests:\n");
  TEST(concurrent_read_write);
  TEST(concurrent_sort_queries);
  printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
  return tests_failed > 0 ? 1 : 0;
}
