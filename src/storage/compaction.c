#include "compaction.h"
#include <stdlib.h>
#include <string.h>

struct kanbudb_compactor {
  int placeholder;
};

kanbudb_compactor_t* compactor_create(void) {
  kanbudb_compactor_t* c = (kanbudb_compactor_t*)calloc(1, sizeof(*c));
  if (!c) return NULL;
  c->placeholder = 0;
  return c;
}

void compactor_destroy(kanbudb_compactor_t* c) {
  if (!c) return;
  free(c);
}

int compactor_compact(kanbudb_compactor_t* c,
                      const uint8_t* sstable_data, size_t sstable_len,
                      uint8_t** out_btree_data, size_t* out_btree_len) {
  (void)c;
  if (!sstable_data || !out_btree_data || !out_btree_len)
    return KANBUDB_ERR_INVAL;

  uint8_t* copy = (uint8_t*)malloc(sstable_len);
  if (!copy) return KANBUDB_ERR_OOM;

  memcpy(copy, sstable_data, sstable_len);
  *out_btree_data = copy;
  *out_btree_len = sstable_len;
  return KANBUDB_OK;
}
