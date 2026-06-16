#include "hybrid.h"
#include "vector.h"
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

/* ── RRF Mode Tests ─────────────────────────────────────────── */

static int test_rrf_basic_merge(void) {
    kanbudb_vec_result_t vr[] = {
        { .id = 1, .distance = 0.1f },
        { .id = 2, .distance = 0.2f },
        { .id = 3, .distance = 0.3f }
    };
    uint64_t fts_ids[] = { 4, 5, 6 };
    double fts_scores[] = { 0.9, 0.8, 0.7 };
    kanbudb_hybrid_params_t p = KANBUDB_HYBRID_PARAMS_DEFAULT;
    kanbudb_hybrid_result_t r[10];
    int n = kanbudb_hybrid_search(vr, 3, fts_ids, fts_scores, 3, &p, r, 10);
    ASSERT(n == 6, "6 unique results");
    /* First result should be from vector (rank 1) or FTS (rank 1) */
    ASSERT(r[0].id >= 1 && r[0].id <= 6, "valid id");
    return 1;
}

static int test_rrf_overlap_boost(void) {
    /* ID 2 and 3 appear in both lists → should have higher scores than single-source */
    kanbudb_vec_result_t vr[] = {
        { .id = 1, .distance = 0.1f },
        { .id = 2, .distance = 0.2f },
        { .id = 3, .distance = 0.3f }
    };
    uint64_t fts_ids[] = { 2, 4, 3 };
    double fts_scores[] = { 0.9, 0.8, 0.7 };
    kanbudb_hybrid_params_t p = KANBUDB_HYBRID_PARAMS_DEFAULT;
    kanbudb_hybrid_result_t r[10];
    int n = kanbudb_hybrid_search(vr, 3, fts_ids, fts_scores, 3, &p, r, 10);
    ASSERT(n == 4, "4 unique results");

    /* Find scores for overlapping vs single-source IDs */
    double score_overlap = 0, score_single = 0;
    for (int i = 0; i < n; i++) {
        if (r[i].id == 2 || r[i].id == 3) score_overlap = r[i].score;
        if (r[i].id == 1 || r[i].id == 4) score_single = r[i].score;
    }
    ASSERT(score_overlap > score_single, "overlapping IDs score higher");
    return 1;
}

static int test_rrf_empty_vec(void) {
    uint64_t fts_ids[] = { 1, 2 };
    double fts_scores[] = { 0.9, 0.8 };
    kanbudb_hybrid_params_t p = KANBUDB_HYBRID_PARAMS_DEFAULT;
    kanbudb_hybrid_result_t r[10];
    int n = kanbudb_hybrid_search(NULL, 0, fts_ids, fts_scores, 2, &p, r, 10);
    ASSERT(n == 2, "FTS only");
    ASSERT(r[0].id == 1, "FTS rank 1");
    return 1;
}

static int test_rrf_empty_fts(void) {
    kanbudb_vec_result_t vr[] = {
        { .id = 10, .distance = 0.5f },
        { .id = 20, .distance = 0.8f }
    };
    kanbudb_hybrid_params_t p = KANBUDB_HYBRID_PARAMS_DEFAULT;
    kanbudb_hybrid_result_t r[10];
    int n = kanbudb_hybrid_search(vr, 2, NULL, 0, 0, &p, r, 10);
    ASSERT(n == 2, "Vec only");
    ASSERT(r[0].id == 10, "closest first");
    return 1;
}

static int test_rrf_both_empty(void) {
    kanbudb_hybrid_params_t p = KANBUDB_HYBRID_PARAMS_DEFAULT;
    kanbudb_hybrid_result_t r[10];
    int n = kanbudb_hybrid_search(NULL, 0, NULL, 0, 0, &p, r, 10);
    ASSERT(n == 0, "both empty");
    return 1;
}

static int test_rrf_max_results_limit(void) {
    kanbudb_vec_result_t vr[5];
    for (int i = 0; i < 5; i++) {
        vr[i].id = (uint64_t)(i + 1);
        vr[i].distance = (float)i * 0.1f;
    }
    kanbudb_hybrid_params_t p = KANBUDB_HYBRID_PARAMS_DEFAULT;
    kanbudb_hybrid_result_t r[3];
    int n = kanbudb_hybrid_search(vr, 5, NULL, 0, 0, &p, r, 3);
    ASSERT(n == 3, "limited to 3");
    return 1;
}

static int test_rrf_preserves_scores(void) {
    kanbudb_vec_result_t vr[] = { { .id = 1, .distance = 0.5f } };
    kanbudb_hybrid_params_t p = KANBUDB_HYBRID_PARAMS_DEFAULT;
    kanbudb_hybrid_result_t r[10];
    kanbudb_hybrid_search(vr, 1, NULL, 0, 0, &p, r, 10);
    ASSERT(r[0].score > 0.0, "score > 0");
    ASSERT(r[0].vec_distance == 0.5f, "vec_distance preserved");
    return 1;
}

/* ── Weighted Mode Tests ────────────────────────────────────── */

static int test_weighted_mode(void) {
    kanbudb_vec_result_t vr[] = {
        { .id = 1, .distance = 0.1f },
        { .id = 2, .distance = 0.2f }
    };
    uint64_t fts_ids[] = { 1, 3 };
    double fts_scores[] = { 0.9, 0.7 };
    kanbudb_hybrid_params_t p = KANBUDB_HYBRID_PARAMS_DEFAULT;
    p.mode = KANBUDB_HYBRID_WEIGHTED;
    p.vec_weight = 0.7;
    p.fts_weight = 0.3;
    kanbudb_hybrid_result_t r[10];
    int n = kanbudb_hybrid_search(vr, 2, fts_ids, fts_scores, 2, &p, r, 10);
    ASSERT(n == 3, "3 unique");
    /* ID 1 has both vec and FTS → highest score */
    ASSERT(r[0].id == 1, "ID 1 ranked first");
    return 1;
}

static int test_weighted_vec_dominant(void) {
    kanbudb_vec_result_t vr[] = {
        { .id = 1, .distance = 0.01f },
        { .id = 2, .distance = 0.99f }
    };
    kanbudb_hybrid_params_t p = KANBUDB_HYBRID_PARAMS_DEFAULT;
    p.mode = KANBUDB_HYBRID_WEIGHTED;
    p.vec_weight = 1.0;
    p.fts_weight = 0.0;
    kanbudb_hybrid_result_t r[10];
    int n = kanbudb_hybrid_search(vr, 2, NULL, 0, 0, &p, r, 10);
    ASSERT(n == 2, "2 results");
    ASSERT(r[0].id == 1, "closest first when vec_weight=1");
    return 1;
}

static int test_weighted_fts_dominant(void) {
    kanbudb_vec_result_t vr[] = {
        { .id = 1, .distance = 0.01f }
    };
    uint64_t fts_ids[] = { 2 };
    double fts_scores[] = { 0.99 };
    kanbudb_hybrid_params_t p = KANBUDB_HYBRID_PARAMS_DEFAULT;
    p.mode = KANBUDB_HYBRID_WEIGHTED;
    p.vec_weight = 0.0;
    p.fts_weight = 1.0;
    kanbudb_hybrid_result_t r[10];
    int n = kanbudb_hybrid_search(vr, 1, fts_ids, fts_scores, 1, &p, r, 10);
    ASSERT(n == 2, "2 results");
    ASSERT(r[0].id == 2, "FTS-only ranked first when fts_weight=1");
    return 1;
}

/* ── Edge Cases ─────────────────────────────────────────────── */

static int test_large_result_set(void) {
    kanbudb_vec_result_t vr[100];
    for (int i = 0; i < 100; i++) {
        vr[i].id = (uint64_t)i;
        vr[i].distance = (float)i * 0.01f;
    }
    uint64_t fts_ids[100];
    double fts_scores[100];
    for (int i = 0; i < 100; i++) {
        fts_ids[i] = (uint64_t)(i + 50); /* overlap in 50..99 */
        fts_scores[i] = 1.0 - (double)i * 0.01;
    }
    kanbudb_hybrid_params_t p = KANBUDB_HYBRID_PARAMS_DEFAULT;
    kanbudb_hybrid_result_t r[200];
    int n = kanbudb_hybrid_search(vr, 100, fts_ids, fts_scores, 100, &p, r, 200);
    ASSERT(n == 150, "150 unique (0..149)");
    return 1;
}

static int test_single_vec_single_fts(void) {
    kanbudb_vec_result_t vr[] = { { .id = 1, .distance = 0.5f } };
    uint64_t fts_ids[] = { 2 };
    double fts_scores[] = { 0.8 };
    kanbudb_hybrid_params_t p = KANBUDB_HYBRID_PARAMS_DEFAULT;
    kanbudb_hybrid_result_t r[10];
    int n = kanbudb_hybrid_search(vr, 1, fts_ids, fts_scores, 1, &p, r, 10);
    ASSERT(n == 2, "2 results");
    return 1;
}

static int test_same_ids_both_sources(void) {
    /* All IDs overlap */
    kanbudb_vec_result_t vr[] = {
        { .id = 1, .distance = 0.1f },
        { .id = 2, .distance = 0.2f }
    };
    uint64_t fts_ids[] = { 1, 2 };
    double fts_scores[] = { 0.9, 0.8 };
    kanbudb_hybrid_params_t p = KANBUDB_HYBRID_PARAMS_DEFAULT;
    kanbudb_hybrid_result_t r[10];
    int n = kanbudb_hybrid_search(vr, 2, fts_ids, fts_scores, 2, &p, r, 10);
    ASSERT(n == 2, "2 unique");
    /* Both have dual scores → highest combined */
    ASSERT(r[0].score > r[1].score, "sorted by score desc");
    return 1;
}

static int test_duplicate_fts_ids(void) {
    kanbudb_vec_result_t vr[] = { { .id = 1, .distance = 0.1f } };
    uint64_t fts_ids[] = { 2, 2, 3 };
    double fts_scores[] = { 0.9, 0.9, 0.7 };
    kanbudb_hybrid_params_t p = KANBUDB_HYBRID_PARAMS_DEFAULT;
    kanbudb_hybrid_result_t r[10];
    int n = kanbudb_hybrid_search(vr, 1, fts_ids, fts_scores, 3, &p, r, 10);
    ASSERT(n == 3, "3 unique (dedup)");
    return 1;
}

static int test_score_monotonic(void) {
    kanbudb_vec_result_t vr[10];
    for (int i = 0; i < 10; i++) {
        vr[i].id = (uint64_t)(i + 1);
        vr[i].distance = (float)i * 0.1f;
    }
    kanbudb_hybrid_params_t p = KANBUDB_HYBRID_PARAMS_DEFAULT;
    kanbudb_hybrid_result_t r[10];
    int n = kanbudb_hybrid_search(vr, 10, NULL, 0, 0, &p, r, 10);
    ASSERT(n == 10, "10 results");
    for (int i = 0; i < n - 1; i++) {
        ASSERT(r[i].score >= r[i+1].score, "monotonic decreasing");
    }
    return 1;
}

int main(void) {
    printf("=== hybrid tests ===\n");
    TEST(rrf_basic_merge);
    TEST(rrf_overlap_boost);
    TEST(rrf_empty_vec);
    TEST(rrf_empty_fts);
    TEST(rrf_both_empty);
    TEST(rrf_max_results_limit);
    TEST(rrf_preserves_scores);
    TEST(weighted_mode);
    TEST(weighted_vec_dominant);
    TEST(weighted_fts_dominant);
    TEST(large_result_set);
    TEST(single_vec_single_fts);
    TEST(same_ids_both_sources);
    TEST(duplicate_fts_ids);
    TEST(score_monotonic);
    printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
