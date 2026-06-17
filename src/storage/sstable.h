#ifndef KANBUDB_SSTABLE_H
#define KANBUDB_SSTABLE_H

#include "macros.h"
#include "bloom.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── SSTable on-disk format (v2) ──────────────────────────
 *
 * [HEADER]  48 bytes (see sstable_header_t)
 * [DATA]    sorted entries, each:
 *             key_len(u32) + key_bytes + flag(u8) + val_len(u32) + val_bytes
 *           flag: 0x01=alive, 0x02=tombstone
 * [INDEX]   sparse index, every 32 entries:
 *             num_entries(u32) + { key_len(u32) + key + offset(u64) } * N
 * [BLOOM]   serialized bloom filter (optional, v2+)
 *             num_bits(u32) + num_hashes(u32) + num_keys(u32) + bit_array
 * [FOOTER]  CHECKSUM_ALL(u32)  CRC32 of data+index+bloom bytes
 *──────────────────────────────────────────────────────────*/

#define KANBUDB_SSTABLE_MAGIC         0x4B534E54U  /* "KSNT" */
#define KANBUDB_SSTABLE_VERSION       2
#define KANBUDB_SSTABLE_SPARSE_INTERVAL 32

#define KANBUDB_SSTABLE_FLAG_ALIVE     0x01U
#define KANBUDB_SSTABLE_FLAG_TOMBSTONE 0x02U

#pragma pack(push, 1)
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint64_t num_entries;
    uint64_t data_len;
    uint64_t index_offset;   /* byte offset from file start to index block */
    uint64_t bloom_offset;   /* byte offset to bloom filter (0 = none, v2+) */
    uint64_t sequence;       /* WAL seq at flush time */
    uint64_t created_at;     /* unix sec */
} sstable_header_t;

typedef struct {
    uint32_t key_len;
    uint8_t  flag;           /* KANBUDB_SSTABLE_FLAG_* */
    uint32_t val_len;
    /* key + value follow inline */
} sstable_entry_prefix_t;

typedef struct {
    uint32_t key_len;
    /* key[key_len] follows */
    uint64_t offset;         /* byte offset of the entry in the data block */
} sstable_index_entry_t;
#pragma pack(pop)

/* ── Writer ──────────────────────────────────────────────── */

typedef struct sstable_writer sstable_writer_t;

sstable_writer_t* sstable_writer_create(const char* tmp_path,
                                        const char* final_path,
                                        uint64_t sequence);
int  sstable_writer_add(sstable_writer_t* w,
                        const void* key, size_t key_len,
                        const void* value, size_t val_len,
                        uint8_t flag);
int  sstable_writer_finish(sstable_writer_t* w);  /* fsync + rename */
void sstable_writer_destroy(sstable_writer_t* w);

/* ── Reader ──────────────────────────────────────────────── */

typedef struct sstable_reader sstable_reader_t;

sstable_reader_t* sstable_reader_open(const char* path);
void              sstable_reader_close(sstable_reader_t* r);

/* Point get — returns KANBUDB_OK / KANBUDB_ERR_NOTFOUND */
int sstable_reader_get(sstable_reader_t* r,
                       const void* key, size_t key_len,
                       void** out_value, size_t* out_val_len,
                       uint8_t* out_flag);

/* Full scan in sorted order */
int sstable_reader_scan(sstable_reader_t* r,
                        int (*cb)(const void* key, size_t key_len,
                                  const void* value, size_t val_len,
                                  uint8_t flag, void* ctx),
                        void* ctx);

/* Metadata */
uint64_t sstable_reader_num_entries(sstable_reader_t* r);
uint64_t sstable_reader_sequence(sstable_reader_t* r);
int      sstable_reader_crc_ok(sstable_reader_t* r);

/* ── Scan helper — find all .sst files in a directory ────── */
/* Returns number of .sst files found (up to max_count).
 * Fills out *paths with strdup'd filenames. Caller must free each. */
int sstable_scan_dir(const char* dir_prefix,
                     char** out_paths, int max_count,
                     uint64_t* out_max_sequence);

#ifdef __cplusplus
}
#endif

#endif /* KANBUDB_SSTABLE_H */
