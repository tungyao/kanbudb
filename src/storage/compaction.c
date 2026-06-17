#include "compaction.h"
#include "sstable.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <math.h>

/* ── Merge entry (for sorting) ────────────────────────────── */

typedef struct {
    uint8_t* key;
    size_t   key_len;
    uint8_t* value;
    size_t   val_len;
    uint8_t  flag;
    uint64_t sequence;
} merge_entry_t;

typedef struct {
    merge_entry_t** entries;
    size_t*         cap;
    size_t*         count;
    uint64_t        sequence;
} collect_ctx_t;

static int merge_entry_compare(const void* a, const void* b) {
    const merge_entry_t* ea = (const merge_entry_t*)a;
    const merge_entry_t* eb = (const merge_entry_t*)b;
    size_t min_len = ea->key_len < eb->key_len ? ea->key_len : eb->key_len;
    int cmp = memcmp(ea->key, eb->key, min_len);
    if (cmp) return cmp;
    if (ea->key_len < eb->key_len) return -1;
    if (ea->key_len > eb->key_len) return 1;
    if (ea->sequence > eb->sequence) return -1;
    if (ea->sequence < eb->sequence) return 1;
    return 0;
}

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

/* ── Scan directory, group SSTables by level ────────────── */

int compaction_scan_levels(const char* db_path,
                           kanbudb_level_t* levels, int max_levels)
{
    if (!db_path || !levels || max_levels <= 0) return KANBUDB_ERR_INVAL;

    /* Initialize all levels */
    for (int i = 0; i < max_levels; i++) {
        levels[i].level = i;
        levels[i].file_paths = NULL;
        levels[i].num_files = 0;
        levels[i].total_size = 0;
    }

    /* Extract directory */
    char dir[1024];
    const char* slash = strrchr(db_path, '/');
    if (slash) {
        size_t dlen = (size_t)(slash - db_path);
        if (dlen >= sizeof(dir)) dlen = sizeof(dir) - 1;
        memcpy(dir, db_path, dlen);
        dir[dlen] = '\0';
    } else {
        strcpy(dir, ".");
    }

    const char* base = slash ? slash + 1 : db_path;
    size_t blen = strlen(base);

    DIR* d = opendir(dir);
    if (!d) return KANBUDB_OK; /* No directory yet — not an error */

    struct dirent* entry;
    while ((entry = readdir(d)) != NULL) {
        if (entry->d_type == DT_DIR) continue;
        const char* name = entry->d_name;
        if (strncmp(name, base, blen) != 0) continue;
        if (name[blen] != '.') continue;
        const char* rest = name + blen + 1;
        if (strncmp(rest, "sst.", 4) != 0) continue;

        /* Parse: .sst.LEVEL.SEQ */
        const char* level_str = rest + 4;
        char* end = NULL;
        long level = strtol(level_str, &end, 10);
        if (!end || *end != '.') continue;
        if (level < 0 || level >= max_levels) continue;

        /* Build full path */
        char full[1024];
        if (strcmp(dir, ".") == 0) {
            snprintf(full, sizeof(full), "%s", name);
        } else {
            snprintf(full, sizeof(full), "%s/%s", dir, name);
        }

        /* Get file size */
        struct stat st;
        uint64_t fsize = 0;
        if (stat(full, &st) == 0) fsize = (uint64_t)st.st_size;

        kanbudb_level_t* lv = &levels[level];
        char** new_paths = (char**)realloc(lv->file_paths,
                              (size_t)(lv->num_files + 1) * sizeof(char*));
        if (!new_paths) { closedir(d); return KANBUDB_ERR_OOM; }
        lv->file_paths = new_paths;
        lv->file_paths[lv->num_files] = strdup(full);
        if (!lv->file_paths[lv->num_files]) { closedir(d); return KANBUDB_ERR_OOM; }
        lv->num_files++;
        lv->total_size += fsize;
    }
    closedir(d);
    return KANBUDB_OK;
}

void compaction_free_levels(kanbudb_level_t* levels, int max_levels)
{
    if (!levels) return;
    for (int i = 0; i < max_levels; i++) {
        if (levels[i].file_paths) {
            for (int j = 0; j < levels[i].num_files; j++)
                free(levels[i].file_paths[j]);
            free(levels[i].file_paths);
            levels[i].file_paths = NULL;
        }
        levels[i].num_files = 0;
        levels[i].total_size = 0;
    }
}

/* ── Compaction picker ──────────────────────────────────── */

int compaction_pick_level(const kanbudb_level_t* levels, int max_levels)
{
    if (!levels || max_levels < 2) return -1;

    /* L0: triggered by file count */
    if (levels[0].num_files >= KANBUDB_L0_COMPACTION_TRIGGER) {
        return 0;
    }

    /* L1+: triggered by total size exceeding target */
    for (int i = 1; i < max_levels - 1; i++) {
        uint64_t max_bytes = compaction_max_bytes_for_level(i);
        if (levels[i].total_size > max_bytes && levels[i].num_files > 0) {
            return i;
        }
    }
    return -1;
}

/* ── Size target per level ──────────────────────────────── */

uint64_t compaction_max_bytes_for_level(int level)
{
    if (level <= 0) return 0; /* L0 has no size limit */
    /* L1=10MB, L2=100MB, L3=1GB, L4=10GB, L5=100GB, L6=1TB, L7=10TB */
    uint64_t base = 10ULL * 1024 * 1024; /* 10 MB */
    for (int i = 1; i < level; i++) {
        base *= 10;
    }
    return base;
}

/* ── Path builder ───────────────────────────────────────── */

void compaction_make_path(const char* db_path, int level,
                          uint64_t seq, char* out, size_t out_sz)
{
    snprintf(out, out_sz, "%s.sst.%d.%llu",
             db_path, level, (unsigned long long)seq);
}

/* ── Merge executor ─────────────────────────────────────── */

int compaction_merge_to_level(const char** input_paths, int num_inputs,
                              const char* output_path,
                              uint64_t output_sequence)
{
    if (!input_paths || num_inputs <= 0 || !output_path)
        return KANBUDB_ERR_INVAL;

    /* Phase 1: collect all entries from all input SSTables */
    merge_entry_t* entries = NULL;
    size_t cap = 0;
    size_t count = 0;

    for (int i = 0; i < num_inputs; i++) {
        sstable_reader_t* r = sstable_reader_open(input_paths[i]);
        if (!r) continue;
        uint64_t seq = sstable_reader_sequence(r);
        collect_ctx_t ctx = { &entries, &cap, &count, seq };
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

    if (count == 0) {
        char tmp_path[1024];
        snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", output_path);
        sstable_writer_t* w = sstable_writer_create(tmp_path, output_path,
                                                     output_sequence);
        if (!w) return KANBUDB_ERR_OOM;
        int rc = sstable_writer_finish(w);
        sstable_writer_destroy(w);
        return rc;
    }

    /* Phase 2: sort by key (descending seq for same key) */
    qsort(entries, count, sizeof(merge_entry_t), merge_entry_compare);

    /* Phase 3: write merged output, deduplicating by key */
    char tmp_path[1024];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", output_path);
    sstable_writer_t* w = sstable_writer_create(tmp_path, output_path,
                                                 output_sequence);
    if (!w) {
        for (size_t j = 0; j < count; j++) { free(entries[j].key); free(entries[j].value); }
        free(entries);
        return KANBUDB_ERR_OOM;
    }

    int rc = KANBUDB_OK;
    int first = 1;
    const uint8_t* prev_key = NULL;
    size_t prev_key_len = 0;

    for (size_t i = 0; i < count; i++) {
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
    }
    sstable_writer_destroy(w);

    for (size_t j = 0; j < count; j++) {
        free(entries[j].key);
        free(entries[j].value);
    }
    free(entries);
    return rc;
}

/* ── Legacy compatibility (delegate to new API) ────────── */

struct kanbudb_compactor {
    int placeholder;
};

kanbudb_compactor_t* compactor_create(void) {
    kanbudb_compactor_t* c = (kanbudb_compactor_t*)calloc(1, sizeof(struct kanbudb_compactor));
    if (c) c->placeholder = 0;
    return c;
}

void compactor_destroy(kanbudb_compactor_t* c) {
    free(c);
}

int compactor_merge_sstables(kanbudb_compactor_t* c,
                              const char** input_paths, int num_inputs,
                              const char* output_path,
                              uint64_t output_sequence) {
    (void)c;
    return compaction_merge_to_level(input_paths, num_inputs,
                                     output_path, output_sequence);
}
