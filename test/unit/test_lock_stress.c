#include "db.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

#define TEST_DB_PATH "/tmp/kanbudb_test_lock_stress"
#define NUM_WRITERS 8
#define NUM_READERS 8
#define ITERS_PER_THREAD 500

static volatile int stop_flag = 0;

static void cleanup(void) {
    char path[256];
    snprintf(path, sizeof(path), "%s.wal", TEST_DB_PATH); unlink(path);
    snprintf(path, sizeof(path), "%s.lsm", TEST_DB_PATH); unlink(path);
    snprintf(path, sizeof(path), "%s.system", TEST_DB_PATH); unlink(path);
    snprintf(path, sizeof(path), "%s.seq", TEST_DB_PATH); unlink(path);
    for (int i = 0; i < 50; i++) {
        snprintf(path, sizeof(path), "%s.sst.0.%d", TEST_DB_PATH, i); unlink(path);
        snprintf(path, sizeof(path), "%s.ckpt.%d", TEST_DB_PATH, i); unlink(path);
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
    int ops;
} worker_ctx_t;

static void* heavy_writer(void* arg) {
    worker_ctx_t* ctx = (worker_ctx_t*)arg;
    unsigned char buf[256];
    char key[32];
    int errors = 0;

    for (int i = 0; i < ITERS_PER_THREAD; i++) {
        int id = ctx->thread_id * ITERS_PER_THREAD + i;
        snprintf(key, sizeof(key), "key_%d", id);
        snprintf((char*)buf, sizeof(buf), "value_%d_from_thread_%d", id, ctx->thread_id);
        int len = (int)strlen((char*)buf) + 1;

        int rc = db_put(ctx->db, "stress", key, strlen(key) + 1, buf, (size_t)len);
        if (rc != KANBUDB_OK) errors++;

        if (i % 5 == 0) {
            rc = db_delete(ctx->db, "stress", key, strlen(key) + 1);
            if (rc != KANBUDB_OK && rc != KANBUDB_ERR_NOTFOUND) errors++;
        }

        if (i % 3 == 0) {
            void* val = NULL;
            size_t vlen = 0;
            db_get(ctx->db, "stress", key, strlen(key) + 1, &val, &vlen);
        }
    }

    ctx->errors = errors;
    ctx->ops = ITERS_PER_THREAD;
    return NULL;
}

static void* heavy_reader(void* arg) {
    worker_ctx_t* ctx = (worker_ctx_t*)arg;
    int errors = 0;

    for (int i = 0; i < ITERS_PER_THREAD; i++) {
        int id = (ctx->thread_id * ITERS_PER_THREAD + i) % (NUM_WRITERS * ITERS_PER_THREAD);
        char key[32];
        snprintf(key, sizeof(key), "key_%d", id);

        void* val = NULL;
        size_t vlen = 0;
        int rc = db_get(ctx->db, "stress", key, strlen(key) + 1, &val, &vlen);
        (void)rc;
    }

    ctx->errors = errors;
    return NULL;
}

static int test_deadlock_free_heavy_load(void) {
    cleanup();
    stop_flag = 0;

    db_t* db = NULL;
    if (db_open(TEST_DB_PATH, NULL, &db) != KANBUDB_OK) return 0;

    const char* col_names[] = {"id", "data"};
    kanbudb_col_type_t col_types[] = {KANBUDB_INT32, KANBUDB_STRING};
    if (db_create_table(db, "stress", col_names, col_types, 2, "id") != KANBUDB_OK) {
        db_close(db); return 0;
    }

    pthread_t writers[NUM_WRITERS];
    worker_ctx_t wctx[NUM_WRITERS];
    pthread_t readers[NUM_READERS];
    worker_ctx_t rctx[NUM_READERS];

    for (int i = 0; i < NUM_WRITERS; i++) {
        wctx[i].db = db;
        wctx[i].thread_id = i;
        wctx[i].errors = 0;
        pthread_create(&writers[i], NULL, heavy_writer, &wctx[i]);
    }

    for (int i = 0; i < NUM_READERS; i++) {
        rctx[i].db = db;
        rctx[i].thread_id = i;
        rctx[i].errors = 0;
        pthread_create(&readers[i], NULL, heavy_reader, &rctx[i]);
    }

    int all_ok = 1;
    for (int i = 0; i < NUM_WRITERS; i++) {
        pthread_join(writers[i], NULL);
        if (wctx[i].errors > 0) {
            fprintf(stderr, "  writer %d had %d errors\n", i, wctx[i].errors);
            all_ok = 0;
        }
    }
    for (int i = 0; i < NUM_READERS; i++) {
        pthread_join(readers[i], NULL);
        if (rctx[i].errors > 0) {
            fprintf(stderr, "  reader %d had %d errors\n", i, rctx[i].errors);
            all_ok = 0;
        }
    }

    int total_expected = NUM_WRITERS * ITERS_PER_THREAD;
    int found = 0;
    for (int i = 0; i < total_expected; i++) {
        char key[32];
        snprintf(key, sizeof(key), "key_%d", i);
        void* val = NULL;
        size_t vlen = 0;
        if (db_get(db, "stress", key, strlen(key) + 1, &val, &vlen) == KANBUDB_OK)
            found++;
    }

    db_close(db);
    cleanup();

    if (!all_ok) return 0;
    return 1;
}

static int test_table_wal_lsm_btree_lock_order(void) {
    cleanup();

    db_t* db = NULL;
    if (db_open(TEST_DB_PATH, NULL, &db) != KANBUDB_OK) return 0;

    const char* col_names[] = {"id", "name"};
    kanbudb_col_type_t col_types[] = {KANBUDB_INT32, KANBUDB_STRING};
    if (db_create_table(db, "orders", col_names, col_types, 2, "id") != KANBUDB_OK) {
        db_close(db); return 0;
    }

    int n = 2000;
    for (int i = 0; i < n; i++) {
        char key[32], val[64];
        snprintf(key, sizeof(key), "order_%d", i);
        snprintf(val, sizeof(val), "order_data_%d", i);
        int rc = db_put(db, "orders", key, strlen(key) + 1,
                        val, strlen(val) + 1);
        if (rc != KANBUDB_OK) { db_close(db); cleanup(); return 0; }
    }

    int found = 0;
    for (int i = 0; i < n; i++) {
        char key[32];
        snprintf(key, sizeof(key), "order_%d", i);
        void* val = NULL;
        size_t vlen = 0;
        if (db_get(db, "orders", key, strlen(key) + 1, &val, &vlen) == KANBUDB_OK)
            found++;
    }

    if (found != n) {
        fprintf(stderr, "  expected %d, found %d\n", n, found);
        db_close(db); cleanup(); return 0;
    }

    for (int i = 0; i < n; i += 2) {
        char key[32];
        snprintf(key, sizeof(key), "order_%d", i);
        db_delete(db, "orders", key, strlen(key) + 1);
    }

    db_close(db);
    cleanup();
    return 1;
}

typedef struct {
    db_t* db;
    int id;
    int* error;
} ct_ctx_t;

static void* create_table_thread(void* arg) {
    ct_ctx_t* ctx = (ct_ctx_t*)arg;
    char tname[32];
    snprintf(tname, sizeof(tname), "table_%d", ctx->id);

    const char* cols[] = {"id"};
    kanbudb_col_type_t types[] = {KANBUDB_INT32};
    int rc = db_create_table(ctx->db, tname, cols, types, 1, "id");
    if (rc != KANBUDB_OK && rc != KANBUDB_ERR_EXISTS) {
        *ctx->error = 1;
    }
    return NULL;
}

static int test_concurrent_table_create(void) {
    cleanup();

    db_t* db = NULL;
    if (db_open(TEST_DB_PATH, NULL, &db) != KANBUDB_OK) return 0;

    int error = 0;
    pthread_t threads[4];
    ct_ctx_t ctxs[4];

    for (int i = 0; i < 4; i++) {
        ctxs[i].db = db;
        ctxs[i].id = i;
        ctxs[i].error = &error;
        pthread_create(&threads[i], NULL, create_table_thread, &ctxs[i]);
    }

    for (int i = 0; i < 4; i++) pthread_join(threads[i], NULL);

    if (error) { db_close(db); cleanup(); return 0; }

    for (int i = 0; i < 4; i++) {
        char key[32] = "test";
        void* val = NULL;
        size_t vlen = 0;
        db_put(db, "table_0", key, strlen(key) + 1, "data", 5);
        int rc = db_get(db, "table_0", key, strlen(key) + 1, &val, &vlen);
        if (rc != KANBUDB_OK) { db_close(db); cleanup(); return 0; }
    }

    db_close(db);
    cleanup();
    return 1;
}

int main(void) {
    printf("fine-grained lock stress tests:\n");
    TEST(deadlock_free_heavy_load);
    TEST(table_wal_lsm_btree_lock_order);
    TEST(concurrent_table_create);
    printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
