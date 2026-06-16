#include "vec_filter.h"
#include "vector_internal.h"
#include "hnsw.h"
#include "distance.h"
#include <stdlib.h>
#include <string.h>

/* Min-heap for top-K filtered search */
typedef struct {
    float    dist;
    uint64_t id;
} filtered_heap_t;

static void fheap_push(filtered_heap_t* heap, uint32_t k, float dist, uint64_t id)
{
    if (heap[0].dist >= 0.0f && heap[0].dist <= dist) return;
    heap[0].dist = dist;
    heap[0].id = id;
    uint32_t i = 0;
    for (;;) {
        uint32_t smallest = i;
        uint32_t left = 2 * i + 1;
        uint32_t right = 2 * i + 2;
        if (left < k && heap[left].dist < heap[smallest].dist) smallest = left;
        if (right < k && heap[right].dist < heap[smallest].dist) smallest = right;
        if (smallest == i) break;
        filtered_heap_t tmp = heap[i];
        heap[i] = heap[smallest];
        heap[smallest] = tmp;
        i = smallest;
    }
}

static int cmp_fresult(const void* a, const void* b)
{
    float da = ((const kanbudb_vec_result_t*)a)->distance;
    float db = ((const kanbudb_vec_result_t*)b)->distance;
    return (da > db) - (da < db);
}

/* Flat filtered search: scan all vectors, apply filter, collect top-K */
static int flat_search_filtered(struct kanbudb_vec_index* idx,
                                const float* query, uint32_t k,
                                kanbudb_vec_filter_fn filter, void* filter_ctx,
                                kanbudb_vec_result_t* results)
{
    uint32_t dim = idx->params.dimension;
    uint32_t active = 0;
    for (uint64_t i = 0; i < idx->flat_count; i++)
        if (!idx->flat_deleted[i]) active++;
    if (active == 0 || k > active) k = active;
    if (k == 0) return 0;

    filtered_heap_t* heap = (filtered_heap_t*)calloc(k, sizeof(filtered_heap_t));
    if (!heap) return -1;
    for (uint32_t i = 0; i < k; i++) heap[i].dist = -1.0f;

    uint32_t found = 0;
    for (uint64_t i = 0; i < idx->flat_count; i++) {
        if (idx->flat_deleted[i]) continue;
        if (filter && !filter(idx->flat_ids[i], filter_ctx)) continue;

        float d = idx->dist_func(query, idx->flat_vectors + i * dim, dim);
        if (found < k) {
            heap[found].dist = d;
            heap[found].id = idx->flat_ids[i];
            found++;
            if (found == k) {
                /* Build heap */
                for (int32_t j = (int32_t)(k / 2) - 1; j >= 0; j--) {
                    uint32_t p = (uint32_t)j;
                    for (;;) {
                        uint32_t s = p;
                        uint32_t l = 2 * p + 1;
                        uint32_t r = 2 * p + 2;
                        if (l < k && heap[l].dist < heap[s].dist) s = l;
                        if (r < k && heap[r].dist < heap[s].dist) s = r;
                        if (s == p) break;
                        filtered_heap_t t = heap[p]; heap[p] = heap[s]; heap[s] = t;
                        p = s;
                    }
                }
            }
        } else {
            fheap_push(heap, k, d, idx->flat_ids[i]);
        }
    }

    for (uint32_t i = 0; i < found; i++) {
        results[i].id = heap[i].id;
        results[i].distance = heap[i].dist;
    }
    qsort(results, found, sizeof(kanbudb_vec_result_t), cmp_fresult);
    free(heap);
    return (int)found;
}

/* HNSW filtered search: use wider ef_search + post-filter */
static int hnsw_search_filtered(struct kanbudb_vec_index* idx,
                                const float* query, uint32_t k,
                                kanbudb_vec_filter_fn filter, void* filter_ctx,
                                kanbudb_vec_result_t* results)
{
    hnsw_index_t* hnsw = (hnsw_index_t*)idx->algo_data;

    /* Use ef_search * 4 to get enough candidates after filtering */
    int ef = (int)k * 4;
    if (ef < (int)hnsw->ef_search) ef = hnsw->ef_search;
    if (ef > hnsw->element_count) ef = hnsw->element_count;
    if (ef == 0) return 0;

    int* res_nodes = (int*)malloc((size_t)ef * sizeof(int));
    float* res_dists = (float*)malloc((size_t)ef * sizeof(float));
    if (!res_nodes || !res_dists) { free(res_nodes); free(res_dists); return -1; }

    /* Run HNSW search with wider ef */
    int n_found = hnsw_search(hnsw, query, ef, (kanbudb_vec_result_t*)NULL);
    /* We need to call hnsw_search_layer directly since hnsw_search returns
       results as kanbudb_vec_result_t but we need node indices for filtering.
       Instead, let's do the search inline. */
    
    /* Actually, let's use the public API and post-filter */
    kanbudb_vec_result_t* all_results = (kanbudb_vec_result_t*)malloc((size_t)ef * sizeof(kanbudb_vec_result_t));
    if (!all_results) { free(res_nodes); free(res_dists); return -1; }

    n_found = hnsw_search(hnsw, query, ef, all_results);

    /* Post-filter */
    int out = 0;
    for (int i = 0; i < n_found && out < (int)k; i++) {
        if (filter && !filter(all_results[i].id, filter_ctx)) continue;
        results[out++] = all_results[i];
    }

    free(all_results);
    free(res_nodes);
    free(res_dists);
    return out;
}

/* Public API */
int kanbudb_vec_search_filtered(kanbudb_vec_index_t* idx,
                                const float* query, uint32_t k,
                                kanbudb_vec_filter_fn filter,
                                void* filter_ctx,
                                kanbudb_vec_result_t* results)
{
    if (!idx || !query || !results || k == 0) return -1;

    pthread_rwlock_rdlock(&idx->rwlock);

    int rc;
    if (idx->algo_type == 1) {
        rc = hnsw_search_filtered(idx, query, k, filter, filter_ctx, results);
    } else {
        rc = flat_search_filtered(idx, query, k, filter, filter_ctx, results);
    }

    pthread_rwlock_unlock(&idx->rwlock);
    return rc;
}
