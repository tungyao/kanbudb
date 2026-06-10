#ifndef KANBUDB_FST_H
#define KANBUDB_FST_H

#include "macros.h"

typedef struct kanbudb_fst kanbudb_fst_t;

#define FST_MAX_KEY_LEN 256

kanbudb_fst_t* fst_create(void);
void          fst_destroy(kanbudb_fst_t* fst);
int           fst_insert(kanbudb_fst_t* fst, const char* key, uint64_t value);
int           fst_get(const kanbudb_fst_t* fst, const char* key, uint64_t* out_value);
int           fst_prefix_search(const kanbudb_fst_t* fst, const char* prefix,
                                uint64_t* results, int max_results);
int           fst_fuzzy_search(const kanbudb_fst_t* fst, const char* key, int max_edits,
                               uint64_t* results, int max_results);
size_t        fst_size(const kanbudb_fst_t* fst);
size_t        fst_memory_used(const kanbudb_fst_t* fst);

#endif /* KANBUDB_FST_H */
