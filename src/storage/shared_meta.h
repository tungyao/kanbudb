#ifndef KANBUDB_SHARED_META_H
#define KANBUDB_SHARED_META_H

#include <stdint.h>

#define KANBUDB_SHARED_MAGIC 0x4B414E4255444200ULL

typedef struct __attribute__((packed)) {
  uint64_t magic;
  uint64_t flushed_seq;
  uint32_t wal_version;
  uint32_t reader_count;
} kanbudb_shared_meta_t;

int kanbudb_shared_meta_open(const char* db_path, kanbudb_shared_meta_t* meta);
int kanbudb_shared_meta_write(const char* db_path, const kanbudb_shared_meta_t* meta);
int kanbudb_shared_meta_reader_join(const char* db_path, kanbudb_shared_meta_t* meta);
int kanbudb_shared_meta_reader_leave(const char* db_path, kanbudb_shared_meta_t* meta);

#endif
