#include "wal.h"
#include "db.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#ifdef KANBUDB_HAVE_IOURING
#include <liburing.h>
#endif

#define KANBUDB_WAL_MAGIC  0x4845524D4553ULL
#define KANBUDB_WAL_VERSION 1
#define KANBUDB_WAL_PERIODIC_THRESHOLD 1000

/* Write buffer size: 64 KB — large enough to batch many small writes */
#define KANBUDB_WAL_BUF_SIZE 65536

typedef struct __attribute__((packed)) {
  uint64_t magic;
  uint32_t version;
  uint64_t seq;
} wal_header_t;

struct kanbudb_wal {
  char* path;
  int   fd;              /* file descriptor (io_uring + fallback) */
  int   fsync_mode;
  uint64_t seq;
  uint64_t file_pos;     /* current write position in the file (for io_uring) */
  int   periodic_count;

  /* Write buffer for batching (used in both io_uring and fallback mode) */
  uint8_t* buf;
  size_t   buf_len;

#ifdef KANBUDB_HAVE_IOURING
  /* io_uring state */
  int     use_io_uring;   /* 1 = io_uring active, 0 = fallback */
  struct io_uring ring;
#endif

  /* Fallback stdio — used when io_uring unavailable OR for replay/truncate */
  FILE* file;
};

/* ── Serialize one WAL entry into the buffer ────────────── */
/* Returns the number of bytes written, or <0 on error. */
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

/* ── Flush the write buffer to disk ─────────────────────── */

static int wal_flush_buf(kanbudb_wal_t* wal)
{
  if (wal->buf_len == 0) return KANBUDB_OK;

#ifdef KANBUDB_HAVE_IOURING
  if (wal->use_io_uring) {
    /* io_uring path: submit write + optional fsync, wait for completion */
    struct io_uring_sqe* sqe = io_uring_get_sqe(&wal->ring);
    if (!sqe) {
      /* Ring full — flush completions first */
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

    /* Submit and wait for write completion */
    int ret = io_uring_submit_and_wait(&wal->ring, 1);
    if (ret < 0) return KANBUDB_ERR_IO;

    struct io_uring_cqe* cqe;
    unsigned head;
    io_uring_for_each_cqe(&wal->ring, head, cqe) {
      if (cqe->res < 0) {
        io_uring_cqe_seen(&wal->ring, cqe);
        wal->buf_len = 0;
        return KANBUDB_ERR_IO;
      }
      io_uring_cqe_seen(&wal->ring, cqe);
    }

    wal->file_pos += wal->buf_len;
    wal->buf_len = 0;
    return KANBUDB_OK;
  }
#endif

  /* Fallback stdio path */
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

/* ── wal_create ─────────────────────────────────────────── */

kanbudb_wal_t* wal_create(const char* path, int fsync_mode)
{
  kanbudb_wal_t* wal = (kanbudb_wal_t*)calloc(1, sizeof(*wal));
  if (!wal) return NULL;

  wal->path = strdup(path);
  if (!wal->path) { free(wal); return NULL; }
  wal->fsync_mode = fsync_mode;

  /* Allocate write buffer */
  wal->buf = (uint8_t*)malloc(KANBUDB_WAL_BUF_SIZE);
  if (!wal->buf) { free(wal->path); free(wal); return NULL; }

#ifdef KANBUDB_HAVE_IOURING
  /* Try io_uring init — fall back to stdio on failure */
  wal->use_io_uring = 0;
  memset(&wal->ring, 0, sizeof(wal->ring));
  unsigned uring_flags = 0;
#ifdef IORING_SETUP_COOP_TASKRUN
  uring_flags |= IORING_SETUP_COOP_TASKRUN;
#endif
  /* Note: IORING_SETUP_SINGLE_ISSUER not used — db_put can be called
   * from multiple threads concurrently, which violates its requirement. */
  int uring_ret = io_uring_queue_init(32, &wal->ring, uring_flags);
  if (uring_ret == 0) {
    wal->use_io_uring = 1;
  }
#endif

  /* Open the WAL file (needed for both io_uring and fallback) */
  int flags = O_RDWR | O_CREAT;
  wal->fd = open(path, flags, 0644);
  if (wal->fd < 0) {
    free(wal->buf); free(wal->path); free(wal);
    return NULL;
  }

  /* Also open FILE* for replay (reading) */
  wal->file = fopen(path, "a+b");
  if (!wal->file) {
    close(wal->fd); free(wal->buf); free(wal->path); free(wal);
    return NULL;
  }

  fseek(wal->file, 0, SEEK_END);
  long size = ftell(wal->file);

  if (size == 0) {
    /* New WAL: write header using stdio (one-time, not perf-critical) */
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
    /* Write position now at end of header */
  } else {
    /* Existing WAL: read header to restore seq */
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
    /* Calculate write position: header + existing entries */
    long end_pos = ftell(wal->file);
    wal->file_pos = (end_pos > 0) ? (uint64_t)end_pos : sizeof(wal_header_t);
  }

  wal->buf_len = 0;
  wal->periodic_count = 0;
  return wal;
}

/* ── wal_destroy ────────────────────────────────────────── */

void wal_destroy(kanbudb_wal_t* wal)
{
  if (!wal) return;

  /* Flush any pending data */
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

/* ── wal_append ─────────────────────────────────────────── */

int wal_append(kanbudb_wal_t* wal, int op,
               uint64_t table_id, const void* key, size_t key_len,
               const void* value, size_t val_len)
{
  if (!wal) return KANBUDB_ERR_INVAL;

  wal->seq++;

  /* Serialize into the buffer */
  int written = wal_serialize(wal->buf + wal->buf_len,
                               KANBUDB_WAL_BUF_SIZE - wal->buf_len,
                               wal->seq, op, table_id,
                               key, key_len, value, val_len);
  if (written < 0) {
    /* Buffer full — flush first, then retry */
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

  /* Auto-flush: in ALWAYS mode, flush immediately.
   * In PERIODIC mode, flush periodically.
   * In NONE mode, let wal_sync or buffer-full trigger flush. */
  if (wal->fsync_mode == KANBUDB_FSYNC_ALWAYS) {
    return wal_flush_buf(wal);
  }

  if (wal->buf_len >= KANBUDB_WAL_BUF_SIZE / 2) {
    return wal_flush_buf(wal);
  }

  return KANBUDB_OK;
}

/* ── wal_sync ───────────────────────────────────────────── */

int wal_sync(kanbudb_wal_t* wal)
{
  if (!wal) return KANBUDB_ERR_INVAL;
  int rc = wal_flush_buf(wal);
  if (rc != KANBUDB_OK) return rc;

  /* Ensure all data is on disk */
#ifdef KANBUDB_HAVE_IOURING
  if (wal->use_io_uring) {
    struct io_uring_sqe* sqe = io_uring_get_sqe(&wal->ring);
    if (!sqe) return KANBUDB_ERR_BUSY;
    io_uring_prep_fsync(sqe, wal->fd, IORING_FSYNC_DATASYNC);
    sqe->user_data = 3;
    io_uring_submit_and_wait(&wal->ring, 1);
    struct io_uring_cqe* cqe;
    unsigned head;
    io_uring_for_each_cqe(&wal->ring, head, cqe) {
      if (cqe->res < 0) { io_uring_cqe_seen(&wal->ring, cqe); return KANBUDB_ERR_IO; }
      io_uring_cqe_seen(&wal->ring, cqe);
    }
    return KANBUDB_OK;
  }
#endif

  fflush(wal->file);
  wal->periodic_count = 0;
  return KANBUDB_OK;
}

/* ── wal_replay (always uses stdio — read path) ────────── */

int wal_replay(kanbudb_wal_t* wal,
               int (*callback)(int op, uint64_t table_id,
                               const void* key, size_t key_len,
                               const void* value, size_t val_len,
                               void* ctx),
               void* ctx)
{
  if (!wal || !wal->file) return KANBUDB_ERR_INVAL;

  /* Flush any pending writes first */
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

/* ── wal_last_seq ───────────────────────────────────────── */

uint64_t wal_last_seq(kanbudb_wal_t* wal)
{
  return wal ? wal->seq : 0;
}

/* ── wal_truncate ───────────────────────────────────────── */

int wal_truncate(kanbudb_wal_t* wal)
{
  if (!wal) return KANBUDB_ERR_INVAL;

  /* Flush any pending data */
  wal_flush_buf(wal);

  /* Close old fd and file */
  if (wal->file) { fclose(wal->file); wal->file = NULL; }
  if (wal->fd >= 0) { close(wal->fd); wal->fd = -1; }

  /* Reopen with truncation */
  wal->fd = open(wal->path, O_RDWR | O_CREAT | O_TRUNC, 0644);
  if (wal->fd < 0) return KANBUDB_ERR_IO;

  wal->file = fopen(wal->path, "wb");
  if (!wal->file) { close(wal->fd); wal->fd = -1; return KANBUDB_ERR_IO; }

  /* Write fresh header with current seq */
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
