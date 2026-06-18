#include "shared_meta.h"
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

static void make_path(const char* db_path, char* out, size_t out_len) {
  snprintf(out, out_len, "%s.shared", db_path);
}

int kanbudb_shared_meta_open(const char* db_path, kanbudb_shared_meta_t* meta) {
  char path[512];
  make_path(db_path, path, sizeof(path));

  int fd = open(path, O_RDONLY);
  if (fd < 0) {
    meta->magic = KANBUDB_SHARED_MAGIC;
    meta->flushed_seq = 0;
    meta->wal_version = 1;
    meta->reader_count = 0;
    return kanbudb_shared_meta_write(db_path, meta);
  }

  memset(meta, 0, sizeof(*meta));
  ssize_t n = read(fd, meta, sizeof(*meta));
  close(fd);

  if (n != sizeof(*meta) || meta->magic != KANBUDB_SHARED_MAGIC) {
    meta->magic = KANBUDB_SHARED_MAGIC;
    meta->flushed_seq = 0;
    meta->wal_version = 1;
    meta->reader_count = 0;
    return kanbudb_shared_meta_write(db_path, meta);
  }
  return 0;
}

int kanbudb_shared_meta_write(const char* db_path, const kanbudb_shared_meta_t* meta) {
  char path[512];
  make_path(db_path, path, sizeof(path));

  char tmp_path[516];
  snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);

  int fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) return -1;

  ssize_t written = write(fd, meta, sizeof(*meta));
  close(fd);

  if (written != sizeof(*meta)) return -1;

  if (rename(tmp_path, path) < 0) return -1;
  return 0;
}

int kanbudb_shared_meta_reader_join(const char* db_path, kanbudb_shared_meta_t* meta) {
  meta->reader_count++;
  return kanbudb_shared_meta_write(db_path, meta);
}

int kanbudb_shared_meta_reader_leave(const char* db_path, kanbudb_shared_meta_t* meta) {
  if (meta->reader_count > 0) meta->reader_count--;
  return kanbudb_shared_meta_write(db_path, meta);
}
