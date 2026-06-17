#include "wal.h"
#include "db.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

#ifdef KANBUDB_HAVE_IOURING
#include <liburing.h>
#endif

#define KANBUDB_WAL_MAGIC  0x4845524D4553ULL
#define KANBUDB_WAL_VERSION 1
#define KANBUDB_WAL_PERIODIC_THRESHOLD 1000

#define KANBUDB_WAL_BUF_SIZE 65536

/* Simple CRC32 for WAL frame validation */
static uint32_t crc32_wal(const void* data, size_t len, uint32_t crc) {
    static uint32_t table[256];
    static int init = 0;
    if (!init) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int j = 0; j < 8; j++)
                c = (c >> 1) ^ (c & 1 ? 0xEDB88320 : 0);
            table[i] = c;
        }
        init = 1;
    }
    const uint8_t* buf = (const uint8_t*)data;
    for (size_t i = 0; i < len; i++)
        crc = table[(crc ^ buf[i]) & 0xFF] ^ (crc >> 8);
    return crc;
}

typedef struct __attribute__((packed)) {
  uint64_t magic;
  uint32_t version;
  uint64_t seq;
} wal_header_t;

struct kanbudb_wal {
  char* path;
  int   fd;
  int   fsync_mode;
  uint64_t seq;
  uint64_t file_pos;
  int   periodic_count;

  uint8_t* buf;
  size_t   buf_len;

#ifdef KANBUDB_HAVE_IOURING
  int     use_io_uring;
  struct io_uring ring;
#endif

  FILE* file;
};

static int wal_serialize(uint8_t* buf, size_t cap,
                         uint64_t seq, int op,
                         uint64_t table_id,
                         const void* key, size_t key_len,
                         const void* value, size_t val_len)
{
  uint8_t op_u8 = (op == KANBUDB_WAL_DELETE) ? 1 : 0;
  uint64_t key_len_u64 = key_len;
  uint64_t val_len_u64 = val_len;

  size_t needed = sizeof(seq) + sizeof(op_u8) + sizeof(table_id) +
                  sizeof(key_len_u64) + sizeof(val_len_u64) +
                  key_len + (op_u8 == 0 ? val_len : 0);
  if (needed > cap) return -1;

  size_t off = 0;
  memcpy(buf + off, &seq, sizeof(seq)); off += sizeof(seq);
  memcpy(buf + off, &op_u8, sizeof(op_u8)); off += sizeof(op_u8);
  memcpy(buf + off, &table_id, sizeof(table_id)); off += sizeof(table_id);
  memcpy(buf + off, &key_len_u64, sizeof(key_len_u64)); off += sizeof(key_len_u64);
  memcpy(buf + off, &val_len_u64, sizeof(val_len_u64)); off += sizeof(val_len_u64);
  if (key_len > 0) {
    memcpy(buf + off, key, key_len); off += key_len;
  }
  if (op_u8 == 0 && value && val_len > 0) {
    memcpy(buf + off, value, val_len); off += val_len;
  }
  return (int)off;
}

static int wal_flush_buf(kanbudb_wal_t* wal)
{
  if (wal->buf_len == 0) return KANBUDB_OK;

#ifdef KANBUDB_HAVE_IOURING
  if (wal->use_io_uring) {
    struct io_uring_sqe* sqe = io_uring_get_sqe(&wal->ring);
    if (!sqe) {
      io_uring_submit(&wal->ring);
      struct io_uring_cqe* cqe;
      while (io_uring_peek_cqe(&wal->ring, &cqe) == 0) {
        io_uring_cqe_seen(&wal->ring, cqe);
      }
      sqe = io_uring_get_sqe(&wal->ring);
      if (!sqe) return KANBUDB_ERR_BUSY;
    }
    io_uring_prep_write(sqe, wal->fd, wal->buf, wal->buf_len,
                         (off_t)wal->file_pos);
    sqe->user_data = 1;

    if (wal->fsync_mode == KANBUDB_FSYNC_ALWAYS ||
        (wal->fsync_mode == KANBUDB_FSYNC_PERIODIC &&
         wal->periodic_count >= KANBUDB_WAL_PERIODIC_THRESHOLD)) {
      struct io_uring_sqe* fsync_sqe = io_uring_get_sqe(&wal->ring);
      if (fsync_sqe) {
        io_uring_prep_fsync(fsync_sqe, wal->fd, IORING_FSYNC_DATASYNC);
        fsync_sqe->user_data = 2;
      }
      wal->periodic_count = 0;
    }

    io_uring_submit(&wal->ring);
    while (1) {
      struct io_uring_cqe* cqe = NULL;
      int ret = io_uring_wait_cqe(&wal->ring, &cqe);
      if (ret < 0) return KANBUDB_ERR_IO;
      int err = cqe->res;
      uint64_t ud = cqe->user_data;
      io_uring_cqe_seen(&wal->ring, cqe);
      if (ud == 1) {
        if (err < 0) { wal->buf_len = 0; return KANBUDB_ERR_IO; }
        break;
      }
    }

    wal->file_pos += wal->buf_len;
    wal->buf_len = 0;
    return KANBUDB_OK;
  }
#endif

  if (wal->file) {
    if (fwrite(wal->buf, 1, wal->buf_len, wal->file) != wal->buf_len)
      return KANBUDB_ERR_IO;
    if (wal->fsync_mode == KANBUDB_FSYNC_ALWAYS ||
        (wal->fsync_mode == KANBUDB_FSYNC_PERIODIC &&
         wal->periodic_count >= KANBUDB_WAL_PERIODIC_THRESHOLD)) {
      fflush(wal->file);
      wal->periodic_count = 0;
    }
    wal->file_pos += wal->buf_len;
    wal->buf_len = 0;
  }
  return KANBUDB_OK;
}

kanbudb_wal_t* wal_create(const char* path, int fsync_mode)
{
  kanbudb_wal_t* wal = (kanbudb_wal_t*)calloc(1, sizeof(*wal));
  if (!wal) return NULL;

  wal->path = strdup(path);
  if (!wal->path) { free(wal); return NULL; }
  wal->fsync_mode = fsync_mode;

  wal->buf = (uint8_t*)malloc(KANBUDB_WAL_BUF_SIZE);
  if (!wal->buf) { free(wal->path); free(wal); return NULL; }

#ifdef KANBUDB_HAVE_IOURING
  wal->use_io_uring = 0;
  memset(&wal->ring, 0, sizeof(wal->ring));
  int uring_ret = io_uring_queue_init(32, &wal->ring, 0);
  if (uring_ret == 0) {
    wal->use_io_uring = 1;
  }
#endif

  int flags = O_RDWR | O_CREAT;
  wal->fd = open(path, flags, 0644);
  if (wal->fd < 0) {
    free(wal->buf); free(wal->path); free(wal);
    return NULL;
  }

  wal->file = fopen(path, "a+b");
  if (!wal->file) {
    close(wal->fd); free(wal->buf); free(wal->path); free(wal);
    return NULL;
  }

  fseek(wal->file, 0, SEEK_END);
  long size = ftell(wal->file);

  if (size == 0) {
    uint64_t magic = KANBUDB_WAL_MAGIC;
    uint32_t version = KANBUDB_WAL_VERSION;
    uint64_t seq = 0;
    if (fwrite(&magic, sizeof(magic), 1, wal->file) != 1 ||
        fwrite(&version, sizeof(version), 1, wal->file) != 1 ||
        fwrite(&seq, sizeof(seq), 1, wal->file) != 1) {
      wal_destroy(wal); return NULL;
    }
    wal->seq = 0;
    wal->file_pos = sizeof(wal_header_t);
    fflush(wal->file);
  } else {
    fseek(wal->file, 0, SEEK_SET);
    uint64_t magic;
    uint32_t version;
    if (fread(&magic, sizeof(magic), 1, wal->file) != 1 ||
        fread(&version, sizeof(version), 1, wal->file) != 1 ||
        magic != KANBUDB_WAL_MAGIC ||
        version != KANBUDB_WAL_VERSION) {
      wal_destroy(wal); return NULL;
    }
    if (fread(&wal->seq, sizeof(wal->seq), 1, wal->file) != 1) {
      wal_destroy(wal); return NULL;
    }
    fseek(wal->file, 0, SEEK_END);
    long end_pos = ftell(wal->file);
    wal->file_pos = (end_pos > 0) ? (uint64_t)end_pos : sizeof(wal_header_t);
  }

  wal->buf_len = 0;
  wal->periodic_count = 0;
  return wal;
}

void wal_destroy(kanbudb_wal_t* wal)
{
  if (!wal) return;

  if (wal->buf_len > 0) {
    wal_flush_buf(wal);
  }

#ifdef KANBUDB_HAVE_IOURING
  if (wal->use_io_uring) {
    io_uring_queue_exit(&wal->ring);
  }
#endif

  if (wal->file) fclose(wal->file);
  if (wal->fd >= 0) close(wal->fd);
  free(wal->buf);
  free(wal->path);
  free(wal);
}

int wal_append(kanbudb_wal_t* wal, int op,
               uint64_t table_id, const void* key, size_t key_len,
               const void* value, size_t val_len)
{
  if (!wal) return KANBUDB_ERR_INVAL;

  wal->seq++;

  int written = wal_serialize(wal->buf + wal->buf_len,
                               KANBUDB_WAL_BUF_SIZE - wal->buf_len,
                               wal->seq, op, table_id,
                               key, key_len, value, val_len);
  if (written < 0) {
    int rc = wal_flush_buf(wal);
    if (rc != KANBUDB_OK) return rc;

    written = wal_serialize(wal->buf + wal->buf_len,
                             KANBUDB_WAL_BUF_SIZE - wal->buf_len,
                             wal->seq, op, table_id,
                             key, key_len, value, val_len);
    if (written < 0) return KANBUDB_ERR_IO;
  }

  wal->buf_len += (size_t)written;
  wal->periodic_count++;

  if (wal->fsync_mode == KANBUDB_FSYNC_ALWAYS) {
    return wal_flush_buf(wal);
  }

  if (wal->buf_len >= KANBUDB_WAL_BUF_SIZE / 2) {
    return wal_flush_buf(wal);
  }

  return KANBUDB_OK;
}

int wal_sync(kanbudb_wal_t* wal)
{
  if (!wal) return KANBUDB_ERR_INVAL;
  int rc = wal_flush_buf(wal);
  if (rc != KANBUDB_OK) return rc;

#ifdef KANBUDB_HAVE_IOURING
  if (wal->use_io_uring) {
    struct io_uring_sqe* sqe = io_uring_get_sqe(&wal->ring);
    if (!sqe) return KANBUDB_ERR_BUSY;
    io_uring_prep_fsync(sqe, wal->fd, IORING_FSYNC_DATASYNC);
    sqe->user_data = 3;
    io_uring_submit(&wal->ring);
    while (1) {
      struct io_uring_cqe* cqe = NULL;
      int ret = io_uring_wait_cqe(&wal->ring, &cqe);
      if (ret < 0) return KANBUDB_ERR_IO;
      int err = cqe->res;
      uint64_t ud = cqe->user_data;
      io_uring_cqe_seen(&wal->ring, cqe);
      if (ud == 3) {
        if (err < 0) return KANBUDB_ERR_IO;
        return KANBUDB_OK;
      }
    }
  }
#endif

  fflush(wal->file);
  wal->periodic_count = 0;
  return KANBUDB_OK;
}

int wal_replay(kanbudb_wal_t* wal,
               int (*callback)(int op, uint64_t table_id,
                               const void* key, size_t key_len,
                               const void* value, size_t val_len,
                               void* ctx),
               void* ctx)
{
  if (!wal || !wal->file) return KANBUDB_ERR_INVAL;

  wal_flush_buf(wal);

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

uint64_t wal_last_seq(kanbudb_wal_t* wal)
{
  return wal ? wal->seq : 0;
}

int wal_truncate(kanbudb_wal_t* wal)
{
  if (!wal) return KANBUDB_ERR_INVAL;

  wal_flush_buf(wal);

  if (wal->file) { fclose(wal->file); wal->file = NULL; }
  if (wal->fd >= 0) { close(wal->fd); wal->fd = -1; }

  wal->fd = open(wal->path, O_RDWR | O_CREAT | O_TRUNC, 0644);
  if (wal->fd < 0) return KANBUDB_ERR_IO;

  wal->file = fopen(wal->path, "w+b");
  if (!wal->file) { close(wal->fd); wal->fd = -1; return KANBUDB_ERR_IO; }

  uint64_t magic = KANBUDB_WAL_MAGIC;
  uint32_t version = KANBUDB_WAL_VERSION;
  uint64_t seq = wal->seq;

  if (fwrite(&magic, sizeof(magic), 1, wal->file) != 1 ||
      fwrite(&version, sizeof(version), 1, wal->file) != 1 ||
      fwrite(&seq, sizeof(seq), 1, wal->file) != 1) {
    return KANBUDB_ERR_IO;
  }
  fflush(wal->file);
  wal->file_pos = sizeof(wal_header_t);
  wal->buf_len = 0;
  wal->periodic_count = 0;
  return KANBUDB_OK;
}

/* ── Multi-process WAL functions ───────────────────────────── */

#define KANBUDB_WAL_FRAME_MAGIC 0x57414C46524DULL

typedef struct __attribute__((packed)) {
    uint64_t frame_magic;
    uint64_t seq;
    uint8_t  op;
    uint64_t table_id;
    uint64_t key_len;
    uint64_t val_len;
    uint32_t crc;
} wal_frame_header_t;

int kanbudb_wal_append_frame(kanbudb_wal_t* wal,
                              uint64_t seq, int op,
                              uint64_t table_id,
                              const void* key, size_t key_len,
                              const void* value, size_t val_len)
{
    if (!wal) return KANBUDB_ERR_INVAL;

    /* Flush any buffered data first */
    int rc = wal_flush_buf(wal);
    if (rc != KANBUDB_OK) return rc;

    size_t frame_size = sizeof(wal_frame_header_t) + key_len +
                        (op == KANBUDB_WAL_PUT && value ? val_len : 0);
    uint8_t* frame = (uint8_t*)malloc(frame_size);
    if (!frame) return KANBUDB_ERR_OOM;

    wal_frame_header_t* hdr = (wal_frame_header_t*)frame;
    hdr->frame_magic = KANBUDB_WAL_FRAME_MAGIC;
    hdr->seq = seq;
    hdr->op = (op == KANBUDB_WAL_DELETE) ? 1 : 0;
    hdr->table_id = table_id;
    hdr->key_len = key_len;
    hdr->val_len = (op == KANBUDB_WAL_PUT) ? val_len : 0;

    size_t off = sizeof(wal_frame_header_t);
    if (key_len > 0) {
        memcpy(frame + off, key, key_len);
        off += key_len;
    }
    if (hdr->op == 0 && value && val_len > 0) {
        memcpy(frame + off, value, val_len);
        off += val_len;
    }

    /* CRC covers everything after the crc field (payload: key + value).
     * The header fields (seq, op, table_id, key_len, val_len) are validated
     * by the magic check and structural parsing. */
    size_t after_crc = offsetof(wal_frame_header_t, crc) + sizeof(hdr->crc);
    size_t payload_len = frame_size > after_crc ? frame_size - after_crc : 0;
    uint32_t crc = payload_len > 0
        ? crc32_wal(frame + after_crc, payload_len, 0)
        : 0;
    hdr->crc = crc;

    /* Get actual file end — another process may have truncated the WAL
     * (e.g. after flush), so wal->file_pos may be stale. */
    off_t file_end = lseek(wal->fd, 0, SEEK_END);
    if (file_end < 0) { free(frame); return KANBUDB_ERR_IO; }

    /* pwrite directly to kernel page cache — visible to other processes */
    ssize_t written = pwrite(wal->fd, frame, frame_size, file_end);

    int ret = KANBUDB_OK;
    if ((size_t)written != frame_size) {
        ret = KANBUDB_ERR_IO;
    } else {
        wal->file_pos = (uint64_t)(file_end + frame_size);
        wal->seq = seq;

        if (wal->fsync_mode == KANBUDB_FSYNC_ALWAYS) {
            fdatasync(wal->fd);
        }
    }

    free(frame);
    return ret;
}

uint64_t kanbudb_wal_file_size(kanbudb_wal_t* wal) {
    if (!wal) return 0;
    struct stat st;
    if (fstat(wal->fd, &st) < 0) return wal->file_pos;
    return (uint64_t)st.st_size;
}

int kanbudb_wal_scan_frames(const char* wal_path,
                             uint64_t start_offset, uint64_t end_offset,
                             int (*cb)(const kanbudb_wal_frame_t* frame, void* ctx),
                             void* ctx)
{
    int fd = open(wal_path, O_RDONLY);
    if (fd < 0) return 0;

    uint8_t buf[1048576 + sizeof(wal_frame_header_t)];
    size_t buflen = 0;
    uint64_t offset = start_offset;
    int count = 0;

    while (offset < end_offset) {
        size_t to_read = sizeof(buf) - buflen;
        if (to_read > end_offset - offset)
            to_read = (size_t)(end_offset - offset);

        ssize_t n = pread(fd, buf + buflen, to_read, (off_t)offset);
        if (n <= 0) break;

        offset += (uint64_t)n;
        buflen += (size_t)n;

        while (buflen >= sizeof(wal_frame_header_t)) {
            wal_frame_header_t* hdr = (wal_frame_header_t*)buf;
            if (hdr->frame_magic != KANBUDB_WAL_FRAME_MAGIC) {
                goto done;
            }

            size_t frame_sz = sizeof(wal_frame_header_t) +
                              (size_t)hdr->key_len +
                              (hdr->op == 0 ? (size_t)hdr->val_len : 0);

            if (buflen < frame_sz) break;

            /* Verify CRC over payload (everything after crc field) */
            size_t after_crc = offsetof(wal_frame_header_t, crc) + sizeof(hdr->crc);
            size_t payload_len = frame_sz > after_crc ? frame_sz - after_crc : 0;
            uint32_t computed = payload_len > 0
                ? crc32_wal((uint8_t*)hdr + after_crc, payload_len, 0)
                : 0;
            if (computed != hdr->crc) {
                goto done;
            }

            kanbudb_wal_frame_t frame;
            frame.seq = hdr->seq;
            frame.op = hdr->op;
            frame.table_id = hdr->table_id;
            frame.key_len = (size_t)hdr->key_len;
            frame.val_len = (hdr->op == 0) ? (size_t)hdr->val_len : 0;
            frame.key = frame.key_len > 0 ? buf + sizeof(wal_frame_header_t) : NULL;
            frame.value = frame.val_len > 0 ?
                buf + sizeof(wal_frame_header_t) + frame.key_len : NULL;

            if (cb) {
                int rc = cb(&frame, ctx);
                if (rc != 0) { close(fd); return count; }
            }

            count++;
            size_t consumed = frame_sz;
            if (buflen > consumed) {
                memmove(buf, buf + consumed, buflen - consumed);
            }
            buflen -= consumed;
        }
    }

done:
    close(fd);
    return count;
}
