#include "wal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define KANBUDB_WAL_MAGIC  0x4845524D4553ULL
#define KANBUDB_WAL_VERSION 1
#define KANBUDB_WAL_PERIODIC_THRESHOLD 1000

typedef struct __attribute__((packed)) {
  uint64_t magic;
  uint32_t version;
  uint64_t seq;
} wal_header_t;

struct kanbudb_wal {
  FILE* file;
  char* path;
  int fsync_mode;
  uint64_t seq;
  int periodic_count;
};

kanbudb_wal_t* wal_create(const char* path, int fsync_mode) {
  kanbudb_wal_t* wal = (kanbudb_wal_t*)calloc(1, sizeof(*wal));
  if (!wal) return NULL;

  wal->path = (char*)malloc(strlen(path) + 1);
  if (!wal->path) { free(wal); return NULL; }
  strcpy(wal->path, path);
  wal->fsync_mode = fsync_mode;

  FILE* f = fopen(path, "a+b");
  if (!f) { free(wal->path); free(wal); return NULL; }

  fseek(f, 0, SEEK_END);
  long size = ftell(f);

  if (size == 0) {
    uint64_t magic = KANBUDB_WAL_MAGIC;
    uint32_t version = KANBUDB_WAL_VERSION;
    uint64_t seq = 0;
    if (fwrite(&magic, sizeof(magic), 1, f) != 1 ||
        fwrite(&version, sizeof(version), 1, f) != 1 ||
        fwrite(&seq, sizeof(seq), 1, f) != 1) {
      fclose(f);
      free(wal->path);
      free(wal);
      return NULL;
    }
    wal->seq = 0;
    fflush(f);
  } else {
    fseek(f, 0, SEEK_SET);
    uint64_t magic;
    uint32_t version;
    if (fread(&magic, sizeof(magic), 1, f) != 1 ||
        fread(&version, sizeof(version), 1, f) != 1 ||
        magic != KANBUDB_WAL_MAGIC ||
        version != KANBUDB_WAL_VERSION) {
      fclose(f);
      free(wal->path);
      free(wal);
      return NULL;
    }
    if (fread(&wal->seq, sizeof(wal->seq), 1, f) != 1) {
      fclose(f);
      free(wal->path);
      free(wal);
      return NULL;
    }
    fseek(f, 0, SEEK_END);
  }

  wal->file = f;
  return wal;
}

void wal_destroy(kanbudb_wal_t* wal) {
  if (!wal) return;
  if (wal->file) fclose(wal->file);
  free(wal->path);
  free(wal);
}

int wal_append(kanbudb_wal_t* wal, int op,
               uint64_t table_id, const void* key, size_t key_len,
               const void* value, size_t val_len) {
  wal->seq++;
  uint64_t seq = wal->seq;
  uint8_t op_u8 = (op == KANBUDB_WAL_DELETE) ? 1 : 0;
  uint64_t key_len_u64 = key_len;
  uint64_t val_len_u64 = val_len;

  if (fwrite(&seq, sizeof(seq), 1, wal->file) != 1) return KANBUDB_ERR_IO;
  if (fwrite(&op_u8, sizeof(op_u8), 1, wal->file) != 1) return KANBUDB_ERR_IO;
  if (fwrite(&table_id, sizeof(table_id), 1, wal->file) != 1) return KANBUDB_ERR_IO;
  if (fwrite(&key_len_u64, sizeof(key_len_u64), 1, wal->file) != 1) return KANBUDB_ERR_IO;
  if (fwrite(&val_len_u64, sizeof(val_len_u64), 1, wal->file) != 1) return KANBUDB_ERR_IO;
  if (fwrite(key, 1, key_len, wal->file) != key_len) return KANBUDB_ERR_IO;
  if (op_u8 == 0 && value && val_len > 0) {
    if (fwrite(value, 1, val_len, wal->file) != val_len) return KANBUDB_ERR_IO;
  }

  if (wal->fsync_mode == 2) {
    fflush(wal->file);
  } else if (wal->fsync_mode == 1) {
    wal->periodic_count++;
    if (wal->periodic_count >= KANBUDB_WAL_PERIODIC_THRESHOLD) {
      fflush(wal->file);
      wal->periodic_count = 0;
    }
  }

  return KANBUDB_OK;
}

int wal_sync(kanbudb_wal_t* wal) {
  if (fflush(wal->file) != 0) return KANBUDB_ERR_IO;
  wal->periodic_count = 0;
  return KANBUDB_OK;
}

int wal_replay(kanbudb_wal_t* wal,
               int (*callback)(int op, uint64_t table_id,
                               const void* key, size_t key_len,
                               const void* value, size_t val_len,
                               void* ctx),
               void* ctx) {
  fseek(wal->file, sizeof(wal_header_t), SEEK_SET);

  uint64_t seq;
  uint8_t op_u8;
  uint64_t table_id;
  uint64_t key_len;
  uint64_t val_len;

  while (1) {
    if (fread(&seq, sizeof(seq), 1, wal->file) != 1) break;
    if (fread(&op_u8, sizeof(op_u8), 1, wal->file) != 1) break;
    if (fread(&table_id, sizeof(table_id), 1, wal->file) != 1) break;
    if (fread(&key_len, sizeof(key_len), 1, wal->file) != 1) break;
    if (fread(&val_len, sizeof(val_len), 1, wal->file) != 1) break;

    if (key_len > 1048576 || val_len > 1048576) return KANBUDB_ERR_CORRUPT;

    unsigned char* key = (unsigned char*)malloc(key_len);
    if (!key) return KANBUDB_ERR_OOM;
    if (fread(key, 1, key_len, wal->file) != key_len) {
      free(key);
      return KANBUDB_ERR_CORRUPT;
    }

    unsigned char* value = NULL;
    if (op_u8 == 0 && val_len > 0) {
      value = (unsigned char*)malloc(val_len);
      if (!value) { free(key); return KANBUDB_ERR_OOM; }
      if (fread(value, 1, val_len, wal->file) != val_len) {
        free(key);
        free(value);
        return KANBUDB_ERR_CORRUPT;
      }
    }

    wal->seq = seq;

    if (callback) {
      int rc = callback((op_u8 == 0) ? KANBUDB_WAL_PUT : KANBUDB_WAL_DELETE,
                        table_id, key, key_len, value, val_len, ctx);
      if (rc != 0) {
        free(key);
        free(value);
        return rc;
      }
    }

    free(key);
    free(value);
  }

  fseek(wal->file, 0, SEEK_END);
  return KANBUDB_OK;
}

uint64_t wal_last_seq(kanbudb_wal_t* wal) {
  return wal->seq;
}

int wal_truncate(kanbudb_wal_t* wal) {
  if (!wal || !wal->file) return KANBUDB_ERR_INVAL;
  
  /* Close current WAL */
  fclose(wal->file);
  
  /* Reopen with "wb" to truncate */
  wal->file = fopen(wal->path, "wb");
  if (!wal->file) return KANBUDB_ERR_IO;
  
  /* Write fresh header with current seq */
  uint64_t magic = KANBUDB_WAL_MAGIC;
  uint32_t version = KANBUDB_WAL_VERSION;
  uint64_t seq = wal->seq;
  
  if (fwrite(&magic, sizeof(magic), 1, wal->file) != 1 ||
      fwrite(&version, sizeof(version), 1, wal->file) != 1 ||
      fwrite(&seq, sizeof(seq), 1, wal->file) != 1) {
    fclose(wal->file);
    return KANBUDB_ERR_IO;
  }
  
  fflush(wal->file);
  wal->periodic_count = 0;
  return KANBUDB_OK;
}
