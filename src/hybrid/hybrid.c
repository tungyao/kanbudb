#include "hybrid.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint64_t id;
    double   vec_distance;
    double   fts_score;
    double   combined;
    int      has_vec;
    int      has_fts;
    int      vec_rank;
    int      fts_rank;
} hybrid_accum_t;

static int cmp_accum_by_score(const void* a, const void* b) {
    double sa = ((const hybrid_accum_t*)b)->combined;
    double sb = ((const hybrid_accum_t*)a)->combined;
    return (sa > sb) - (sa < sb);
}

static int find_accum(hybrid_accum_t* acc, int count, uint64_t id) {
    for (int i = 0; i < count; i++) {
        if (acc[i].id == id) return i;
    }
    return -1;
}

int kanbudb_hybrid_search(
    const kanbudb_vec_result_t* vec_results, int vec_count,
    const uint64_t* fts_ids, const double* fts_scores, int fts_count,
    const kanbudb_hybrid_params_t* params,
    kanbudb_hybrid_result_t* results, int max_results)
{
    if (!params || !results || max_results <= 0) return 0;
    if ((!vec_results || vec_count <= 0) && (!fts_ids || fts_count <= 0)) return 0;

    int cap = (vec_count > fts_count ? vec_count : fts_count) * 2;
    if (cap < 64) cap = 64;
    hybrid_accum_t* acc = (hybrid_accum_t*)calloc((size_t)cap, sizeof(hybrid_accum_t));
    if (!acc) return 0;
    int acc_count = 0;

    for (int i = 0; i < vec_count; i++) {
        uint64_t id = vec_results[i].id;
        int idx = find_accum(acc, acc_count, id);
        if (idx < 0) {
            if (acc_count >= cap) {
                cap *= 2;
                hybrid_accum_t* na = (hybrid_accum_t*)realloc(acc, (size_t)cap * sizeof(hybrid_accum_t));
                if (!na) { free(acc); return 0; }
                acc = na;
                memset(acc + acc_count, 0, (size_t)(cap - acc_count) * sizeof(hybrid_accum_t));
            }
            idx = acc_count++;
            acc[idx].id = id;
        }
        acc[idx].vec_distance = (double)vec_results[i].distance;
        acc[idx].vec_rank = i + 1;
        acc[idx].has_vec = 1;
    }

    for (int i = 0; i < fts_count; i++) {
        uint64_t id = fts_ids[i];
        int idx = find_accum(acc, acc_count, id);
        if (idx < 0) {
            if (acc_count >= cap) {
                cap *= 2;
                hybrid_accum_t* na = (hybrid_accum_t*)realloc(acc, (size_t)cap * sizeof(hybrid_accum_t));
                if (!na) { free(acc); return 0; }
                acc = na;
                memset(acc + acc_count, 0, (size_t)(cap - acc_count) * sizeof(hybrid_accum_t));
            }
            idx = acc_count++;
            acc[idx].id = id;
        }
        acc[idx].fts_score = fts_scores[i];
        acc[idx].fts_rank = i + 1;
        acc[idx].has_fts = 1;
    }

    /* Compute combined scores */
    for (int i = 0; i < acc_count; i++) {
        double score = 0.0;
        if (params->mode == KANBUDB_HYBRID_RRF) {
            const double k = 60.0;
            if (acc[i].has_vec)
                score += params->vec_weight * (1.0 / (k + (double)acc[i].vec_rank));
            if (acc[i].has_fts)
                score += params->fts_weight * (1.0 / (k + (double)acc[i].fts_rank));
        } else {
            if (acc[i].has_vec) {
                double nd = acc[i].vec_distance;
                if (nd > 1.0) nd = 1.0;
                score += params->vec_weight * (1.0 - nd);
            }
            if (acc[i].has_fts)
                score += params->fts_weight * acc[i].fts_score;
        }
        acc[i].combined = score;
    }

    qsort(acc, (size_t)acc_count, sizeof(hybrid_accum_t), cmp_accum_by_score);

    int n = acc_count < max_results ? acc_count : max_results;
    for (int i = 0; i < n; i++) {
        results[i].id = acc[i].id;
        results[i].score = acc[i].combined;
        results[i].vec_distance = acc[i].vec_distance;
        results[i].fts_score = acc[i].fts_score;
    }

    free(acc);
    return n;
}
