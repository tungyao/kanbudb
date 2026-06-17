#include "compaction.h"
#include "sstable.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define TEST_DIR "/tmp/kanbudb_test_compact"

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) do { \
  printf("  TEST: %s ... ", #name); \
  if (test_##name()) { printf("PASS\n"); tests_passed++; } \
  else { printf("FAIL\n"); tests_failed++; } \
} while(0)

static void cleanup(void) {
    unlink("/tmp/kanbudb_test_compact_in1.sst.0.1");
    unlink("/tmp/kanbudb_test_compact_in2.sst.0.2");
    unlink("/tmp/kanbudb_test_compact_out.sst.1.1");
    unlink("/tmp/kanbudb_test_compact_out.sst.1.1.tmp");
}

/* Create an SSTable with given key-value pairs */
static int create_sstable(const char* path, uint64_t seq,
                           const char* const* keys, const char* const* vals,
                           int num, uint8_t flag) {
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    sstable_writer_t* w = sstable_writer_create(tmp, path, seq);
    if (!w) return 0;
    for (int i = 0; i < num; i++) {
        if (sstable_writer_add(w, keys[i], strlen(keys[i]) + 1,
                                vals[i], strlen(vals[i]) + 1, flag) != 0) {
            sstable_writer_destroy(w);
            return 0;
        }
    }
    int rc = sstable_writer_finish(w);
    sstable_writer_destroy(w);
    return rc == KANBUDB_OK;
}

static int test_merge_two_files(void) {
    cleanup();

    /* SSTable 1: a->1, b->2 */
    const char* k1[] = {"a", "b"};
    const char* v1[] = {"1", "2"};
    if (!create_sstable("/tmp/kanbudb_test_compact_in1.sst.0.1", 1, k1, v1, 2,
                         KANBUDB_SSTABLE_FLAG_ALIVE)) return 0;

    /* SSTable 2: b->3, c->4  (b has a newer value) */
    const char* k2[] = {"b", "c"};
    const char* v2[] = {"3", "4"};
    if (!create_sstable("/tmp/kanbudb_test_compact_in2.sst.0.2", 2, k2, v2, 2,
                         KANBUDB_SSTABLE_FLAG_ALIVE)) return 0;

    kanbudb_compactor_t* c = compactor_create();
    if (!c) return 0;

    const char* inputs[] = {
        "/tmp/kanbudb_test_compact_in1.sst.0.1",
        "/tmp/kanbudb_test_compact_in2.sst.0.2"
    };
    int rc = compactor_merge_sstables(c, inputs, 2,
                                       "/tmp/kanbudb_test_compact_out.sst.1.1", 10);
    compactor_destroy(c);
    if (rc != KANBUDB_OK) return 0;

    /* Verify: should have a->1, b->3 (seq 2 wins), c->4 */
    sstable_reader_t* r = sstable_reader_open(
        "/tmp/kanbudb_test_compact_out.sst.1.1");
    if (!r) return 0;
    if (sstable_reader_num_entries(r) != 3) {
        sstable_reader_close(r); return 0;
    }

    /* Check values */
    void* val = NULL;
    size_t vlen = 0;
    uint8_t flag = 0;

    rc = sstable_reader_get(r, "a", 2, &val, &vlen, &flag);
    if (rc != KANBUDB_OK || !val || strcmp((char*)val, "1") != 0) {
        sstable_reader_close(r); return 0;
    }

    rc = sstable_reader_get(r, "b", 2, &val, &vlen, &flag);
    if (rc != KANBUDB_OK || !val || strcmp((char*)val, "3") != 0) {
        sstable_reader_close(r); return 0;
    }

    rc = sstable_reader_get(r, "c", 2, &val, &vlen, &flag);
    if (rc != KANBUDB_OK || !val || strcmp((char*)val, "4") != 0) {
        sstable_reader_close(r); return 0;
    }

    sstable_reader_close(r);
    cleanup();
    return 1;
}

static int test_merge_single_file(void) {
    cleanup();

    const char* k[] = {"x", "y", "z"};
    const char* v[] = {"10", "20", "30"};
    if (!create_sstable("/tmp/kanbudb_test_compact_in1.sst.0.1", 1, k, v, 3,
                         KANBUDB_SSTABLE_FLAG_ALIVE)) return 0;

    kanbudb_compactor_t* c = compactor_create();
    if (!c) return 0;

    const char* inputs[] = {"/tmp/kanbudb_test_compact_in1.sst.0.1"};
    int rc = compactor_merge_sstables(c, inputs, 1,
                                       "/tmp/kanbudb_test_compact_out.sst.1.1", 5);
    compactor_destroy(c);
    if (rc != KANBUDB_OK) return 0;

    sstable_reader_t* r = sstable_reader_open(
        "/tmp/kanbudb_test_compact_out.sst.1.1");
    if (!r) return 0;
    if (sstable_reader_num_entries(r) != 3) {
        sstable_reader_close(r); return 0;
    }
    sstable_reader_close(r);
    cleanup();
    return 1;
}

int main(void) {
    printf("compaction tests:\n");
    TEST(merge_two_files);
    TEST(merge_single_file);
    printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
