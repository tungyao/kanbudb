#include "vector.h"
#include "util/macros.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <pthread.h>
#include <stdatomic.h>

#define DIM         768
#define N_VECTORS   10000
#define N_READERS   4
#define RUN_SECS    5
#define MAX_SAMPLES 2000000

static atomic_int stop_flag = 0;

typedef struct {
    kanbudb_vec_index_t* idx;
    uint32_t             thread_id;
    uint64_t             ops;
    double*              latencies;
    uint64_t             lat_count;
    uint64_t             lat_cap;
} reader_state_t;

typedef struct {
    kanbudb_vec_index_t* idx;
    uint32_t             thread_id;
    uint64_t             insert_ops;
    uint64_t             delete_ops;
    double*              latencies;
    uint64_t             lat_count;
    uint64_t             lat_cap;
    uint64_t             next_id;
} writer_state_t;

static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static int cmp_double(const void* a, const void* b)
{
    double da = *(const double*)a;
    double db = *(const double*)b;
    return (da > db) - (da < db);
}

static double percentile(double* samples, uint64_t n, int p)
{
    if (n == 0) return 0.0;
    size_t idx = (size_t)((double)p / 100.0 * (n - 1));
    return samples[idx];
}

static void* reader_thread(void* arg)
{
    reader_state_t* st = (reader_state_t*)arg;
    kanbudb_vec_result_t* results = malloc((size_t)10 * sizeof(*results));
    float* query = malloc((size_t)DIM * sizeof(float));

    st->latencies = malloc((size_t)st->lat_cap * sizeof(double));
    st->lat_count = 0;
    st->ops = 0;

    /* Initialize query with random data */
    for (uint32_t i = 0; i < DIM; i++)
        query[i] = (float)rand() / (float)RAND_MAX;

    while (!atomic_load(&stop_flag)) {
        double t0 = now_sec();
        int rc = kanbudb_vec_search(st->idx, query, 10, results);
        double t1 = now_sec();

        if (rc >= 0) {
            st->ops++;
            if (st->lat_count < st->lat_cap) {
                st->latencies[st->lat_count++] = (t1 - t0) * 1e6;
            }
        }
    }

    free(results);
    free(query);
    return NULL;
}

static void* writer_thread(void* arg)
{
    writer_state_t* st = (writer_state_t*)arg;
    float* vec = malloc((size_t)DIM * sizeof(float));

    st->latencies = malloc((size_t)st->lat_cap * sizeof(double));
    st->lat_count = 0;
    st->insert_ops = 0;
    st->delete_ops = 0;
    st->next_id = (uint64_t)N_VECTORS + 1;

    while (!atomic_load(&stop_flag)) {
        /* Generate random vector */
        for (uint32_t i = 0; i < DIM; i++)
            vec[i] = (float)rand() / (float)RAND_MAX;

        /* Interleave insert and delete */
        if (rand() % 2 == 0) {
            double t0 = now_sec();
            int rc = kanbudb_vec_insert(st->idx, st->next_id++, vec);
            double t1 = now_sec();
            if (rc == KANBUDB_VEC_OK) {
                st->insert_ops++;
                if (st->lat_count < st->lat_cap) {
                    st->latencies[st->lat_count++] = (t1 - t0) * 1e6;
                }
            }
        } else {
            uint64_t del_id = (uint64_t)(rand() % N_VECTORS) + 1;
            double t0 = now_sec();
            int rc = kanbudb_vec_delete(st->idx, del_id);
            double t1 = now_sec();
            if (rc == KANBUDB_VEC_OK) {
                st->delete_ops++;
                if (st->lat_count < st->lat_cap) {
                    st->latencies[st->lat_count++] = (t1 - t0) * 1e6;
                }
            }
        }
    }

    free(vec);
    return NULL;
}

int main(void)
{
    printf("=== KanbuDB Thread Safety Benchmark ===\n");
    printf("  Dimensions: %u\n", DIM);
    printf("  Vectors:    %u\n", N_VECTORS);
    printf("  Readers:    %u\n", N_READERS);
    printf("  Writers:    1\n");
    printf("  Duration:   %u seconds\n\n", RUN_SECS);

    /* Create vector index */
    kanbudb_vec_params_t params = {
        .metric = KANBUDB_VEC_METRIC_L2,
        .dimension = DIM,
        .initial_capacity = N_VECTORS,
        .enable_persistence = 0
    };

    kanbudb_vec_index_t* idx = NULL;
    if (kanbudb_vec_create(NULL, &params, &idx) != KANBUDB_VEC_OK) {
        printf("FAIL: create index\n");
        return 1;
    }

    /* Generate and insert initial vectors */
    srand(42);
    float* data = malloc((size_t)N_VECTORS * DIM * sizeof(float));
    for (uint32_t i = 0; i < N_VECTORS * DIM; i++)
        data[i] = (float)rand() / (float)RAND_MAX;

    uint64_t* ids = malloc((size_t)N_VECTORS * sizeof(uint64_t));
    for (uint32_t i = 0; i < N_VECTORS; i++) ids[i] = (uint64_t)(i + 1);

    double t0 = now_sec();
    if (kanbudb_vec_insert_batch(idx, N_VECTORS, ids, data) != KANBUDB_VEC_OK) {
        printf("FAIL: batch insert\n");
        return 1;
    }
    double t1 = now_sec();
    printf("Seed: %u vectors in %.3f s\n\n", N_VECTORS, t1 - t0);

    /* Spawn reader threads */
    pthread_t readers[N_READERS];
    reader_state_t rstates[N_READERS];
    for (int i = 0; i < N_READERS; i++) {
        rstates[i].idx = idx;
        rstates[i].thread_id = (uint32_t)i;
        rstates[i].lat_cap = MAX_SAMPLES;
        pthread_create(&readers[i], NULL, reader_thread, &rstates[i]);
    }

    /* Spawn writer thread */
    pthread_t writer;
    writer_state_t wstate;
    wstate.idx = idx;
    wstate.thread_id = 0;
    wstate.lat_cap = MAX_SAMPLES;
    pthread_create(&writer, NULL, writer_thread, &wstate);

    /* Run for RUN_SECS seconds */
    struct timespec sleep_ts;
    sleep_ts.tv_sec = RUN_SECS;
    sleep_ts.tv_nsec = 0;
    nanosleep(&sleep_ts, NULL);

    atomic_store(&stop_flag, 1);

    /* Join all threads */
    for (int i = 0; i < N_READERS; i++)
        pthread_join(readers[i], NULL);
    pthread_join(writer, NULL);

    /* Aggregate results */
    uint64_t total_reads = 0;
    uint64_t total_read_samples = 0;
    double* all_read_lat = malloc((size_t)N_READERS * MAX_SAMPLES * sizeof(double));

    for (int i = 0; i < N_READERS; i++) {
        total_reads += rstates[i].ops;
        for (uint64_t j = 0; j < rstates[i].lat_count; j++)
            all_read_lat[total_read_samples++] = rstates[i].latencies[j];
    }

    /* Sort latencies for percentile calculation */
    qsort(all_read_lat, (size_t)total_read_samples, sizeof(double), cmp_double);

    double read_p50 = percentile(all_read_lat, total_read_samples, 50);
    double read_p99 = percentile(all_read_lat, total_read_samples, 99);

    uint64_t total_writes = wstate.insert_ops + wstate.delete_ops;
    qsort(wstate.latencies, (size_t)wstate.lat_count, sizeof(double), cmp_double);
    double write_p50 = percentile(wstate.latencies, wstate.lat_count, 50);
    double write_p99 = percentile(wstate.latencies, wstate.lat_count, 99);

    /* Report */
    printf("Results (%u seconds):\n", RUN_SECS);
    printf("  Total reads:   %llu (%.0f reads/sec)\n",
           (unsigned long long)total_reads,
           (double)total_reads / RUN_SECS);
    printf("  Read latency:  p50=%.2f us  p99=%.2f us\n", read_p50, read_p99);
    printf("  Total writes:  %llu (%.0f writes/sec) [%llu inserts, %llu deletes]\n",
           (unsigned long long)total_writes,
           (double)total_writes / RUN_SECS,
           (unsigned long long)wstate.insert_ops,
           (unsigned long long)wstate.delete_ops);
    printf("  Write latency: p50=%.2f us  p99=%.2f us\n", write_p50, write_p99);

    /* Cleanup */
    free(data);
    free(ids);
    free(all_read_lat);
    for (int i = 0; i < N_READERS; i++)
        free(rstates[i].latencies);
    free(wstate.latencies);
    kanbudb_vec_close(idx);

    return 0;
}
