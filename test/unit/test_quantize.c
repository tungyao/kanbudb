#include "vector.h"
#include "quantize.h"
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

/* ── SQ8 Tests ──────────────────────────────────────────────── */

static int test_sq8_create_destroy(void) {
    kanbudb_quant_params_t p = { .type = KANBUDB_QUANT_SQ8, .dimension = 8 };
    kanbudb_quantizer_t* q = NULL;
    ASSERT(kanbudb_quant_create(&p, &q) == 0, "create");
    ASSERT(q != NULL, "not null");
    kanbudb_quant_destroy(q);
    return 1;
}

static int test_sq8_invalid_params(void) {
    kanbudb_quantizer_t* q = NULL;
    ASSERT(kanbudb_quant_create(NULL, &q) == -1, "null params");
    kanbudb_quant_params_t p = { .type = KANBUDB_QUANT_SQ8, .dimension = 0 };
    ASSERT(kanbudb_quant_create(&p, &q) == -1, "zero dim");
    return 1;
}

static int test_sq8_train_single_vector(void) {
    kanbudb_quant_params_t p = { .type = KANBUDB_QUANT_SQ8, .dimension = 4 };
    kanbudb_quantizer_t* q = NULL;
    kanbudb_quant_create(&p, &q);
    float v[] = {1.0f, 2.0f, 3.0f, 4.0f};
    ASSERT(kanbudb_quant_train(q, v, 1) == 0, "train 1 vec");
    kanbudb_quant_destroy(q);
    return 1;
}

static int test_sq8_encode_decode_roundtrip(void) {
    kanbudb_quant_params_t p = { .type = KANBUDB_QUANT_SQ8, .dimension = 4 };
    kanbudb_quantizer_t* q = NULL;
    kanbudb_quant_create(&p, &q);

    float train[] = {
        0.0f, 0.0f, 0.0f, 0.0f,
        10.0f, 10.0f, 10.0f, 10.0f,
        5.0f, 5.0f, 5.0f, 5.0f
    };
    kanbudb_quant_train(q, train, 3);

    float v[] = {3.0f, 7.0f, 2.0f, 9.0f};
    uint8_t code[4];
    size_t code_len;
    ASSERT(kanbudb_quant_encode(q, v, code, &code_len) == 0, "encode");
    ASSERT(code_len == 4, "code_len=dim");

    float decoded[4];
    ASSERT(kanbudb_quant_decode(q, code, code_len, decoded) == 0, "decode");

    /* SQ8 quantization has ~0.5% error per dimension */
    for (int i = 0; i < 4; i++) {
        float err = fabsf(decoded[i] - v[i]);
        float range = 10.0f;
        ASSERT(err < range / 255.0f + 0.01f, "approximate reconstruction");
    }

    kanbudb_quant_destroy(q);
    return 1;
}

static int test_sq8_code_size(void) {
    kanbudb_quant_params_t p = { .type = KANBUDB_QUANT_SQ8, .dimension = 128 };
    kanbudb_quantizer_t* q = NULL;
    kanbudb_quant_create(&p, &q);
    ASSERT(kanbudb_quant_code_size(q) == 128, "code_size=dim");
    kanbudb_quant_destroy(q);
    return 1;
}

static int test_sq8_distance_symmetry(void) {
    kanbudb_quant_params_t p = { .type = KANBUDB_QUANT_SQ8, .dimension = 3 };
    kanbudb_quantizer_t* q = NULL;
    kanbudb_quant_create(&p, &q);

    float train[] = {0,0,0, 5,5,5, 10,10,10};
    kanbudb_quant_train(q, train, 3);

    float a[] = {2.0f, 3.0f, 4.0f};
    float b[] = {7.0f, 8.0f, 9.0f};
    uint8_t ca[3], cb[3];
    kanbudb_quant_encode(q, a, ca, NULL);
    kanbudb_quant_encode(q, b, cb, NULL);

    float d_ab = kanbudb_quant_distance(q, ca, 3, cb, 3);
    float d_ba = kanbudb_quant_distance(q, cb, 3, ca, 3);
    ASSERT(fabsf(d_ab - d_ba) < 1e-5f, "symmetric distance");

    kanbudb_quant_destroy(q);
    return 1;
}

static int test_sq8_distance_self_zero(void) {
    kanbudb_quant_params_t p = { .type = KANBUDB_QUANT_SQ8, .dimension = 4 };
    kanbudb_quantizer_t* q = NULL;
    kanbudb_quant_create(&p, &q);

    float train[] = {0,0,0,0, 10,10,10,10};
    kanbudb_quant_train(q, train, 2);

    float v[] = {5.0f, 5.0f, 5.0f, 5.0f};
    uint8_t code[4];
    kanbudb_quant_encode(q, v, code, NULL);

    float d = kanbudb_quant_distance(q, code, 4, code, 4);
    ASSERT(d == 0.0f, "self-distance=0");

    kanbudb_quant_destroy(q);
    return 1;
}

static int test_sq8_batch_encode(void) {
    kanbudb_quant_params_t p = { .type = KANBUDB_QUANT_SQ8, .dimension = 2 };
    kanbudb_quantizer_t* q = NULL;
    kanbudb_quant_create(&p, &q);

    float train[] = {0,0, 5,5, 10,10};
    kanbudb_quant_train(q, train, 3);

    float vectors[][2] = {{1,2}, {3,4}, {8,9}};
    for (int i = 0; i < 3; i++) {
        uint8_t code[2];
        ASSERT(kanbudb_quant_encode(q, vectors[i], code, NULL) == 0, "encode");
        float decoded[2];
        ASSERT(kanbudb_quant_decode(q, code, 2, decoded) == 0, "decode");
    }

    kanbudb_quant_destroy(q);
    return 1;
}

static int test_sq8_high_dimensional(void) {
    kanbudb_quant_params_t p = { .type = KANBUDB_QUANT_SQ8, .dimension = 256 };
    kanbudb_quantizer_t* q = NULL;
    kanbudb_quant_create(&p, &q);

    /* Train with 10 random vectors */
    float train[2560];
    srand(42);
    for (int i = 0; i < 2560; i++) train[i] = (float)rand() / (float)RAND_MAX * 10.0f;
    ASSERT(kanbudb_quant_train(q, train, 10) == 0, "train 256d");

    float v[256];
    for (int i = 0; i < 256; i++) v[i] = 0.5f;
    uint8_t code[256];
    ASSERT(kanbudb_quant_encode(q, v, code, NULL) == 0, "encode 256d");

    float decoded[256];
    ASSERT(kanbudb_quant_decode(q, code, 256, decoded) == 0, "decode 256d");

    kanbudb_quant_destroy(q);
    return 1;
}

/* ── PQ Tests ───────────────────────────────────────────────── */

static int test_pq_create_destroy(void) {
    kanbudb_quant_params_t p = { .type = KANBUDB_QUANT_PQ, .dimension = 8, .pq_subspaces = 4 };
    kanbudb_quantizer_t* q = NULL;
    ASSERT(kanbudb_quant_create(&p, &q) == 0, "create");
    kanbudb_quant_destroy(q);
    return 1;
}

static int test_pq_invalid_subspaces(void) {
    kanbudb_quant_params_t p = { .type = KANBUDB_QUANT_PQ, .dimension = 8, .pq_subspaces = 0 };
    kanbudb_quantizer_t* q = NULL;
    ASSERT(kanbudb_quant_create(&p, &q) == -1, "zero subspaces");
    return 1;
}

static int test_pq_train_encode_decode(void) {
    kanbudb_quant_params_t p = { .type = KANBUDB_QUANT_PQ, .dimension = 8, .pq_subspaces = 4 };
    kanbudb_quantizer_t* q = NULL;
    kanbudb_quant_create(&p, &q);

    /* Train with 20 vectors */
    float train[160];
    srand(123);
    for (int i = 0; i < 160; i++) train[i] = (float)rand() / (float)RAND_MAX;
    ASSERT(kanbudb_quant_train(q, train, 20) == 0, "train pq");

    float v[] = {0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f, 0.7f, 0.8f};
    uint8_t code[4];
    size_t code_len;
    ASSERT(kanbudb_quant_encode(q, v, code, &code_len) == 0, "encode");
    ASSERT(code_len == 4, "code_len=subspaces");

    float decoded[8];
    ASSERT(kanbudb_quant_decode(q, code, 4, decoded) == 0, "decode");

    /* PQ reconstruction error is larger than SQ8 */
    for (int i = 0; i < 8; i++) {
        float err = fabsf(decoded[i] - v[i]);
        ASSERT(err < 1.0f, "pq approximate reconstruction");
    }

    kanbudb_quant_destroy(q);
    return 1;
}

static int test_pq_code_size(void) {
    kanbudb_quant_params_t p = { .type = KANBUDB_QUANT_PQ, .dimension = 16, .pq_subspaces = 8 };
    kanbudb_quantizer_t* q = NULL;
    kanbudb_quant_create(&p, &q);
    ASSERT(kanbudb_quant_code_size(q) == 8, "code_size=subspaces");
    kanbudb_quant_destroy(q);
    return 1;
}

static int test_pq_distance(void) {
    kanbudb_quant_params_t p = { .type = KANBUDB_QUANT_PQ, .dimension = 8, .pq_subspaces = 4 };
    kanbudb_quantizer_t* q = NULL;
    kanbudb_quant_create(&p, &q);

    float train[160];
    srand(456);
    for (int i = 0; i < 160; i++) train[i] = (float)rand() / (float)RAND_MAX;
    kanbudb_quant_train(q, train, 20);

    float a[] = {0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f, 0.7f, 0.8f};
    float b[] = {0.8f, 0.7f, 0.6f, 0.5f, 0.4f, 0.3f, 0.2f, 0.1f};
    uint8_t ca[4], cb[4];
    kanbudb_quant_encode(q, a, ca, NULL);
    kanbudb_quant_encode(q, b, cb, NULL);

    float d = kanbudb_quant_distance(q, ca, 4, cb, 4);
    ASSERT(d >= 0.0f, "distance non-negative");

    /* Self distance = 0 */
    float d_self = kanbudb_quant_distance(q, ca, 4, ca, 4);
    ASSERT(d_self == 0.0f, "self-distance=0");

    /* Symmetry */
    float d_ba = kanbudb_quant_distance(q, cb, 4, ca, 4);
    ASSERT(fabsf(d - d_ba) < 1e-5f, "symmetric");

    kanbudb_quant_destroy(q);
    return 1;
}

static int test_pq_cluster_quality(void) {
    kanbudb_quant_params_t p = { .type = KANBUDB_QUANT_PQ, .dimension = 4, .pq_subspaces = 2 };
    kanbudb_quantizer_t* q = NULL;
    kanbudb_quant_create(&p, &q);

    /* Two well-separated clusters: 100 vectors * 4 dims = 400 floats */
    float train[400];
    for (int i = 0; i < 50; i++) {
        train[i*4+0] = 0.0f + (float)rand() / (float)RAND_MAX * 0.1f;
        train[i*4+1] = 0.0f + (float)rand() / (float)RAND_MAX * 0.1f;
        train[i*4+2] = 0.0f + (float)rand() / (float)RAND_MAX * 0.1f;
        train[i*4+3] = 0.0f + (float)rand() / (float)RAND_MAX * 0.1f;
    }
    for (int i = 50; i < 100; i++) {
        train[i*4+0] = 10.0f + (float)rand() / (float)RAND_MAX * 0.1f;
        train[i*4+1] = 10.0f + (float)rand() / (float)RAND_MAX * 0.1f;
        train[i*4+2] = 10.0f + (float)rand() / (float)RAND_MAX * 0.1f;
        train[i*4+3] = 10.0f + (float)rand() / (float)RAND_MAX * 0.1f;
    }
    kanbudb_quant_train(q, train, 100);

    /* Encode cluster 0 and cluster 1 members */
    float v0[] = {0.05f, 0.05f, 0.05f, 0.05f};
    float v1[] = {10.05f, 10.05f, 10.05f, 10.05f};
    uint8_t c0[2], c1[2];
    kanbudb_quant_encode(q, v0, c0, NULL);
    kanbudb_quant_encode(q, v1, c1, NULL);

    /* Distance between clusters should be much larger than within cluster */
    float d_between = kanbudb_quant_distance(q, c0, 2, c1, 2);
    float v0b[] = {0.06f, 0.06f, 0.06f, 0.06f};
    uint8_t c0b[2];
    kanbudb_quant_encode(q, v0b, c0b, NULL);
    float d_within = kanbudb_quant_distance(q, c0, 2, c0b, 2);

    ASSERT(d_between > d_within * 10.0f, "inter-cluster >> intra-cluster");

    kanbudb_quant_destroy(q);
    return 1;
}

static int test_pq_different_subspaces(void) {
    /* Test with different subspace counts */
    for (int nsub = 1; nsub <= 8; nsub++) {
        kanbudb_quant_params_t p = { .type = KANBUDB_QUANT_PQ, .dimension = 8, .pq_subspaces = (uint32_t)nsub };
        kanbudb_quantizer_t* q = NULL;
        if (kanbudb_quant_create(&p, &q) != 0) continue;

        float train[160];
        srand(789);
        for (int i = 0; i < 160; i++) train[i] = (float)rand() / (float)RAND_MAX;
        kanbudb_quant_train(q, train, 20);

        float v[] = {0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f, 0.7f, 0.8f};
        uint8_t code[8];
        ASSERT(kanbudb_quant_encode(q, v, code, NULL) == 0, "encode");
        ASSERT(kanbudb_quant_code_size(q) == (uint32_t)nsub, "code_size");

        kanbudb_quant_destroy(q);
    }
    return 1;
}

static int test_pq_larger_dimension(void) {
    kanbudb_quant_params_t p = { .type = KANBUDB_QUANT_PQ, .dimension = 64, .pq_subspaces = 16 };
    kanbudb_quantizer_t* q = NULL;
    kanbudb_quant_create(&p, &q);

    float train[3200];
    srand(999);
    for (int i = 0; i < 3200; i++) train[i] = (float)rand() / (float)RAND_MAX;
    ASSERT(kanbudb_quant_train(q, train, 50) == 0, "train 64d pq");

    float v[64];
    for (int i = 0; i < 64; i++) v[i] = 0.5f;
    uint8_t code[16];
    ASSERT(kanbudb_quant_encode(q, v, code, NULL) == 0, "encode 64d");

    float decoded[64];
    ASSERT(kanbudb_quant_decode(q, code, 16, decoded) == 0, "decode 64d");

    kanbudb_quant_destroy(q);
    return 1;
}

static int test_pq_distance_ordering(void) {
    kanbudb_quant_params_t p = { .type = KANBUDB_QUANT_PQ, .dimension = 8, .pq_subspaces = 4 };
    kanbudb_quantizer_t* q = NULL;
    kanbudb_quant_create(&p, &q);

    float train[160];
    srand(111);
    for (int i = 0; i < 160; i++) train[i] = (float)rand() / (float)RAND_MAX;
    kanbudb_quant_train(q, train, 20);

    float query[] = {0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f};
    float near[] = {0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.51f};
    float far[]  = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};

    uint8_t cq[4], cn[4], cf[4];
    kanbudb_quant_encode(q, query, cq, NULL);
    kanbudb_quant_encode(q, near, cn, NULL);
    kanbudb_quant_encode(q, far, cf, NULL);

    float d_near = kanbudb_quant_distance(q, cq, 4, cn, 4);
    float d_far = kanbudb_quant_distance(q, cq, 4, cf, 4);

    ASSERT(d_near < d_far, "near < far");

    kanbudb_quant_destroy(q);
    return 1;
}

static int test_quant_null_destroy(void) {
    kanbudb_quant_destroy(NULL); /* should not crash */
    return 1;
}

static int test_quant_not_trained(void) {
    kanbudb_quant_params_t p = { .type = KANBUDB_QUANT_SQ8, .dimension = 4 };
    kanbudb_quantizer_t* q = NULL;
    kanbudb_quant_create(&p, &q);

    /* Encoding without training should fail (no min/range computed) */
    float v[] = {1,2,3,4};
    uint8_t code[4];
    ASSERT(kanbudb_quant_encode(q, v, code, NULL) == -1, "encode untrained fails");

    kanbudb_quant_destroy(q);
    return 1;
}

int main(void) {
    printf("=== quantize tests ===\n");
    TEST(sq8_create_destroy);
    TEST(sq8_invalid_params);
    TEST(sq8_train_single_vector);
    TEST(sq8_encode_decode_roundtrip);
    TEST(sq8_code_size);
    TEST(sq8_distance_symmetry);
    TEST(sq8_distance_self_zero);
    TEST(sq8_batch_encode);
    TEST(sq8_high_dimensional);
    TEST(pq_create_destroy);
    TEST(pq_invalid_subspaces);
    TEST(pq_train_encode_decode);
    TEST(pq_code_size);
    TEST(pq_distance);
    TEST(pq_cluster_quality);
    TEST(pq_different_subspaces);
    TEST(pq_larger_dimension);
    TEST(pq_distance_ordering);
    TEST(quant_null_destroy);
    TEST(quant_not_trained);
    printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
