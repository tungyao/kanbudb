#include "sstable.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <errno.h>

/* ── CRC32 (table-driven) ────────────────────────────────── */

static uint32_t crc32_table[256];

__attribute__((constructor))
static void crc32_init_table(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t crc = i;
        for (int j = 0; j < 8; j++) {
            if (crc & 1)
                crc = (crc >> 1) ^ 0xEDB88320U;
            else
                crc >>= 1;
        }
        crc32_table[i] = crc;
    }
}

static uint32_t crc32_bytes(const void* data, size_t len, uint32_t crc) {
    const uint8_t* buf = (const uint8_t*)data;
    crc = ~crc;
    for (size_t i = 0; i < len; i++) {
        crc = crc32_table[(crc ^ buf[i]) & 0xFF] ^ (crc >> 8);
    }
    return ~crc;
}

/* ── Writer ──────────────────────────────────────────────── */

struct sstable_writer {
    FILE*       tmp_file;
    char*       tmp_path;
    char*       final_path;
    uint64_t    sequence;
    uint64_t    num_entries;
    uint64_t    data_len;
    uint64_t    entry_count_since_index;
    /* buffered index entries */
    uint8_t*    index_buf;
    size_t      index_cap;
    size_t      index_len;
};

sstable_writer_t* sstable_writer_create(const char* tmp_path,
                                         const char* final_path,
                                         uint64_t sequence) {
    sstable_writer_t* w = (sstable_writer_t*)calloc(1, sizeof(*w));
    if (!w) return NULL;

    w->tmp_path = strdup(tmp_path);
    w->final_path = strdup(final_path);
    if (!w->tmp_path || !w->final_path) {
        free(w->tmp_path); free(w->final_path); free(w);
        return NULL;
    }
    w->sequence = sequence;
    w->num_entries = 0;
    w->entry_count_since_index = 0;
    w->index_cap = 4096;
    w->index_buf = (uint8_t*)malloc(w->index_cap);
    if (!w->index_buf) {
        free(w->tmp_path); free(w->final_path); free(w);
        return NULL;
    }
    w->index_len = 0;

    w->tmp_file = fopen(tmp_path, "wb");
    if (!w->tmp_file) {
        free(w->tmp_path); free(w->final_path);
        free(w->index_buf); free(w);
        return NULL;
    }

    /* Write placeholder header — will be patched at finish */
    sstable_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    if (fwrite(&hdr, sizeof(hdr), 1, w->tmp_file) != 1) {
        fclose(w->tmp_file); unlink(tmp_path);
        free(w->tmp_path); free(w->final_path);
        free(w->index_buf); free(w);
        return NULL;
    }

    w->data_len = 0;
    return w;
}

int sstable_writer_add(sstable_writer_t* w,
                        const void* key, size_t key_len,
                        const void* value, size_t val_len,
                        uint8_t flag) {
    if (!w || !w->tmp_file || !key) return KANBUDB_ERR_INVAL;
    if (key_len > 1048576 || val_len > 1048576) return KANBUDB_ERR_INVAL;

    /* Every K interval entries, emit an index entry */
    if (w->entry_count_since_index == 0) {
        /* index entry: key_len(u32) + key + offset(u64) */
        size_t entry_sz = sizeof(uint32_t) + key_len + sizeof(uint64_t);
        if (w->index_len + entry_sz > w->index_cap) {
            while (w->index_len + entry_sz > w->index_cap) w->index_cap *= 2;
            uint8_t* new_buf = (uint8_t*)realloc(w->index_buf, w->index_cap);
            if (!new_buf) return KANBUDB_ERR_OOM;
            w->index_buf = new_buf;
        }
        uint64_t off = (uint64_t)ftell(w->tmp_file);
        memcpy(w->index_buf + w->index_len, &key_len, sizeof(uint32_t));
        w->index_len += sizeof(uint32_t);
        if (key_len > 0) {
            memcpy(w->index_buf + w->index_len, key, key_len);
            w->index_len += key_len;
        }
        memcpy(w->index_buf + w->index_len, &off, sizeof(uint64_t));
        w->index_len += sizeof(uint64_t);
    }
    w->entry_count_since_index++;
    if (w->entry_count_since_index >= KANBUDB_SSTABLE_SPARSE_INTERVAL)
        w->entry_count_since_index = 0;

    /* Write data entry: key_len(u32) + key + flag(u8) + val_len(u32) + val */
    size_t entry_bytes = sizeof(uint32_t) + key_len + sizeof(uint8_t) + sizeof(uint32_t) + val_len;
    if (fwrite(&key_len, sizeof(uint32_t), 1, w->tmp_file) != 1)
        return KANBUDB_ERR_IO;
    if (key_len > 0 && fwrite(key, 1, key_len, w->tmp_file) != key_len)
        return KANBUDB_ERR_IO;
    if (fwrite(&flag, sizeof(uint8_t), 1, w->tmp_file) != 1)
        return KANBUDB_ERR_IO;
    if (fwrite(&val_len, sizeof(uint32_t), 1, w->tmp_file) != 1)
        return KANBUDB_ERR_IO;
    if (val_len > 0 && value && fwrite(value, 1, val_len, w->tmp_file) != val_len)
        return KANBUDB_ERR_IO;

    w->num_entries++;
    w->data_len += entry_bytes;
    return KANBUDB_OK;
}

int sstable_writer_finish(sstable_writer_t* w) {
    if (!w || !w->tmp_file) return KANBUDB_ERR_INVAL;

    /* Write index block: num_entries(u32) + entries */
    uint64_t index_offset = (uint64_t)ftell(w->tmp_file);
    uint32_t num_idx = (uint32_t)(w->index_len > 0
                        ? w->index_len / (sizeof(uint32_t) + 0 + sizeof(uint64_t))
                        : 0);
    /* ugh, can't compute num_idx without counting — let's re-derive.
       Actually index entries are variable because key_len varies.
       Let me store just the raw bytes and prepend count. */
    uint32_t index_count = 0;
    size_t pos = 0;
    (void)index_count;
    /* Write count of index entries first */
    /* We need to count them. Let's count from the buffer. */
    {
        size_t p = 0;
        uint32_t cnt = 0;
        while (p < w->index_len) {
            uint32_t kl;
            if (p + sizeof(uint32_t) > w->index_len) break;
            memcpy(&kl, w->index_buf + p, sizeof(uint32_t));
            p += sizeof(uint32_t) + kl + sizeof(uint64_t);
            cnt++;
        }
        if (fwrite(&cnt, sizeof(uint32_t), 1, w->tmp_file) != 1)
            return KANBUDB_ERR_IO;
    }
    if (w->index_len > 0) {
        if (fwrite(w->index_buf, 1, w->index_len, w->tmp_file) != w->index_len)
            return KANBUDB_ERR_IO;
    }

    /* Compute CRC32 over data + index bytes */
    uint64_t footer_start = (uint64_t)ftell(w->tmp_file);
    (void)footer_start;

    /* Go back and read all data bytes for CRC */
    uint32_t checksum = 0;
    {
        fflush(w->tmp_file);
        /* Read data block */
        size_t data_bytes = (size_t)(index_offset - sizeof(sstable_header_t));
        uint8_t* buf = (uint8_t*)malloc(data_bytes + 1);
        if (buf) {
            fseek(w->tmp_file, sizeof(sstable_header_t), SEEK_SET);
            if (fread(buf, 1, data_bytes, w->tmp_file) == data_bytes) {
                checksum = crc32_bytes(buf, data_bytes, 0);
                /* Also include index bytes */
                size_t idx_bytes_total = (size_t)(footer_start - index_offset);
                uint8_t* idx_buf = (uint8_t*)malloc(idx_bytes_total);
                if (idx_buf) {
                    fseek(w->tmp_file, (long)index_offset, SEEK_SET);
                    if (fread(idx_buf, 1, idx_bytes_total, w->tmp_file) == idx_bytes_total) {
                        checksum = crc32_bytes(idx_buf, idx_bytes_total, checksum);
                    }
                    free(idx_buf);
                }
            }
            free(buf);
        }
        fseek(w->tmp_file, 0, SEEK_END);
    }

    /* Write footer checksum */
    if (fwrite(&checksum, sizeof(uint32_t), 1, w->tmp_file) != 1)
        return KANBUDB_ERR_IO;

    /* Patch header */
    sstable_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic       = KANBUDB_SSTABLE_MAGIC;
    hdr.version     = KANBUDB_SSTABLE_VERSION;
    hdr.num_entries = w->num_entries;
    hdr.data_len    = index_offset - sizeof(sstable_header_t);
    hdr.index_offset = index_offset;
    hdr.sequence    = w->sequence;
    hdr.created_at  = (uint64_t)time(NULL);

    fseek(w->tmp_file, 0, SEEK_SET);
    if (fwrite(&hdr, sizeof(hdr), 1, w->tmp_file) != 1)
        return KANBUDB_ERR_IO;

    /* fsync tmp then rename */
    fflush(w->tmp_file);
    if (fsync(fileno(w->tmp_file)) != 0)
        return KANBUDB_ERR_IO;

    fclose(w->tmp_file);
    w->tmp_file = NULL;

    if (rename(w->tmp_path, w->final_path) != 0)
        return KANBUDB_ERR_IO;

    /* fsync parent directory for rename durability */
    char dir_buf[1024];
    const char* slash = strrchr(w->final_path, '/');
    if (slash) {
        size_t dlen = (size_t)(slash - w->final_path);
        if (dlen >= sizeof(dir_buf)) dlen = sizeof(dir_buf) - 1;
        memcpy(dir_buf, w->final_path, dlen);
        dir_buf[dlen] = '\0';
        int dir_fd = open(dir_buf, O_RDONLY | O_DIRECTORY);
        if (dir_fd >= 0) {
            fsync(dir_fd);
            close(dir_fd);
        }
    }

    return KANBUDB_OK;
}

void sstable_writer_destroy(sstable_writer_t* w) {
    if (!w) return;
    if (w->tmp_file) {
        fclose(w->tmp_file);
        unlink(w->tmp_path);
    }
    free(w->tmp_path);
    free(w->final_path);
    free(w->index_buf);
    free(w);
}

/* ── Internal index entry parsing ───────────────────────── */

/* In-memory sparse index entry: key pointer + offset */
typedef struct {
    uint8_t* key;
    uint32_t key_len;
    uint64_t offset;
} sstable_idx_entry_t;

/* ── Reader ──────────────────────────────────────────────── */

struct sstable_reader {
    char*          path;
    FILE*          file;
    sstable_header_t header;
    sstable_idx_entry_t* index;
    uint32_t       index_count;
    uint32_t       crc_ok;    /* 0=not checked, 1=ok, -1=fail */
};

static int idx_cmp(const void* a, const void* b) {
    const uint8_t* key_a = (const uint8_t*)a;
    sstable_idx_entry_t* ib = (sstable_idx_entry_t*)b;
    size_t ka_len = strlen((const char*)key_a);  /* caller passes full key via a */
    (void)ka_len;
    /* Actually we need a custom comparator that takes lengths.
       The bsearch callback doesn't support passing lengths. We'll do binary search manually. */
    return 0; /* dummy */
}

sstable_reader_t* sstable_reader_open(const char* path) {
    if (!path) return NULL;

    sstable_reader_t* r = (sstable_reader_t*)calloc(1, sizeof(*r));
    if (!r) return NULL;

    r->path = strdup(path);
    if (!r->path) { free(r); return NULL; }

    r->file = fopen(path, "rb");
    if (!r->file) { free(r->path); free(r); return NULL; }

    /* Read header */
    if (fread(&r->header, sizeof(r->header), 1, r->file) != 1) {
        fclose(r->file); free(r->path); free(r);
        return NULL;
    }

    if (r->header.magic != KANBUDB_SSTABLE_MAGIC ||
        r->header.version != KANBUDB_SSTABLE_VERSION) {
        fclose(r->file); free(r->path); free(r);
        return NULL;
    }

    /* Read index block */
    fseek(r->file, (long)r->header.index_offset, SEEK_SET);
    uint32_t num_idx = 0;
    if (fread(&num_idx, sizeof(uint32_t), 1, r->file) != 1) {
        fclose(r->file); free(r->path); free(r);
        return NULL;
    }

    r->index_count = num_idx;
    if (num_idx > 0) {
        r->index = (sstable_idx_entry_t*)calloc(num_idx, sizeof(sstable_idx_entry_t));
        if (!r->index) {
            fclose(r->file); free(r->path); free(r);
            return NULL;
        }
        for (uint32_t i = 0; i < num_idx; i++) {
            uint32_t kl;
            if (fread(&kl, sizeof(uint32_t), 1, r->file) != 1) {
                sstable_reader_close(r); return NULL;
            }
            r->index[i].key_len = kl;
            r->index[i].key = (uint8_t*)malloc(kl + 1);
            if (!r->index[i].key) { sstable_reader_close(r); return NULL; }
            if (kl > 0 && fread(r->index[i].key, 1, kl, r->file) != kl) {
                sstable_reader_close(r); return NULL;
            }
            r->index[i].key[kl] = '\0';
            if (fread(&r->index[i].offset, sizeof(uint64_t), 1, r->file) != 1) {
                sstable_reader_close(r); return NULL;
            }
        }
    }

    /* Verify CRC32 (optional — tolerate failure) */
    {
        fseek(r->file, 0, SEEK_END);
        long file_len = ftell(r->file);
        if (file_len > (long)sizeof(uint32_t)) {
            uint32_t stored_crc = 0;
            fseek(r->file, -(long)sizeof(uint32_t), SEEK_END);
            if (fread(&stored_crc, sizeof(uint32_t), 1, r->file) == 1) {
                size_t verify_len = (size_t)(file_len - (long)sizeof(uint32_t));
                uint8_t* verify_buf = (uint8_t*)malloc(verify_len);
                if (verify_buf) {
                    fseek(r->file, 0, SEEK_SET);
                    if (fread(verify_buf, 1, verify_len, r->file) == verify_len) {
                        uint32_t computed = crc32_bytes(verify_buf, verify_len, 0);
                        r->crc_ok = (computed == stored_crc) ? 1 : -1;
                    }
                    free(verify_buf);
                }
            }
        }
        /* Reset to data start */
        fseek(r->file, sizeof(sstable_header_t), SEEK_SET);
    }

    return r;
}

void sstable_reader_close(sstable_reader_t* r) {
    if (!r) return;
    if (r->file) fclose(r->file);
    free(r->path);
    if (r->index) {
        for (uint32_t i = 0; i < r->index_count; i++)
            free(r->index[i].key);
        free(r->index);
    }
    free(r);
}

/* Binary search over sparse index to find the entry with key >= target.
   Returns index of the first index entry with key >= target, or index_count if all keys < target. */
static uint32_t index_lower_bound(sstable_idx_entry_t* idx, uint32_t count,
                                   const void* key, size_t key_len) {
    uint32_t lo = 0, hi = count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        size_t min_len = key_len < idx[mid].key_len ? key_len : idx[mid].key_len;
        int cmp = memcmp(key, idx[mid].key, min_len);
        if (cmp == 0) {
            if (key_len < idx[mid].key_len) cmp = -1;
            else if (key_len > idx[mid].key_len) cmp = 1;
        }
        if (cmp <= 0)
            hi = mid;
        else
            lo = mid + 1;
    }
    return lo;
}

int sstable_reader_get(sstable_reader_t* r,
                        const void* key, size_t key_len,
                        void** out_value, size_t* out_val_len,
                        uint8_t* out_flag) {
    if (!r || !key) return KANBUDB_ERR_INVAL;

    /* Binary search sparse index to find starting point.
     * index_lower_bound returns the first idx entry with key >= target.
     * We need to scan from the previous block (or idx 0 if the target
     * falls before/between first index key). */
    uint32_t idx_pos = index_lower_bound(r->index, r->index_count, key, key_len);
    uint32_t max_entries = (uint32_t)(r->header.num_entries);

    uint32_t start_entry = 0;
    if (r->index_count > 0) {
        if (idx_pos > 0) {
            /* Target falls after idx[idx_pos-1].key — scan from that block */
            uint32_t bi = idx_pos - 1;
            fseek(r->file, (long)r->index[bi].offset, SEEK_SET);
            start_entry = bi * KANBUDB_SSTABLE_SPARSE_INTERVAL;
        } else {
            /* Target <= first index key — scan from very start */
            fseek(r->file, (long)r->index[0].offset, SEEK_SET);
            start_entry = 0;
        }
    } else {
        /* No index — full scan from start of data */
        fseek(r->file, sizeof(sstable_header_t), SEEK_SET);
        start_entry = 0;
    }

    /* Linear scan up to 2*SPARSE_INTERVAL entries */
    uint32_t limit = start_entry + 2 * KANBUDB_SSTABLE_SPARSE_INTERVAL;
    if (limit > max_entries) limit = max_entries;

    uint32_t entry_idx = start_entry;
    while (entry_idx < limit) {
        uint32_t kl;
        if (fread(&kl, sizeof(uint32_t), 1, r->file) != 1) break;
        uint8_t* kbuf = (uint8_t*)malloc(kl + 1);
        if (!kbuf) return KANBUDB_ERR_OOM;
        if (kl > 0 && fread(kbuf, 1, kl, r->file) != kl) { free(kbuf); break; }
        kbuf[kl] = '\0';

        uint8_t  fl;
        uint32_t vl;
        if (fread(&fl, sizeof(uint8_t), 1, r->file) != 1) { free(kbuf); break; }
        if (fread(&vl, sizeof(uint32_t), 1, r->file) != 1) { free(kbuf); break; }

        int cmp = 0;
        size_t min_len = key_len < kl ? key_len : kl;
        cmp = memcmp(key, kbuf, min_len);
        if (cmp == 0) {
            if (key_len < kl) cmp = -1;
            else if (key_len > kl) cmp = 1;
        }

        if (cmp == 0) {
            /* Found */
            if (out_flag) *out_flag = fl;
            if (fl & KANBUDB_SSTABLE_FLAG_TOMBSTONE) {
                free(kbuf);
                return KANBUDB_ERR_NOTFOUND;
            }
            if (out_value && out_val_len) {
                *out_value = malloc(vl);
                if (!*out_value && vl > 0) { free(kbuf); return KANBUDB_ERR_OOM; }
                if (vl > 0 && fread(*out_value, 1, vl, r->file) != vl) {
                    free(*out_value); free(kbuf); return KANBUDB_ERR_CORRUPT;
                }
                *out_val_len = vl;
            } else if (vl > 0) {
                /* skip value bytes */
                fseek(r->file, (long)vl, SEEK_CUR);
            }
            free(kbuf);
            return KANBUDB_OK;
        }

        if (cmp < 0) {
            /* Past where key would be — not found */
            free(kbuf);
            return KANBUDB_ERR_NOTFOUND;
        }

        /* Skip value bytes */
        if (vl > 0) fseek(r->file, (long)vl, SEEK_CUR);
        free(kbuf);
        entry_idx++;
    }

    return KANBUDB_ERR_NOTFOUND;
}

int sstable_reader_scan(sstable_reader_t* r,
                         int (*cb)(const void* key, size_t key_len,
                                   const void* value, size_t val_len,
                                   uint8_t flag, void* ctx),
                         void* ctx) {
    if (!r || !cb) return KANBUDB_ERR_INVAL;

    fseek(r->file, sizeof(sstable_header_t), SEEK_SET);

    for (uint64_t i = 0; i < r->header.num_entries; i++) {
        uint32_t kl;
        if (fread(&kl, sizeof(uint32_t), 1, r->file) != 1) break;
        uint8_t* kbuf = (uint8_t*)malloc(kl + 1);
        if (!kbuf) return KANBUDB_ERR_OOM;
        if (kl > 0 && fread(kbuf, 1, kl, r->file) != kl) { free(kbuf); break; }
        kbuf[kl] = '\0';

        uint8_t  fl;
        uint32_t vl;
        if (fread(&fl, sizeof(uint8_t), 1, r->file) != 1) { free(kbuf); break; }
        if (fread(&vl, sizeof(uint32_t), 1, r->file) != 1) { free(kbuf); break; }

        uint8_t* vbuf = NULL;
        if (vl > 0) {
            vbuf = (uint8_t*)malloc(vl);
            if (!vbuf) { free(kbuf); return KANBUDB_ERR_OOM; }
            if (fread(vbuf, 1, vl, r->file) != vl) { free(kbuf); free(vbuf); break; }
        }

        int rc = cb(kbuf, kl, vbuf, vl, fl, ctx);
        free(kbuf);
        free(vbuf);
        if (rc != 0) return rc;
    }

    return KANBUDB_OK;
}

uint64_t sstable_reader_num_entries(sstable_reader_t* r) {
    return r ? r->header.num_entries : 0;
}

uint64_t sstable_reader_sequence(sstable_reader_t* r) {
    return r ? r->header.sequence : 0;
}

int sstable_reader_crc_ok(sstable_reader_t* r) {
    return r ? r->crc_ok : 0;
}

/* ── Scan directory for .sst files ───────────────────────── */

int sstable_scan_dir(const char* dir_prefix,
                      char** out_paths, int max_count,
                      uint64_t* out_max_sequence) {
    if (!dir_prefix || !out_paths || max_count <= 0)
        return KANBUDB_ERR_INVAL;

    /* Extract directory from prefix */
    char dir[1024];
    const char* slash = strrchr(dir_prefix, '/');
    if (slash) {
        size_t dlen = (size_t)(slash - dir_prefix);
        if (dlen >= sizeof(dir)) dlen = sizeof(dir) - 1;
        memcpy(dir, dir_prefix, dlen);
        dir[dlen] = '\0';
    } else {
        strcpy(dir, ".");
    }

    /* Extract base name */
    const char* base = slash ? slash + 1 : dir_prefix;

    DIR* d = opendir(dir);
    if (!d) return 0;

    int count = 0;
    uint64_t max_seq = 0;

    struct dirent* entry;
    while ((entry = readdir(d)) != NULL && count < max_count) {
        /* Match: base.sst.* */
        if (entry->d_type == DT_DIR) continue;
        const char* name = entry->d_name;
        size_t blen = strlen(base);
        if (strncmp(name, base, blen) != 0) continue;
        if (name[blen] != '.') continue;
        const char* rest = name + blen + 1;
        if (strncmp(rest, "sst.", 4) != 0) continue;

        /* Parse .sst.LEVEL.SEQ */
        const char* seq_str = rest + 4;
        const char* dot = strchr(seq_str, '.');
        if (!dot) continue;
        uint64_t seq = (uint64_t)atoll(dot + 1);
        if (seq > max_seq) max_seq = seq;

        char full[1024];
        if (dir[0] == '.') {
            snprintf(full, sizeof(full), "%s", name);
        } else {
            snprintf(full, sizeof(full), "%s/%s", dir, name);
        }
        out_paths[count] = strdup(full);
        if (!out_paths[count]) break;
        count++;
    }
    closedir(d);

    if (out_max_sequence) *out_max_sequence = max_seq;
    return count;
}
