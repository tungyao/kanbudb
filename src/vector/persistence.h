#ifndef KANBUDB_PERSISTENCE_H
#define KANBUDB_PERSISTENCE_H

#include <stddef.h>
#include <stdint.h>

#include "vector_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

#define KANBUDB_VEC_MAGIC   0x4B425643
#define KANBUDB_VEC_VERSION 2

#define WAL_OP_INSERT     1
#define WAL_OP_DELETE     2
#define WAL_OP_CHECKPOINT 3

#define WAL_AUTO_CHECKPOINT_SIZE (10UL * 1024 * 1024)

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t dimension;
    uint32_t metric;
    uint64_t count;
    uint64_t capacity;
    uint32_t algo;
    uint32_t M;
    uint32_t ef_construction;
    uint32_t ef_search;
} vec_file_header_t;

int vec_persist_write(const char* path, const kanbudb_vec_index_t* idx);
int vec_persist_read(const char* path, kanbudb_vec_index_t** out);

int  vec_wal_open(kanbudb_vec_index_t* idx);
int  vec_wal_append_insert(kanbudb_vec_index_t* idx, uint64_t id, const float* vector);
int  vec_wal_append_delete(kanbudb_vec_index_t* idx, uint64_t id);
int  vec_checkpoint(kanbudb_vec_index_t* idx);
void vec_wal_close(kanbudb_vec_index_t* idx);

#ifdef __cplusplus
}
#endif

#endif /* KANBUDB_PERSISTENCE_H */
