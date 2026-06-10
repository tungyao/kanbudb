#ifndef KANBUDB_FTS_INDEX_H
#define KANBUDB_FTS_INDEX_H

#include "macros.h"
#include "fst.h"

typedef struct kanbudb_fts_index {
  kanbudb_fst_t*   term_dict;
  uint64_t        next_doc_id;
  int             total_docs;
  int             total_terms;
} kanbudb_fts_index_t;

kanbudb_fts_index_t* fts_index_create(void);
void                fts_index_destroy(kanbudb_fts_index_t* idx);
int fts_index_add_document(kanbudb_fts_index_t* idx, const char** terms,
                           const size_t* term_lens, const size_t* positions,
                           int num_terms);
int fts_index_search(const kanbudb_fts_index_t* idx, const char* term,
                     uint64_t* results, int max_results);
int fts_index_search_fuzzy(const kanbudb_fts_index_t* idx, const char* term,
                           int max_edits, uint64_t* results, int max_results);

#endif /* KANBUDB_FTS_INDEX_H */
