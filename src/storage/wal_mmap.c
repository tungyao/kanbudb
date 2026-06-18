#include "wal_mmap.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

static size_t page_align(size_t size) {
  size_t page = 4096;
  return (size + page - 1) & ~(page - 1);
}

static int serialize_entry(uint8_t* buf, size_t cap,
                           uint64_t seq, int op, uint64_t table_id,
                           const void* key, size_t key_len,
                           const void* value, size_t val_len) {
  uint8_t op_u8 = (op == 1) ? 1 : 0;
  size_t needed = sizeof(seq) + 1 + sizeof(table_id) +
                  sizeof(uint64_t) + sizeof(uint64_t) +
                  key_len + ((op_u8 == 0) ? val_len : 0);
  if (needed > cap) return -1;

  size_t off = 0;
  memcpy(buf + off, &seq, sizeof(seq)); off += sizeof(seq);
  memcpy(buf + off, &op_u8, 1); off += 1;
  memcpy(buf + off, &table_id, sizeof(table_id)); off += sizeof(table_id);
  uint64_t kl = key_len, vl = val_len;
  memcpy(buf + off, &kl, sizeof(kl)); off += sizeof(kl);
  memcpy(buf + off, &vl, sizeof(vl)); off += sizeof(vl);
  if (key_len > 0) { memcpy(buf + off, key, key_len); off += key_len; }
  if (op_u8 == 0 && value && val_len > 0) {
    memcpy(buf + off, value, val_len); off += val_len;
  }
  return (int)off;
}

int wal_mmap_open(const char* path, int rw, size_t data_cap,
                  kanbudb_wal_mmap_t* wm) {
  memset(wm, 0, sizeof(*wm));

  size_t total_size = page_align(KANBUDB_WAL_MMAP_HEADER_SIZE + data_cap);
  if (data_cap == 0) total_size = 0;

  if (rw && total_size > 0) {
    int fd = open(path, O_RDWR | O_CREAT, 0644);
    if (fd >= 0) {
      uint64_t cur_size = 0;
      kanbudb_file_size(path, &cur_size);
      if (cur_size < total_size) {
        uint8_t zeros[4096] = {0};
        size_t remaining = total_size - cur_size;
        while (remaining > 0) {
          size_t to_write = remaining < sizeof(zeros) ? remaining : sizeof(zeros);
          write(fd, zeros, to_write);
          remaining -= to_write;
        }
      }
      close(fd);
    }
  }

  int rc = kanbudb_mmap_open(path, rw, total_size, &wm->region);
  if (rc < 0) return -1;

  wm->header = (kanbudb_wal_mmap_header_t*)wm->region.addr;
  wm->data = wm->region.addr + KANBUDB_WAL_MMAP_HEADER_SIZE;
  wm->data_cap = wm->region.size - KANBUDB_WAL_MMAP_HEADER_SIZE;
  wm->last_seq = 0;

  if (rw && wm->header->magic != KANBUDB_WAL_MMAP_MAGIC) {
    memset(wm->region.addr, 0, wm->region.size);
    kanbudb_atomic_store_u64(&wm->header->magic, KANBUDB_WAL_MMAP_MAGIC);
    kanbudb_atomic_store_u32(&wm->header->version, KANBUDB_WAL_MMAP_VERSION);
    kanbudb_atomic_store_u64(&wm->header->write_pos, KANBUDB_WAL_MMAP_HEADER_SIZE);
    kanbudb_atomic_store_u64(&wm->header->data_size, wm->data_cap);
    kanbudb_mmap_sync(&wm->region);
  }

  return 0;
}

int wal_mmap_append(kanbudb_wal_mmap_t* wm, uint64_t seq, int op,
                    uint64_t table_id, const void* key, size_t key_len,
                    const void* value, size_t val_len) {
  uint64_t write_pos = kanbudb_atomic_load_u64(&wm->header->write_pos);
  size_t data_offset = write_pos - KANBUDB_WAL_MMAP_HEADER_SIZE;

  int written = serialize_entry(wm->data + data_offset,
                                wm->data_cap - data_offset,
                                seq, op, table_id, key, key_len, value, val_len);
  if (written < 0) return -1;

  kanbudb_atomic_store_u64(&wm->header->write_pos, write_pos + written);
  wm->last_seq = write_pos + written - KANBUDB_WAL_MMAP_HEADER_SIZE;
  return written;
}

int wal_mmap_read_entry(kanbudb_wal_mmap_t* wm, uint64_t* out_seq,
                        int* out_op, uint64_t* out_table_id,
                        void** out_key, size_t* out_key_len,
                        void** out_value, size_t* out_val_len) {
  uint64_t write_pos = kanbudb_atomic_load_u64(&wm->header->write_pos);
  uint64_t read_pos = wm->last_seq + KANBUDB_WAL_MMAP_HEADER_SIZE;

  if (read_pos >= write_pos) return -1;

  size_t off = wm->last_seq;
  size_t remaining = (size_t)(write_pos - read_pos);
  if (remaining < 8 + 1 + 8 + 8 + 8) return -1;

  uint64_t seq, table_id, kl, vl;
  uint8_t op_u8;

  memcpy(&seq, wm->data + off, sizeof(seq)); off += sizeof(seq);
  memcpy(&op_u8, wm->data + off, 1); off += 1;
  memcpy(&table_id, wm->data + off, sizeof(table_id)); off += sizeof(table_id);
  memcpy(&kl, wm->data + off, sizeof(kl)); off += sizeof(kl);
  memcpy(&vl, wm->data + off, sizeof(vl)); off += sizeof(vl);

  *out_seq = seq;
  *out_op = (op_u8 == 0) ? 0 : 1;
  *out_table_id = table_id;
  *out_key = wm->data + off;
  *out_key_len = (size_t)kl;
  off += (size_t)kl;

  if (op_u8 == 0 && vl > 0) {
    *out_value = wm->data + off;
    *out_val_len = (size_t)vl;
    off += (size_t)vl;
  } else {
    *out_value = NULL;
    *out_val_len = 0;
  }

  wm->last_seq = off;
  return 0;
}

uint64_t wal_mmap_get_write_pos(kanbudb_wal_mmap_t* wm) {
  return kanbudb_atomic_load_u64(&wm->header->write_pos);
}

int wal_mmap_sync(kanbudb_wal_mmap_t* wm) {
  return kanbudb_mmap_sync(&wm->region);
}

int wal_mmap_close(kanbudb_wal_mmap_t* wm) {
  return kanbudb_mmap_close(&wm->region);
}

int wal_mmap_switch_file(const char* wal_path, kanbudb_wal_mmap_t* wm) {
  char new_path[512];
  snprintf(new_path, sizeof(new_path), "%s.new", wal_path);

  kanbudb_wal_mmap_t new_wm;
  size_t data_cap = wm->data_cap > 0 ? wm->data_cap : 64 * 1024 * 1024;
  int rc = wal_mmap_open(new_path, 1, data_cap, &new_wm);
  if (rc < 0) return -1;

  wal_mmap_sync(&new_wm);
  wal_mmap_close(&new_wm);

  kanbudb_mmap_close(&wm->region);

  if (rename(new_path, wal_path) < 0) return -1;

  return wal_mmap_open(wal_path, 1, data_cap, wm);
}
