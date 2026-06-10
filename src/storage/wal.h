#ifndef KANBUDB_WAL_H
#define KANBUDB_WAL_H

#include "macros.h"

typedef struct kanbudb_wal kanbudb_wal_t;

typedef enum {
  KANBUDB_WAL_PUT,
  KANBUDB_WAL_DELETE
} kanbudb_wal_op_t;

kanbudb_wal_t* wal_create(const char* path, int fsync_mode);
void          wal_destroy(kanbudb_wal_t* wal);
int           wal_append(kanbudb_wal_t* wal, int op,
                         uint64_t table_id, const void* key, size_t key_len,
                         const void* value, size_t val_len);
int           wal_sync(kanbudb_wal_t* wal);
int           wal_replay(kanbudb_wal_t* wal,
                         int (*callback)(int op, uint64_t table_id,
                                         const void* key, size_t key_len,
                                         const void* value, size_t val_len,
                                         void* ctx),
                         void* ctx);
uint64_t      wal_last_seq(kanbudb_wal_t* wal);

#endif
