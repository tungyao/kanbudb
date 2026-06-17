#ifndef KANBUDB_COMPACTION_H
#define KANBUDB_COMPACTION_H

#include "macros.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Leveled Compaction ─────────────────────────────────────
 *
 * SSTables are organized into levels (L0..L7):
 *   L0: flush results (files may overlap in key range)
 *   L1-L7: sorted runs (no overlap within level)
 *
 * File naming: {db_path}.sst.{level}.{seq}
 *   e.g. /tmp/mydb.sst.0.42  (L0, seq=42)
 *        /tmp/mydb.sst.1.100 (L1, seq=100)
 *
 * Compaction policy:
 *   L0 trigger: level0_file_num_compaction_trigger (default 4)
 *   Ln trigger: max_bytes_for_level[n] exceeded
 *
 * Size targets (exponential):
 *   L1: 10 MB, L2: 100 MB, L3: 1 GB, L4: 10 GB, L5+: 100 GB
 */

#define KANBUDB_MAX_LEVELS 8
#define KANBUDB_L0_COMPACTION_TRIGGER 4
#define KANBUDB_LEVEL0_FILE_NUM_COMPACTION_TRIGGER 4

/* Per-level metadata */
typedef struct {
    int      level;          /* 0..7 */
    char**   file_paths;     /* full paths to SSTable files at this level */
    int      num_files;
    uint64_t total_size;     /* sum of file sizes */
} kanbudb_level_t;

/* ── Level management ───────────────────────────────────── */

/* Scan db_path for all .sst files and group by level.
 * Fills levels[0..max_levels-1]. Caller must free file_paths via
 * compaction_free_levels(). Returns 0 on success. */
int compaction_scan_levels(const char* db_path,
                           kanbudb_level_t* levels, int max_levels);

/* Free file_paths arrays allocated by compaction_scan_levels. */
void compaction_free_levels(kanbudb_level_t* levels, int max_levels);

/* ── Compaction picker ──────────────────────────────────── */

/* Check if compaction is needed. Returns the source level to compact
 * (0..max_levels-2), or -1 if no compaction needed. */
int compaction_pick_level(const kanbudb_level_t* levels, int max_levels);

/* ── Compaction executor ────────────────────────────────── */

/* Merge all files at `from_level` into one output file at `to_level`.
 * output_path: full path for the output file (e.g. /tmp/db.sst.1.100)
 * On success, the old input files should be deleted by the caller.
 * Returns KANBUDB_OK on success. */
int compaction_merge_to_level(const char** input_paths, int num_inputs,
                              const char* output_path,
                              uint64_t output_sequence);

/* ── Legacy compactor API (backwards compat) ───────────── */
typedef struct kanbudb_compactor kanbudb_compactor_t;

kanbudb_compactor_t* compactor_create(void);
void                 compactor_destroy(kanbudb_compactor_t* c);
int compactor_merge_sstables(kanbudb_compactor_t* c,
                              const char** input_paths, int num_inputs,
                              const char* output_path,
                              uint64_t output_sequence);

/* ── Utility ────────────────────────────────────────────── */

/* Build an SSTable path for a given level and sequence. */
void compaction_make_path(const char* db_path, int level,
                          uint64_t seq, char* out, size_t out_sz);

/* Return the max bytes target for a given level (L1=10MB, L2=100MB, etc.) */
uint64_t compaction_max_bytes_for_level(int level);

#ifdef __cplusplus
}
#endif

#endif /* KANBUDB_COMPACTION_H */
