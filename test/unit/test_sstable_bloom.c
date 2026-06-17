#include "sstable.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>

#define SST_PATH "/tmp/kanbudb_test_sst_bloom"
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

static int test_bloom_reduces_misses(void) {
    cleanup();

    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s.tmp", FINAL_PATH);
    sstable_writer_t* w = sstable_writer_create(tmp, FINAL_PATH, 1);
    if (!w) return 0;

    int n = 500;
    for (int i = 0; i < n; i++) {
        char key[32], val[32];
        snprintf(key, sizeof(key), "present_%04d", i);
        snprintf(val, sizeof(val), "val_%04d", i);
        if (sstable_writer_add(w, key, strlen(key) + 1,
                               val, strlen(val) + 1,
                               KANBUDB_SSTABLE_FLAG_ALIVE) != 0) {
            sstable_writer_destroy(w); return 0;
        }
    }

    if (sstable_writer_finish(w) != 0) { sstable_writer_destroy(w); return 0; }
    sstable_writer_destroy(w);

    sstable_reader_t* r = sstable_reader_open(FINAL_PATH);
    if (!r) return 0;

    if (sstable_reader_num_entries(r) != (uint64_t)n) {
        sstable_reader_close(r); cleanup(); return 0;
    }

    if (!sstable_reader_crc_ok(r)) {
        fprintf(stderr, "  CRC check failed\n");
        sstable_reader_close(r); cleanup(); return 0;
    }

    for (int i = 0; i < n; i++) {
        char key[32];
        snprintf(key, sizeof(key), "present_%04d", i);
        void* val = NULL;
        size_t vlen = 0;
        uint8_t flag = 0;
        int rc = sstable_reader_get(r, key, strlen(key) + 1, &val, &vlen, &flag);
        if (rc != KANBUDB_OK) {
            fprintf(stderr, "  missing key '%s'\n", key);
            sstable_reader_close(r); cleanup(); return 0;
        }
    }

    for (int i = 0; i < 500; i++) {
        char key[32];
        snprintf(key, sizeof(key), "absent_%04d", i + 100000);
        void* val = NULL;
        size_t vlen = 0;
        uint8_t flag = 0;
        int rc = sstable_reader_get(r, key, strlen(key) + 1, &val, &vlen, &flag);
        if (rc != KANBUDB_ERR_NOTFOUND) {
            fprintf(stderr, "  absent key '%s' found (bloom false positive)\n", key);
            sstable_reader_close(r); cleanup(); return 0;
        }
    }

    sstable_reader_close(r);
    cleanup();
    return 1;
}

static int test_mmap_zero_copy_get(void) {
    cleanup();

    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s.tmp", FINAL_PATH);
    sstable_writer_t* w = sstable_writer_create(tmp, FINAL_PATH, 42);
    if (!w) return 0;

    const char* k = "mmap_test_key";
    const char* v = "mmap_test_value_12345";
    if (sstable_writer_add(w, k, strlen(k) + 1,
                           v, strlen(v) + 1,
                           KANBUDB_SSTABLE_FLAG_ALIVE) != 0) {
        sstable_writer_destroy(w); return 0;
    }
    if (sstable_writer_finish(w) != 0) { sstable_writer_destroy(w); return 0; }
    sstable_writer_destroy(w);

    sstable_reader_t* r = sstable_reader_open(FINAL_PATH);
    if (!r) return 0;

    void* val = NULL;
    size_t vlen = 0;
    uint8_t flag = 0;
    int rc = sstable_reader_get(r, k, strlen(k) + 1, &val, &vlen, &flag);
    if (rc != KANBUDB_OK) { sstable_reader_close(r); cleanup(); return 0; }

    if (memcmp(val, v, vlen) != 0) {
        fprintf(stderr, "  mmap value mismatch\n");
        sstable_reader_close(r); cleanup(); return 0;
    }

    if (sstable_reader_sequence(r) != 42) {
        sstable_reader_close(r); cleanup(); return 0;
    }

    sstable_reader_close(r);
    cleanup();
    return 1;
}

static int test_mmap_large_sstable(void) {
    cleanup();

    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s.tmp", FINAL_PATH);
    sstable_writer_t* w = sstable_writer_create(tmp, FINAL_PATH, 100);
    if (!w) return 0;

    int n = 2000;
    for (int i = 0; i < n; i++) {
        char key[64], val[128];
        snprintf(key, sizeof(key), "big_sst_key_%04d", i);
        snprintf(val, sizeof(val), "big_sst_val_%04d_", i);
        int kl = (int)strlen(key) + 1;
        int vl = (int)strlen(val) + 1;
        if (sstable_writer_add(w, key, (size_t)kl, val, (size_t)vl,
                               KANBUDB_SSTABLE_FLAG_ALIVE) != 0) {
            sstable_writer_destroy(w); return 0;
        }
    }

    if (sstable_writer_finish(w) != 0) { sstable_writer_destroy(w); return 0; }
    sstable_writer_destroy(w);

    sstable_reader_t* r = sstable_reader_open(FINAL_PATH);
    if (!r) return 0;

    if (sstable_reader_num_entries(r) != (uint64_t)n) {
        fprintf(stderr, "  num_entries = %llu, expected %d\n",
                (unsigned long long)sstable_reader_num_entries(r), n);
        sstable_reader_close(r); cleanup(); return 0;
    }

    if (!sstable_reader_crc_ok(r)) {
        fprintf(stderr, "  CRC failed\n");
        sstable_reader_close(r); cleanup(); return 0;
    }

    for (int i = 0; i < n; i++) {
        char key[64], expected[128];
        snprintf(key, sizeof(key), "big_sst_key_%04d", i);
        snprintf(expected, sizeof(expected), "big_sst_val_%04d_", i);
        void* val = NULL;
        size_t vlen = 0;
        uint8_t flag = 0;
        int rc = sstable_reader_get(r, key, strlen(key) + 1, &val, &vlen, &flag);
        if (rc != KANBUDB_OK) {
            fprintf(stderr, "  missing key '%s' at idx %d\n", key, i);
            sstable_reader_close(r); cleanup(); return 0;
        }
        if (memcmp(val, expected, vlen) != 0) {
            fprintf(stderr, "  value mismatch for '%s'\n", key);
            sstable_reader_close(r); cleanup(); return 0;
        }
    }

    sstable_reader_close(r);
    cleanup();
    return 1;
}

static int test_empty_sstable(void) {
    cleanup();

    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s.tmp", FINAL_PATH);
    sstable_writer_t* w = sstable_writer_create(tmp, FINAL_PATH, 1);
    if (!w) return 0;

    if (sstable_writer_finish(w) != 0) { sstable_writer_destroy(w); return 0; }
    sstable_writer_destroy(w);

    sstable_reader_t* r = sstable_reader_open(FINAL_PATH);
    if (!r) return 0;

    if (sstable_reader_num_entries(r) != 0) {
        sstable_reader_close(r); cleanup(); return 0;
    }

    void* val = NULL;
    size_t vlen = 0;
    uint8_t flag = 0;
    int rc = sstable_reader_get(r, "anything", 9, &val, &vlen, &flag);
    if (rc != KANBUDB_ERR_NOTFOUND) {
        sstable_reader_close(r); cleanup(); return 0;
    }

    sstable_reader_close(r);
    cleanup();
    return 1;
}

int main(void) {
    printf("sstable + bloom/mmap rigorous tests:\n");
    TEST(bloom_reduces_misses);
    TEST(mmap_zero_copy_get);
    TEST(mmap_large_sstable);
    TEST(empty_sstable);
    printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
