#include "wal.h"
#include "sstable.h"
#include "btree.h"
#include "db.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define TEST_WAL_PATH "/tmp/kanbudb_test_wal_io"

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) do { \
  printf("  TEST: %s ... ", #name); \
  if (test_##name()) { printf("PASS\n"); tests_passed++; } \
  else { printf("FAIL\n"); tests_failed++; } \
} while(0)

static void cleanup(void) {
    unlink(TEST_WAL_PATH ".wal");
}

typedef struct {
    int entries[4096];
    int count;
    uint64_t last_seq;
} replay_ctx_t;

static int replay_cb(int op, uint64_t table_id,
                     const void* key, size_t key_len,
                     const void* value, size_t val_len,
                     void* ctx) {
    (void)op; (void)table_id; (void)key; (void)key_len;
    (void)value; (void)val_len;
    replay_ctx_t* rc = (replay_ctx_t*)ctx;
    rc->last_seq = wal_last_seq(NULL);
    rc->count++;
    return KANBUDB_OK;
}

static int test_wal_append_sync(void) {
    cleanup();

    kanbudb_wal_t* wal = wal_create(TEST_WAL_PATH ".wal", KANBUDB_FSYNC_ALWAYS);
    if (!wal) return 0;

    for (int i = 0; i < 100; i++) {
        char key[32], val[32];
        snprintf(key, sizeof(key), "key_%d", i);
        snprintf(val, sizeof(val), "val_%d", i);
        int rc = wal_append(wal, KANBUDB_WAL_PUT, 1,
                            key, strlen(key) + 1,
                            val, strlen(val) + 1);
        if (rc != KANBUDB_OK) { wal_destroy(wal); return 0; }
    }

    if (wal_last_seq(wal) != 100) { wal_destroy(wal); return 0; }

    wal_destroy(wal);
    cleanup();
    return 1;
}

static int test_wal_replay_full(void) {
    cleanup();

    kanbudb_wal_t* wal = wal_create(TEST_WAL_PATH ".wal", KANBUDB_FSYNC_NONE);
    if (!wal) return 0;

    int n = 500;
    for (int i = 0; i < n; i++) {
        char key[32], val[32];
        snprintf(key, sizeof(key), "rk_%d", i);
        snprintf(val, sizeof(val), "rv_%d", i);
        int rc = wal_append(wal, KANBUDB_WAL_PUT, 2,
                            key, strlen(key) + 1,
                            val, strlen(val) + 1);
        if (rc != KANBUDB_OK) { wal_destroy(wal); return 0; }
    }

    wal_destroy(wal);

    kanbudb_wal_t* wal2 = wal_create(TEST_WAL_PATH ".wal", KANBUDB_FSYNC_NONE);
    if (!wal2) return 0;

    replay_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    int rc = wal_replay(wal2, replay_cb, &ctx);
    if (rc != KANBUDB_OK) { wal_destroy(wal2); return 0; }
    if (ctx.count != n) {
        fprintf(stderr, "  replayed %d entries, expected %d\n", ctx.count, n);
        wal_destroy(wal2); return 0;
    }

    if (wal_last_seq(wal2) != (uint64_t)n) {
        fprintf(stderr, "  seq mismatch: %llu vs %d\n",
                (unsigned long long)wal_last_seq(wal2), n);
        wal_destroy(wal2); return 0;
    }

    wal_destroy(wal2);
    cleanup();
    return 1;
}

static int test_wal_truncate_reuse(void) {
    cleanup();

    kanbudb_wal_t* wal = wal_create(TEST_WAL_PATH ".wal", KANBUDB_FSYNC_NONE);
    if (!wal) return 0;

    for (int i = 0; i < 50; i++) {
        char key[32], val[32];
        snprintf(key, sizeof(key), "tk_%d", i);
        snprintf(val, sizeof(val), "tv_%d", i);
        wal_append(wal, KANBUDB_WAL_PUT, 1, key, strlen(key) + 1,
                   val, strlen(val) + 1);
    }

    int rc = wal_truncate(wal);
    if (rc != KANBUDB_OK) { wal_destroy(wal); return 0; }

    if (wal_last_seq(wal) != 50) { wal_destroy(wal); return 0; }

    for (int i = 0; i < 30; i++) {
        char key[32], val[32];
        snprintf(key, sizeof(key), "new_tk_%d", i);
        snprintf(val, sizeof(val), "new_tv_%d", i);
        wal_append(wal, KANBUDB_WAL_PUT, 1, key, strlen(key) + 1,
                   val, strlen(val) + 1);
    }

    if (wal_last_seq(wal) != 80) { wal_destroy(wal); return 0; }

    replay_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    rc = wal_replay(wal, replay_cb, &ctx);
    if (rc != KANBUDB_OK) { wal_destroy(wal); return 0; }
    if (ctx.count != 30) {
        fprintf(stderr, "  after truncate replayed %d entries, expected 30\n", ctx.count);
        wal_destroy(wal); return 0;
    }

    wal_destroy(wal);
    cleanup();
    return 1;
}

static int test_wal_delete_entries(void) {
    cleanup();

    kanbudb_wal_t* wal = wal_create(TEST_WAL_PATH ".wal", KANBUDB_FSYNC_ALWAYS);
    if (!wal) return 0;

    for (int i = 0; i < 20; i++) {
        char key[32], val[32];
        snprintf(key, sizeof(key), "dk_%d", i);
        if (i % 2 == 0) {
            snprintf(val, sizeof(val), "dv_%d", i);
            wal_append(wal, KANBUDB_WAL_PUT, 1, key, strlen(key) + 1,
                       val, strlen(val) + 1);
        } else {
            wal_append(wal, KANBUDB_WAL_DELETE, 1, key, strlen(key) + 1,
                       NULL, 0);
        }
    }

    wal_destroy(wal);

    kanbudb_wal_t* wal2 = wal_create(TEST_WAL_PATH ".wal", KANBUDB_FSYNC_NONE);
    if (!wal2) return 0;

    replay_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    int rc = wal_replay(wal2, replay_cb, &ctx);
    if (rc != KANBUDB_OK) {
        fprintf(stderr, "  wal_replay returned %d\n", rc);
        wal_destroy(wal2); return 0;
    }
    if (ctx.count != 20) {
        fprintf(stderr, "  replayed %d entries, expected 20\n", ctx.count);
        wal_destroy(wal2); return 0;
    }

    wal_destroy(wal2);
    cleanup();
    return 1;
}

static int test_wal_large_batch(void) {
    cleanup();

    kanbudb_wal_t* wal = wal_create(TEST_WAL_PATH ".wal", KANBUDB_FSYNC_NONE);
    if (!wal) return 0;

    int n = 5000;
    for (int i = 0; i < n; i++) {
        char key[64], val[128];
        snprintf(key, sizeof(key), "large_batch_key_%d_with_padding_", i);
        snprintf(val, sizeof(val), "large_batch_val_%d_", i);
        int klen = (int)strlen(key);
        int vlen = (int)strlen(val);
        int rc = wal_append(wal, KANBUDB_WAL_PUT, 1,
                            key, (size_t)klen + 1,
                            val, (size_t)vlen + 1);
        if (rc != KANBUDB_OK) { wal_destroy(wal); return 0; }
    }

    if (wal_last_seq(wal) != (uint64_t)n) {
        wal_destroy(wal); return 0;
    }

    if (wal_sync(wal) != KANBUDB_OK) { wal_destroy(wal); return 0; }

    wal_destroy(wal);

    kanbudb_wal_t* wal2 = wal_create(TEST_WAL_PATH ".wal", KANBUDB_FSYNC_NONE);
    if (!wal2) return 0;

    replay_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    int rc = wal_replay(wal2, replay_cb, &ctx);
    if (rc != KANBUDB_OK) { wal_destroy(wal2); return 0; }
    if (ctx.count != n) {
        fprintf(stderr, "  replayed %d entries, expected %d\n", ctx.count, n);
        wal_destroy(wal2); return 0;
    }

    if (wal_last_seq(wal2) != (uint64_t)n) {
        fprintf(stderr, "  after replay seq=%llu, expected %d\n",
                (unsigned long long)wal_last_seq(wal2), n);
        wal_destroy(wal2); return 0;
    }

    wal_destroy(wal2);

    cleanup();
    return 1;
}

static int test_wal_stress(void) {
    cleanup();

    kanbudb_wal_t* wal = wal_create(TEST_WAL_PATH ".wal", KANBUDB_FSYNC_NONE);
    if (!wal) return 0;

    int n = 2000;
    for (int i = 0; i < n; i++) {
        char key[32];
        snprintf(key, sizeof(key), "sk_%d", i);

        int op = (i % 3 == 2) ? KANBUDB_WAL_DELETE : KANBUDB_WAL_PUT;
        char val[32];
        snprintf(val, sizeof(val), "sv_%d", i);

        int rc;
        if (op == KANBUDB_WAL_PUT) {
            rc = wal_append(wal, op, 1, key, strlen(key) + 1,
                           val, strlen(val) + 1);
        } else {
            rc = wal_append(wal, op, 1, key, strlen(key) + 1, NULL, 0);
        }
        if (rc != KANBUDB_OK) { wal_destroy(wal); return 0; }
    }

    int rc = wal_sync(wal);
    if (rc != KANBUDB_OK) { wal_destroy(wal); return 0; }

    wal_destroy(wal);

    kanbudb_wal_t* wal2 = wal_create(TEST_WAL_PATH ".wal", KANBUDB_FSYNC_NONE);
    if (!wal2) return 0;

    int put_count = 0, del_count = 0;


    replay_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    rc = wal_replay(wal2, replay_cb, &ctx);
    if (rc != KANBUDB_OK) { wal_destroy(wal2); return 0; }

    if (ctx.count != n) {
        fprintf(stderr, "  replayed %d, expected %d\n", ctx.count, n);
        wal_destroy(wal2); return 0;
    }

    wal_destroy(wal2);
    cleanup();
    return 1;
}

int main(void) {
    printf("wal io_uring rigorous tests:\n");
    TEST(wal_append_sync);
    TEST(wal_replay_full);
    TEST(wal_truncate_reuse);
    TEST(wal_delete_entries);
    TEST(wal_large_batch);
    TEST(wal_stress);
    printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
