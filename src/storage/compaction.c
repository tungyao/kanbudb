#include "compaction.h"
#include "sstable.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ── Data structures ──────────────────────────────────────── */

typedef struct {
    uint8_t* key;
    size_t   key_len;
    uint8_t* value;
    size_t   val_len;
    uint8_t  flag;
    uint64_t sequence;   /* from source SSTable, for dedup */
} merge_entry_t;

/* Context for the collection callback */
typedef struct {
    merge_entry_t** entries;
    size_t*         cap;
    size_t*         count;
    uint64_t        sequence;
} collect_ctx_t;

/* ── Comparator: sort by key, then by descending sequence ── */
static int merge_entry_compare(const void* a, const void* b) {
    const merge_entry_t* ea = (const merge_entry_t*)a;
    const merge_entry_t* eb = (const merge_entry_t*)b;
    size_t min_len = ea->key_len < eb->key_len ? ea->key_len : eb->key_len;
    int cmp = memcmp(ea->key, eb->key, min_len);
    if (cmp) return cmp;
    if (ea->key_len < eb->key_len) return -1;
    if (ea->key_len > eb->key_len) return 1;
    /* Same key — sort by descending sequence so newest comes first in dedup pass */
    if (ea->sequence > eb->sequence) return -1;
    if (ea->sequence < eb->sequence) return 1;
    return 0;
}

/* ── Scan callback: collect one entry into the array ─────── */
static int merge_collect_cb(const void* key, size_t key_len,
                             const void* value, size_t val_len,
                             uint8_t flag, void* ctx) {
    collect_ctx_t* cc = (collect_ctx_t*)ctx;

    if (*cc->count >= *cc->cap) {
        size_t new_cap = *cc->cap == 0 ? 4096 : *cc->cap * 2;
        merge_entry_t* new_entries = (merge_entry_t*)realloc(
            *cc->entries, new_cap * sizeof(merge_entry_t));
        if (!new_entries) return KANBUDB_ERR_OOM;
        *cc->entries = new_entries;
        *cc->cap = new_cap;
    }

    merge_entry_t* e = &(*cc->entries)[*cc->count];
    e->key = NULL;
    if (key_len > 0) {
        e->key = (uint8_t*)malloc(key_len);
        if (!e->key) return KANBUDB_ERR_OOM;
        memcpy(e->key, key, key_len);
    }
    e->key_len = key_len;

    e->value = NULL;
    if (val_len > 0) {
        e->value = (uint8_t*)malloc(val_len);
        if (!e->value) { free(e->key); return KANBUDB_ERR_OOM; }
        memcpy(e->value, value, val_len);
    }
    e->val_len = val_len;
    e->flag = flag;
    e->sequence = cc->sequence;

    (*cc->count)++;
    return KANBUDB_OK;
}

/* ── Compactor (stateless) ────────────────────────────────── */

struct kanbudb_compactor {
    int placeholder;
};

kanbudb_compactor_t* compactor_create(void) {
    kanbudb_compactor_t* c = (kanbudb_compactor_t*)calloc(1, sizeof(*c));
    if (c) c->placeholder = 0;
    return c;
}

void compactor_destroy(kanbudb_compactor_t* c) {
    free(c);
}

/* ── Main merge function ──────────────────────────────────── */

int compactor_merge_sstables(kanbudb_compactor_t* c,
                              const char** input_paths, int num_inputs,
                              const char* output_path,
                              uint64_t output_sequence) {
    (void)c;
    if (!input_paths || num_inputs <= 0 || !output_path)
        return KANBUDB_ERR_INVAL;

    /* Phase 1: collect all entries from all SSTables */
    merge_entry_t* entries = NULL;
    size_t         cap = 0;
    size_t         count = 0;

    for (int i = 0; i < num_inputs; i++) {
        sstable_reader_t* r = sstable_reader_open(input_paths[i]);
        if (!r) continue;

        uint64_t seq = sstable_reader_sequence(r);

        collect_ctx_t ctx;
        ctx.entries  = &entries;
        ctx.cap      = &cap;
        ctx.count    = &count;
        ctx.sequence = seq;

        int rc = sstable_reader_scan(r, merge_collect_cb, &ctx);
        sstable_reader_close(r);
        if (rc != KANBUDB_OK) {
            for (size_t j = 0; j < count; j++) {
                free(entries[j].key);
                free(entries[j].value);
            }
            free(entries);
            return rc;
        }
    }

    /* No entries? Write an empty SSTable */
    if (count == 0) {
        char tmp_path[1024];
        snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", output_path);
        sstable_writer_t* w = sstable_writer_create(tmp_path, output_path, output_sequence);
        if (!w) return KANBUDB_ERR_OOM;
        int rc = sstable_writer_finish(w);
        sstable_writer_destroy(w);
        return rc;
    }

    /* Phase 2: sort by key (desc seq for same key) */
    qsort(entries, count, sizeof(merge_entry_t), merge_entry_compare);

    /* Phase 3: write merged SSTable, deduping by key (keep first/highest-seq) */
    char tmp_path[1024];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", output_path);

    sstable_writer_t* w = sstable_writer_create(tmp_path, output_path, output_sequence);
    if (!w) {
        for (size_t j = 0; j < count; j++) {
            free(entries[j].key);
            free(entries[j].value);
        }
        free(entries);
        return KANBUDB_ERR_OOM;
    }

    int rc = KANBUDB_OK;
    int first = 1;
    const uint8_t* prev_key = NULL;
    size_t prev_key_len = 0;

    for (size_t i = 0; i < count; i++) {
        /* Skip if same key as previous (keep the first/highest-seq one) */
        if (!first &&
            entries[i].key_len == prev_key_len &&
            memcmp(entries[i].key, prev_key, entries[i].key_len) == 0) {
            continue;
        }

        rc = sstable_writer_add(w, entries[i].key, entries[i].key_len,
                                 entries[i].value, entries[i].val_len,
                                 entries[i].flag);
        if (rc != KANBUDB_OK) break;

        prev_key = entries[i].key;
        prev_key_len = entries[i].key_len;
        first = 0;
    }

    if (rc == KANBUDB_OK) {
        rc = sstable_writer_finish(w);
    } else {
        sstable_writer_destroy(w);
    }

    /* Cleanup */
    for (size_t j = 0; j < count; j++) {
        free(entries[j].key);
        free(entries[j].value);
    }
    free(entries);

    return rc;
}
