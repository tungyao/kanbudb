#ifndef KANBUDB_WAL_H
#define KANBUDB_WAL_H

#include "macros.h"

typedef struct kanbudb_wal kanbudb_wal_t;

typedef enum {
  KANBUDB_WAL_PUT,
  KANBUDB_WAL_DELETE
} kanbudb_wal_op_t;

/* Forward: a parsed WAL frame for multi-process reader */
typedef struct {
  uint64_t seq;
  uint8_t  op;          
  uint64_t table_id;
  uint8_t* key;
  size_t   key_len;
  uint8_t* value;
  size_t   val_len;
} kanbudb_wal_frame_t;

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
int           wal_truncate(kanbudb_wal_t* wal);

/* Size of WAL file header (offset where first frame starts) */
#define KANBUDB_WAL_HEADER_SIZE 20

/* ── Multi-process WAL (frame-based, pwrite, CRC-validated) ── */

int kanbudb_wal_append_frame(kanbudb_wal_t* wal,
                              uint64_t seq, int op,
                              uint64_t table_id,
                              const void* key, size_t key_len,
                              const void* value, size_t val_len);

/* Scan WAL from start_offset to end_offset, calling cb for each valid frame.
 * Stops on CRC error or end_offset. Returns number of frames parsed. */
int kanbudb_wal_scan_frames(const char* wal_path,
                             uint64_t start_offset, uint64_t end_offset,
                             int (*cb)(const kanbudb_wal_frame_t* frame, void* ctx),
                             void* ctx);

/* Get the current file size of WAL (for setting wal_committed_end) */
uint64_t kanbudb_wal_file_size(kanbudb_wal_t* wal);

#endif
