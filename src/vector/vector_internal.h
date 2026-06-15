#ifndef KANBUDB_VECTOR_INTERNAL_H
#define KANBUDB_VECTOR_INTERNAL_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <pthread.h>

#include "vector.h"
#include "macros.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Internal vector index structure */
struct kanbudb_vec_index {
    kanbudb_vec_params_t params;
    float (*dist_func)(const float*, const float*, uint32_t);
    pthread_rwlock_t rwlock;
    char*   path;
    int     dirty;

    /* Algorithm-specific data */
    void* algo_data;
    int   algo_type; /* 0=flat, 1=hnsw */

    /* Flat search data (used when algo_type==0) */
    float*   flat_vectors;
    uint64_t* flat_ids;
    uint64_t  flat_count;
    uint64_t  flat_capacity;
    uint8_t*  flat_deleted;

    /* WAL */
    FILE*    wal_fp;
    uint64_t wal_bytes;

    /* Background flush */
    int       flush_interval_ms;
    pthread_t flush_thread;
    volatile int flush_thread_running;
};

#ifdef __cplusplus
}
#endif

#endif /* KANBUDB_VECTOR_INTERNAL_H */
