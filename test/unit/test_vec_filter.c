#include "vector.h"
#include "vec_filter.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

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

/* ── Filter Functions ───────────────────────────────────────── */

static int filter_even(uint64_t id, void* ctx) {
    (void)ctx;
    return (id % 2 == 0);
}

static int filter_odd(uint64_t id, void* ctx) {
    (void)ctx;
    return (id % 2 != 0);
}

static int filter_range_ctx;
static int filter_gt_ctx(uint64_t id, void* ctx) {
    int threshold = *(int*)ctx;
    return ((int)id > threshold);
}

static int filter_always_true(uint64_t id, void* ctx) {
    (void)id; (void)ctx;
    return 1;
}

static int filter_always_false(uint64_t id, void* ctx) {
    (void)id; (void)ctx;
    return 0;
}

/* ── Flat Index Tests ───────────────────────────────────────── */

static int test_flat_no_filter(void) {
    kanbudb_vec_params_t p = {
        .metric = KANBUDB_VEC_METRIC_L2, .dimension = 2,
        .initial_capacity = 100
    };
    kanbudb_vec_index_t* idx;
    kanbudb_vec_create(NULL, &p, &idx);

    float v[2];
    for (uint64_t i = 1; i <= 10; i++) {
        v[0] = (float)i; v[1] = 0.0f;
        kanbudb_vec_insert(idx, i, v);
    }

    float q[] = {5.0f, 0.0f};
    kanbudb_vec_result_t r[10];
    int n = kanbudb_vec_search_filtered(idx, q, 10, NULL, NULL, r);
    ASSERT(n == 10, "no filter → all results");
    ASSERT(r[0].id == 5, "closest is id=5");
    kanbudb_vec_close(idx);
    return 1;
}

static int test_flat_filter_even(void) {
    kanbudb_vec_params_t p = {
        .metric = KANBUDB_VEC_METRIC_L2, .dimension = 2,
        .initial_capacity = 100
    };
    kanbudb_vec_index_t* idx;
    kanbudb_vec_create(NULL, &p, &idx);

    float v[2];
    for (uint64_t i = 1; i <= 10; i++) {
        v[0] = (float)i; v[1] = 0.0f;
        kanbudb_vec_insert(idx, i, v);
    }

    float q[] = {5.0f, 0.0f};
    kanbudb_vec_result_t r[10];
    int n = kanbudb_vec_search_filtered(idx, q, 10, filter_even, NULL, r);
    ASSERT(n == 5, "5 even ids");
    for (int i = 0; i < n; i++) {
        ASSERT(r[i].id % 2 == 0, "all even");
    }
    /* Results should be sorted by distance - closest even to 5 is 4 or 6 */
    int first_is_close = (r[0].id == 4 || r[0].id == 6);
    ASSERT(first_is_close, "closest even is 4 or 6");
    kanbudb_vec_close(idx);
    return 1;
}

static int test_flat_filter_odd(void) {
    kanbudb_vec_params_t p = {
        .metric = KANBUDB_VEC_METRIC_L2, .dimension = 2,
        .initial_capacity = 100
    };
    kanbudb_vec_index_t* idx;
    kanbudb_vec_create(NULL, &p, &idx);

    float v[2];
    for (uint64_t i = 1; i <= 10; i++) {
        v[0] = (float)i; v[1] = 0.0f;
        kanbudb_vec_insert(idx, i, v);
    }

    float q[] = {5.0f, 0.0f};
    kanbudb_vec_result_t r[10];
    int n = kanbudb_vec_search_filtered(idx, q, 10, filter_odd, NULL, r);
    ASSERT(n == 5, "5 odd ids");
    for (int i = 0; i < n; i++) {
        ASSERT(r[i].id % 2 != 0, "all odd");
    }
    ASSERT(r[0].id == 5, "closest odd is 5");
    kanbudb_vec_close(idx);
    return 1;
}

static int test_flat_filter_with_context(void) {
    kanbudb_vec_params_t p = {
        .metric = KANBUDB_VEC_METRIC_L2, .dimension = 2,
        .initial_capacity = 100
    };
    kanbudb_vec_index_t* idx;
    kanbudb_vec_create(NULL, &p, &idx);

    float v[2];
    for (uint64_t i = 1; i <= 10; i++) {
        v[0] = (float)i; v[1] = 0.0f;
        kanbudb_vec_insert(idx, i, v);
    }

    float q[] = {5.0f, 0.0f};
    kanbudb_vec_result_t r[10];
    int threshold = 5;
    int n = kanbudb_vec_search_filtered(idx, q, 10, filter_gt_ctx, &threshold, r);
    ASSERT(n == 5, "5 ids > 5");
    for (int i = 0; i < n; i++) {
        ASSERT((int)r[i].id > 5, "all > 5");
    }
    kanbudb_vec_close(idx);
    return 1;
}

static int test_flat_filter_none_match(void) {
    kanbudb_vec_params_t p = {
        .metric = KANBUDB_VEC_METRIC_L2, .dimension = 2,
        .initial_capacity = 100
    };
    kanbudb_vec_index_t* idx;
    kanbudb_vec_create(NULL, &p, &idx);

    float v[2] = {1.0f, 0.0f};
    kanbudb_vec_insert(idx, 1, v);

    float q[] = {1.0f, 0.0f};
    kanbudb_vec_result_t r[1];
    int n = kanbudb_vec_search_filtered(idx, q, 1, filter_always_false, NULL, r);
    ASSERT(n == 0, "no matches");
    kanbudb_vec_close(idx);
    return 1;
}

static int test_flat_filter_all_match(void) {
    kanbudb_vec_params_t p = {
        .metric = KANBUDB_VEC_METRIC_L2, .dimension = 2,
        .initial_capacity = 100
    };
    kanbudb_vec_index_t* idx;
    kanbudb_vec_create(NULL, &p, &idx);

    float v[2];
    for (uint64_t i = 1; i <= 5; i++) {
        v[0] = (float)i; v[1] = 0.0f;
        kanbudb_vec_insert(idx, i, v);
    }

    float q[] = {3.0f, 0.0f};
    kanbudb_vec_result_t r[5];
    int n = kanbudb_vec_search_filtered(idx, q, 5, filter_always_true, NULL, r);
    ASSERT(n == 5, "all match");
    kanbudb_vec_close(idx);
    return 1;
}

static int test_flat_filter_k_smaller_than_matches(void) {
    kanbudb_vec_params_t p = {
        .metric = KANBUDB_VEC_METRIC_L2, .dimension = 2,
        .initial_capacity = 100
    };
    kanbudb_vec_index_t* idx;
    kanbudb_vec_create(NULL, &p, &idx);

    float v[2];
    for (uint64_t i = 1; i <= 10; i++) {
        v[0] = (float)i; v[1] = 0.0f;
        kanbudb_vec_insert(idx, i, v);
    }

    float q[] = {5.0f, 0.0f};
    kanbudb_vec_result_t r[3];
    int n = kanbudb_vec_search_filtered(idx, q, 3, filter_even, NULL, r);
    ASSERT(n == 3, "limited to k=3");
    kanbudb_vec_close(idx);
    return 1;
}

static int test_flat_filter_ordering(void) {
    kanbudb_vec_params_t p = {
        .metric = KANBUDB_VEC_METRIC_L2, .dimension = 1,
        .initial_capacity = 100
    };
    kanbudb_vec_index_t* idx;
    kanbudb_vec_create(NULL, &p, &idx);

    /* Insert at positions 1..10 */
    float v[1];
    for (uint64_t i = 1; i <= 10; i++) {
        v[0] = (float)i * 10.0f;
        kanbudb_vec_insert(idx, i, v);
    }

    float q[] = {55.0f}; /* closest to id=6 (dist=1) */
    kanbudb_vec_result_t r[10];
    int n = kanbudb_vec_search_filtered(idx, q, 10, filter_even, NULL, r);
    ASSERT(n == 5, "5 even");
    /* Should be sorted by distance: 6, 4, 8, 2, 10 */
    ASSERT(r[0].id == 6, "closest even is 6");
    ASSERT(r[1].id == 4, "next closest even is 4");
    kanbudb_vec_close(idx);
    return 1;
}

static int test_flat_filter_delete_interacts(void) {
    kanbudb_vec_params_t p = {
        .metric = KANBUDB_VEC_METRIC_L2, .dimension = 2,
        .initial_capacity = 100
    };
    kanbudb_vec_index_t* idx;
    kanbudb_vec_create(NULL, &p, &idx);

    float v[2];
    for (uint64_t i = 1; i <= 6; i++) {
        v[0] = (float)i; v[1] = 0.0f;
        kanbudb_vec_insert(idx, i, v);
    }

    /* Delete even IDs 2, 4, 6 */
    kanbudb_vec_delete(idx, 2);
    kanbudb_vec_delete(idx, 4);
    kanbudb_vec_delete(idx, 6);

    float q[] = {3.5f, 0.0f};
    kanbudb_vec_result_t r[10];
    int n = kanbudb_vec_search_filtered(idx, q, 10, filter_even, NULL, r);
    ASSERT(n == 0, "all even deleted");
    kanbudb_vec_close(idx);
    return 1;
}

/* ── HNSW Index Tests ───────────────────────────────────────── */

static int test_hnsw_no_filter(void) {
    kanbudb_vec_params_t p = {
        .metric = KANBUDB_VEC_METRIC_L2, .dimension = 4,
        .algo = KANBUDB_VEC_ALGO_HNSW,
        .M = 8, .ef_construction = 50, .ef_search = 20,
        .initial_capacity = 100
    };
    kanbudb_vec_index_t* idx;
    kanbudb_vec_create(NULL, &p, &idx);

    float v[4];
    for (uint64_t i = 1; i <= 20; i++) {
        v[0] = (float)i * 0.1f;
        v[1] = (float)i * 0.05f;
        v[2] = 0.0f;
        v[3] = 0.0f;
        kanbudb_vec_insert(idx, i, v);
    }

    float q[] = {1.0f, 0.5f, 0.0f, 0.0f};
    kanbudb_vec_result_t r[20];
    int n = kanbudb_vec_search_filtered(idx, q, 20, NULL, NULL, r);
    ASSERT(n == 20, "no filter → all results");
    kanbudb_vec_close(idx);
    return 1;
}

static int test_hnsw_filter_even(void) {
    kanbudb_vec_params_t p = {
        .metric = KANBUDB_VEC_METRIC_L2, .dimension = 4,
        .algo = KANBUDB_VEC_ALGO_HNSW,
        .M = 8, .ef_construction = 50, .ef_search = 20,
        .initial_capacity = 100
    };
    kanbudb_vec_index_t* idx;
    kanbudb_vec_create(NULL, &p, &idx);

    float v[4];
    for (uint64_t i = 1; i <= 20; i++) {
        v[0] = (float)i * 0.1f;
        v[1] = (float)i * 0.05f;
        v[2] = 0.0f;
        v[3] = 0.0f;
        kanbudb_vec_insert(idx, i, v);
    }

    float q[] = {1.0f, 0.5f, 0.0f, 0.0f};
    kanbudb_vec_result_t r[20];
    int n = kanbudb_vec_search_filtered(idx, q, 10, filter_even, NULL, r);
    ASSERT(n > 0, "some results");
    for (int i = 0; i < n; i++) {
        ASSERT(r[i].id % 2 == 0, "all even");
    }
    kanbudb_vec_close(idx);
    return 1;
}

static int test_hnsw_filter_with_context(void) {
    kanbudb_vec_params_t p = {
        .metric = KANBUDB_VEC_METRIC_L2, .dimension = 4,
        .algo = KANBUDB_VEC_ALGO_HNSW,
        .M = 8, .ef_construction = 50, .ef_search = 20,
        .initial_capacity = 100
    };
    kanbudb_vec_index_t* idx;
    kanbudb_vec_create(NULL, &p, &idx);

    float v[4];
    for (uint64_t i = 1; i <= 20; i++) {
        v[0] = (float)i * 0.1f;
        v[1] = (float)i * 0.05f;
        v[2] = 0.0f;
        v[3] = 0.0f;
        kanbudb_vec_insert(idx, i, v);
    }

    float q[] = {1.0f, 0.5f, 0.0f, 0.0f};
    kanbudb_vec_result_t r[20];
    int threshold = 10;
    int n = kanbudb_vec_search_filtered(idx, q, 10, filter_gt_ctx, &threshold, r);
    ASSERT(n > 0, "some results");
    for (int i = 0; i < n; i++) {
        ASSERT((int)r[i].id > 10, "all > 10");
    }
    kanbudb_vec_close(idx);
    return 1;
}

static int test_hnsw_filter_none_match(void) {
    kanbudb_vec_params_t p = {
        .metric = KANBUDB_VEC_METRIC_L2, .dimension = 4,
        .algo = KANBUDB_VEC_ALGO_HNSW,
        .M = 8, .ef_construction = 50, .ef_search = 20,
        .initial_capacity = 100
    };
    kanbudb_vec_index_t* idx;
    kanbudb_vec_create(NULL, &p, &idx);

    float v[4] = {1.0f, 0.0f, 0.0f, 0.0f};
    kanbudb_vec_insert(idx, 1, v);

    float q[] = {1.0f, 0.0f, 0.0f, 0.0f};
    kanbudb_vec_result_t r[1];
    int n = kanbudb_vec_search_filtered(idx, q, 1, filter_always_false, NULL, r);
    ASSERT(n == 0, "no matches");
    kanbudb_vec_close(idx);
    return 1;
}

static int test_hnsw_filter_ordering(void) {
    kanbudb_vec_params_t p = {
        .metric = KANBUDB_VEC_METRIC_L2, .dimension = 2,
        .algo = KANBUDB_VEC_ALGO_HNSW,
        .M = 8, .ef_construction = 50, .ef_search = 20,
        .initial_capacity = 100
    };
    kanbudb_vec_index_t* idx;
    kanbudb_vec_create(NULL, &p, &idx);

    float v[2];
    for (uint64_t i = 1; i <= 10; i++) {
        v[0] = (float)i * 10.0f;
        v[1] = 0.0f;
        kanbudb_vec_insert(idx, i, v);
    }

    float q[] = {55.0f, 0.0f};
    kanbudb_vec_result_t r[10];
    int n = kanbudb_vec_search_filtered(idx, q, 10, filter_even, NULL, r);
    ASSERT(n > 0, "some results");
    /* Results should be roughly ordered by distance */
    for (int i = 0; i < n - 1; i++) {
        ASSERT(r[i].distance <= r[i+1].distance + 0.01f, "roughly ordered");
    }
    kanbudb_vec_close(idx);
    return 1;
}

/* ── Edge Cases ─────────────────────────────────────────────── */

static int test_empty_index_filtered(void) {
    kanbudb_vec_params_t p = {
        .metric = KANBUDB_VEC_METRIC_L2, .dimension = 2,
        .initial_capacity = 100
    };
    kanbudb_vec_index_t* idx;
    kanbudb_vec_create(NULL, &p, &idx);

    float q[] = {0.0f, 0.0f};
    kanbudb_vec_result_t r[1];
    int n = kanbudb_vec_search_filtered(idx, q, 1, filter_always_true, NULL, r);
    ASSERT(n == 0, "empty index");
    kanbudb_vec_close(idx);
    return 1;
}

static int test_single_element_filtered(void) {
    kanbudb_vec_params_t p = {
        .metric = KANBUDB_VEC_METRIC_L2, .dimension = 2,
        .initial_capacity = 100
    };
    kanbudb_vec_index_t* idx;
    kanbudb_vec_create(NULL, &p, &idx);

    float v[] = {1.0f, 0.0f};
    kanbudb_vec_insert(idx, 1, v);

    float q[] = {1.0f, 0.0f};
    kanbudb_vec_result_t r[1];

    int n = kanbudb_vec_search_filtered(idx, q, 1, filter_even, NULL, r);
    ASSERT(n == 0, "id=1 is odd, filtered out");

    n = kanbudb_vec_search_filtered(idx, q, 1, filter_odd, NULL, r);
    ASSERT(n == 1, "id=1 is odd, matches");
    ASSERT(r[0].id == 1, "id=1");
    kanbudb_vec_close(idx);
    return 1;
}

static int test_high_dimensional_filtered(void) {
    kanbudb_vec_params_t p = {
        .metric = KANBUDB_VEC_METRIC_L2, .dimension = 128,
        .initial_capacity = 100
    };
    kanbudb_vec_index_t* idx;
    kanbudb_vec_create(NULL, &p, &idx);

    float v[128];
    for (uint64_t i = 1; i <= 20; i++) {
        for (int d = 0; d < 128; d++) v[d] = (float)(i * 100 + d);
        kanbudb_vec_insert(idx, i, v);
    }

    float q[128];
    for (int d = 0; d < 128; d++) q[d] = 500.0f;
    kanbudb_vec_result_t r[20];
    int n = kanbudb_vec_search_filtered(idx, q, 10, filter_even, NULL, r);
    ASSERT(n > 0, "some results");
    for (int i = 0; i < n; i++) {
        ASSERT(r[i].id % 2 == 0, "all even");
    }
    kanbudb_vec_close(idx);
    return 1;
}

int main(void) {
    printf("=== vec_filter tests ===\n");
    printf("  -- Flat --\n");
    TEST(flat_no_filter);
    TEST(flat_filter_even);
    TEST(flat_filter_odd);
    TEST(flat_filter_with_context);
    TEST(flat_filter_none_match);
    TEST(flat_filter_all_match);
    TEST(flat_filter_k_smaller_than_matches);
    TEST(flat_filter_ordering);
    TEST(flat_filter_delete_interacts);
    printf("  -- HNSW --\n");
    TEST(hnsw_no_filter);
    TEST(hnsw_filter_even);
    TEST(hnsw_filter_with_context);
    TEST(hnsw_filter_none_match);
    TEST(hnsw_filter_ordering);
    printf("  -- Edge Cases --\n");
    TEST(empty_index_filtered);
    TEST(single_element_filtered);
    TEST(high_dimensional_filtered);
    printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
