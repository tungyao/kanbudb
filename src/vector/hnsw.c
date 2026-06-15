#include "hnsw.h"
#include "distance.h"
#include <math.h>
#include <float.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  Heap helpers for search                                            */
/* ------------------------------------------------------------------ */

typedef struct {
    float dist;
    int   node;
} hnsw_cand_t;

/* Min-heap for candidates */
static void cand_push(hnsw_cand_t* heap, int* size, float dist, int node)
{
    int i = (*size)++;
    heap[i].dist = dist;
    heap[i].node = node;
    while (i > 0) {
        int p = (i - 1) / 2;
        if (heap[p].dist <= heap[i].dist) break;
        hnsw_cand_t tmp = heap[p];
        heap[p] = heap[i];
        heap[i] = tmp;
        i = p;
    }
}

static hnsw_cand_t cand_pop(hnsw_cand_t* heap, int* size)
{
    hnsw_cand_t top = heap[0];
    heap[0] = heap[--(*size)];
    int i = 0;
    for (;;) {
        int smallest = i;
        int left = 2 * i + 1;
        int right = 2 * i + 2;
        if (left < *size && heap[left].dist < heap[smallest].dist)
            smallest = left;
        if (right < *size && heap[right].dist < heap[smallest].dist)
            smallest = right;
        if (smallest == i) break;
        hnsw_cand_t tmp = heap[i];
        heap[i] = heap[smallest];
        heap[smallest] = tmp;
        i = smallest;
    }
    return top;
}

/* Max-heap for result set (stores largest dist at root) */
static void res_push(hnsw_cand_t* heap, int* size, float dist, int node)
{
    int i = (*size)++;
    heap[i].dist = dist;
    heap[i].node = node;
    while (i > 0) {
        int p = (i - 1) / 2;
        if (heap[p].dist >= heap[i].dist) break;
        hnsw_cand_t tmp = heap[p];
        heap[p] = heap[i];
        heap[i] = tmp;
        i = p;
    }
}

static hnsw_cand_t res_peek(hnsw_cand_t* heap, int size)
{
    return heap[0];
}

static hnsw_cand_t res_pop(hnsw_cand_t* heap, int* size)
{
    hnsw_cand_t top = heap[0];
    heap[0] = heap[--(*size)];
    int i = 0;
    for (;;) {
        int largest = i;
        int left = 2 * i + 1;
        int right = 2 * i + 2;
        if (left < *size && heap[left].dist > heap[largest].dist)
            largest = left;
        if (right < *size && heap[right].dist > heap[largest].dist)
            largest = right;
        if (largest == i) break;
        hnsw_cand_t tmp = heap[i];
        heap[i] = heap[largest];
        heap[largest] = tmp;
        i = largest;
    }
    return top;
}

/* ------------------------------------------------------------------ */
/*  Level generation                                                    */
/* ------------------------------------------------------------------ */

static int hnsw_random_level(float rev_size)
{
    float r = (float)rand() / (float)RAND_MAX;
    if (r == 0.0f) r = 1e-10f;
    return (int)(-logf(r) * rev_size);
}

/* ------------------------------------------------------------------ */
/*  Layer allocation                                                    */
/* ------------------------------------------------------------------ */

static int hnsw_ensure_layers(hnsw_index_t* idx, int target_level)
{
    if (target_level <= idx->max_level) return 0;

    int old_count = idx->max_level + 1;
    int new_count = target_level + 1;

    int** new_links = realloc(idx->links, (size_t)new_count * sizeof(int*));
    if (!new_links) return KANBUDB_VEC_ERR_OOM;
    idx->links = new_links;

    for (int l = old_count; l < new_count; l++) {
        int max_nb = (l == 0) ? (idx->M_max0 + 1) : (idx->M_max + 1);
        idx->links[l] = calloc((size_t)idx->max_elements * max_nb, sizeof(int));
        if (!idx->links[l]) return KANBUDB_VEC_ERR_OOM;
    }

    int* new_lc = realloc(idx->link_counts,
                          (size_t)new_count * idx->max_elements * sizeof(int));
    if (!new_lc) return KANBUDB_VEC_ERR_OOM;
    memset(new_lc + (size_t)old_count * idx->max_elements, 0,
           (size_t)(new_count - old_count) * idx->max_elements * sizeof(int));
    idx->link_counts = new_lc;

    idx->max_level = target_level;
    return KANBUDB_VEC_OK;
}

/* ------------------------------------------------------------------ */
/*  Greedy search on a single layer (ef=1)                              */
/* ------------------------------------------------------------------ */

static int hnsw_greedy_search(hnsw_index_t* idx, int entry_point,
                               const float* query, int layer)
{
    int best_node = entry_point;
    float best_dist = idx->dist_func(query, idx->nodes[best_node].vector, idx->dim);

    int max_neighbors = (layer == 0) ? (idx->M_max0 + 1) : (idx->M_max + 1);

    int changed = 1;
    while (changed) {
        changed = 0;
        int node = best_node;
        int nb_count = idx->link_counts[(size_t)layer * idx->max_elements + node];
        int* nb_links = idx->links[layer] + (size_t)node * max_neighbors;

        for (int i = 0; i < nb_count; i++) {
            int neighbor = nb_links[i];
            if (idx->nodes[neighbor].deleted) continue;
            float d = idx->dist_func(query, idx->nodes[neighbor].vector, idx->dim);
            if (d < best_dist) {
                best_dist = d;
                best_node = neighbor;
                changed = 1;
            }
        }
    }

    return best_node;
}

/* ------------------------------------------------------------------ */
/*  Full search on a single layer (ef >= 1)                            */
/*  Fills result_nodes/result_dists sorted by distance ascending       */
/*  Returns number of results found                                    */
/* ------------------------------------------------------------------ */

static int hnsw_search_layer(hnsw_index_t* idx, int entry_point,
                              const float* query, int ef, int layer,
                              int* result_nodes, float* result_dists)
{
    idx->visit_tag++;
    if (idx->visit_tag <= 0) {
        memset(idx->visited, 0, (size_t)idx->max_elements * sizeof(int));
        idx->visit_tag = 1;
    }

    hnsw_cand_t* candidates = malloc(sizeof(hnsw_cand_t) * (size_t)idx->max_elements);
    hnsw_cand_t* results    = malloc(sizeof(hnsw_cand_t) * (size_t)(ef + 1));
    int cand_size = 0;
    int res_size  = 0;

    int max_neighbors = (layer == 0) ? (idx->M_max0 + 1) : (idx->M_max + 1);

    float ep_dist = idx->dist_func(query, idx->nodes[entry_point].vector, idx->dim);
    cand_push(candidates, &cand_size, ep_dist, entry_point);
    res_push(results, &res_size, ep_dist, entry_point);
    idx->visited[entry_point] = idx->visit_tag;

    while (cand_size > 0) {
        hnsw_cand_t cand = cand_pop(candidates, &cand_size);

        float worst_dist = (res_size < ef) ? FLT_MAX : res_peek(results, res_size).dist;
        if (cand.dist > worst_dist) break;

        int node = cand.node;
        int nb_count = idx->link_counts[(size_t)layer * idx->max_elements + node];
        int* nb_links = idx->links[layer] + (size_t)node * max_neighbors;

        for (int i = 0; i < nb_count; i++) {
            int neighbor = nb_links[i];
            if (idx->visited[neighbor] == idx->visit_tag) continue;
            if (idx->nodes[neighbor].deleted) continue;
            idx->visited[neighbor] = idx->visit_tag;

            float d = idx->dist_func(query, idx->nodes[neighbor].vector, idx->dim);

            cand_push(candidates, &cand_size, d, neighbor);

            if (res_size < ef || d < res_peek(results, res_size).dist) {
                res_push(results, &res_size, d, neighbor);
                if (res_size > ef)
                    res_pop(results, &res_size);
            }
        }
    }

    int n = res_size < ef ? res_size : ef;
    for (int i = 0; i < n; i++) {
        result_nodes[i] = results[i].node;
        result_dists[i] = results[i].dist;
    }

    /* Sort by distance ascending (insertion sort, fine for small ef) */
    for (int i = 1; i < n; i++) {
        float key_d = result_dists[i];
        int key_n = result_nodes[i];
        int j = i - 1;
        while (j >= 0 && result_dists[j] > key_d) {
            result_dists[j + 1] = result_dists[j];
            result_nodes[j + 1] = result_nodes[j];
            j--;
        }
        result_dists[j + 1] = key_d;
        result_nodes[j + 1] = key_n;
    }

    free(candidates);
    free(results);
    return n;
}

/* ------------------------------------------------------------------ */
/*  Simple neighbor selection: pick the M nearest by distance           */
/* ------------------------------------------------------------------ */

static void hnsw_select_neighbors_simple(const float* dists, const int* nodes,
                                          int count, int M,
                                          int* out_nodes, int* out_count)
{
    if (count == 0) { *out_count = 0; return; }

    int* idx = malloc(sizeof(int) * (size_t)count);
    for (int i = 0; i < count; i++) idx[i] = i;

    for (int i = 0; i < count - 1; i++) {
        for (int j = 0; j < count - i - 1; j++) {
            if (dists[idx[j]] > dists[idx[j + 1]]) {
                int tmp = idx[j]; idx[j] = idx[j + 1]; idx[j + 1] = tmp;
            }
        }
    }

    *out_count = count < M ? count : M;
    for (int i = 0; i < *out_count; i++)
        out_nodes[i] = nodes[idx[i]];

    free(idx);
}

/* ------------------------------------------------------------------ */
/*  Add bidirectional link between two nodes at a given layer           */
/*  Prunes existing node's connections if at capacity                   */
/* ------------------------------------------------------------------ */

static void hnsw_add_bidirectional_link(hnsw_index_t* idx, int src, int dst, int layer)
{
    int max_keep = (layer == 0) ? idx->M_max0 : idx->M_max;
    int max_neighbors = max_keep + 1;

    /* Connect src -> dst (src is new, always has room) */
    int* src_links = idx->links[layer] + (size_t)src * max_neighbors;
    int src_count = idx->link_counts[(size_t)layer * idx->max_elements + src];
    src_links[src_count] = dst;
    idx->link_counts[(size_t)layer * idx->max_elements + src] = src_count + 1;

    /* Connect dst -> src */
    int* dst_links = idx->links[layer] + (size_t)dst * max_neighbors;
    int dst_count = idx->link_counts[(size_t)layer * idx->max_elements + dst];

    if (dst_count < max_keep) {
        dst_links[dst_count] = src;
        idx->link_counts[(size_t)layer * idx->max_elements + dst] = dst_count + 1;
        return;
    }

    /* Prune: collect all neighbors + new, keep nearest max_keep */
    float* all_dists = malloc(sizeof(float) * (size_t)(dst_count + 1));
    int* all_nodes   = malloc(sizeof(int)   * (size_t)(dst_count + 1));

    for (int i = 0; i < dst_count; i++) {
        all_nodes[i] = dst_links[i];
        all_dists[i] = idx->dist_func(idx->nodes[dst].vector,
                                       idx->nodes[dst_links[i]].vector, idx->dim);
    }
    all_nodes[dst_count] = src;
    all_dists[dst_count] = idx->dist_func(idx->nodes[dst].vector,
                                           idx->nodes[src].vector, idx->dim);

    /* Sort by distance (bubble sort, M <= 32 so fine) */
    int total = dst_count + 1;
    for (int i = 0; i < total - 1; i++) {
        for (int j = 0; j < total - i - 1; j++) {
            if (all_dists[j] > all_dists[j + 1]) {
                float td = all_dists[j]; all_dists[j] = all_dists[j + 1]; all_dists[j + 1] = td;
                int   tn = all_nodes[j]; all_nodes[j] = all_nodes[j + 1]; all_nodes[j + 1] = tn;
            }
        }
    }

    for (int i = 0; i < max_keep; i++)
        dst_links[i] = all_nodes[i];
    idx->link_counts[(size_t)layer * idx->max_elements + dst] = max_keep;

    free(all_dists);
    free(all_nodes);
}

/* ------------------------------------------------------------------ */
/*  API: create                                                        */
/* ------------------------------------------------------------------ */

int hnsw_create(int dim, int M, int ef_construction, int ef_search,
                int max_elements, kanbudb_vec_metric_t metric, hnsw_index_t** out)
{
    if (!out || dim <= 0 || M <= 0 || max_elements <= 0)
        return KANBUDB_VEC_ERR_INVAL;

    hnsw_index_t* idx = calloc(1, sizeof(hnsw_index_t));
    if (!idx) return KANBUDB_VEC_ERR_OOM;

    idx->dim             = dim;
    idx->M               = M;
    idx->M_max           = M;
    idx->M_max0          = 2 * M;
    idx->ef_construction = ef_construction;
    idx->ef_search       = ef_search;
    idx->rev_size        = 1.0f / logf((float)M);
    idx->max_elements    = max_elements;
    idx->element_count   = 0;

    idx->vector_pool = calloc((size_t)dim * max_elements, sizeof(float));
    idx->nodes       = calloc((size_t)max_elements, sizeof(hnsw_node_t));
    idx->levels      = calloc((size_t)max_elements, sizeof(int));
    idx->visited     = calloc((size_t)max_elements, sizeof(int));

    if (!idx->vector_pool || !idx->nodes || !idx->levels || !idx->visited) {
        free(idx->vector_pool); free(idx->nodes); free(idx->levels);
        free(idx->visited); free(idx);
        return KANBUDB_VEC_ERR_OOM;
    }

    idx->enterpoint_idx = -1;
    idx->max_level      = -1;
    idx->visit_tag      = 0;
    idx->links          = NULL;
    idx->link_counts    = NULL;

    idx->dist_func = (float (*)(const float*, const float*, int))
                      vec_get_dist_func(metric);

    *out = idx;
    return KANBUDB_VEC_OK;
}

/* ------------------------------------------------------------------ */
/*  API: destroy                                                        */
/* ------------------------------------------------------------------ */

void hnsw_destroy(hnsw_index_t* idx)
{
    if (!idx) return;

    for (int l = 0; l <= idx->max_level; l++)
        free(idx->links[l]);
    free(idx->links);
    free(idx->link_counts);
    free(idx->vector_pool);
    free(idx->nodes);
    free(idx->levels);
    free(idx->visited);
    memset(idx, 0, sizeof(*idx));
    free(idx);
}

/* ------------------------------------------------------------------ */
/*  API: insert                                                        */
/* ------------------------------------------------------------------ */

int hnsw_insert(hnsw_index_t* idx, uint64_t id, const float* vector)
{
    if (!idx || !vector) return KANBUDB_VEC_ERR_INVAL;

    /* Check for existing (update) */
    for (int i = 0; i < idx->element_count; i++) {
        if (idx->nodes[i].id == id && !idx->nodes[i].deleted) {
            memcpy(idx->vector_pool + (size_t)i * idx->dim,
                   vector, (size_t)idx->dim * sizeof(float));
            return KANBUDB_VEC_OK;
        }
    }

    if (idx->element_count >= idx->max_elements)
        return KANBUDB_VEC_ERR_OOM;

    int slot = idx->element_count;

    memcpy(idx->vector_pool + (size_t)slot * idx->dim,
           vector, (size_t)idx->dim * sizeof(float));

    int node_level = hnsw_random_level(idx->rev_size);

    idx->nodes[slot].vector   = idx->vector_pool + (size_t)slot * idx->dim;
    idx->nodes[slot].id       = id;
    idx->nodes[slot].level    = node_level;
    idx->nodes[slot].deleted  = 0;
    idx->levels[slot]         = node_level;
    idx->element_count++;

    /* First element */
    if (idx->enterpoint_idx < 0) {
        hnsw_ensure_layers(idx, node_level);
        idx->enterpoint_idx = slot;
        return KANBUDB_VEC_OK;
    }

    /* Allocate layers up to the max needed */
    int needed_level = node_level > idx->max_level ? node_level : idx->max_level;
    hnsw_ensure_layers(idx, needed_level);

    int curr_ep = idx->enterpoint_idx;

    /* Greedy descent from max_level to level+1 */
    for (int lc = idx->max_level; lc > node_level; lc--)
        curr_ep = hnsw_greedy_search(idx, curr_ep, vector, lc);

    /* Search and connect from min(level, max_level) down to 0 */
    int search_top = node_level < idx->max_level ? node_level : idx->max_level;
    for (int lc = search_top; lc >= 0; lc--) {
        int* res_nodes = malloc(sizeof(int) * (size_t)(idx->ef_construction + 1));
        float* res_dists = malloc(sizeof(float) * (size_t)(idx->ef_construction + 1));
        if (!res_nodes || !res_dists) {
            free(res_nodes); free(res_dists);
            return KANBUDB_VEC_ERR_OOM;
        }

        int n_found = hnsw_search_layer(idx, curr_ep, vector,
                                         idx->ef_construction, lc,
                                         res_nodes, res_dists);

        int M_layer = (lc == 0) ? idx->M_max0 : idx->M_max;
        int n_select = n_found < M_layer ? n_found : M_layer;

        for (int i = 0; i < n_select; i++)
            hnsw_add_bidirectional_link(idx, slot, res_nodes[i], lc);

        free(res_nodes);
        free(res_dists);

        curr_ep = slot;
    }

    /* Update entry point if needed */
    if (node_level > idx->max_level) {
        idx->enterpoint_idx = slot;
        idx->max_level = node_level;
    }

    return KANBUDB_VEC_OK;
}

/* ------------------------------------------------------------------ */
/*  API: search                                                        */
/* ------------------------------------------------------------------ */

int hnsw_search(const hnsw_index_t* idx, const float* query, int k,
                 kanbudb_vec_result_t* results)
{
    if (!idx || !query || !results || k <= 0)
        return 0;

    if (idx->enterpoint_idx < 0)
        return 0;

    hnsw_index_t* non_const = (hnsw_index_t*)idx;
    int curr_ep = idx->enterpoint_idx;

    /* Descend from max_level to 1 using greedy search */
    for (int lc = idx->max_level; lc >= 1; lc--)
        curr_ep = hnsw_greedy_search(non_const, curr_ep, query, lc);

    /* Search at layer 0 with ef_search */
    int ef = idx->ef_search < k ? k : idx->ef_search;
    int* res_nodes = malloc(sizeof(int) * (size_t)ef);
    float* res_dists = malloc(sizeof(float) * (size_t)ef);
    if (!res_nodes || !res_dists) {
        free(res_nodes); free(res_dists);
        return 0;
    }

    int n_found = hnsw_search_layer(non_const, curr_ep, query, ef, 0,
                                     res_nodes, res_dists);

    int n_return = n_found < k ? n_found : k;
    for (int i = 0; i < n_return; i++) {
        results[i].id       = idx->nodes[res_nodes[i]].id;
        results[i].distance = res_dists[i];
    }

    free(res_nodes);
    free(res_dists);
    return n_return;
}

/* ------------------------------------------------------------------ */
/*  API: delete                                                        */
/* ------------------------------------------------------------------ */

int hnsw_delete(hnsw_index_t* idx, uint64_t id)
{
    if (!idx) return KANBUDB_VEC_ERR_INVAL;

    for (int i = 0; i < idx->element_count; i++) {
        if (idx->nodes[i].id == id && !idx->nodes[i].deleted) {
            idx->nodes[i].deleted = 1;

            if (i == idx->enterpoint_idx) {
                idx->enterpoint_idx = -1;
                for (int j = 0; j < idx->element_count; j++) {
                    if (!idx->nodes[j].deleted) {
                        idx->enterpoint_idx = j;
                        break;
                    }
                }
            }
            return KANBUDB_VEC_OK;
        }
    }
    return KANBUDB_VEC_ERR_NOTFOUND;
}

/* ------------------------------------------------------------------ */
/*  API: get                                                           */
/* ------------------------------------------------------------------ */

int hnsw_get(const hnsw_index_t* idx, uint64_t id, float* out_vector)
{
    if (!idx || !out_vector) return KANBUDB_VEC_ERR_INVAL;

    for (int i = 0; i < idx->element_count; i++) {
        if (idx->nodes[i].id == id && !idx->nodes[i].deleted) {
            memcpy(out_vector, idx->vector_pool + (size_t)i * idx->dim,
                   (size_t)idx->dim * sizeof(float));
            return KANBUDB_VEC_OK;
        }
    }
    return KANBUDB_VEC_ERR_NOTFOUND;
}

/* ------------------------------------------------------------------ */
/*  API: count                                                         */
/* ------------------------------------------------------------------ */

int hnsw_count(const hnsw_index_t* idx)
{
    if (!idx) return 0;

    int c = 0;
    for (int i = 0; i < idx->element_count; i++)
        if (!idx->nodes[i].deleted) c++;
    return c;
}
