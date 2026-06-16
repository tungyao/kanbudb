#ifndef KANBUDB_HYBRID_H
#define KANBUDB_HYBRID_H

#include <stddef.h>
#include <stdint.h>
#include "vector.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    KANBUDB_HYBRID_RRF = 0,
    KANBUDB_HYBRID_WEIGHTED = 1
} kanbudb_hybrid_mode_t;

typedef struct {
    kanbudb_hybrid_mode_t mode;
    double vec_weight;
    double fts_weight;
    uint32_t k;
} kanbudb_hybrid_params_t;

#define KANBUDB_HYBRID_PARAMS_DEFAULT \
    { .mode = KANBUDB_HYBRID_RRF, .vec_weight = 0.5, .fts_weight = 0.5, .k = 10 }

typedef struct {
    uint64_t id;
    double   score;
    double   vec_distance;
    double   fts_score;
} kanbudb_hybrid_result_t;

int kanbudb_hybrid_search(
    const kanbudb_vec_result_t* vec_results, int vec_count,
    const uint64_t* fts_ids, const double* fts_scores, int fts_count,
    const kanbudb_hybrid_params_t* params,
    kanbudb_hybrid_result_t* results, int max_results);

#ifdef __cplusplus
}
#endif

#endif /* KANBUDB_HYBRID_H */
