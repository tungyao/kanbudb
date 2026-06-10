#ifndef KANBUDB_COMPACTION_H
#define KANBUDB_COMPACTION_H
#include "macros.h"
typedef struct kanbudb_compactor kanbudb_compactor_t;
kanbudb_compactor_t* compactor_create(void);
void                compactor_destroy(kanbudb_compactor_t* c);
int                 compactor_compact(kanbudb_compactor_t* c,
                                      const uint8_t* sstable_data, size_t sstable_len,
                                      uint8_t** out_btree_data, size_t* out_btree_len);
#endif
