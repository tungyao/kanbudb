#include "compaction.h"
#include "sstable.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#define TEST_DIR "/tmp/kanbudb_test_leveled"

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) do { \
  printf("  TEST: %s ... ", #name); \
  if (test_##name()) { printf("PASS\n"); tests_passed++; } \
  else { printf("FAIL\n"); tests_failed++; } \
} while(0)

static void cleanup(void) {
    for (int lvl = 0; lvl < 8; lvl++) {
        for (int s = 0; s < 100; s++) {
            char path[256];
            snprintf(path, sizeof(path), TEST_DIR ".sst.%d.%d", lvl, s);
            unlink(path);
        }
    }
}

static int create_sstable_at(const char* db_path, int level, uint64_t seq,
                              const char* const* keys, const char* const* vals,
                              int num, uint8_t flag) {
    char path[512], tmp[512];
    compaction_make_path(db_path, level, seq, path, sizeof(path));
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);

    sstable_writer_t* w = sstable_writer_create(tmp, path, seq);
    if (!w) return 0;
    for (int i = 0; i < num; i++) {
        if (sstable_writer_add(w, keys[i], strlen(keys[i]) + 1,
                                vals[i], strlen(vals[i]) + 1, flag) != 0) {
            sstable_writer_destroy(w); return 0;
        }
    }
    int rc = sstable_writer_finish(w);
    sstable_writer_destroy(w);
    return rc == KANBUDB_OK;
}

static int test_scan_levels_empty(void) {
    cleanup();

    kanbudb_level_t levels[KANBUDB_MAX_LEVELS];
    int rc = compaction_scan_levels(TEST_DIR, levels, KANBUDB_MAX_LEVELS);
    if (rc != KANBUDB_OK) return 0;

    for (int i = 0; i < KANBUDB_MAX_LEVELS; i++) {
        if (levels[i].num_files != 0) {
            compaction_free_levels(levels, KANBUDB_MAX_LEVELS);
            return 0;
        }
    }

    compaction_free_levels(levels, KANBUDB_MAX_LEVELS);
    return 1;
}

static int test_scan_levels_with_files(void) {
    cleanup();

    const char* k[] = {"a", "b"};
    const char* v[] = {"1", "2"};
    if (!create_sstable_at(TEST_DIR, 0, 1, k, v, 2, KANBUDB_SSTABLE_FLAG_ALIVE))
        return 0;
    if (!create_sstable_at(TEST_DIR, 0, 2, k, v, 2, KANBUDB_SSTABLE_FLAG_ALIVE))
        return 0;
    if (!create_sstable_at(TEST_DIR, 1, 3, k, v, 2, KANBUDB_SSTABLE_FLAG_ALIVE))
        return 0;
    if (!create_sstable_at(TEST_DIR, 3, 4, k, v, 2, KANBUDB_SSTABLE_FLAG_ALIVE))
        return 0;

    kanbudb_level_t levels[KANBUDB_MAX_LEVELS];
    int rc = compaction_scan_levels(TEST_DIR, levels, KANBUDB_MAX_LEVELS);
    if (rc != KANBUDB_OK) { compaction_free_levels(levels, KANBUDB_MAX_LEVELS); return 0; }

    int ok = 1;
    if (levels[0].num_files != 2) { fprintf(stderr, "  L0: %d != 2\n", levels[0].num_files); ok = 0; }
    if (levels[1].num_files != 1) { fprintf(stderr, "  L1: %d != 1\n", levels[1].num_files); ok = 0; }
    if (levels[2].num_files != 0) { fprintf(stderr, "  L2: %d != 0\n", levels[2].num_files); ok = 0; }
    if (levels[3].num_files != 1) { fprintf(stderr, "  L3: %d != 1\n", levels[3].num_files); ok = 0; }

    compaction_free_levels(levels, KANBUDB_MAX_LEVELS);
    cleanup();
    return ok;
}

static int test_pick_level_triggers(void) {
    cleanup();

    const char* k[] = {"a", "b"};
    const char* v[] = {"1", "2"};

    for (int i = 0; i < KANBUDB_L0_COMPACTION_TRIGGER; i++) {
        if (!create_sstable_at(TEST_DIR, 0, (uint64_t)(i + 1), k, v, 2,
                                KANBUDB_SSTABLE_FLAG_ALIVE))
            return 0;
    }

    kanbudb_level_t levels[KANBUDB_MAX_LEVELS];
    int rc = compaction_scan_levels(TEST_DIR, levels, KANBUDB_MAX_LEVELS);
    if (rc != KANBUDB_OK) { compaction_free_levels(levels, KANBUDB_MAX_LEVELS); return 0; }

    int picked = compaction_pick_level(levels, KANBUDB_MAX_LEVELS);
    if (picked != 0) {
        fprintf(stderr, "  picked L%d, expected L0 (0)\n", picked);
        compaction_free_levels(levels, KANBUDB_MAX_LEVELS);
        cleanup(); return 0;
    }

    compaction_free_levels(levels, KANBUDB_MAX_LEVELS);
    cleanup();
    return 1;
}

static int test_no_pick_when_below_threshold(void) {
    cleanup();

    const char* k[] = {"a", "b"};
    const char* v[] = {"1", "2"};

    for (int i = 0; i < KANBUDB_L0_COMPACTION_TRIGGER - 1; i++) {
        if (!create_sstable_at(TEST_DIR, 0, (uint64_t)(i + 1), k, v, 2,
                                KANBUDB_SSTABLE_FLAG_ALIVE))
            return 0;
    }

    kanbudb_level_t levels[KANBUDB_MAX_LEVELS];
    int rc = compaction_scan_levels(TEST_DIR, levels, KANBUDB_MAX_LEVELS);
    if (rc != KANBUDB_OK) { compaction_free_levels(levels, KANBUDB_MAX_LEVELS); return 0; }

    int picked = compaction_pick_level(levels, KANBUDB_MAX_LEVELS);
    if (picked >= 0) {
        fprintf(stderr, "  picked L%d when should be none\n", picked);
        compaction_free_levels(levels, KANBUDB_MAX_LEVELS);
        cleanup(); return 0;
    }

    compaction_free_levels(levels, KANBUDB_MAX_LEVELS);
    cleanup();
    return 1;
}

static int test_multi_level_merge(void) {
    cleanup();

    const char* k1[] = {"a", "c", "e"};
    const char* v1[] = {"1", "3", "5"};
    if (!create_sstable_at(TEST_DIR, 0, 1, k1, v1, 3, KANBUDB_SSTABLE_FLAG_ALIVE))
        return 0;

    const char* k2[] = {"b", "d", "f"};
    const char* v2[] = {"2", "4", "6"};
    if (!create_sstable_at(TEST_DIR, 0, 2, k2, v2, 3, KANBUDB_SSTABLE_FLAG_ALIVE))
        return 0;

    const char* inputs[] = {
        TEST_DIR ".sst.0.1",
        TEST_DIR ".sst.0.2"
    };

    char out_path[256];
    compaction_make_path(TEST_DIR, 1, 10, out_path, sizeof(out_path));

    int rc = compaction_merge_to_level(inputs, 2, out_path, 10);
    if (rc != KANBUDB_OK) {
        fprintf(stderr, "  merge_to_level returned %d\n", rc);
        cleanup(); return 0;
    }

    sstable_reader_t* r = sstable_reader_open(out_path);
    if (!r) { cleanup(); return 0; }

    if (sstable_reader_num_entries(r) != 6) {
        fprintf(stderr, "  merged should have 6 entries, got %llu\n",
                (unsigned long long)sstable_reader_num_entries(r));
        sstable_reader_close(r); cleanup(); return 0;
    }

    const char* expected_keys[] = {"a", "b", "c", "d", "e", "f"};
    const char* expected_vals[] = {"1", "2", "3", "4", "5", "6"};

    for (int i = 0; i < 6; i++) {
        void* val = NULL;
        size_t vlen = 0;
        uint8_t flag = 0;
        int grc = sstable_reader_get(r, expected_keys[i],
                                      strlen(expected_keys[i]) + 1,
                                      &val, &vlen, &flag);
        if (grc != KANBUDB_OK) {
            fprintf(stderr, "  missing key '%s' after merge\n", expected_keys[i]);
            sstable_reader_close(r); cleanup(); return 0;
        }
        if (memcmp(val, expected_vals[i], vlen) != 0) {
            fprintf(stderr, "  key '%s': val mismatch\n", expected_keys[i]);
            sstable_reader_close(r); cleanup(); return 0;
        }
    }

    sstable_reader_close(r);
    unlink(out_path);
    cleanup();
    return 1;
}

static int test_merge_dedup_tombstone(void) {
    cleanup();

    const char* k1[] = {"a", "b", "c"};
    const char* v1[] = {"1", "2", "3"};
    if (!create_sstable_at(TEST_DIR, 0, 1, k1, v1, 3, KANBUDB_SSTABLE_FLAG_ALIVE))
        return 0;

    const char* k2[] = {"b"};
    const char* v2[] = {""};
    if (!create_sstable_at(TEST_DIR, 0, 2, k2, v2, 1, KANBUDB_SSTABLE_FLAG_TOMBSTONE))
        return 0;

    const char* inputs[] = {
        TEST_DIR ".sst.0.1",
        TEST_DIR ".sst.0.2"
    };

    char out_path[256];
    compaction_make_path(TEST_DIR, 1, 20, out_path, sizeof(out_path));

    int rc = compaction_merge_to_level(inputs, 2, out_path, 20);
    if (rc != KANBUDB_OK) { cleanup(); return 0; }

    sstable_reader_t* r = sstable_reader_open(out_path);
    if (!r) { cleanup(); return 0; }

    if (sstable_reader_num_entries(r) != 3) {
        fprintf(stderr, "  merge should have 3 entries (a, b-tombstone, c), got %llu\n",
                (unsigned long long)sstable_reader_num_entries(r));
        sstable_reader_close(r); cleanup(); return 0;
    }

    void* val = NULL;
    size_t vlen = 0;
    uint8_t flag = 0;

    rc = sstable_reader_get(r, "a", 2, &val, &vlen, &flag);
    if (rc != KANBUDB_OK) { sstable_reader_close(r); cleanup(); return 0; }

    rc = sstable_reader_get(r, "b", 2, &val, &vlen, &flag);
    if (rc == KANBUDB_OK) {
        sstable_reader_close(r);
        fprintf(stderr, "  'b' was deleted via tombstone but present after merge\n");
        cleanup(); return 0;
    }

    rc = sstable_reader_get(r, "c", 2, &val, &vlen, &flag);
    if (rc != KANBUDB_OK) { sstable_reader_close(r); cleanup(); return 0; }

    sstable_reader_close(r);
    unlink(out_path);
    cleanup();
    return 1;
}

static int test_size_target(void) {
    uint64_t l1 = compaction_max_bytes_for_level(1);
    uint64_t l2 = compaction_max_bytes_for_level(2);
    uint64_t l3 = compaction_max_bytes_for_level(3);

    if (l1 != 10ULL * 1024 * 1024) {
        fprintf(stderr, "  L1 max bytes = %llu, expected 10MB\n",
                (unsigned long long)l1);
        return 0;
    }
    if (l2 != 100ULL * 1024 * 1024) {
        fprintf(stderr, "  L2 max bytes = %llu, expected 100MB\n",
                (unsigned long long)l2);
        return 0;
    }
    if (l3 != 1048576000ULL) { /* 10MB * 10 * 10 = 1000 MB */
        fprintf(stderr, "  L3 max bytes = %llu, expected 1000MB\n",
                (unsigned long long)l3);
        return 0;
    }

    return 1;
}

int main(void) {
    printf("leveled compaction rigorous tests:\n");
    TEST(scan_levels_empty);
    TEST(scan_levels_with_files);
    TEST(pick_level_triggers);
    TEST(no_pick_when_below_threshold);
    TEST(multi_level_merge);
    TEST(merge_dedup_tombstone);
    TEST(size_target);
    printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
