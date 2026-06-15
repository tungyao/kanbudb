#ifndef KANBUDB_HNSW_H
#define KANBUDB_HNSW_H

#include <stddef.h>
#include <stdint.h>
#include "vector.h"

#ifdef __cplusplus
extern "C" {
#endif

/* HNSW node */
typedef struct hnsw_node {
    float*   vector;        /* Pointer into vector_pool */
    uint64_t id;            /* User-provided ID */
    int      level;         /* Layer of this node (0 = base) */
    int      deleted;       /* Soft delete flag */
} hnsw_node_t;

/* HNSW index */
typedef struct {
    /* Parameters */
    int      dim;
    int      M;              /* Max connections per layer */
    int      M_max;          /* Max connections for upper layers (M) */
    int      M_max0;         /* Max connections for layer 0 (2*M) */
    int      ef_construction;
    int      ef_search;
    float    rev_size;       /* 1/ln(M) for level generation */

    /* Data storage */
    float*   vector_pool;    /* dim * max_elements */
    hnsw_node_t* nodes;      /* Array of nodes */
    int*     levels;          /* level for each slot */
    int      max_elements;
    int      element_count;

    /* Graph structure:
     * links[layer][node_idx] = array of neighbor indices
     * We store as flat array: for each node, M_max0+1 neighbors at layer 0,
     * and M_max+1 neighbors at layer > 0 */
    int**   links;           /* links[layer] = flat array of neighbors */
    int*    link_counts;      /* Per-layer link counts stored per node */

    /* Entry point */
    int      enterpoint_idx;
    int      max_level;

    /* Visited array for search */
    int*    visited;
    int     visit_tag;

    /* Distance function */
    float (*dist_func)(const float*, const float*, int);
} hnsw_index_t;

/* API */
int  hnsw_create(int dim, int M, int ef_construction, int ef_search,
                 int max_elements, kanbudb_vec_metric_t metric, hnsw_index_t** out);
void hnsw_destroy(hnsw_index_t* idx);
int  hnsw_insert(hnsw_index_t* idx, uint64_t id, const float* vector);
int  hnsw_search(const hnsw_index_t* idx, const float* query, int k,
                 kanbudb_vec_result_t* results);
int  hnsw_delete(hnsw_index_t* idx, uint64_t id);
int  hnsw_get(const hnsw_index_t* idx, uint64_t id, float* out_vector);
int  hnsw_count(const hnsw_index_t* idx);

#ifdef __cplusplus
}
#endif

#endif /* KANBUDB_HNSW_H */
