#include "quantize.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <float.h>

struct kanbudb_quantizer {
    kanbudb_quant_params_t params;

    /* SQ8 state */
    float* sq_min;       /* per-dimension min */
    float* sq_range;     /* per-dimension range (max - min) */
    int    sq_trained;

    /* PQ state */
    float** pq_codebooks;  /* [subspaces][256 * sub_dim] */
    uint32_t pq_sub_dim;
    int     pq_trained;
};

/* ── SQ8 ────────────────────────────────────────────────────── */

static int sq8_train(kanbudb_quantizer_t* q, const float* vectors, uint32_t count)
{
    uint32_t dim = q->params.dimension;
    q->sq_min = (float*)calloc(dim, sizeof(float));
    q->sq_range = (float*)calloc(dim, sizeof(float));
    if (!q->sq_min || !q->sq_range) return -1;

    for (uint32_t d = 0; d < dim; d++) {
        float mn = vectors[d];
        float mx = vectors[d];
        for (uint32_t i = 1; i < count; i++) {
            float v = vectors[i * dim + d];
            if (v < mn) mn = v;
            if (v > mx) mx = v;
        }
        q->sq_min[d] = mn;
        q->sq_range[d] = (mx > mn) ? (mx - mn) : 1.0f;
    }
    q->sq_trained = 1;
    return 0;
}

static int sq8_encode(const kanbudb_quantizer_t* q, const float* vector, uint8_t* out)
{
    uint32_t dim = q->params.dimension;
    for (uint32_t d = 0; d < dim; d++) {
        float normalized = (vector[d] - q->sq_min[d]) / q->sq_range[d];
        if (normalized < 0.0f) normalized = 0.0f;
        if (normalized > 1.0f) normalized = 1.0f;
        out[d] = (uint8_t)(normalized * 255.0f + 0.5f);
    }
    return 0;
}

static int sq8_decode(const kanbudb_quantizer_t* q, const uint8_t* code, float* out)
{
    uint32_t dim = q->params.dimension;
    for (uint32_t d = 0; d < dim; d++) {
        out[d] = q->sq_min[d] + ((float)code[d] / 255.0f) * q->sq_range[d];
    }
    return 0;
}

static float sq8_distance(const kanbudb_quantizer_t* q,
                           const uint8_t* a, const uint8_t* b)
{
    uint32_t dim = q->params.dimension;
    float sum = 0.0f;
    for (uint32_t d = 0; d < dim; d++) {
        float da = q->sq_min[d] + ((float)a[d] / 255.0f) * q->sq_range[d];
        float db = q->sq_min[d] + ((float)b[d] / 255.0f) * q->sq_range[d];
        float diff = da - db;
        sum += diff * diff;
    }
    return sum;
}

/* ── PQ ─────────────────────────────────────────────────────── */

/* Simple k-means for codebook training */
static void pq_kmeans(const float* training, uint32_t n, uint32_t dim,
                       float* codebook, uint32_t k, uint32_t max_iter)
{
    /* Initialize codebook with random samples */
    for (uint32_t c = 0; c < k; c++) {
        uint32_t idx = (uint32_t)((float)c / (float)k * (float)n);
        if (idx >= n) idx = n - 1;
        memcpy(codebook + c * dim, training + idx * dim, dim * sizeof(float));
    }

    uint32_t* assignments = (uint32_t*)malloc(n * sizeof(uint32_t));
    float* new_centroids = (float*)malloc(k * dim * sizeof(float));
    uint32_t* counts = (uint32_t*)malloc(k * sizeof(uint32_t));
    if (!assignments || !new_centroids || !counts) {
        free(assignments); free(new_centroids); free(counts);
        return;
    }

    for (uint32_t iter = 0; iter < max_iter; iter++) {
        /* Assign */
        for (uint32_t i = 0; i < n; i++) {
            float best_dist = FLT_MAX;
            uint32_t best_c = 0;
            for (uint32_t c = 0; c < k; c++) {
                float dist = 0.0f;
                for (uint32_t d = 0; d < dim; d++) {
                    float diff = training[i * dim + d] - codebook[c * dim + d];
                    dist += diff * diff;
                }
                if (dist < best_dist) {
                    best_dist = dist;
                    best_c = c;
                }
            }
            assignments[i] = best_c;
        }

        /* Update centroids */
        memset(new_centroids, 0, k * dim * sizeof(float));
        memset(counts, 0, k * sizeof(uint32_t));
        for (uint32_t i = 0; i < n; i++) {
            uint32_t c = assignments[i];
            counts[c]++;
            for (uint32_t d = 0; d < dim; d++) {
                new_centroids[c * dim + d] += training[i * dim + d];
            }
        }
        for (uint32_t c = 0; c < k; c++) {
            if (counts[c] > 0) {
                for (uint32_t d = 0; d < dim; d++) {
                    codebook[c * dim + d] = new_centroids[c * dim + d] / (float)counts[c];
                }
            }
        }
    }

    free(assignments);
    free(new_centroids);
    free(counts);
}

static int pq_train(kanbudb_quantizer_t* q, const float* vectors, uint32_t count)
{
    uint32_t dim = q->params.dimension;
    uint32_t nsub = q->params.pq_subspaces;
    uint32_t sub_dim = dim / nsub;
    if (sub_dim == 0) return -1;
    q->pq_sub_dim = sub_dim;

    q->pq_codebooks = (float**)malloc(nsub * sizeof(float*));
    if (!q->pq_codebooks) return -1;

    /* Training data per subspace: reshape to [count * sub_dim] */
    float* sub_training = (float*)malloc((size_t)count * sub_dim * sizeof(float));
    if (!sub_training) { free(q->pq_codebooks); return -1; }

    for (uint32_t s = 0; s < nsub; s++) {
        q->pq_codebooks[s] = (float*)malloc(256 * sub_dim * sizeof(float));
        if (!q->pq_codebooks[s]) {
            for (uint32_t j = 0; j < s; j++) free(q->pq_codebooks[j]);
            free(q->pq_codebooks); free(sub_training);
            return -1;
        }

        /* Extract subspace vectors */
        for (uint32_t i = 0; i < count; i++) {
            memcpy(sub_training + i * sub_dim,
                   vectors + i * dim + s * sub_dim,
                   sub_dim * sizeof(float));
        }

        pq_kmeans(sub_training, count, sub_dim, q->pq_codebooks[s], 256, 20);
    }

    free(sub_training);
    q->pq_trained = 1;
    return 0;
}

static int pq_encode(const kanbudb_quantizer_t* q, const float* vector, uint8_t* out)
{
    uint32_t nsub = q->params.pq_subspaces;
    uint32_t sub_dim = q->pq_sub_dim;

    for (uint32_t s = 0; s < nsub; s++) {
        float best_dist = FLT_MAX;
        uint8_t best_idx = 0;
        const float* cb = q->pq_codebooks[s];
        for (uint32_t c = 0; c < 256; c++) {
            float dist = 0.0f;
            for (uint32_t d = 0; d < sub_dim; d++) {
                float diff = vector[s * sub_dim + d] - cb[c * sub_dim + d];
                dist += diff * diff;
            }
            if (dist < best_dist) {
                best_dist = dist;
                best_idx = (uint8_t)c;
            }
        }
        out[s] = best_idx;
    }
    return 0;
}

static int pq_decode(const kanbudb_quantizer_t* q, const uint8_t* code, float* out)
{
    uint32_t nsub = q->params.pq_subspaces;
    uint32_t sub_dim = q->pq_sub_dim;

    for (uint32_t s = 0; s < nsub; s++) {
        uint8_t idx = code[s];
        memcpy(out + s * sub_dim,
               q->pq_codebooks[s] + idx * sub_dim,
               sub_dim * sizeof(float));
    }
    return 0;
}

/* ── PQ distance: scalar fallback ──────────────────────── */

static float pq_distance_scalar(const kanbudb_quantizer_t* q,
                                 const uint8_t* a, const uint8_t* b)
{
    uint32_t nsub = q->params.pq_subspaces;
    uint32_t sub_dim = q->pq_sub_dim;
    float sum = 0.0f;

    for (uint32_t s = 0; s < nsub; s++) {
        const float* ca = q->pq_codebooks[s] + a[s] * sub_dim;
        const float* cb = q->pq_codebooks[s] + b[s] * sub_dim;
        for (uint32_t d = 0; d < sub_dim; d++) {
            float diff = ca[d] - cb[d];
            sum += diff * diff;
        }
    }
    return sum;
}

/* ── PQ distance: SIMD (function-specific targets) ────── */
/* Uses __attribute__((target(...))) for runtime dispatch,
 * no compile-time -mavx2/-mavx512f flags needed. */

#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>

__attribute__((target("avx2,fma")))
static float pq_distance_avx2(const kanbudb_quantizer_t* q,
                               const uint8_t* a, const uint8_t* b)
{
    uint32_t nsub = q->params.pq_subspaces;
    uint32_t sub_dim = q->pq_sub_dim;
    uint32_t vec_loops = sub_dim / 8;
    uint32_t tail_start = vec_loops * 8;
    __m256 sum = _mm256_setzero_ps();
    float tail = 0.0f;

    for (uint32_t s = 0; s < nsub; s++) {
        const float* ca = q->pq_codebooks[s] + a[s] * sub_dim;
        const float* cb = q->pq_codebooks[s] + b[s] * sub_dim;

        for (uint32_t d = 0; d < vec_loops * 8; d += 8) {
            __m256 va = _mm256_loadu_ps(ca + d);
            __m256 vb = _mm256_loadu_ps(cb + d);
            __m256 diff = _mm256_sub_ps(va, vb);
            sum = _mm256_fmadd_ps(diff, diff, sum);
        }
        for (uint32_t d = tail_start; d < sub_dim; d++) {
            float diff = ca[d] - cb[d];
            tail += diff * diff;
        }
    }

    __m128 hi = _mm256_extractf128_ps(sum, 1);
    __m128 lo = _mm256_castps256_ps128(sum);
    __m128 s = _mm_add_ps(lo, hi);
    s = _mm_hadd_ps(s, s);
    s = _mm_hadd_ps(s, s);
    return _mm_cvtss_f32(s) + tail;
}

__attribute__((target("avx512f")))
static float pq_distance_avx512(const kanbudb_quantizer_t* q,
                                 const uint8_t* a, const uint8_t* b)
{
    uint32_t nsub = q->params.pq_subspaces;
    uint32_t sub_dim = q->pq_sub_dim;
    uint32_t vec_loops = sub_dim / 16;
    uint32_t tail_start = vec_loops * 16;
    __m512 sum = _mm512_setzero_ps();
    float tail = 0.0f;

    for (uint32_t s = 0; s < nsub; s++) {
        const float* ca = q->pq_codebooks[s] + a[s] * sub_dim;
        const float* cb = q->pq_codebooks[s] + b[s] * sub_dim;

        for (uint32_t d = 0; d < vec_loops * 16; d += 16) {
            __m512 va = _mm512_loadu_ps(ca + d);
            __m512 vb = _mm512_loadu_ps(cb + d);
            __m512 diff = _mm512_sub_ps(va, vb);
            sum = _mm512_fmadd_ps(diff, diff, sum);
        }
        for (uint32_t d = tail_start; d < sub_dim; d++) {
            float diff = ca[d] - cb[d];
            tail += diff * diff;
        }
    }

    __m128 hi = _mm512_extractf32x4_ps(sum, 1);
    __m128 lo = _mm512_castps512_ps128(sum);
    __m128 s = _mm_add_ps(lo, hi);
    s = _mm_hadd_ps(s, s);
    s = _mm_hadd_ps(s, s);
    return _mm_cvtss_f32(s) + tail;
}

#else
static float pq_distance_avx2(const kanbudb_quantizer_t* q,
                               const uint8_t* a, const uint8_t* b) {
    (void)q; (void)a; (void)b;
    return pq_distance_scalar(q, a, b);
}
static float pq_distance_avx512(const kanbudb_quantizer_t* q,
                                 const uint8_t* a, const uint8_t* b) {
    (void)q; (void)a; (void)b;
    return pq_distance_scalar(q, a, b);
}
#endif

/* ── PQ distance: runtime dispatch ─────────────────────── */

static float pq_distance(const kanbudb_quantizer_t* q,
                          const uint8_t* a, const uint8_t* b)
{
#if defined(__x86_64__) || defined(__i386__)
    static int checked = 0;
    static int use_avx512 = 0;
    static int use_avx2 = 0;
    if (!checked) {
        checked = 1;
        use_avx512 = __builtin_cpu_supports("avx512f");
        use_avx2 = __builtin_cpu_supports("avx2");
    }
    if (use_avx512) {
        return pq_distance_avx512(q, a, b);
    }
    if (use_avx2) {
        return pq_distance_avx2(q, a, b);
    }
#endif
    return pq_distance_scalar(q, a, b);
}

/* ── Public API ─────────────────────────────────────────────── */

int kanbudb_quant_create(const kanbudb_quant_params_t* params,
                         kanbudb_quantizer_t** out)
{
    if (!params || !out || params->dimension == 0) return -1;
    if (params->type == KANBUDB_QUANT_PQ && params->pq_subspaces == 0) return -1;

    kanbudb_quantizer_t* q = (kanbudb_quantizer_t*)calloc(1, sizeof(*q));
    if (!q) return -1;
    q->params = *params;
    *out = q;
    return 0;
}

void kanbudb_quant_destroy(kanbudb_quantizer_t* q)
{
    if (!q) return;
    free(q->sq_min);
    free(q->sq_range);
    if (q->pq_codebooks) {
        for (uint32_t i = 0; i < q->params.pq_subspaces; i++)
            free(q->pq_codebooks[i]);
        free(q->pq_codebooks);
    }
    free(q);
}

int kanbudb_quant_train(kanbudb_quantizer_t* q,
                        const float* vectors, uint32_t count)
{
    if (!q || !vectors || count == 0) return -1;

    switch (q->params.type) {
        case KANBUDB_QUANT_SQ8:
            return sq8_train(q, vectors, count);
        case KANBUDB_QUANT_PQ:
            return pq_train(q, vectors, count);
        default:
            return -1;
    }
}

int kanbudb_quant_encode(const kanbudb_quantizer_t* q,
                         const float* vector,
                         uint8_t* out_code, size_t* out_code_len)
{
    if (!q || !vector || !out_code) return -1;
    uint32_t cs = kanbudb_quant_code_size(q);
    if (out_code_len) *out_code_len = cs;

    switch (q->params.type) {
        case KANBUDB_QUANT_SQ8:
            if (!q->sq_trained) return -1;
            return sq8_encode(q, vector, out_code);
        case KANBUDB_QUANT_PQ:
            if (!q->pq_trained) return -1;
            return pq_encode(q, vector, out_code);
        default:
            return -1;
    }
}

int kanbudb_quant_decode(const kanbudb_quantizer_t* q,
                         const uint8_t* code, size_t code_len,
                         float* out_vector)
{
    if (!q || !code || !out_vector) return -1;
    (void)code_len;

    switch (q->params.type) {
        case KANBUDB_QUANT_SQ8:
            return sq8_decode(q, code, out_vector);
        case KANBUDB_QUANT_PQ:
            return pq_decode(q, code, out_vector);
        default:
            return -1;
    }
}

float kanbudb_quant_distance(const kanbudb_quantizer_t* q,
                             const uint8_t* code_a, size_t code_len_a,
                             const uint8_t* code_b, size_t code_len_b)
{
    if (!q || !code_a || !code_b) return FLT_MAX;
    (void)code_len_a; (void)code_len_b;

    switch (q->params.type) {
        case KANBUDB_QUANT_SQ8:
            return sq8_distance(q, code_a, code_b);
        case KANBUDB_QUANT_PQ:
            return pq_distance(q, code_a, code_b);
        default:
            return FLT_MAX;
    }
}

uint32_t kanbudb_quant_code_size(const kanbudb_quantizer_t* q)
{
    if (!q) return 0;
    switch (q->params.type) {
        case KANBUDB_QUANT_SQ8:
            return q->params.dimension;
        case KANBUDB_QUANT_PQ:
            return q->params.pq_subspaces;
        default:
            return 0;
    }
}
