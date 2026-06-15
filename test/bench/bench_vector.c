#include "vector.h"
#include "util/macros.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

int main(void)
{
    const uint32_t dim = 768;
    const uint32_t n = 10000;
    const uint32_t k = 10;
    const uint32_t n_queries = 100;

    printf("Benchmark: %u vectors, %u dim, Top-%u search, %u queries\n\n",
           n, dim, k, n_queries);

    kanbudb_vec_params_t params = {
        .metric = KANBUDB_VEC_METRIC_L2,
        .dimension = dim,
        .initial_capacity = n,
        .enable_persistence = 0
    };

    kanbudb_vec_index_t* idx = NULL;
    if (kanbudb_vec_create(NULL, &params, &idx) != KANBUDB_VEC_OK) {
        printf("FAIL: create\n");
        return 1;
    }

    /* Generate random data */
    srand(42);
    float* data = malloc((size_t)n * dim * sizeof(float));
    for (uint32_t i = 0; i < n * dim; i++)
        data[i] = (float)rand() / (float)RAND_MAX;

    /* Batch insert */
    uint64_t* ids = malloc((size_t)n * sizeof(uint64_t));
    for (uint32_t i = 0; i < n; i++) ids[i] = (uint64_t)(i + 1);

    double t0 = now_sec();
    if (kanbudb_vec_insert_batch(idx, n, ids, data) != KANBUDB_VEC_OK) {
        printf("FAIL: insert_batch\n");
        return 1;
    }
    double t1 = now_sec();
    printf("Insert:    %u vectors in %.3f s (%.0f vec/s)\n",
           n, t1 - t0, n / (t1 - t0));

    /* Generate query vectors */
    float* queries = malloc((size_t)n_queries * dim * sizeof(float));
    for (uint32_t i = 0; i < n_queries * dim; i++)
        queries[i] = (float)rand() / (float)RAND_MAX;

    /* Warm up */
    kanbudb_vec_result_t* results = malloc((size_t)k * sizeof(*results));
    for (uint32_t i = 0; i < 10; i++)
        kanbudb_vec_search(idx, queries + (size_t)i * dim, k, results);

    /* Benchmark */
    t0 = now_sec();
    for (uint32_t i = 0; i < n_queries; i++)
        kanbudb_vec_search(idx, queries + (size_t)i * dim, k, results);
    t1 = now_sec();
    double avg_ms = (t1 - t0) / n_queries * 1000.0;
    printf("Search:    %u queries in %.3f s (%.1f ms avg)\n",
           n_queries, t1 - t0, avg_ms);

    /* Verify first result */
    printf("\nFirst query top-5:\n");
    for (uint32_t i = 0; i < 5 && i < k; i++)
        printf("  [%u] id=%llu dist=%.4f\n", i,
               (unsigned long long)results[i].id, results[i].distance);

    kanbudb_vec_stats_t stats;
    kanbudb_vec_stats(idx, &stats);
    printf("\nStats: count=%llu, capacity=%llu, memory=%.2f MB\n",
           (unsigned long long)stats.count, (unsigned long long)stats.capacity,
           stats.memory_bytes / (1024.0 * 1024.0));

    free(data);
    free(ids);
    free(queries);
    free(results);
    kanbudb_vec_close(idx);
    return 0;
}
