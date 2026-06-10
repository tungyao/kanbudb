#include "sstable.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define SST_PATH "/tmp/kanbudb_test_sst"
#define FINAL_PATH SST_PATH ".sst.0.1"

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) do { \
  printf("  TEST: %s ... ", #name); \
  if (test_##name()) { printf("PASS\n"); tests_passed++; } \
  else { printf("FAIL\n"); tests_failed++; } \
} while(0)

static void cleanup(void) {
    unlink(SST_PATH ".sst.0.1.tmp");
    unlink(FINAL_PATH);
}

/* ── Scan callback context ────────────────────────────────── */

typedef struct {
    char values[8][16];
    int  count;
} scan_ctx_t;

static int scan_cb(const void* key, size_t key_len,
                    const void* value, size_t val_len,
                    uint8_t flag, void* ctx) {
    (void)key; (void)key_len; (void)flag;
    scan_ctx_t* sc = (scan_ctx_t*)ctx;
    if (sc->count < 8 && value && val_len > 0) {
        size_t cpy = val_len < 16 ? val_len : 15;
        memcpy(sc->values[sc->count], value, cpy);
        sc->values[sc->count][cpy] = '\0';
    }
    sc->count++;
    return KANBUDB_OK;
}

static int test_write_read_basic(void) {
    cleanup();

    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s.tmp", FINAL_PATH);
    sstable_writer_t* w = sstable_writer_create(tmp, FINAL_PATH, 42);
    if (!w) return 0;

    const char* k1 = "hello";
    const char* v1 = "world";
    if (sstable_writer_add(w, k1, strlen(k1) + 1, v1, strlen(v1) + 1,
                            KANBUDB_SSTABLE_FLAG_ALIVE) != 0) {
        sstable_writer_destroy(w); return 0;
    }

    const char* k2 = "key2";
    const char* v2 = "value2";
    if (sstable_writer_add(w, k2, strlen(k2) + 1, v2, strlen(v2) + 1,
                            KANBUDB_SSTABLE_FLAG_ALIVE) != 0) {
        sstable_writer_destroy(w); return 0;
    }

    if (sstable_writer_finish(w) != 0) { sstable_writer_destroy(w); return 0; }
    sstable_writer_destroy(w);

    sstable_reader_t* r = sstable_reader_open(FINAL_PATH);
    if (!r) return 0;

    /* Check metadata */
    if (sstable_reader_num_entries(r) != 2) { sstable_reader_close(r); return 0; }
    if (sstable_reader_sequence(r) != 42) { sstable_reader_close(r); return 0; }
    if (!sstable_reader_crc_ok(r)) { sstable_reader_close(r); return 0; }

    /* Point get */
    void* val = NULL;
    size_t vlen = 0;
    uint8_t flag = 0;
    int rc = sstable_reader_get(r, "hello", 6, &val, &vlen, &flag);
    if (rc != KANBUDB_OK || !val || strcmp((char*)val, "world") != 0) {
        free(val); sstable_reader_close(r); return 0;
    }
    free(val); val = NULL;

    rc = sstable_reader_get(r, "key2", 5, &val, &vlen, &flag);
    if (rc != KANBUDB_OK || !val || strcmp((char*)val, "value2") != 0) {
        free(val); sstable_reader_close(r); return 0;
    }
    free(val); val = NULL;

    /* Not found */
    rc = sstable_reader_get(r, "nope", 5, &val, &vlen, &flag);
    if (rc != KANBUDB_ERR_NOTFOUND) { sstable_reader_close(r); return 0; }

    sstable_reader_close(r);
    cleanup();
    return 1;
}

static int test_tombstone(void) {
    cleanup();

    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s.tmp", FINAL_PATH);
    sstable_writer_t* w = sstable_writer_create(tmp, FINAL_PATH, 1);
    if (!w) return 0;

    const char* k1 = "alive";
    const char* v1 = "data";
    sstable_writer_add(w, k1, strlen(k1) + 1, v1, strlen(v1) + 1,
                        KANBUDB_SSTABLE_FLAG_ALIVE);

    const char* k2 = "gone";
    sstable_writer_add(w, k2, strlen(k2) + 1, NULL, 0,
                        KANBUDB_SSTABLE_FLAG_TOMBSTONE);

    sstable_writer_finish(w);
    sstable_writer_destroy(w);

    sstable_reader_t* r = sstable_reader_open(FINAL_PATH);
    if (!r) return 0;
    if (sstable_reader_num_entries(r) != 2) { sstable_reader_close(r); return 0; }

    void* val = NULL;
    size_t vlen = 0;
    uint8_t flag = 0;

    /* Alive key returns OK */
    int rc = sstable_reader_get(r, "alive", 6, &val, &vlen, &flag);
    if (rc != KANBUDB_OK) { sstable_reader_close(r); return 0; }
    free(val); val = NULL;

    /* Tombstone returns NOTFOUND */
    rc = sstable_reader_get(r, "gone", 5, &val, &vlen, &flag);
    if (rc != KANBUDB_ERR_NOTFOUND) { sstable_reader_close(r); return 0; }

    sstable_reader_close(r);
    cleanup();
    return 1;
}

static int test_many_entries(void) {
    cleanup();

    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s.tmp", FINAL_PATH);
    sstable_writer_t* w = sstable_writer_create(tmp, FINAL_PATH, 100);
    if (!w) return 0;

    int n = 200;
    char keys[200][16];
    char vals[200][16];
    for (int i = 0; i < n; i++) {
        snprintf(keys[i], 16, "k%04d", i);
        snprintf(vals[i], 16, "v%04d", i);
        if (sstable_writer_add(w, keys[i], strlen(keys[i]) + 1,
                                vals[i], strlen(vals[i]) + 1,
                                KANBUDB_SSTABLE_FLAG_ALIVE) != 0) {
            sstable_writer_destroy(w); return 0;
        }
    }

    if (sstable_writer_finish(w) != 0) { sstable_writer_destroy(w); return 0; }
    sstable_writer_destroy(w);

    sstable_reader_t* r = sstable_reader_open(FINAL_PATH);
    if (!r) return 0;
    if (sstable_reader_num_entries(r) != (uint64_t)n) {
        sstable_reader_close(r); return 0;
    }

    for (int i = 0; i < n; i++) {
        void* val = NULL;
        size_t vlen = 0;
        uint8_t flag = 0;
        int rc = sstable_reader_get(r, keys[i], strlen(keys[i]) + 1,
                                     &val, &vlen, &flag);
        if (rc != KANBUDB_OK) {
            fprintf(stderr, "  DEBUG: key '%s' rc=%d\n", keys[i], rc);
            free(val); sstable_reader_close(r); return 0;
        }
        if (!val || vlen != strlen(vals[i]) + 1 ||
            memcmp(val, vals[i], vlen) != 0) {
            fprintf(stderr, "  DEBUG: key '%s' val mismatch (vlen=%zu, expected=%zu)\n",
                    keys[i], vlen, strlen(vals[i]) + 1);
            free(val); sstable_reader_close(r); return 0;
        }
        free(val);
    }

    sstable_reader_close(r);
    cleanup();
    return 1;
}

static int test_scan(void) {
    cleanup();

    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s.tmp", FINAL_PATH);
    sstable_writer_t* w = sstable_writer_create(tmp, FINAL_PATH, 1);
    if (!w) return 0;

    const char* keys[] = {"a", "b", "c"};
    const char* vals[] = {"1", "2", "3"};
    for (int i = 0; i < 3; i++) {
        sstable_writer_add(w, keys[i], strlen(keys[i]) + 1,
                            vals[i], strlen(vals[i]) + 1,
                            KANBUDB_SSTABLE_FLAG_ALIVE);
    }
    sstable_writer_finish(w);
    sstable_writer_destroy(w);

    sstable_reader_t* r = sstable_reader_open(FINAL_PATH);
    if (!r) return 0;

    scan_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));

    int scan_rc = sstable_reader_scan(r, scan_cb, &ctx);
    if (scan_rc != KANBUDB_OK) { sstable_reader_close(r); return 0; }
    if (ctx.count != 3) { sstable_reader_close(r); return 0; }
    if (strcmp(ctx.values[0], "1") != 0 ||
        strcmp(ctx.values[1], "2") != 0 ||
        strcmp(ctx.values[2], "3") != 0) {
        sstable_reader_close(r); return 0;
    }

    sstable_reader_close(r);
    cleanup();
    return 1;
}

int main(void) {
    printf("sstable tests:\n");
    TEST(write_read_basic);
    TEST(tombstone);
    TEST(many_entries);
    TEST(scan);
    printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
