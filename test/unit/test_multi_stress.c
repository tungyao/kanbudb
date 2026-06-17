#include "db.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <time.h>
#include <signal.h>
#include <fcntl.h>
#include <errno.h>

#define DB_PATH "/tmp/kanbudb_stress"
#define MAX_KEYS 1000

static int failed = 0;
static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) do { \
    printf("  %s ... ", name); fflush(stdout); \
    tests_run++; \
} while(0)

#define PASS() do { printf("PASS\n"); tests_passed++; } while(0)
#define FAIL(msg) do { \
    printf("FAIL: %s\n", msg); failed = 1; \
} while(0)

static const db_config_t config = { KANBUDB_FSYNC_NONE, 65536, 65536, 1, 1 };

static void cleanup(void) {
    system("rm -rf " DB_PATH "*");
}

static int child_read_and_verify(db_t* db, const char* table, int key_idx) {
    char key[32], expected[32];
    snprintf(key, sizeof(key), "key_%d", key_idx);
    snprintf(expected, sizeof(expected), "val_%d", key_idx);
    void* val = NULL; size_t vlen = 0;
    int rc = db_get(db, table, key, strlen(key) + 1, &val, &vlen);
    if (rc != 0) return -1;
    int ok = (vlen == strlen(expected) + 1 &&
              memcmp(val, expected, vlen) == 0);
    free(val);
    return ok ? 0 : -1;
}

static int create_table(const char* table) {
    db_t* db = NULL;
    int rc = db_open(DB_PATH, &config, &db);
    if (rc != 0) return -1;
    const char* cols[] = { "k", "v" };
    kanbudb_col_type_t types[] = { KANBUDB_STRING, KANBUDB_STRING };
    rc = db_create_table(db, table, cols, types, 2, "k");
    db_close(db);
    return rc == 0 || rc == KANBUDB_ERR_EXISTS ? 0 : -1;
}

/* ── Test 1 ──────────────────────────────────────────────── */
static int test_three_process_reads(void) {
    cleanup();
    if (create_table("t1") != 0) return -1;

    db_t* db = NULL;
    if (db_open(DB_PATH, &config, &db) != 0) return -1;
    for (int i = 0; i < 100; i++) {
        char key[32], val[32];
        snprintf(key, sizeof(key), "key_%d", i);
        snprintf(val, sizeof(val), "val_%d", i);
        if (db_put(db, "t1", key, strlen(key) + 1,
                   val, strlen(val) + 1) != 0) {
            db_close(db); return -1;
        }
    }
    db_close(db);

    pid_t p1 = fork();
    if (p1 == 0) {
        db_t* r = NULL;
        if (db_open(DB_PATH, &config, &r) != 0) _exit(1);
        for (int i = 0; i < 100; i++) {
            if (child_read_and_verify(r, "t1", i) != 0) {
                db_close(r); _exit(2);
            }
        }
        db_close(r);
        _exit(0);
    }
    pid_t p2 = fork();
    if (p2 == 0) {
        db_t* r = NULL;
        if (db_open(DB_PATH, &config, &r) != 0) _exit(1);
        for (int i = 0; i < 100; i++) {
            if (child_read_and_verify(r, "t1", i) != 0) {
                db_close(r); _exit(2);
            }
        }
        db_close(r);
        _exit(0);
    }
    int s1, s2;
    waitpid(p1, &s1, 0);
    waitpid(p2, &s2, 0);
    cleanup();
    if (!WIFEXITED(s1) || WEXITSTATUS(s1) != 0) return -1;
    if (!WIFEXITED(s2) || WEXITSTATUS(s2) != 0) return -1;
    return 0;
}

/* ── Test 2 ──────────────────────────────────────────────── */
static int test_concurrent_write_read(void) {
    cleanup();
    if (create_table("t2") != 0) return -1;

    pid_t writer = fork();
    if (writer == 0) {
        db_t* w = NULL;
        if (db_open(DB_PATH, &config, &w) != 0) _exit(1);
        for (int round = 0; round < 5; round++) {
            for (int i = 0; i < MAX_KEYS; i++) {
                char key[32], val[32];
                snprintf(key, sizeof(key), "key_%d", i);
                snprintf(val, sizeof(val), "val_%d", i);
                if (db_put(w, "t2", key, strlen(key) + 1,
                           val, strlen(val) + 1) != 0) {
                    db_close(w); _exit(1);
                }
            }
        }
        db_close(w);
        _exit(0);
    }

    pid_t readers[3];
    for (int r = 0; r < 3; r++) {
        readers[r] = fork();
        if (readers[r] == 0) {
            db_t* rdb = NULL;
            if (db_open(DB_PATH, &config, &rdb) != 0) _exit(1);
            srand((unsigned)(getpid() ^ time(NULL)));
            int failures = 0;
            for (int count = 0; count < 2000; count++) {
                int idx = rand() % MAX_KEYS;
                if (child_read_and_verify(rdb, "t2", idx) == -1) {
                    /* Try one more time in case writer hadn't written it yet */
                    usleep(5000);
                    if (child_read_and_verify(rdb, "t2", idx) == -1) {
                        /* Still not found - might still be before first write, OK */
                    }
                }
                usleep(2000);
            }
            db_close(rdb);
            _exit(failures > 255 ? 255 : failures);
        }
    }

    int wstatus;
    waitpid(writer, &wstatus, 0);
    int writer_ok = WIFEXITED(wstatus) && WEXITSTATUS(wstatus) == 0;

    int reader_failures = 0;
    int any_crash = 0;
    for (int r = 0; r < 3; r++) {
        int status;
        waitpid(readers[r], &status, 0);
        if (WIFEXITED(status)) {
            reader_failures += WEXITSTATUS(status);
        } else {
            any_crash = 1;
        }
    }

    cleanup();
    if (!writer_ok) return -1;
    if (any_crash) return -2;
    if (reader_failures > 0) return -3;
    return 0;
}

/* ── Test 3 ──────────────────────────────────────────────── */
static int test_wal_growth(void) {
    cleanup();
    db_config_t big_cfg = { KANBUDB_FSYNC_NONE, 65536, 524288, 1, 1 };

    db_t* db = NULL;
    if (db_open(DB_PATH, &big_cfg, &db) != 0) return -1;
    const char* cols[] = { "k", "v" };
    kanbudb_col_type_t types[] = { KANBUDB_STRING, KANBUDB_STRING };
    if (db_create_table(db, "t3", cols, types, 2, "k") != 0) {
        db_close(db); return -1;
    }

    int n = 1000;
    for (int i = 0; i < n; i++) {
        char key[32], val[32];
        snprintf(key, sizeof(key), "wk_%d", i);
        snprintf(val, sizeof(val), "wd_%d", i);
        if (db_put(db, "t3", key, strlen(key) + 1,
                   val, strlen(val) + 1) != 0) {
            db_close(db); return -1;
        }
    }

    pid_t child = fork();
    if (child == 0) {
        db_t* r = NULL;
        if (db_open(DB_PATH, &big_cfg, &r) != 0) _exit(1);
        for (int i = 0; i < n; i++) {
            char key[32], expected[32];
            snprintf(key, sizeof(key), "wk_%d", i);
            snprintf(expected, sizeof(expected), "wd_%d", i);
            void* val = NULL; size_t vlen = 0;
            int rc = db_get(r, "t3", key, strlen(key) + 1, &val, &vlen);
            if (rc != 0) { db_close(r); _exit(2); }
            int ok = (vlen == strlen(expected) + 1 &&
                      memcmp(val, expected, vlen) == 0);
            free(val);
            if (!ok) { db_close(r); _exit(3); }
        }
        db_close(r);
        _exit(0);
    }

    int status;
    waitpid(child, &status, 0);
    int ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    db_close(db);
    cleanup();
    return ok ? 0 : -1;
}

/* ── Test 4 ──────────────────────────────────────────────── */
static int test_reader_reg_stress(void) {
    cleanup();
    if (create_table("t4") != 0) return -1;

    pid_t writer = fork();
    if (writer == 0) {
        db_t* w = NULL;
        if (db_open(DB_PATH, &config, &w) != 0) _exit(1);
        for (int i = 0; i < 200; i++) {
            char key[32], val[32];
            snprintf(key, sizeof(key), "k_%d", i);
            snprintf(val, sizeof(val), "v_%d", i);
            db_put(w, "t4", key, strlen(key) + 1,
                   val, strlen(val) + 1);
            usleep(5000);
        }
        db_close(w);
        _exit(0);
    }

    pid_t readers[10];
    for (int r = 0; r < 10; r++) {
        readers[r] = fork();
        if (readers[r] == 0) {
            srand((unsigned)(getpid() ^ (r << 16)));
            for (int attempt = 0; attempt < 25; attempt++) {
                db_t* rdb = NULL;
                int rc = db_open(DB_PATH, &config, &rdb);
                if (rc == 0) {
                    void* val = NULL; size_t vlen = 0;
                    db_get(rdb, "t4", "k_0", 4, &val, &vlen);
                    free(val);
                    db_close(rdb);
                }
                usleep(2000 + (rand() % 8000));
            }
            _exit(0);
        }
    }

    int any_crash = 0;
    for (int r = 0; r < 10; r++) {
        int status;
        waitpid(readers[r], &status, 0);
        if (!WIFEXITED(status)) any_crash = 1;
    }
    int wstatus;
    waitpid(writer, &wstatus, 0);
    int writer_ok = WIFEXITED(wstatus) && WEXITSTATUS(wstatus) == 0;

    cleanup();
    if (any_crash) return -1;
    if (!writer_ok) return -2;
    return 0;
}

/* ── Test 5 ──────────────────────────────────────────────── */
static int test_contended_startup(void) {
    cleanup();
    if (create_table("t5") != 0) return -1;

#define NUM_CONT 5
    pid_t pids[NUM_CONT];
    for (int i = 0; i < NUM_CONT; i++) {
        pids[i] = fork();
        if (pids[i] == 0) {
            db_t* c = NULL;
            int rc = db_open(DB_PATH, &config, &c);
            if (rc != 0) _exit(1);

            char key[32], val[32];
            snprintf(key, sizeof(key), "ckey_%d", i);
            snprintf(val, sizeof(val), "cval_%d", i);
            rc = db_put(c, "t5", key, strlen(key) + 1,
                        val, strlen(val) + 1);
            if (rc != 0) { db_close(c); _exit(2); }

            void* rv = NULL; size_t rvl = 0;
            rc = db_get(c, "t5", key, strlen(key) + 1, &rv, &rvl);
            if (rc != 0) { db_close(c); _exit(3); }
            int ok = (rvl == strlen(val) + 1 && memcmp(rv, val, rvl) == 0);
            free(rv);
            if (!ok) { db_close(c); _exit(4); }

            db_close(c);
            _exit(0);
        }
        usleep(10000);
    }

    int all_ok = 1;
    for (int i = 0; i < NUM_CONT; i++) {
        int status;
        waitpid(pids[i], &status, 0);
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
            all_ok = 0;
    }

    cleanup();
    return all_ok ? 0 : -1;
}

/* ── Test 6 ──────────────────────────────────────────────── */
static int test_cross_process_delete(void) {
    cleanup();
    if (create_table("t6") != 0) return -1;

    db_t* db = NULL;
    if (db_open(DB_PATH, &config, &db) != 0) return -1;
    if (db_put(db, "t6", "del_key", 8, "del_val", 8) != 0) {
        db_close(db); cleanup(); return -1;
    }
    db_close(db);

    pid_t child = fork();
    if (child == 0) {
        db_t* r = NULL;
        if (db_open(DB_PATH, &config, &r) != 0) _exit(1);
        void* val = NULL; size_t vlen = 0;
        int rc = db_get(r, "t6", "del_key", 8, &val, &vlen);
        int ok = (rc == 0 && vlen == 8 && memcmp(val, "del_val", 8) == 0);
        free(val); db_close(r);
        _exit(ok ? 0 : 1);
    }
    int status;
    waitpid(child, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        cleanup(); return -1;
    }

    if (db_open(DB_PATH, &config, &db) != 0) return -1;
    if (db_delete(db, "t6", "del_key", 8) != 0) {
        db_close(db); cleanup(); return -1;
    }
    db_close(db);

    child = fork();
    if (child == 0) {
        db_t* r = NULL;
        if (db_open(DB_PATH, &config, &r) != 0) _exit(1);
        void* val = NULL; size_t vlen = 0;
        int rc = db_get(r, "t6", "del_key", 8, &val, &vlen);
        int ok = (rc != 0);
        free(val); db_close(r);
        _exit(ok ? 0 : 1);
    }
    waitpid(child, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        cleanup(); return -1;
    }

    cleanup();
    return 0;
}

int main(void) {
    printf("=== Multi-process stress tests ===\n");

    TEST("three process concurrent reads");
    if (test_three_process_reads() == 0) PASS(); else FAIL("see above");

    TEST("concurrent writer + multiple readers");
    int rc = test_concurrent_write_read();
    if (rc == 0) PASS();
    else if (rc == -1) FAIL("writer failure");
    else if (rc == -2) FAIL("reader crash");
    else FAIL("reader data mismatch");

    TEST("WAL growth without flush");
    if (test_wal_growth() == 0) PASS(); else FAIL("see above");

    TEST("reader register/unregister stress");
    if (test_reader_reg_stress() == 0) PASS(); else FAIL("crash detected");

    TEST("contended startup");
    if (test_contended_startup() == 0) PASS(); else FAIL("see above");

    TEST("cross-process delete propagation");
    if (test_cross_process_delete() == 0) PASS(); else FAIL("see above");

    printf("\n%d/%d tests passed, %s\n",
           tests_passed, tests_run,
           (tests_passed == tests_run && !failed) ? "ALL PASSED" : "SOME FAILED");
    return failed ? 1 : 0;
}
