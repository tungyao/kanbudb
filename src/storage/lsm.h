#ifndef KANBUDB_LSM_H
#define KANBUDB_LSM_H

#include "macros.h"

typedef struct kanbudb_memtable kanbudb_memtable_t;
typedef struct kanbudb_sstable  kanbudb_sstable_t;
typedef struct kanbudb_lsm     kanbudb_lsm_t;

typedef struct {
  uint64_t    seq;
  const void* key;
  size_t      key_len;
  const void* value;
  size_t      val_len;
  int         deleted;
} lsm_entry_t;

kanbudb_memtable_t* memtable_create(size_t max_size);
void               memtable_destroy(kanbudb_memtable_t* mt);
int                memtable_put(kanbudb_memtable_t* mt, uint64_t seq,
                                const void* key, size_t key_len,
                                const void* value, size_t val_len);
int                memtable_delete(kanbudb_memtable_t* mt, uint64_t seq,
                                   const void* key, size_t key_len);
int                memtable_get(kanbudb_memtable_t* mt,
                                const void* key, size_t key_len,
                                void** out_value, size_t* out_val_len,
                                int* out_deleted);
int                memtable_is_full(kanbudb_memtable_t* mt);
size_t             memtable_size(kanbudb_memtable_t* mt);
int                memtable_iterate(kanbudb_memtable_t* mt,
                                    int (*cb)(const lsm_entry_t* entry, void* ctx),
                                    void* ctx);

kanbudb_lsm_t* lsm_create(const char* path, size_t memtable_size);
void          lsm_destroy(kanbudb_lsm_t* lsm);
int           lsm_put(kanbudb_lsm_t* lsm, uint64_t table_id,
                      const void* key, size_t key_len,
                      const void* value, size_t val_len);
int           lsm_get(kanbudb_lsm_t* lsm, uint64_t table_id,
                      const void* key, size_t key_len,
                      void** out_value, size_t* out_val_len);
int           lsm_delete(kanbudb_lsm_t* lsm, uint64_t table_id,
                         const void* key, size_t key_len);
int           lsm_flush(kanbudb_lsm_t* lsm);
int           lsm_is_full(kanbudb_lsm_t* lsm);
int           lsm_has_data(kanbudb_lsm_t* lsm);
uint64_t      lsm_next_seq(kanbudb_lsm_t* lsm);

/* Iterate the flushing memtable (after lsm_flush but before it's consumed).
 * Returns 0 on success, callback return value on abort. */
int lsm_iterate_flushing(kanbudb_lsm_t* lsm,
                          int (*cb)(const lsm_entry_t* entry, void* ctx),
                          void* ctx);
void lsm_destroy_flushing(kanbudb_lsm_t* lsm);

#endif
