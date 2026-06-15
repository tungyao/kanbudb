#include <math.h>
#include "distance.h"

float
vec_l2_distance(const float* a, const float* b, uint32_t dim)
{
    float sum = 0.0f;
    for (uint32_t i = 0; i < dim; i++) {
        float diff = a[i] - b[i];
        sum += diff * diff;
    }
    return sum;
}

float
vec_cosine_distance(const float* a, const float* b, uint32_t dim)
{
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

float
vec_ip_distance(const float* a, const float* b, uint32_t dim)
{
    float dot = 0.0f;
    for (uint32_t i = 0; i < dim; i++) {
        dot += a[i] * b[i];
    }
    return -dot;
}

/* ------------------------------------------------------------------ */
/*  AVX2 implementations                                              */
/* ------------------------------------------------------------------ */

#ifdef __AVX2__
#include <immintrin.h>

float
vec_l2_distance_avx2(const float* a, const float* b, uint32_t dim)
{
    __m256 sum = _mm256_setzero_ps();
    uint32_t i = 0;
    for (; i + 8 <= dim; i += 8) {
        __m256 va = _mm256_loadu_ps(a + i);
        __m256 vb = _mm256_loadu_ps(b + i);
        __m256 diff = _mm256_sub_ps(va, vb);
        sum = _mm256_fmadd_ps(diff, diff, sum);
    }
    __m128 hi = _mm256_extractf128_ps(sum, 1);
    __m128 lo = _mm256_castps256_ps128(sum);
    __m128 s = _mm_add_ps(lo, hi);
    s = _mm_hadd_ps(s, s);
    s = _mm_hadd_ps(s, s);
    float result = _mm_cvtss_f32(s);
    for (; i < dim; i++) { float d = a[i] - b[i]; result += d * d; }
    return result;
}

float
vec_cosine_distance_avx2(const float* a, const float* b, uint32_t dim)
{
    __m256 dotv = _mm256_setzero_ps();
    __m256 noma = _mm256_setzero_ps();
    __m256 nomb = _mm256_setzero_ps();
    uint32_t i = 0;
    for (; i + 8 <= dim; i += 8) {
        __m256 va = _mm256_loadu_ps(a + i);
        __m256 vb = _mm256_loadu_ps(b + i);
        dotv = _mm256_fmadd_ps(va, vb, dotv);
        noma = _mm256_fmadd_ps(va, va, noma);
        nomb = _mm256_fmadd_ps(vb, vb, nomb);
    }
    __m128 hi = _mm256_extractf128_ps(dotv, 1);
    __m128 lo = _mm256_castps256_ps128(dotv);
    __m128 s = _mm_add_ps(lo, hi);
    s = _mm_hadd_ps(s, s);
    s = _mm_hadd_ps(s, s);
    float dot = _mm_cvtss_f32(s);
    hi = _mm256_extractf128_ps(noma, 1);
    lo = _mm256_castps256_ps128(noma);
    s = _mm_add_ps(lo, hi);
    s = _mm_hadd_ps(s, s);
    s = _mm_hadd_ps(s, s);
    float na = _mm_cvtss_f32(s);
    hi = _mm256_extractf128_ps(nomb, 1);
    lo = _mm256_castps256_ps128(nomb);
    s = _mm_add_ps(lo, hi);
    s = _mm_hadd_ps(s, s);
    s = _mm_hadd_ps(s, s);
    float nb = _mm_cvtss_f32(s);
    for (; i < dim; i++) {
        dot += a[i] * b[i];
        na  += a[i] * a[i];
        nb  += b[i] * b[i];
    }
    float denom = na * nb;
    if (denom == 0.0f) return 1.0f;
    return 1.0f - dot / sqrtf(denom);
}

float
vec_ip_distance_avx2(const float* a, const float* b, uint32_t dim)
{
    __m256 sum = _mm256_setzero_ps();
    uint32_t i = 0;
    for (; i + 8 <= dim; i += 8) {
        __m256 va = _mm256_loadu_ps(a + i);
        __m256 vb = _mm256_loadu_ps(b + i);
        sum = _mm256_fmadd_ps(va, vb, sum);
    }
    __m128 hi = _mm256_extractf128_ps(sum, 1);
    __m128 lo = _mm256_castps256_ps128(sum);
    __m128 s = _mm_add_ps(lo, hi);
    s = _mm_hadd_ps(s, s);
    s = _mm_hadd_ps(s, s);
    float result = _mm_cvtss_f32(s);
    for (; i < dim; i++) { result += a[i] * b[i]; }
    return -result;
}
#endif /* __AVX2__ */

/* ------------------------------------------------------------------ */
/*  NEON implementations                                              */
/* ------------------------------------------------------------------ */

#ifdef __ARM_NEON__
#include <arm_neon.h>

float
vec_l2_distance_neon(const float* a, const float* b, uint32_t dim)
{
    float32x4_t sum = vdupq_n_f32(0.0f);
    uint32_t i = 0;
    for (; i + 4 <= dim; i += 4) {
        float32x4_t va = vld1q_f32(a + i);
        float32x4_t vb = vld1q_f32(b + i);
        float32x4_t diff = vsubq_f32(va, vb);
        sum = vmlaq_f32(sum, diff, diff);
    }
    float result = vaddvq_f32(sum);
    for (; i < dim; i++) { float d = a[i] - b[i]; result += d * d; }
    return result;
}

float
vec_cosine_distance_neon(const float* a, const float* b, uint32_t dim)
{
    float32x4_t dotv = vdupq_n_f32(0.0f);
    float32x4_t noma = vdupq_n_f32(0.0f);
    float32x4_t nomb = vdupq_n_f32(0.0f);
    uint32_t i = 0;
    for (; i + 4 <= dim; i += 4) {
        float32x4_t va = vld1q_f32(a + i);
        float32x4_t vb = vld1q_f32(b + i);
        dotv = vmlaq_f32(dotv, va, vb);
        noma = vmlaq_f32(noma, va, va);
        nomb = vmlaq_f32(nomb, vb, vb);
    }
    float dot = vaddvq_f32(dotv);
    float na  = vaddvq_f32(noma);
    float nb  = vaddvq_f32(nomb);
    for (; i < dim; i++) {
        dot += a[i] * b[i];
        na  += a[i] * a[i];
        nb  += b[i] * b[i];
    }
    float denom = na * nb;
    if (denom == 0.0f) return 1.0f;
    return 1.0f - dot / sqrtf(denom);
}

float
vec_ip_distance_neon(const float* a, const float* b, uint32_t dim)
{
    float32x4_t sum = vdupq_n_f32(0.0f);
    uint32_t i = 0;
    for (; i + 4 <= dim; i += 4) {
        float32x4_t va = vld1q_f32(a + i);
        float32x4_t vb = vld1q_f32(b + i);
        sum = vmlaq_f32(sum, va, vb);
    }
    float result = vaddvq_f32(sum);
    for (; i < dim; i++) { result += a[i] * b[i]; }
    return -result;
}
#endif /* __ARM_NEON__ */

/* ------------------------------------------------------------------ */
/*  Dispatcher                                                         */
/* ------------------------------------------------------------------ */

vec_dist_func_t
vec_get_dist_func(kanbudb_vec_metric_t metric)
{
    vec_dist_func_t funcs[] = {
#ifdef __AVX2__
        [KANBUDB_VEC_METRIC_L2]     = vec_l2_distance_avx2,
        [KANBUDB_VEC_METRIC_COSINE] = vec_cosine_distance_avx2,
        [KANBUDB_VEC_METRIC_IP]     = vec_ip_distance_avx2,
#elif defined(__ARM_NEON__)
        [KANBUDB_VEC_METRIC_L2]     = vec_l2_distance_neon,
        [KANBUDB_VEC_METRIC_COSINE] = vec_cosine_distance_neon,
        [KANBUDB_VEC_METRIC_IP]     = vec_ip_distance_neon,
#else
        [KANBUDB_VEC_METRIC_L2]     = vec_l2_distance,
        [KANBUDB_VEC_METRIC_COSINE] = vec_cosine_distance,
        [KANBUDB_VEC_METRIC_IP]     = vec_ip_distance,
#endif
    };
    switch (metric) {
        case KANBUDB_VEC_METRIC_L2:
            return funcs[KANBUDB_VEC_METRIC_L2];
        case KANBUDB_VEC_METRIC_COSINE:
            return funcs[KANBUDB_VEC_METRIC_COSINE];
        case KANBUDB_VEC_METRIC_IP:
            return funcs[KANBUDB_VEC_METRIC_IP];
        default:
            return funcs[KANBUDB_VEC_METRIC_L2];
    }
}
