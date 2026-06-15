#include "vector.h"
#include "vector_internal.h"
#include "distance.h"
#include "persistence.h"
#include "hnsw.h"

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/stat.h>

/* ------------------------------------------------------------------ */
/*  Internal helpers                                                   */
/* ------------------------------------------------------------------ */

static int vec_grow(struct kanbudb_vec_index* idx)
{
    uint64_t new_cap = idx->flat_capacity ? idx->flat_capacity * 2 : 1024;
    if (idx->flat_capacity >= (SIZE_MAX / sizeof(float)) / idx->params.dimension)
        return KANBUDB_VEC_ERR_OOM;

    float* v = realloc(idx->flat_vectors,
                       (size_t)new_cap * idx->params.dimension * sizeof(float));
    uint64_t* ids = realloc(idx->flat_ids, (size_t)new_cap * sizeof(uint64_t));
    uint8_t* del = realloc(idx->flat_deleted, (size_t)new_cap);
    if (!v || !ids || !del) {
        free(v); free(ids); free(del);
        return KANBUDB_VEC_ERR_OOM;
    }
    memset(del + idx->flat_capacity, 0, (size_t)(new_cap - idx->flat_capacity));
    idx->flat_vectors  = v;
    idx->flat_ids      = ids;
    idx->flat_deleted  = del;
    idx->flat_capacity = new_cap;
    return KANBUDB_VEC_OK;
}

static int vec_find_idx(const struct kanbudb_vec_index* idx, uint64_t id)
{
    for (uint64_t i = 0; i < idx->flat_count; i++)
        if (idx->flat_ids[i] == id && !idx->flat_deleted[i])
            return (int)i;
    return -1;
}

/* Simple min-heap for top-K */
typedef struct {
    float    dist;
    uint64_t id;
} heap_entry_t;

static void heap_push(heap_entry_t* heap, uint32_t k, float dist, uint64_t id)
{
    if (heap[0].dist >= 0.0f && heap[0].dist <= dist)
        return;
    heap[0].dist = dist;
    heap[0].id   = id;
    /* Sift down */
    uint32_t i = 0;
    for (;;) {
        uint32_t smallest = i;
        uint32_t left = 2 * i + 1;
        uint32_t right = 2 * i + 2;
        if (left < k && heap[left].dist < heap[smallest].dist)
            smallest = left;
        if (right < k && heap[right].dist < heap[smallest].dist)
            smallest = right;
        if (smallest == i) break;
        heap_entry_t tmp = heap[i];
        heap[i] = heap[smallest];
        heap[smallest] = tmp;
        i = smallest;
    }
}

/* Sort results by distance (ascending) */
static int cmp_result(const void* a, const void* b)
{
    float da = ((const kanbudb_vec_result_t*)a)->distance;
    float db = ((const kanbudb_vec_result_t*)b)->distance;
    return (da > db) - (da < db);
}

/* ------------------------------------------------------------------ */
/*  Background flush thread                                            */
/* ------------------------------------------------------------------ */

static void* vec_flush_worker(void* arg)
{
    kanbudb_vec_index_t* idx = (kanbudb_vec_index_t*)arg;
    while (idx->flush_thread_running) {
        usleep((useconds_t)(idx->flush_interval_ms * 1000));
        if (!idx->flush_thread_running) break;
        if (idx->dirty) {
            kanbudb_vec_flush(idx);
        }
    }
    return NULL;
}

int kanbudb_vec_flush_interval(kanbudb_vec_index_t* idx, int interval_ms)
{
    if (!idx) return KANBUDB_VEC_ERR_INVAL;

    if (interval_ms <= 0) {
        if (idx->flush_thread_running) {
            idx->flush_thread_running = 0;
            pthread_join(idx->flush_thread, NULL);
        }
        idx->flush_interval_ms = 0;
        return KANBUDB_VEC_OK;
    }

    if (idx->flush_thread_running) {
        idx->flush_interval_ms = interval_ms;
        return KANBUDB_VEC_OK;
    }

    idx->flush_interval_ms = interval_ms;
    idx->flush_thread_running = 1;
    if (pthread_create(&idx->flush_thread, NULL, vec_flush_worker, idx) != 0) {
        idx->flush_thread_running = 0;
        return KANBUDB_VEC_ERR_OOM;
    }
    return KANBUDB_VEC_OK;
}

/* ------------------------------------------------------------------ */
/*  Lifecycle                                                          */
/* ------------------------------------------------------------------ */

int kanbudb_vec_create(const char* path, const kanbudb_vec_params_t* params,
                       kanbudb_vec_index_t** out)
{
    if (!params || !out || params->dimension == 0)
        return KANBUDB_VEC_ERR_INVAL;

    struct kanbudb_vec_index* idx = calloc(1, sizeof(*idx));
    if (!idx) return KANBUDB_VEC_ERR_OOM;

    idx->params = *params;
    idx->dist_func = vec_get_dist_func(params->metric);

    if (params->algo == KANBUDB_VEC_ALGO_HNSW) {
        int max_elems = params->initial_capacity > 0
                        ? (int)params->initial_capacity : 4096*1024;
        int rc = hnsw_create((int)params->dimension, (int)params->M,
                             (int)params->ef_construction, (int)params->ef_search,
                             max_elems, params->metric,
                             (hnsw_index_t**)&idx->algo_data);
        if (rc != 0) { free(idx); return rc; }
        idx->algo_type = 1;
    } else {
        idx->algo_type = 0;
        if (params->initial_capacity > 0) {
            idx->flat_vectors = calloc((size_t)params->initial_capacity *
                                  params->dimension, sizeof(float));
            idx->flat_ids     = calloc((size_t)params->initial_capacity, sizeof(uint64_t));
            idx->flat_deleted = calloc((size_t)params->initial_capacity, 1);
            if (!idx->flat_vectors || !idx->flat_ids || !idx->flat_deleted) {
                free(idx->flat_vectors); free(idx->flat_ids); free(idx->flat_deleted);
                free(idx); return KANBUDB_VEC_ERR_OOM;
            }
            idx->flat_capacity = params->initial_capacity;
        }
    }

    pthread_rwlock_init(&idx->rwlock, NULL);

    if (path && params->enable_persistence) {
        idx->path = strdup(path);
        if (!idx->path) { kanbudb_vec_close(idx); return KANBUDB_VEC_ERR_OOM; }
        mkdir(path, 0755);
        int rc2 = vec_wal_open(idx);
        if (rc2 != KANBUDB_VEC_OK) { kanbudb_vec_close(idx); return rc2; }
    }

    *out = idx;
    return KANBUDB_VEC_OK;
}

int kanbudb_vec_open(const char* path, kanbudb_vec_index_t** out)
{
    return vec_persist_read(path, out);
}

int kanbudb_vec_close(kanbudb_vec_index_t* idx)
{
    if (!idx) return KANBUDB_VEC_ERR_INVAL;

    /* Stop background flush thread */
    if (idx->flush_thread_running) {
        idx->flush_thread_running = 0;
        pthread_join(idx->flush_thread, NULL);
    }

    if (idx->dirty && idx->path)
        kanbudb_vec_flush(idx);

    vec_wal_close(idx);
    pthread_rwlock_destroy(&idx->rwlock);

    if (idx->algo_type == 1) {
        hnsw_destroy((hnsw_index_t*)idx->algo_data);
    } else {
        free(idx->flat_vectors);
        free(idx->flat_ids);
        free(idx->flat_deleted);
    }

    free(idx->path);
    memset(idx, 0, sizeof(*idx));
    free(idx);
    return KANBUDB_VEC_OK;
}

int kanbudb_vec_flush(kanbudb_vec_index_t* idx)
{
    if (!idx || !idx->path) return KANBUDB_VEC_ERR_INVAL;
    pthread_rwlock_wrlock(&idx->rwlock);
    int rc = vec_checkpoint(idx);
    pthread_rwlock_unlock(&idx->rwlock);
    return rc;
}

int kanbudb_vec_destroy(const char* path)
{
    if (!path) return KANBUDB_VEC_ERR_INVAL;
    /* Remove all files in the index directory */
    char buf[1024];
    snprintf(buf, sizeof(buf), "%s/meta", path);    unlink(buf);
    snprintf(buf, sizeof(buf), "%s/vectors", path);  unlink(buf);
    snprintf(buf, sizeof(buf), "%s/ids", path);      unlink(buf);
    snprintf(buf, sizeof(buf), "%s/wal", path);      unlink(buf);
    rmdir(path);
    return KANBUDB_VEC_OK;
}

/* ------------------------------------------------------------------ */
/*  Data operations                                                    */
/* ------------------------------------------------------------------ */

int kanbudb_vec_insert(kanbudb_vec_index_t* idx, uint64_t id,
                       const float* vector)
{
    if (!idx || !vector) return KANBUDB_VEC_ERR_INVAL;

    pthread_rwlock_wrlock(&idx->rwlock);

    int rc = KANBUDB_VEC_OK;

    if (idx->algo_type == 1) {
        rc = hnsw_insert((hnsw_index_t*)idx->algo_data, id, vector);
        if (rc == 0) {
            idx->dirty = 1;
            if (idx->wal_fp) vec_wal_append_insert(idx, id, vector);
        }
        goto done;
    }

    /* Check for existing (update) */
    int existing = vec_find_idx(idx, id);
    if (existing >= 0) {
        memcpy(idx->flat_vectors + (size_t)existing * idx->params.dimension,
               vector, (size_t)idx->params.dimension * sizeof(float));
        idx->flat_deleted[existing] = 0;
        idx->dirty = 1;
        if (idx->wal_fp) vec_wal_append_insert(idx, id, vector);
        goto done;
    }

    /* Grow if needed */
    if (idx->flat_count >= idx->flat_capacity) {
        rc = vec_grow(idx);
        if (rc != KANBUDB_VEC_OK) goto done;
    }

    /* Append */
    memcpy(idx->flat_vectors + (size_t)idx->flat_count * idx->params.dimension,
           vector, (size_t)idx->params.dimension * sizeof(float));
    idx->flat_ids[idx->flat_count] = id;
    idx->flat_deleted[idx->flat_count] = 0;
    idx->flat_count++;
    idx->dirty = 1;
    if (idx->wal_fp) rc = vec_wal_append_insert(idx, id, vector);

done:
    /* Auto-checkpoint if WAL exceeds limit */
    if (rc == KANBUDB_VEC_OK && idx->wal_bytes >= WAL_AUTO_CHECKPOINT_SIZE
        && idx->path) {
        vec_checkpoint(idx);
    }
    pthread_rwlock_unlock(&idx->rwlock);
    return rc;
}

int kanbudb_vec_insert_batch(kanbudb_vec_index_t* idx, uint32_t count,
                             const uint64_t* ids, const float* vectors)
{
    if (!idx || !ids || !vectors || count == 0)
        return KANBUDB_VEC_ERR_INVAL;

    pthread_rwlock_wrlock(&idx->rwlock);

    int rc = KANBUDB_VEC_OK;

    if (idx->algo_type == 1) {
        for (uint32_t i = 0; i < count; i++) {
            rc = hnsw_insert((hnsw_index_t*)idx->algo_data, ids[i],
                             vectors + (size_t)i * idx->params.dimension);
            if (rc != 0) break;
            if (idx->wal_fp)
                vec_wal_append_insert(idx, ids[i],
                                      vectors + (size_t)i * idx->params.dimension);
        }
        if (rc == 0) idx->dirty = 1;
        goto done;
    }

    /* Ensure capacity */
    while (idx->flat_count + count > idx->flat_capacity) {
        rc = vec_grow(idx);
        if (rc != KANBUDB_VEC_OK) goto done;
    }

    /* Copy vectors */
    memcpy(idx->flat_vectors + (size_t)idx->flat_count * idx->params.dimension,
           vectors, (size_t)count * idx->params.dimension * sizeof(float));
    memcpy(idx->flat_ids + idx->flat_count, ids, (size_t)count * sizeof(uint64_t));
    memset(idx->flat_deleted + idx->flat_count, 0, count);
    idx->flat_count += count;
    idx->dirty = 1;
    if (idx->wal_fp) {
        for (uint32_t i = 0; i < count; i++) {
            rc = vec_wal_append_insert(idx, ids[i],
                                       vectors + (size_t)i * idx->params.dimension);
            if (rc != KANBUDB_VEC_OK) break;
        }
    }

done:
    pthread_rwlock_unlock(&idx->rwlock);
    return rc;
}

int kanbudb_vec_delete(kanbudb_vec_index_t* idx, uint64_t id)
{
    if (!idx) return KANBUDB_VEC_ERR_INVAL;

    pthread_rwlock_wrlock(&idx->rwlock);

    int rc;
    if (idx->algo_type == 1) {
        rc = hnsw_delete((hnsw_index_t*)idx->algo_data, id);
        if (rc == 0) {
            idx->dirty = 1;
            if (idx->wal_fp) vec_wal_append_delete(idx, id);
        }
    } else {
        int pos = vec_find_idx(idx, id);
        if (pos < 0) {
            pthread_rwlock_unlock(&idx->rwlock);
            return KANBUDB_VEC_ERR_NOTFOUND;
        }
        idx->flat_deleted[pos] = 1;
        idx->dirty = 1;
        if (idx->wal_fp) vec_wal_append_delete(idx, id);
        rc = KANBUDB_VEC_OK;
    }

    pthread_rwlock_unlock(&idx->rwlock);
    return rc;
}

int kanbudb_vec_get(kanbudb_vec_index_t* idx, uint64_t id,
                    float* out_vector)
{
    if (!idx || !out_vector) return KANBUDB_VEC_ERR_INVAL;

    pthread_rwlock_rdlock(&idx->rwlock);

    int rc;
    if (idx->algo_type == 1) {
        rc = hnsw_get((const hnsw_index_t*)idx->algo_data, id, out_vector);
    } else {
        int pos = vec_find_idx(idx, id);
        if (pos < 0) {
            pthread_rwlock_unlock(&idx->rwlock);
            return KANBUDB_VEC_ERR_NOTFOUND;
        }
        memcpy(out_vector, idx->flat_vectors + (size_t)pos * idx->params.dimension,
               (size_t)idx->params.dimension * sizeof(float));
        rc = KANBUDB_VEC_OK;
    }

    pthread_rwlock_unlock(&idx->rwlock);
    return rc;
}

/* ------------------------------------------------------------------ */
/*  Search                                                             */
/* ------------------------------------------------------------------ */

int kanbudb_vec_search(kanbudb_vec_index_t* idx, const float* query,
                       uint32_t k, kanbudb_vec_result_t* results)
{
    if (!idx || !query || !results || k == 0)
        return KANBUDB_VEC_ERR_INVAL;

    pthread_rwlock_rdlock(&idx->rwlock);

    if (idx->algo_type == 1) {
        int rc = hnsw_search((const hnsw_index_t*)idx->algo_data, query, (int)k,
                             results);
        pthread_rwlock_unlock(&idx->rwlock);
        return rc;
    }

    uint32_t active = 0;
    for (uint64_t i = 0; i < idx->flat_count; i++)
        if (!idx->flat_deleted[i]) active++;

    if (active == 0 || k > active) k = active;

    /* Use a max-heap (stored as min-heap of negative distances) for top-K */
    heap_entry_t* heap = NULL;
    if (k > 0) {
        heap = calloc((size_t)k, sizeof(heap_entry_t));
        if (!heap) { pthread_rwlock_unlock(&idx->rwlock); return KANBUDB_VEC_ERR_OOM; }
        for (uint32_t i = 0; i < k; i++)
            heap[i].dist = -1.0f; /* sentinel, will be replaced */
    }

    uint32_t dim = idx->params.dimension;
    uint32_t found = 0;

    for (uint64_t i = 0; i < idx->flat_count; i++) {
        if (idx->flat_deleted[i]) continue;
        float d = idx->dist_func(query, idx->flat_vectors + (size_t)i * dim, dim);
        if (found < k) {
            heap[found].dist = d;
            heap[found].id   = idx->flat_ids[i];
            found++;
            if (found == k) {
                /* Build heap */
                for (int32_t j = (int32_t)(k / 2) - 1; j >= 0; j--) {
                    uint32_t p = (uint32_t)j;
                    for (;;) {
                        uint32_t smallest = p;
                        uint32_t left = 2 * p + 1;
                        uint32_t right = 2 * p + 2;
                        if (left < k && heap[left].dist < heap[smallest].dist)
                            smallest = left;
                        if (right < k && heap[right].dist < heap[smallest].dist)
                            smallest = right;
                        if (smallest == p) break;
                        heap_entry_t tmp = heap[p];
                        heap[p] = heap[smallest];
                        heap[smallest] = tmp;
                        p = smallest;
                    }
                }
            }
        } else {
            heap_push(heap, k, d, idx->flat_ids[i]);
        }
    }

    /* Extract results sorted by distance */
    for (uint32_t i = 0; i < found; i++) {
        results[i].id       = heap[i].id;
        results[i].distance = heap[i].dist;
    }
    qsort(results, (size_t)found, sizeof(kanbudb_vec_result_t), cmp_result);

    free(heap);
    pthread_rwlock_unlock(&idx->rwlock);
    return (int)found;
}

int kanbudb_vec_search_radius(kanbudb_vec_index_t* idx,
                              const float* query, float radius,
                              kanbudb_vec_result_t* results,
                              uint32_t max_results)
{
    if (!idx || !query || !results)
        return KANBUDB_VEC_ERR_INVAL;

    pthread_rwlock_rdlock(&idx->rwlock);

    if (idx->algo_type == 1) {
        int n = hnsw_search((const hnsw_index_t*)idx->algo_data, query,
                            (int)max_results, results);
        uint32_t filtered = 0;
        for (int i = 0; i < n; i++) {
            if (results[i].distance <= radius) {
                if (filtered < max_results)
                    results[filtered++] = results[i];
            }
        }
        qsort(results, (size_t)filtered, sizeof(kanbudb_vec_result_t), cmp_result);
        pthread_rwlock_unlock(&idx->rwlock);
        return (int)filtered;
    }

    uint32_t dim = idx->params.dimension;
    uint32_t found = 0;

    for (uint64_t i = 0; i < idx->flat_count && found < max_results; i++) {
        if (idx->flat_deleted[i]) continue;
        float d = idx->dist_func(query, idx->flat_vectors + (size_t)i * dim, dim);
        if (d <= radius) {
            results[found].id       = idx->flat_ids[i];
            results[found].distance = d;
            found++;
        }
    }

    qsort(results, (size_t)found, sizeof(kanbudb_vec_result_t), cmp_result);

    pthread_rwlock_unlock(&idx->rwlock);
    return (int)found;
}

/* ------------------------------------------------------------------ */
/*  Info                                                               */
/* ------------------------------------------------------------------ */

int kanbudb_vec_count(kanbudb_vec_index_t* idx)
{
    if (!idx) return 0;
    pthread_rwlock_rdlock(&idx->rwlock);
    uint64_t c;
    if (idx->algo_type == 1) {
        c = (uint64_t)hnsw_count((const hnsw_index_t*)idx->algo_data);
    } else {
        c = idx->flat_count;
    }
    pthread_rwlock_unlock(&idx->rwlock);
    return (int)c;
}

int kanbudb_vec_dimension(kanbudb_vec_index_t* idx)
{
    if (!idx) return 0;
    return (int)idx->params.dimension;
}

int kanbudb_vec_stats(kanbudb_vec_index_t* idx,
                      kanbudb_vec_stats_t* stats)
{
    if (!idx || !stats) return KANBUDB_VEC_ERR_INVAL;
    pthread_rwlock_rdlock(&idx->rwlock);
    if (idx->algo_type == 1) {
        stats->count    = (uint64_t)hnsw_count((const hnsw_index_t*)idx->algo_data);
        stats->capacity = 0;
        stats->memory_bytes = sizeof(*idx) + sizeof(hnsw_index_t);
        stats->dimension = idx->params.dimension;
    } else {
        stats->count    = idx->flat_count;
        stats->capacity = idx->flat_capacity;
        stats->memory_bytes =
            (size_t)idx->flat_capacity * idx->params.dimension * sizeof(float) +
            (size_t)idx->flat_capacity * sizeof(uint64_t) +
            (size_t)idx->flat_capacity +
            sizeof(*idx);
        stats->dimension = idx->params.dimension;
    }
    pthread_rwlock_unlock(&idx->rwlock);
    return KANBUDB_VEC_OK;
}
