#include "db.h"
#include "macros.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define TEST_DB "/tmp/kanbudb_persist_test"

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) do { \
  printf("  TEST: %s ... ", #name); \
  if (test_##name()) { printf("PASS\n"); tests_passed++; } \
  else { printf("FAIL\n"); tests_failed++; } \
} while(0)

/* Note: db_get() may return a pointer to internal LSM/B-tree memory.
 * The caller should NOT free it — it's valid until the next write/flush. */

static void cleanup_files(void) {
    char p[512];
    snprintf(p, sizeof(p), "%s.wal", TEST_DB);
    unlink(p);
    snprintf(p, sizeof(p), "%s.system", TEST_DB);
    unlink(p);
    snprintf(p, sizeof(p), "%s.seq", TEST_DB);
    unlink(p);
    for (int i = 0; i < 100; i++) {
        snprintf(p, sizeof(p), "%s.sst.0.%d", TEST_DB, i);
        unlink(p);
        snprintf(p, sizeof(p), "%s.sst.1.%d", TEST_DB, i);
        unlink(p);
        snprintf(p, sizeof(p), "%s.ckpt.%d", TEST_DB, i);
        unlink(p);
        snprintf(p, sizeof(p), "%s.ckpt.%d.tmp", TEST_DB, i);
        unlink(p);
    }
}

/* ── Test 1: Basic WAL recovery ────────────────────────────
 *
 * Schema is now persisted via .system SSTable.
 * On reopen, the table is automatically restored —
 * no need to call db_create_table again. */

static int test_wal_recovery(void) {
    cleanup_files();

    db_config_t config;
    config.fsync_mode = KANBUDB_FSYNC_NONE;
    config.cache_size = 65536;
    config.memtable_size = 1048576;
    config.compaction_threads = 1;

    db_t* db = NULL;
    int rc = db_open(TEST_DB, &config, &db);
    if (rc != KANBUDB_OK || !db) return 0;

    const char* col_names[] = {"id", "name"};
    kanbudb_col_type_t col_types[] = {KANBUDB_INT32, KANBUDB_STRING};
    rc = db_create_table(db, "users", col_names, col_types, 2, "id");
    if (rc != KANBUDB_OK) { db_close(db); return 0; }

    rc = db_put(db, "users", "user_1", 7, "Alice", 6);
    if (rc != KANBUDB_OK) { db_close(db); return 0; }

    rc = db_put(db, "users", "user_2", 7, "Bob", 4);
    if (rc != KANBUDB_OK) { db_close(db); return 0; }

    db_close(db);

    /* Reopen — schema auto-restored from .system, data from SSTable+WAL */
    db = NULL;
    rc = db_open(TEST_DB, &config, &db);
    if (rc != KANBUDB_OK || !db) return 0;

    /* Table "users" is auto-loaded from .system — don't recreate */

    void* val = NULL;
    size_t vlen = 0;

    rc = db_get(db, "users", "user_1", 7, &val, &vlen);
    if (rc != KANBUDB_OK || !val || vlen != 6 || memcmp(val, "Alice", 6) != 0) {
        db_close(db); return 0;
    }

    vlen = 0;
    rc = db_get(db, "users", "user_2", 7, &val, &vlen);
    if (rc != KANBUDB_OK || !val || vlen != 4 || memcmp(val, "Bob", 4) != 0) {
        db_close(db); return 0;
    }

    db_close(db);
    cleanup_files();
    return 1;
}

/* ── Test 2: Flush + Restart recovery ────────────────────── */

static int test_flush_recovery(void) {
    cleanup_files();

    db_config_t config;
    config.fsync_mode = KANBUDB_FSYNC_NONE;
    config.cache_size = 65536;
    config.memtable_size = 256;
    config.compaction_threads = 1;

    db_t* db = NULL;
    int rc = db_open(TEST_DB, &config, &db);
    if (rc != KANBUDB_OK || !db) return 0;

    const char* col_names[] = {"id", "val"};
    kanbudb_col_type_t col_types[] = {KANBUDB_INT32, KANBUDB_STRING};
    rc = db_create_table(db, "items", col_names, col_types, 2, "id");
    if (rc != KANBUDB_OK) { db_close(db); return 0; }

    int n = 20;
    char keys[20][16];
    char vals[20][16];
    for (int i = 0; i < n; i++) {
        snprintf(keys[i], 16, "item_%d", i);
        snprintf(vals[i], 16, "val_%d", i);
        rc = db_put(db, "items", keys[i], strlen(keys[i]) + 1,
                     vals[i], strlen(vals[i]) + 1);
        if (rc != KANBUDB_OK) { db_close(db); return 0; }
    }

    db_close(db);

    /* Reopen — schema auto-restored, data from SSTable+WAL */
    db = NULL;
    rc = db_open(TEST_DB, &config, &db);
    if (rc != KANBUDB_OK || !db) return 0;

    for (int i = 0; i < n; i++) {
        void* val = NULL;
        size_t vlen = 0;
        rc = db_get(db, "items", keys[i], strlen(keys[i]) + 1, &val, &vlen);
        if (rc != KANBUDB_OK) { db_close(db); return 0; }
        if (vlen != strlen(vals[i]) + 1 || memcmp(val, vals[i], vlen) != 0) {
            db_close(db); return 0;
        }
    }

    db_close(db);
    cleanup_files();
    return 1;
}

/* ── Test 3: Delete + recovery ───────────────────────────── */

static int test_delete_recovery(void) {
    cleanup_files();

    db_config_t config;
    config.fsync_mode = KANBUDB_FSYNC_NONE;
    config.cache_size = 65536;
    config.memtable_size = 1048576;
    config.compaction_threads = 1;

    db_t* db = NULL;
    db_open(TEST_DB, &config, &db);

    const char* col_names[] = {"id"};
    kanbudb_col_type_t col_types[] = {KANBUDB_INT32};
    db_create_table(db, "t", col_names, col_types, 1, "id");

    db_put(db, "t", "x", 2, "keep", 5);
    db_put(db, "t", "y", 2, "delete_me", 10);
    db_delete(db, "t", "y", 2);
    db_close(db);

    /* Reopen */
    db_open(TEST_DB, &config, &db);

    void* val = NULL;
    size_t vlen = 0;

    int rc = db_get(db, "t", "x", 2, &val, &vlen);
    if (rc != KANBUDB_OK) { db_close(db); return 0; }

    rc = db_get(db, "t", "y", 2, &val, &vlen);
    if (rc != KANBUDB_ERR_NOTFOUND) { db_close(db); return 0; }

    db_close(db);
    cleanup_files();
    return 1;
}

/* ── Test 4: Multiple open/close cycles ──────────────────── */

static int test_multi_cycle(void) {
    cleanup_files();

    db_config_t config;
    config.fsync_mode = KANBUDB_FSYNC_NONE;
    config.cache_size = 65536;
    config.memtable_size = 256;
    config.compaction_threads = 1;

    /* Cycle 1: insert some data */
    db_t* db = NULL;
    db_open(TEST_DB, &config, &db);
    const char* col_names[] = {"id"};
    kanbudb_col_type_t col_types[] = {KANBUDB_INT32};
    db_create_table(db, "t", col_names, col_types, 1, "id");
    db_put(db, "t", "k1", 3, "v1", 3);
    db_put(db, "t", "k2", 3, "v2", 3);
    db_put(db, "t", "k3", 3, "v3", 3);
    db_close(db);

    /* Cycle 2: verify, add more (schema auto-restored, don't recreate) */
    db_open(TEST_DB, &config, &db);

    void* val = NULL; size_t vlen = 0;
    int rc2 = db_get(db, "t", "k1", 3, &val, &vlen);
    if (rc2 != KANBUDB_OK) { fprintf(stderr,"  C2:k1 rc=%d\n",rc2); db_close(db); return 0; }
    db_put(db, "t", "k4", 3, "v4", 3);
    db_delete(db, "t", "k2", 3);
    db_close(db);

    /* Cycle 3: final verify */
    db_open(TEST_DB, &config, &db);

    int rc;
    rc = db_get(db, "t", "k1", 3, &val, &vlen);
    if (rc != KANBUDB_OK) { fprintf(stderr,"  C3:k1 rc=%d\n",rc); db_close(db); return 0; }
    rc = db_get(db, "t", "k2", 3, &val, &vlen);
    if (rc != KANBUDB_ERR_NOTFOUND) { fprintf(stderr,"  C3:k2 rc=%d (want NOTFOUND)\n",rc); db_close(db); return 0; }
    rc = db_get(db, "t", "k3", 3, &val, &vlen);
    if (rc != KANBUDB_OK) { fprintf(stderr,"  C3:k3 rc=%d\n",rc); db_close(db); return 0; }
    rc = db_get(db, "t", "k4", 3, &val, &vlen);
    if (rc != KANBUDB_OK) { fprintf(stderr,"  C3:k4 rc=%d\n",rc); db_close(db); return 0; }

    db_close(db);
    cleanup_files();
    return 1;
}

int main(void) {
    printf("persistence integration tests:\n");
    TEST(wal_recovery);
    TEST(flush_recovery);
    TEST(delete_recovery);
    TEST(multi_cycle);
    printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
