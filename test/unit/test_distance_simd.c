#include "distance.h"
#include "quantize_internal.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) do { \
  printf("  TEST: %s ... ", #name); \
  if (test_##name()) { printf("PASS\n"); tests_passed++; } \
  else { printf("FAIL\n"); tests_failed++; } \
} while(0)

static float vec_l2_distance_scalar(const float* a, const float* b, uint32_t dim) {
    float sum = 0.0f;
    for (uint32_t i = 0; i < dim; i++) {
        float d = a[i] - b[i];
        sum += d * d;
    }
    return sum;
}

static float vec_cosine_distance_scalar(const float* a, const float* b, uint32_t dim) {
    float dot = 0.0f, noma = 0.0f, nomb = 0.0f;
    for (uint32_t i = 0; i < dim; i++) {
        dot  += a[i] * b[i];
        noma += a[i] * a[i];
        nomb += b[i] * b[i];
    }
    float denom = noma * nomb;
    if (denom == 0.0f) return 1.0f;
    return 1.0f - dot / sqrtf(denom);
}

static float vec_ip_distance_scalar(const float* a, const float* b, uint32_t dim) {
    float dot = 0.0f;
    for (uint32_t i = 0; i < dim; i++) {
        dot += a[i] * b[i];
    }
    return -dot;
}

static float diff_percent(float a, float b) {
    if (a == b) return 0.0f;
    float max_ab = fmaxf(fabsf(a), fabsf(b));
    if (max_ab == 0.0f) return 0.0f;
    return fabsf(a - b) / max_ab * 100.0f;
}

static int test_l2_scalar_simd_match(void) {
    uint32_t dims[] = {1, 2, 3, 4, 5, 7, 8, 9, 15, 16, 17, 31, 32, 33, 64, 128, 256};
    int num_dims = sizeof(dims) / sizeof(dims[0]);

    for (int d = 0; d < num_dims; d++) {
        uint32_t dim = dims[d];
        float* a = (float*)malloc(dim * sizeof(float));
        float* b = (float*)malloc(dim * sizeof(float));
        if (!a || !b) { free(a); free(b); return 0; }

        for (uint32_t i = 0; i < dim; i++) {
            a[i] = (float)((i * 7 + 13) % 100) / 10.0f;
            b[i] = (float)((i * 3 + 41) % 100) / 10.0f;
        }

        float expected = vec_l2_distance_scalar(a, b, dim);
        float actual = vec_l2_distance(a, b, dim);

        if (diff_percent(expected, actual) > 0.001f) {
            fprintf(stderr, "  dim=%u: scalar=%f simd=%f\n", dim, expected, actual);
            free(a); free(b); return 0;
        }

        free(a); free(b);
    }
    return 1;
}

static int test_cosine_scalar_simd_match(void) {
    uint32_t dims[] = {1, 4, 8, 16, 32, 64, 128, 256};
    int num_dims = sizeof(dims) / sizeof(dims[0]);

    for (int d = 0; d < num_dims; d++) {
        uint32_t dim = dims[d];
        float* a = (float*)malloc(dim * sizeof(float));
        float* b = (float*)malloc(dim * sizeof(float));
        if (!a || !b) { free(a); free(b); return 0; }

        for (uint32_t i = 0; i < dim; i++) {
            a[i] = (float)((i * 17) % 100);
            b[i] = (float)((i * 19 + 7) % 100);
        }

        float expected = vec_cosine_distance_scalar(a, b, dim);
        float actual = vec_cosine_distance(a, b, dim);

        if (diff_percent(expected, actual) > 0.001f) {
            fprintf(stderr, "  cosine dim=%u: scalar=%f simd=%f\n", dim, expected, actual);
            free(a); free(b); return 0;
        }

        free(a); free(b);
    }
    return 1;
}

static int test_ip_scalar_simd_match(void) {
    uint32_t dims[] = {1, 8, 16, 32, 64, 128, 256};
    int num_dims = sizeof(dims) / sizeof(dims[0]);

    for (int d = 0; d < num_dims; d++) {
        uint32_t dim = dims[d];
        float* a = (float*)malloc(dim * sizeof(float));
        float* b = (float*)malloc(dim * sizeof(float));
        if (!a || !b) { free(a); free(b); return 0; }

        for (uint32_t i = 0; i < dim; i++) {
            a[i] = (float)((i * 11) % 100);
            b[i] = (float)((i * 13 + 3) % 100);
        }

        float expected = vec_ip_distance_scalar(a, b, dim);
        float actual = vec_ip_distance(a, b, dim);

        if (diff_percent(expected, actual) > 0.001f) {
            fprintf(stderr, "  ip dim=%u: scalar=%f simd=%f\n", dim, expected, actual);
            free(a); free(b); return 0;
        }

        free(a); free(b);
    }
    return 1;
}

static int test_dispatch_matches_scalar(void) {
    uint32_t dim = 128;
    float* a = (float*)malloc(dim * sizeof(float));
    float* b = (float*)malloc(dim * sizeof(float));
    if (!a || !b) { free(a); free(b); return 0; }

    for (uint32_t i = 0; i < dim; i++) {
        a[i] = (float)(i);
        b[i] = (float)(dim - i);
    }

    vec_dist_func_t f_l2 = vec_get_dist_func(KANBUDB_VEC_METRIC_L2);
    vec_dist_func_t f_cos = vec_get_dist_func(KANBUDB_VEC_METRIC_COSINE);
    vec_dist_func_t f_ip = vec_get_dist_func(KANBUDB_VEC_METRIC_IP);

    float l2_d = f_l2(a, b, dim);
    float cos_d = f_cos(a, b, dim);
    float ip_d = f_ip(a, b, dim);

    float l2_s = vec_l2_distance_scalar(a, b, dim);
    float cos_s = vec_cosine_distance_scalar(a, b, dim);
    float ip_s = vec_ip_distance_scalar(a, b, dim);

    if (diff_percent(l2_d, l2_s) > 0.001f) {
        fprintf(stderr, "  L2 dispatch=%f scalar=%f\n", l2_d, l2_s);
        free(a); free(b); return 0;
    }
    if (diff_percent(cos_d, cos_s) > 0.001f) {
        fprintf(stderr, "  COS dispatch=%f scalar=%f\n", cos_d, cos_s);
        free(a); free(b); return 0;
    }
    if (diff_percent(ip_d, ip_s) > 0.001f) {
        fprintf(stderr, "  IP dispatch=%f scalar=%f\n", ip_d, ip_s);
        free(a); free(b); return 0;
    }

    free(a); free(b);
    return 1;
}

static int test_edge_case_zero_vectors(void) {
    float a[4] = {0, 0, 0, 0};
    float b[4] = {0, 0, 0, 0};

    float l2 = vec_l2_distance(a, b, 4);
    if (l2 != 0.0f) {
        fprintf(stderr, "  zero L2 = %f, expected 0\n", l2);
        return 0;
    }

    float cos_d = vec_cosine_distance(a, b, 4);
    if (cos_d != 1.0f) {
        fprintf(stderr, "  zero cosine = %f, expected 1\n", cos_d);
        return 0;
    }

    float ip = vec_ip_distance(a, b, 4);
    if (ip != 0.0f) {
        fprintf(stderr, "  zero IP = %f, expected 0\n", ip);
        return 0;
    }

    return 1;
}

static int test_pq_distance_consistency(void) {
    uint32_t dim = 64;
    uint32_t subspaces = 8;
    uint32_t codebook_size = 256;

    kanbudb_quant_params_t params;
    params.type = KANBUDB_QUANT_PQ;
    params.dimension = dim;
    params.pq_subspaces = subspaces;

    kanbudb_quantizer_t* q = NULL;
    if (kanbudb_quant_create(&params, &q) != 0 || !q) return 0;

    float* train_data = (float*)malloc(codebook_size * dim * sizeof(float));
    if (!train_data) { kanbudb_quant_destroy(q); return 0; }

    for (uint32_t i = 0; i < codebook_size; i++) {
        for (uint32_t j = 0; j < dim; j++) {
            train_data[i * dim + j] = (float)((i * 7 + j * 13) % 100) / 10.0f;
        }
    }

    if (kanbudb_quant_train(q, train_data, codebook_size) != 0) {
        free(train_data); kanbudb_quant_destroy(q); return 0;
    }
    free(train_data);

    float* vec_a = (float*)malloc(dim * sizeof(float));
    float* vec_b = (float*)malloc(dim * sizeof(float));
    if (!vec_a || !vec_b) { free(vec_a); free(vec_b); kanbudb_quant_destroy(q); return 0; }

    for (uint32_t j = 0; j < dim; j++) {
        vec_a[j] = (float)(j * 3 % 100) / 10.0f;
        vec_b[j] = (float)((j + 50) * 7 % 100) / 10.0f;
    }

    uint32_t code_size = kanbudb_quant_code_size(q);
    uint8_t* code_a = (uint8_t*)malloc(code_size);
    uint8_t* code_b = (uint8_t*)malloc(code_size);
    if (!code_a || !code_b) { free(code_a); free(code_b); free(vec_a); free(vec_b); kanbudb_quant_destroy(q); return 0; }

    kanbudb_quant_encode(q, vec_a, code_a, NULL);
    kanbudb_quant_encode(q, vec_b, code_b, NULL);

    float pq_dist = kanbudb_quant_distance(q, code_a, code_size, code_b, code_size);

    if (pq_dist < 0.0f) {
        fprintf(stderr, "  PQ distance negative: %f\n", pq_dist);
        free(code_a); free(code_b); free(vec_a); free(vec_b);
        kanbudb_quant_destroy(q); return 0;
    }

    free(code_a); free(code_b); free(vec_a); free(vec_b);
    kanbudb_quant_destroy(q);
    return 1;
}

int main(void) {
    printf("SIMD distance / PQ rigorous tests:\n");
    TEST(l2_scalar_simd_match);
    TEST(cosine_scalar_simd_match);
    TEST(ip_scalar_simd_match);
    TEST(dispatch_matches_scalar);
    TEST(edge_case_zero_vectors);
    TEST(pq_distance_consistency);
    printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
