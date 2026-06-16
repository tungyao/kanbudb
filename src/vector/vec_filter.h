#ifndef KANBUDB_VEC_FILTER_H
#define KANBUDB_VEC_FILTER_H

#include <stddef.h>
#include <stdint.h>
#include "vector.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef int (*kanbudb_vec_filter_fn)(uint64_t id, void* ctx);

int kanbudb_vec_search_filtered(kanbudb_vec_index_t* idx,
                                const float* query, uint32_t k,
                                kanbudb_vec_filter_fn filter,
                                void* filter_ctx,
                                kanbudb_vec_result_t* results);

#ifdef __cplusplus
}
#endif

#endif /* KANBUDB_VEC_FILTER_H */
