#include "db.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

static int failed = 0;

#define TEST(name) do { \
    printf("  %s ... ", name); fflush(stdout); \
} while(0)

#define PASS() do { printf("PASS\n"); } while(0)
#define FAIL(msg) do { \
    printf("FAIL: %s\n", msg); failed = 1; \
} while(0)

int main(void) {
    printf("=== Multi-process tests ===\n");

    const char* db_path = "/tmp/kanbudb_mp_test";
    system("rm -rf /tmp/kanbudb_mp_test*");

    db_config_t config = { KANBUDB_FSYNC_NONE, 65536, 65536, 1, 1 };

    /* ── Test 1: basic multi-process open/write/read (no fork) ─ */
    TEST("basic multi-process open/write/read");
    {
        db_t* db = NULL;
        int rc = db_open(db_path, &config, &db);
        if (rc != 0) { FAIL("db_open"); return 1; }

        const char* cols[] = { "k", "v" };
        kanbudb_col_type_t types[] = { KANBUDB_STRING, KANBUDB_STRING };
        rc = db_create_table(db, "t1", cols, types, 2, "k");
        if (rc != 0) { FAIL("create_table"); db_close(db); return 1; }

        rc = db_put(db, "t1", "test_key", 8, "test_val", 8);
        if (rc != 0) { FAIL("put"); db_close(db); return 1; }

        void* val = NULL; size_t vlen = 0;
        rc = db_get(db, "t1", "test_key", 8, &val, &vlen);
        if (rc != 0 || vlen != 8 || memcmp(val, "test_val", 8) != 0) {
            FAIL("get"); free(val); db_close(db); return 1;
        }
        free(val);
        db_close(db);
        PASS();
    }

    /* ── Test 2: write then fork + read ─────────────────────── */
    TEST("write(parent) -> close -> fork -> read(child)");
    {
        db_t* w = NULL;
        db_open(db_path, &config, &w);
        const char* cols[] = { "k", "v" };
        kanbudb_col_type_t types[] = { KANBUDB_STRING, KANBUDB_STRING };
        db_create_table(w, "t2", cols, types, 2, "k");
        db_put(w, "t2", "hello", 5, "world", 5);

        void* val = NULL; size_t vlen = 0;
        db_get(w, "t2", "hello", 5, &val, &vlen);
        free(val);
        db_close(w);

        pid_t pid = fork();
        if (pid == 0) {
            db_t* r = NULL;
            int rc2 = db_open(db_path, &config, &r);
            if (rc2 != 0) _exit(1);
            void* v = NULL; size_t vl = 0;
            rc2 = db_get(r, "t2", "hello", 5, &v, &vl);
            int ok = (rc2 == 0 && vl == 5 && memcmp(v, "world", 5) == 0);
            free(v); db_close(r);
            _exit(ok ? 0 : 1);
        }
        int status;
        waitpid(pid, &status, 0);
        if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
            PASS();
        } else {
            FAIL("child could not read");
        }
    }

    /* ── Test 3: write (no close) then fork + read via WAL ─── */
    TEST("write(parent) -> fork -> read(child) via WAL (no flush)");
    {
        db_t* w = NULL;
        db_open(db_path, &config, &w);
        const char* cols[] = { "k", "v" };
        kanbudb_col_type_t types[] = { KANBUDB_STRING, KANBUDB_STRING };
        db_create_table(w, "t3", cols, types, 2, "k");
        db_put(w, "t3", "foo", 3, "bar", 3);

        pid_t pid = fork();
        if (pid == 0) {
            db_t* r = NULL;
            int rc2 = db_open(db_path, &config, &r);
            if (rc2 != 0) _exit(1);
            void* v = NULL; size_t vl = 0;
            rc2 = db_get(r, "t3", "foo", 3, &v, &vl);
            int ok = (rc2 == 0 && vl == 3 && memcmp(v, "bar", 3) == 0);
            free(v); db_close(r);
            _exit(ok ? 0 : 1);
        }
        int status;
        waitpid(pid, &status, 0);
        if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
            PASS();
        } else {
            FAIL("child could not read from WAL");
        }
        db_close(w);
    }

    /* ── Test 4: concurrent write + read (parent writes, child polls) ─ */
    TEST("concurrent write and read across processes");
    {
        db_t* w = NULL;
        db_open(db_path, &config, &w);
        const char* cols[] = { "k", "v" };
        kanbudb_col_type_t types[] = { KANBUDB_STRING, KANBUDB_STRING };
        db_create_table(w, "t4", cols, types, 2, "k");

        pid_t pid = fork();
        if (pid == 0) {
            db_t* r = NULL;
            db_open(db_path, &config, &r);
            for (int i = 0; i < 50; i++) {
                void* v = NULL; size_t vl = 0;
                int rc2 = db_get(r, "t4", "concurrent", 10, &v, &vl);
                if (rc2 == 0 && vl == 5 && memcmp(v, "data!", 5) == 0) {
                    free(v); db_close(r); _exit(0);
                }
                free(v);
                usleep(100000);
            }
            db_close(r);
            _exit(1);
        }

        usleep(200000);
        db_put(w, "t4", "concurrent", 10, "data!", 5);

        int status;
        waitpid(pid, &status, 0);
        if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
            PASS();
        } else {
            FAIL("concurrent read timed out");
        }
        db_close(w);
    }

    /* ── Test 5: two concurrent readers ────────────────────── */
    TEST("two concurrent readers");
    {
        db_t* w = NULL;
        db_open(db_path, &config, &w);
        const char* cols[] = { "k", "v" };
        kanbudb_col_type_t types[] = { KANBUDB_STRING, KANBUDB_STRING };
        db_create_table(w, "t5", cols, types, 2, "k");
        db_put(w, "t5", "shared", 6, "data", 4);
        db_close(w);

        pid_t p1 = fork();
        if (p1 == 0) {
            db_t* r = NULL; db_open(db_path, &config, &r);
            void* v = NULL; size_t vl = 0;
            int rc2 = db_get(r, "t5", "shared", 6, &v, &vl);
            int ok = (rc2 == 0 && vl == 4 && memcmp(v, "data", 4) == 0);
            free(v); db_close(r); _exit(ok ? 0 : 1);
        }
        pid_t p2 = fork();
        if (p2 == 0) {
            db_t* r = NULL; db_open(db_path, &config, &r);
            void* v = NULL; size_t vl = 0;
            int rc2 = db_get(r, "t5", "shared", 6, &v, &vl);
            int ok = (rc2 == 0 && vl == 4 && memcmp(v, "data", 4) == 0);
            free(v); db_close(r); _exit(ok ? 0 : 1);
        }
        int s1, s2;
        waitpid(p1, &s1, 0); waitpid(p2, &s2, 0);
        if (WIFEXITED(s1) && WEXITSTATUS(s1) == 0 &&
            WIFEXITED(s2) && WEXITSTATUS(s2) == 0) {
            PASS();
        } else {
            FAIL("readers disagree");
        }
    }

    system("rm -rf /tmp/kanbudb_mp_test*");

    printf("\n%s\n", failed ? "SOME TESTS FAILED" : "ALL TESTS PASSED");
    return failed ? 1 : 0;
}
