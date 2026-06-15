#ifndef KANBUDB_VECTOR_DISTANCE_H
#define KANBUDB_VECTOR_DISTANCE_H

#include <stdint.h>
#include "util/macros.h"
#include "vector.h"

typedef float (*vec_dist_func_t)(const float*, const float*, uint32_t);

float vec_l2_distance(const float* a, const float* b, uint32_t dim);
float vec_cosine_distance(const float* a, const float* b, uint32_t dim);
float vec_ip_distance(const float* a, const float* b, uint32_t dim);

#ifdef __AVX2__
float vec_l2_distance_avx2(const float* a, const float* b, uint32_t dim);
float vec_cosine_distance_avx2(const float* a, const float* b, uint32_t dim);
float vec_ip_distance_avx2(const float* a, const float* b, uint32_t dim);
#endif

#ifdef __ARM_NEON__
float vec_l2_distance_neon(const float* a, const float* b, uint32_t dim);
float vec_cosine_distance_neon(const float* a, const float* b, uint32_t dim);
float vec_ip_distance_neon(const float* a, const float* b, uint32_t dim);
#endif

vec_dist_func_t vec_get_dist_func(kanbudb_vec_metric_t metric);

#endif /* KANBUDB_VECTOR_DISTANCE_H */