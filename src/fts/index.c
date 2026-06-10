#include "fts/index.h"
#include <stdlib.h>
#include <string.h>

kanbudb_fts_index_t* fts_index_create(void) {
  kanbudb_fts_index_t* idx = kanbudb_malloc(sizeof(kanbudb_fts_index_t));
  if (!idx) return NULL;
  idx->term_dict = fst_create();
  if (!idx->term_dict) {
    kanbudb_free(idx);
    return NULL;
  }
  idx->next_doc_id = 0;
  idx->total_docs = 0;
  idx->total_terms = 0;
  return idx;
}

void fts_index_destroy(kanbudb_fts_index_t* idx) {
  if (!idx) return;
  fst_destroy(idx->term_dict);
  kanbudb_free(idx);
}

int fts_index_add_document(kanbudb_fts_index_t* idx, const char** terms,
                           const size_t* term_lens, const size_t* positions,
                           int num_terms) {
  (void)term_lens;
  if (!idx || !terms || num_terms < 0) return KANBUDB_ERR_INVAL;
  uint64_t doc_id = idx->next_doc_id++;
  idx->total_docs++;
  for (int i = 0; i < num_terms; i++) {
    if (!terms[i]) continue;
    uint64_t encoded = (doc_id << 16) | (uint64_t)(positions ? positions[i] : 0);
    int rc = fst_insert(idx->term_dict, terms[i], encoded);
    if (rc != KANBUDB_OK) return rc;
    idx->total_terms++;
  }
  return KANBUDB_OK;
}

int fts_index_search(const kanbudb_fts_index_t* idx, const char* term,
                     uint64_t* results, int max_results) {
  if (!idx || !term || !results || max_results <= 0) return 0;
  uint64_t encoded;
  int rc = fst_get(idx->term_dict, term, &encoded);
  if (rc != KANBUDB_OK) return 0;
  results[0] = encoded >> 16;
  return 1;
}

int fts_index_search_fuzzy(const kanbudb_fts_index_t* idx, const char* term,
                           int max_edits, uint64_t* results, int max_results) {
  if (!idx || !term || !results || max_results <= 0 || max_edits < 0) return 0;
  uint64_t encoded[256];
  int n = fst_fuzzy_search(idx->term_dict, term, max_edits, encoded, max_results > 256 ? 256 : max_results);
  for (int i = 0; i < n && i < max_results; i++) {
    results[i] = encoded[i] >> 16;
  }
  return n;
}
