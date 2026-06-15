#include "vector.h"
#include "util/macros.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <unistd.h>
#include <sys/stat.h>

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) do { \
  printf("  TEST: %s ... ", #name); \
  if (test_##name()) { printf("PASS\n"); tests_passed++; } \
  else { printf("FAIL\n"); tests_failed++; } \
} while(0)

#define ASSERT(cond, msg) do { \
  if (!(cond)) { printf("\n    ASSERT FAIL: %s\n", msg); return 0; } \
} while(0)

/* ------------------------------------------------------------------ */
/*  Test helpers                                                       */
/* ------------------------------------------------------------------ */

static void cleanup_path(const char* path)
{
    char buf[1024];
    snprintf(buf, sizeof(buf), "%s/meta", path);    unlink(buf);
    snprintf(buf, sizeof(buf), "%s/vectors", path);  unlink(buf);
    snprintf(buf, sizeof(buf), "%s/ids", path);      unlink(buf);
    snprintf(buf, sizeof(buf), "%s/wal", path);      unlink(buf);
    rmdir(path);
}

#define TEST_PATH "/tmp/kanbudb_vec_test"

/* ------------------------------------------------------------------ */
/*  Tests                                                              */
/* ------------------------------------------------------------------ */

static int test_create_close(void)
{
    kanbudb_vec_params_t params = {
        .metric = KANBUDB_VEC_METRIC_L2,
        .dimension = 4,
        .initial_capacity = 0,
        .enable_persistence = 0
    };
    kanbudb_vec_index_t* idx = NULL;
    int rc = kanbudb_vec_create(NULL, &params, &idx);
    ASSERT(rc == KANBUDB_VEC_OK, "create failed");
    ASSERT(idx != NULL, "idx is null");
    rc = kanbudb_vec_close(idx);
    ASSERT(rc == KANBUDB_VEC_OK, "close failed");
    return 1;
}

static int test_insert_search(void)
{
    kanbudb_vec_params_t params = {
        .metric = KANBUDB_VEC_METRIC_L2,
        .dimension = 3,
        .initial_capacity = 0,
        .enable_persistence = 0
    };
    kanbudb_vec_index_t* idx = NULL;
    ASSERT(kanbudb_vec_create(NULL, &params, &idx) == KANBUDB_VEC_OK, "create");

    float v1[] = {1.0f, 0.0f, 0.0f};
    float v2[] = {0.0f, 1.0f, 0.0f};
    float v3[] = {0.0f, 0.0f, 1.0f};
    float v4[] = {1.0f, 1.0f, 1.0f};

    ASSERT(kanbudb_vec_insert(idx, 1, v1) == KANBUDB_VEC_OK, "insert v1");
    ASSERT(kanbudb_vec_insert(idx, 2, v2) == KANBUDB_VEC_OK, "insert v2");
    ASSERT(kanbudb_vec_insert(idx, 3, v3) == KANBUDB_VEC_OK, "insert v3");
    ASSERT(kanbudb_vec_insert(idx, 4, v4) == KANBUDB_VEC_OK, "insert v4");

    ASSERT(kanbudb_vec_count(idx) == 4, "count");

    float query[] = {1.0f, 0.1f, 0.0f};
    kanbudb_vec_result_t results[4];
    int n = kanbudb_vec_search(idx, query, 4, results);
    ASSERT(n == 4, "search returned 4 results");
    ASSERT(results[0].id == 1, "closest is id=1");
    ASSERT(results[0].distance < 0.011f, "distance ~0.01");

    kanbudb_vec_close(idx);
    return 1;
}

static int test_update(void)
{
    kanbudb_vec_params_t params = {
        .metric = KANBUDB_VEC_METRIC_L2,
        .dimension = 2,
        .enable_persistence = 0
    };
    kanbudb_vec_index_t* idx = NULL;
    ASSERT(kanbudb_vec_create(NULL, &params, &idx) == KANBUDB_VEC_OK, "create");

    float v1[] = {1.0f, 0.0f};
    float v2[] = {100.0f, 100.0f};
    ASSERT(kanbudb_vec_insert(idx, 1, v1) == KANBUDB_VEC_OK, "insert");
    ASSERT(kanbudb_vec_insert(idx, 1, v2) == KANBUDB_VEC_OK, "update same id");

    float query[] = {100.0f, 100.0f};
    kanbudb_vec_result_t results[1];
    int n = kanbudb_vec_search(idx, query, 1, results);
    ASSERT(n == 1, "search");
    ASSERT(results[0].id == 1, "id=1");
    ASSERT(results[0].distance < 0.01f, "distance ~0 (updated)");

    kanbudb_vec_close(idx);
    return 1;
}

static int test_delete(void)
{
    kanbudb_vec_params_t params = {
        .metric = KANBUDB_VEC_METRIC_L2,
        .dimension = 2,
        .enable_persistence = 0
    };
    kanbudb_vec_index_t* idx = NULL;
    ASSERT(kanbudb_vec_create(NULL, &params, &idx) == KANBUDB_VEC_OK, "create");

    float v1[] = {1.0f, 0.0f};
    float v2[] = {0.0f, 1.0f};
    kanbudb_vec_insert(idx, 1, v1);
    kanbudb_vec_insert(idx, 2, v2);

    ASSERT(kanbudb_vec_delete(idx, 1) == KANBUDB_VEC_OK, "delete");
    ASSERT(kanbudb_vec_delete(idx, 999) == KANBUDB_VEC_ERR_NOTFOUND, "delete nonexistent");

    float query[] = {1.0f, 0.0f};
    kanbudb_vec_result_t results[2];
    int n = kanbudb_vec_search(idx, query, 2, results);
    ASSERT(n == 1, "only 1 result after delete");
    ASSERT(results[0].id == 2, "remaining is id=2");

    kanbudb_vec_close(idx);
    return 1;
}

static int test_get(void)
{
    kanbudb_vec_params_t params = {
        .metric = KANBUDB_VEC_METRIC_L2,
        .dimension = 3,
        .enable_persistence = 0
    };
    kanbudb_vec_index_t* idx = NULL;
    ASSERT(kanbudb_vec_create(NULL, &params, &idx) == KANBUDB_VEC_OK, "create");

    float v[] = {3.14f, 2.71f, 1.41f};
    kanbudb_vec_insert(idx, 42, v);

    float out[3];
    ASSERT(kanbudb_vec_get(idx, 42, out) == KANBUDB_VEC_OK, "get ok");
    ASSERT(fabsf(out[0] - 3.14f) < 1e-5f, "val[0]");
    ASSERT(fabsf(out[1] - 2.71f) < 1e-5f, "val[1]");
    ASSERT(fabsf(out[2] - 1.41f) < 1e-5f, "val[2]");

    ASSERT(kanbudb_vec_get(idx, 999, out) == KANBUDB_VEC_ERR_NOTFOUND, "get nonexistent");

    kanbudb_vec_close(idx);
    return 1;
}

static int test_search_radius(void)
{
    kanbudb_vec_params_t params = {
        .metric = KANBUDB_VEC_METRIC_L2,
        .dimension = 2,
        .enable_persistence = 0
    };
    kanbudb_vec_index_t* idx = NULL;
    ASSERT(kanbudb_vec_create(NULL, &params, &idx) == KANBUDB_VEC_OK, "create");

    float pts[][2] = {{0,0}, {1,0}, {0,1}, {3,3}, {5,5}};
    for (int i = 0; i < 5; i++)
        kanbudb_vec_insert(idx, (uint64_t)(i+1), pts[i]);

    float query[] = {0,0};
    kanbudb_vec_result_t results[10];
    int n = kanbudb_vec_search_radius(idx, query, 1.5f, results, 10);
    ASSERT(n == 3, "3 within radius");

    kanbudb_vec_close(idx);
    return 1;
}

static int test_batch_insert(void)
{
    kanbudb_vec_params_t params = {
        .metric = KANBUDB_VEC_METRIC_L2,
        .dimension = 2,
        .enable_persistence = 0
    };
    kanbudb_vec_index_t* idx = NULL;
    ASSERT(kanbudb_vec_create(NULL, &params, &idx) == KANBUDB_VEC_OK, "create");

    uint64_t ids[] = {10, 20, 30};
    float vecs[] = {1,2, 3,4, 5,6};
    ASSERT(kanbudb_vec_insert_batch(idx, 3, ids, vecs) == KANBUDB_VEC_OK, "batch");
    ASSERT(kanbudb_vec_count(idx) == 3, "count after batch");

    kanbudb_vec_close(idx);
    return 1;
}

static int test_cosine_metric(void)
{
    kanbudb_vec_params_t params = {
        .metric = KANBUDB_VEC_METRIC_COSINE,
        .dimension = 2,
        .enable_persistence = 0
    };
    kanbudb_vec_index_t* idx = NULL;
    ASSERT(kanbudb_vec_create(NULL, &params, &idx) == KANBUDB_VEC_OK, "create");

    /* Normalized vectors */
    float v1[] = {1.0f, 0.0f};
    float v2[] = {0.0f, 1.0f};
    float v3[] = {0.707f, 0.707f};
    kanbudb_vec_insert(idx, 1, v1);
    kanbudb_vec_insert(idx, 2, v2);
    kanbudb_vec_insert(idx, 3, v3);

    float query[] = {1.0f, 0.0f};
    kanbudb_vec_result_t results[3];
    int n = kanbudb_vec_search(idx, query, 3, results);
    ASSERT(n == 3, "3 results");
    /* Cosine: query·v1=1 → distance=0, query·v3≈0.707 → distance≈0.293, query·v2=0 → distance=1 */
    ASSERT(results[0].id == 1, "best is id=1 (same direction)");
    ASSERT(results[0].distance < 0.001f, "distance ~0");

    kanbudb_vec_close(idx);
    return 1;
}

static int test_ip_metric(void)
{
    kanbudb_vec_params_t params = {
        .metric = KANBUDB_VEC_METRIC_IP,
        .dimension = 2,
        .enable_persistence = 0
    };
    kanbudb_vec_index_t* idx = NULL;
    ASSERT(kanbudb_vec_create(NULL, &params, &idx) == KANBUDB_VEC_OK, "create");

    float v1[] = {2.0f, 0.0f};
    float v2[] = {1.0f, 1.0f};
    float v3[] = {1.0f, 0.0f};
    kanbudb_vec_insert(idx, 1, v1);
    kanbudb_vec_insert(idx, 2, v2);
    kanbudb_vec_insert(idx, 3, v3);

    float query[] = {1.0f, 0.0f};
    kanbudb_vec_result_t results[3];
    int n = kanbudb_vec_search(idx, query, 3, results);
    ASSERT(n == 3, "3 results");
    /* IP distance = -dot, so we want the largest dot = smallest dist
     * v1·q=2 → dist=-2, v3·q=1 → dist=-1, v2·q=1 → dist=-1 */
    ASSERT(results[0].id == 1, "best is id=1 (dot=2)");
    ASSERT(results[0].distance == -2.0f, "distance = -2");

    kanbudb_vec_close(idx);
    return 1;
}

static int test_persistence(void)
{
    cleanup_path(TEST_PATH);
    mkdir(TEST_PATH, 0755);

    kanbudb_vec_params_t params = {
        .metric = KANBUDB_VEC_METRIC_L2,
        .dimension = 3,
        .initial_capacity = 0,
        .enable_persistence = 1
    };
    kanbudb_vec_index_t* idx = NULL;
    ASSERT(kanbudb_vec_create(TEST_PATH, &params, &idx) == KANBUDB_VEC_OK, "create");

    float v1[] = {1.0f, 0.0f, 0.0f};
    float v2[] = {0.0f, 1.0f, 0.0f};
    float v3[] = {0.0f, 0.0f, 1.0f};
    kanbudb_vec_insert(idx, 1, v1);
    kanbudb_vec_insert(idx, 2, v2);
    kanbudb_vec_insert(idx, 3, v3);

    ASSERT(kanbudb_vec_flush(idx) == KANBUDB_VEC_OK, "flush");
    ASSERT(kanbudb_vec_close(idx) == KANBUDB_VEC_OK, "close");

    /* Reopen */
    kanbudb_vec_index_t* idx2 = NULL;
    ASSERT(kanbudb_vec_open(TEST_PATH, &idx2) == KANBUDB_VEC_OK, "open");
    ASSERT(kanbudb_vec_count(idx2) == 3, "count after reopen");

    float query[] = {1.0f, 0.1f, 0.0f};
    kanbudb_vec_result_t results[3];
    int n = kanbudb_vec_search(idx2, query, 3, results);
    ASSERT(n == 3, "search after reopen");
    ASSERT(results[0].id == 1, "closest is id=1");

    kanbudb_vec_close(idx2);

    /* Destroy */
    ASSERT(kanbudb_vec_destroy(TEST_PATH) == KANBUDB_VEC_OK, "destroy");
    return 1;
}

static int test_empty_index(void)
{
    kanbudb_vec_params_t params = {
        .metric = KANBUDB_VEC_METRIC_L2,
        .dimension = 2,
        .enable_persistence = 0
    };
    kanbudb_vec_index_t* idx = NULL;
    ASSERT(kanbudb_vec_create(NULL, &params, &idx) == KANBUDB_VEC_OK, "create");

    float query[] = {0,0};
    kanbudb_vec_result_t results[1];
    int n = kanbudb_vec_search(idx, query, 1, results);
    ASSERT(n == 0, "search empty returns 0");

    float out[2];
    ASSERT(kanbudb_vec_get(idx, 1, out) == KANBUDB_VEC_ERR_NOTFOUND, "get empty");

    ASSERT(kanbudb_vec_delete(idx, 1) == KANBUDB_VEC_ERR_NOTFOUND, "delete empty");

    kanbudb_vec_close(idx);
    return 1;
}

static int test_wal_replay(void)
{
    cleanup_path(TEST_PATH);
    mkdir(TEST_PATH, 0755);

    /* Phase 1: create, insert 2 vectors, flush (checkpoint), close */
    kanbudb_vec_params_t params = KANBUDB_VEC_PARAMS_DEFAULT;
    params.dimension = 2;
    params.initial_capacity = 10;
    params.enable_persistence = 1;

    kanbudb_vec_index_t* idx = NULL;
    ASSERT(kanbudb_vec_create(TEST_PATH, &params, &idx) == KANBUDB_VEC_OK, "create");

    float v1[] = {1.0f, 0.0f};
    float v2[] = {0.0f, 1.0f};
    kanbudb_vec_insert(idx, 1, v1);
    kanbudb_vec_insert(idx, 2, v2);
    ASSERT(kanbudb_vec_flush(idx) == KANBUDB_VEC_OK, "flush");
    ASSERT(kanbudb_vec_close(idx) == KANBUDB_VEC_OK, "close");

    /* Phase 2: manually write an insert record to the WAL file,
     * simulating a crash that left data in the WAL */
    uint32_t op   = 1; /* WAL_OP_INSERT */
    uint64_t id3  = 3;
    uint32_t dim2 = 2;
    float    vec3[] = {0.5f, 0.5f};
    uint64_t ts   = 0;

    char wal_path[1024];
    snprintf(wal_path, sizeof(wal_path), "%s/wal", TEST_PATH);
    FILE* f = fopen(wal_path, "wb");
    ASSERT(f != NULL, "open wal for write");
    fwrite(&op, sizeof(op), 1, f);
    fwrite(&id3, sizeof(id3), 1, f);
    fwrite(&dim2, sizeof(dim2), 1, f);
    fwrite(vec3, sizeof(float), 2, f);
    fwrite(&ts, sizeof(ts), 1, f);
    fclose(f);

    /* Phase 3: reopen — should have 1,2 from files + 3 from WAL replay */
    kanbudb_vec_index_t* idx2 = NULL;
    ASSERT(kanbudb_vec_open(TEST_PATH, &idx2) == KANBUDB_VEC_OK, "open");
    ASSERT(kanbudb_vec_count(idx2) == 3, "count=3 after WAL replay");

    float query[] = {0.5f, 0.5f};
    kanbudb_vec_result_t results[3];
    int n = kanbudb_vec_search(idx2, query, 3, results);
    ASSERT(n == 3, "search 3 results");
    ASSERT(results[0].id == 3, "closest is id=3");

    /* Phase 4: add more data, flush, close, reopen — WAL should be clean */
    float v4[] = {0.2f, 0.8f};
    kanbudb_vec_insert(idx2, 4, v4);
    ASSERT(kanbudb_vec_flush(idx2) == KANBUDB_VEC_OK, "flush2");
    ASSERT(kanbudb_vec_close(idx2) == KANBUDB_VEC_OK, "close2");

    kanbudb_vec_index_t* idx3 = NULL;
    ASSERT(kanbudb_vec_open(TEST_PATH, &idx3) == KANBUDB_VEC_OK, "open2");
    ASSERT(kanbudb_vec_count(idx3) == 4, "count=4");

    float q2[] = {0.2f, 0.8f};
    kanbudb_vec_result_t r2[4];
    n = kanbudb_vec_search(idx3, q2, 4, r2);
    ASSERT(n == 4, "search 4");
    ASSERT(r2[0].id == 4, "closest id=4");

    kanbudb_vec_close(idx3);

    /* Phase 5: write WAL with a delete op to test delete replay */
    uint64_t iddel = 1;
    op = 2; /* WAL_OP_DELETE */
    f = fopen(wal_path, "wb");
    ASSERT(f != NULL, "open wal for delete");
    fwrite(&op, sizeof(op), 1, f);
    fwrite(&iddel, sizeof(iddel), 1, f);
    fclose(f);

    kanbudb_vec_index_t* idx4 = NULL;
    ASSERT(kanbudb_vec_open(TEST_PATH, &idx4) == KANBUDB_VEC_OK, "open3");
    /* FLAT count includes deleted; search should show 3 non-deleted */
    float q3[] = {1.0f, 0.0f};
    kanbudb_vec_result_t r3[4];
    n = kanbudb_vec_search(idx4, q3, 4, r3);
    ASSERT(n == 3, "search 3 after delete replay");
    ASSERT(kanbudb_vec_get(idx4, 1, q3) == KANBUDB_VEC_ERR_NOTFOUND, "id1 deleted");

    kanbudb_vec_close(idx4);

    kanbudb_vec_destroy(TEST_PATH);
    return 1;
}

static int test_stats(void)
{
    kanbudb_vec_params_t params = {
        .metric = KANBUDB_VEC_METRIC_L2,
        .dimension = 4,
        .initial_capacity = 0,
        .enable_persistence = 0
    };
    kanbudb_vec_index_t* idx = NULL;
    ASSERT(kanbudb_vec_create(NULL, &params, &idx) == KANBUDB_VEC_OK, "create");

    float v[] = {1,2,3,4};
    kanbudb_vec_insert(idx, 1, v);

    kanbudb_vec_stats_t stats;
    ASSERT(kanbudb_vec_stats(idx, &stats) == KANBUDB_VEC_OK, "stats");
    ASSERT(stats.count == 1, "stats count");
    ASSERT(stats.dimension == 4, "stats dim");

    kanbudb_vec_close(idx);
    return 1;
}

/* ------------------------------------------------------------------ */
/*  Main                                                               */
/* ------------------------------------------------------------------ */

int main(void)
{
    printf("vector tests:\n");
    TEST(create_close);
    TEST(insert_search);
    TEST(update);
    TEST(delete);
    TEST(get);
    TEST(search_radius);
    TEST(batch_insert);
    TEST(cosine_metric);
    TEST(ip_metric);
    TEST(persistence);
    TEST(wal_replay);
    TEST(empty_index);
    TEST(stats);
    printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
