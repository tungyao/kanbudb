#ifndef KANBUDB_COMPACTION_H
#define KANBUDB_COMPACTION_H

#include "macros.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Multi-SSTable compaction ───────────────────────────────
 *
 * Merges a list of SSTable files into one output SSTable.
 * - Input files must each be sorted by key
 * - For duplicate keys across files, keeps the entry from the
 *   file with the HIGHEST sequence number (newest wins)
 * - Tombstones are propagated to the output (they signal deletion
 *   to the B-tree load path which skips them)
 *
 * Returns the path to the compacted SSTable in *out_path
 * (caller must free the string). Sets *out_num_entries.
 */

typedef struct kanbudb_compactor kanbudb_compactor_t;

kanbudb_compactor_t* compactor_create(void);
void                compactor_destroy(kanbudb_compactor_t* c);

int compactor_merge_sstables(kanbudb_compactor_t* c,
                              const char** input_paths, int num_inputs,
                              const char* output_path,
                              uint64_t output_sequence);

#ifdef __cplusplus
}
#endif

#endif /* KANBUDB_COMPACTION_H */
