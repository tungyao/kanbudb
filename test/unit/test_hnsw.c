#include "hnsw.h"
#include "vector.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

static int test_create_destroy(void)
{
    hnsw_index_t* idx = NULL;
    int rc = hnsw_create(4, 16, 200, 50, 100, KANBUDB_VEC_METRIC_L2, &idx);
    ASSERT(rc == KANBUDB_VEC_OK, "create ok");
    ASSERT(idx != NULL, "idx not null");
    hnsw_destroy(idx);
    return 1;
}

static int test_insert_search_one(void)
{
    hnsw_index_t* idx = NULL;
    hnsw_create(3, 16, 200, 50, 100, KANBUDB_VEC_METRIC_L2, &idx);

    float v[] = {1.0f, 0.0f, 0.0f};
    ASSERT(hnsw_insert(idx, 1, v) == KANBUDB_VEC_OK, "insert");
    ASSERT(hnsw_count(idx) == 1, "count == 1");

    float q[] = {0.9f, 0.1f, 0.0f};
    kanbudb_vec_result_t res[1];
    int n = hnsw_search(idx, q, 1, res);
    ASSERT(n == 1, "found 1");
    ASSERT(res[0].id == 1, "id == 1");
    ASSERT(res[0].distance < 0.03f, "distance small");

    hnsw_destroy(idx);
    return 1;
}

static int test_insert_search_multi(void)
{
    hnsw_index_t* idx = NULL;
    hnsw_create(2, 16, 200, 50, 100, KANBUDB_VEC_METRIC_L2, &idx);

    float pts[][2] = {{0,0}, {1,0}, {0,1}, {1,1}, {2,2}};
    for (int i = 0; i < 5; i++)
        ASSERT(hnsw_insert(idx, (uint64_t)(i+1), pts[i]) == KANBUDB_VEC_OK, "insert");

    ASSERT(hnsw_count(idx) == 5, "count 5");

    float q[] = {0, 0};
    kanbudb_vec_result_t res[1];
    int n = hnsw_search(idx, q, 1, res);
    ASSERT(n == 1, "found 1");
    ASSERT(res[0].id == 1, "closest to origin is (0,0) id=1");
    ASSERT(res[0].distance < 0.001f, "distance ~0");

    float q2[] = {1.9f, 2.1f};
    n = hnsw_search(idx, q2, 1, res);
    ASSERT(n == 1, "found 1");
    ASSERT(res[0].id == 5, "closest to (2,2) is id=5");
    ASSERT(res[0].distance < 0.05f, "distance small");

    hnsw_destroy(idx);
    return 1;
}

static int test_search_topk(void)
{
    hnsw_index_t* idx = NULL;
    hnsw_create(2, 16, 200, 50, 100, KANBUDB_VEC_METRIC_L2, &idx);

    float pts[][2] = {{0,0}, {1,0}, {0,1}, {1,1}, {2,2}};
    for (int i = 0; i < 5; i++)
        hnsw_insert(idx, (uint64_t)(i+1), pts[i]);

    float q[] = {0.5f, 0.5f};
    kanbudb_vec_result_t res[5];
    int n = hnsw_search(idx, q, 5, res);
    ASSERT(n >= 3, "found >=3 results");
    ASSERT(res[0].distance >= 0.0f, "distance non-negative");
    for (int i = 1; i < n; i++)
        ASSERT(res[i-1].distance <= res[i].distance + 0.001f, "sorted");

    hnsw_destroy(idx);
    return 1;
}

static int test_delete(void)
{
    hnsw_index_t* idx = NULL;
    hnsw_create(2, 16, 200, 50, 100, KANBUDB_VEC_METRIC_L2, &idx);

    float v1[] = {1.0f, 0.0f};
    float v2[] = {0.0f, 1.0f};
    hnsw_insert(idx, 1, v1);
    hnsw_insert(idx, 2, v2);

    ASSERT(hnsw_delete(idx, 1) == KANBUDB_VEC_OK, "delete ok");

    float q[] = {1.0f, 0.0f};
    kanbudb_vec_result_t res[2];
    int n = hnsw_search(idx, q, 2, res);
    ASSERT(n == 1, "only 1 after delete");
    ASSERT(res[0].id == 2, "remaining is id=2");

    ASSERT(hnsw_delete(idx, 999) == KANBUDB_VEC_ERR_NOTFOUND, "delete nonexistent");

    hnsw_destroy(idx);
    return 1;
}

static int test_get(void)
{
    hnsw_index_t* idx = NULL;
    hnsw_create(3, 16, 200, 50, 100, KANBUDB_VEC_METRIC_L2, &idx);

    float v[] = {3.14f, 2.71f, 1.41f};
    hnsw_insert(idx, 42, v);

    float out[3];
    ASSERT(hnsw_get(idx, 42, out) == KANBUDB_VEC_OK, "get ok");
    ASSERT(fabsf(out[0] - 3.14f) < 1e-5f, "val[0]");
    ASSERT(fabsf(out[1] - 2.71f) < 1e-5f, "val[1]");
    ASSERT(fabsf(out[2] - 1.41f) < 1e-5f, "val[2]");

    ASSERT(hnsw_get(idx, 999, out) == KANBUDB_VEC_ERR_NOTFOUND, "get nonexistent");

    hnsw_destroy(idx);
    return 1;
}

static int test_count(void)
{
    hnsw_index_t* idx = NULL;
    hnsw_create(2, 16, 200, 50, 100, KANBUDB_VEC_METRIC_L2, &idx);

    ASSERT(hnsw_count(idx) == 0, "empty count = 0");

    float v[] = {1, 2};
    hnsw_insert(idx, 1, v);
    ASSERT(hnsw_count(idx) == 1, "count = 1");

    hnsw_insert(idx, 2, v);
    ASSERT(hnsw_count(idx) == 2, "count = 2");

    hnsw_destroy(idx);
    return 1;
}

static int test_empty_index(void)
{
    hnsw_index_t* idx = NULL;
    hnsw_create(2, 16, 200, 50, 100, KANBUDB_VEC_METRIC_L2, &idx);

    float q[] = {0, 0};
    kanbudb_vec_result_t res[1];
    int n = hnsw_search(idx, q, 1, res);
    ASSERT(n == 0, "search empty = 0");

    float out[2];
    ASSERT(hnsw_get(idx, 1, out) == KANBUDB_VEC_ERR_NOTFOUND, "get empty");
    ASSERT(hnsw_delete(idx, 1) == KANBUDB_VEC_ERR_NOTFOUND, "delete empty");

    hnsw_destroy(idx);
    return 1;
}

static int test_update(void)
{
    hnsw_index_t* idx = NULL;
    hnsw_create(2, 16, 200, 50, 100, KANBUDB_VEC_METRIC_L2, &idx);

    float v1[] = {1.0f, 0.0f};
    float v2[] = {100.0f, 100.0f};
    hnsw_insert(idx, 1, v1);
    hnsw_insert(idx, 1, v2);

    ASSERT(hnsw_count(idx) == 1, "count still 1 after update");

    float q[] = {100.0f, 100.0f};
    kanbudb_vec_result_t res[1];
    int n = hnsw_search(idx, q, 1, res);
    ASSERT(n == 1, "found 1");
    ASSERT(res[0].id == 1, "id = 1");
    ASSERT(res[0].distance < 0.01f, "distance ~0 (updated)");

    hnsw_destroy(idx);
    return 1;
}

static int test_cosine_metric(void)
{
    hnsw_index_t* idx = NULL;
    hnsw_create(2, 16, 200, 50, 100, KANBUDB_VEC_METRIC_COSINE, &idx);

    float v1[] = {1.0f, 0.0f};
    float v2[] = {0.0f, 1.0f};
    float v3[] = {0.707f, 0.707f};
    hnsw_insert(idx, 1, v1);
    hnsw_insert(idx, 2, v2);
    hnsw_insert(idx, 3, v3);

    float q[] = {1.0f, 0.0f};
    kanbudb_vec_result_t res[3];
    int n = hnsw_search(idx, q, 3, res);
    ASSERT(n >= 1, "at least 1");
    ASSERT(res[0].id == 1, "best is id=1 (same direction)");
    ASSERT(res[0].distance < 0.001f, "distance ~0");

    hnsw_destroy(idx);
    return 1;
}

static int test_ip_metric(void)
{
    hnsw_index_t* idx = NULL;
    hnsw_create(2, 16, 200, 50, 100, KANBUDB_VEC_METRIC_IP, &idx);

    float v1[] = {2.0f, 0.0f};
    float v2[] = {1.0f, 1.0f};
    float v3[] = {1.0f, 0.0f};
    hnsw_insert(idx, 1, v1);
    hnsw_insert(idx, 2, v2);
    hnsw_insert(idx, 3, v3);

    float q[] = {1.0f, 0.0f};
    kanbudb_vec_result_t res[3];
    int n = hnsw_search(idx, q, 3, res);
    ASSERT(n >= 1, "at least 1");
    ASSERT(res[0].id == 1, "best is id=1 (dot=2)");
    ASSERT(res[0].distance < -1.5f, "distance ~ -2");

    hnsw_destroy(idx);
    return 1;
}

static int test_many_inserts(void)
{
    hnsw_index_t* idx = NULL;
    hnsw_create(2, 16, 200, 50, 200, KANBUDB_VEC_METRIC_L2, &idx);

    for (int i = 0; i < 100; i++) {
        float v[2] = {(float)i, (float)(i * 2)};
        hnsw_insert(idx, (uint64_t)i, v);
    }
    ASSERT(hnsw_count(idx) == 100, "count 100");

    float q[] = {50.0f, 100.0f};
    kanbudb_vec_result_t res[1];
    int n = hnsw_search(idx, q, 1, res);
    ASSERT(n == 1, "found 1");
    ASSERT(res[0].id == 50, "nearest is (50,100) id=50");
    ASSERT(res[0].distance < 0.01f, "distance ~0");

    hnsw_destroy(idx);
    return 1;
}

#define HNSW_TEST_PATH "/tmp/kanbudb_hnsw_test"

static void cleanup_hnsw(const char* path)
{
    char buf[1024];
    snprintf(buf, sizeof(buf), "%s/meta", path);    unlink(buf);
    snprintf(buf, sizeof(buf), "%s/vectors", path);  unlink(buf);
    snprintf(buf, sizeof(buf), "%s/ids", path);      unlink(buf);
    snprintf(buf, sizeof(buf), "%s/wal", path);      unlink(buf);
    rmdir(path);
}

static int test_hnsw_persistence(void)
{
    cleanup_hnsw(HNSW_TEST_PATH);
    mkdir(HNSW_TEST_PATH, 0755);

    kanbudb_vec_params_t params = KANBUDB_VEC_PARAMS_DEFAULT;
    params.algo   = KANBUDB_VEC_ALGO_HNSW;
    params.dimension = 3;
    params.M      = 8;
    params.ef_construction = 100;
    params.ef_search = 50;
    params.initial_capacity = 100;
    params.enable_persistence = 1;

    kanbudb_vec_index_t* idx = NULL;
    ASSERT(kanbudb_vec_create(HNSW_TEST_PATH, &params, &idx) == KANBUDB_VEC_OK, "create");

    float v1[] = {1.0f, 0.0f, 0.0f};
    float v2[] = {0.0f, 1.0f, 0.0f};
    float v3[] = {0.0f, 0.0f, 1.0f};
    ASSERT(kanbudb_vec_insert(idx, 1, v1) == KANBUDB_VEC_OK, "insert 1");
    ASSERT(kanbudb_vec_insert(idx, 2, v2) == KANBUDB_VEC_OK, "insert 2");
    ASSERT(kanbudb_vec_insert(idx, 3, v3) == KANBUDB_VEC_OK, "insert 3");

    ASSERT(kanbudb_vec_flush(idx) == KANBUDB_VEC_OK, "flush");
    ASSERT(kanbudb_vec_close(idx) == KANBUDB_VEC_OK, "close");

    /* Reopen and verify */
    kanbudb_vec_index_t* idx2 = NULL;
    ASSERT(kanbudb_vec_open(HNSW_TEST_PATH, &idx2) == KANBUDB_VEC_OK, "open");
    ASSERT(kanbudb_vec_count(idx2) == 3, "count=3");

    float query[] = {1.0f, 0.1f, 0.0f};
    kanbudb_vec_result_t results[3];
    int n = kanbudb_vec_search(idx2, query, 3, results);
    ASSERT(n == 3, "search 3");
    ASSERT(results[0].id == 1, "closest id=1");

    /* Delete and persist */
    ASSERT(kanbudb_vec_delete(idx2, 2) == KANBUDB_VEC_OK, "delete 2");
    ASSERT(kanbudb_vec_flush(idx2) == KANBUDB_VEC_OK, "flush2");
    ASSERT(kanbudb_vec_close(idx2) == KANBUDB_VEC_OK, "close2");

    /* Reopen: should have 2 items (id=1, id=3) */
    kanbudb_vec_index_t* idx3 = NULL;
    ASSERT(kanbudb_vec_open(HNSW_TEST_PATH, &idx3) == KANBUDB_VEC_OK, "open3");
    ASSERT(kanbudb_vec_count(idx3) == 2, "count=2 after delete");
    ASSERT(kanbudb_vec_get(idx3, 1, query) == KANBUDB_VEC_OK, "id1 exists");
    ASSERT(kanbudb_vec_get(idx3, 2, query) == KANBUDB_VEC_ERR_NOTFOUND, "id2 gone");
    ASSERT(kanbudb_vec_get(idx3, 3, query) == KANBUDB_VEC_OK, "id3 exists");

    kanbudb_vec_close(idx3);
    kanbudb_vec_destroy(HNSW_TEST_PATH);
    return 1;
}

int main(void)
{
    srand(42);
    printf("hnsw tests:\n");
    TEST(create_destroy);
    TEST(insert_search_one);
    TEST(insert_search_multi);
    TEST(search_topk);
    TEST(delete);
    TEST(get);
    TEST(count);
    TEST(empty_index);
    TEST(update);
    TEST(cosine_metric);
    TEST(ip_metric);
    TEST(many_inserts);
    TEST(hnsw_persistence);
    printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
