#ifndef KANBUDB_WAL_MMAP_H
#define KANBUDB_WAL_MMAP_H

#include "platform_mmap.h"
#include <stddef.h>
#include <stdint.h>

#define KANBUDB_WAL_MMAP_MAGIC   0x4845524D4553ULL
#define KANBUDB_WAL_MMAP_VERSION 1
#define KANBUDB_WAL_MMAP_HEADER_SIZE 64

typedef struct __attribute__((packed)) {
  uint64_t magic;
  uint32_t version;
  uint32_t _pad0;
  uint64_t write_pos;
  uint64_t data_size;
  uint8_t  _pad[32];
} kanbudb_wal_mmap_header_t;

typedef struct {
  kanbudb_mmap_region_t   region;
  kanbudb_wal_mmap_header_t* header;
  uint8_t*                data;
  size_t                  data_cap;
  uint64_t                last_seq;
} kanbudb_wal_mmap_t;

int wal_mmap_open(const char* path, int rw, size_t data_cap,
                  kanbudb_wal_mmap_t* wm);

int wal_mmap_append(kanbudb_wal_mmap_t* wm, uint64_t seq, int op,
                    uint64_t table_id, const void* key, size_t key_len,
                    const void* value, size_t val_len);

int wal_mmap_read_entry(kanbudb_wal_mmap_t* wm, uint64_t* out_seq,
                        int* out_op, uint64_t* out_table_id,
                        void** out_key, size_t* out_key_len,
                        void** out_value, size_t* out_val_len);

uint64_t wal_mmap_get_write_pos(kanbudb_wal_mmap_t* wm);

int wal_mmap_sync(kanbudb_wal_mmap_t* wm);

int wal_mmap_close(kanbudb_wal_mmap_t* wm);

#endif
