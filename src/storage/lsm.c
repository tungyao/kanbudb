#include "lsm.h"
#include <string.h>
#include <stdio.h>

#define MAX_LEVEL 12

typedef struct skip_node {
  uint64_t seq;
  size_t key_len;
  size_t val_len;
  int     deleted;
  uint16_t level;
} skip_node_t;

static inline skip_node_t** node_forward(skip_node_t* n) {
  return (skip_node_t**)((char*)n + sizeof(skip_node_t));
}

static inline const void* node_key(const skip_node_t* n) {
  return (const void*)((char*)n + sizeof(skip_node_t) +
                       (size_t)(n->level + 1) * sizeof(skip_node_t*));
}

static inline const void* node_value(const skip_node_t* n) {
  return (const void*)((const char*)node_key(n) + n->key_len);
}

static int key_cmp(const void* a, size_t alen, const void* b, size_t blen) {
  size_t min = alen < blen ? alen : blen;
  int r = memcmp(a, b, min);
  if (r) return r;
  if (alen < blen) return -1;
  if (alen > blen) return 1;
  return 0;
}

static skip_node_t* skip_node_create(uint16_t level, uint64_t seq,
                                     const void* key, size_t key_len,
                                     const void* value, size_t val_len,
                                     int deleted) {
  size_t sz = sizeof(skip_node_t) +
              (size_t)(level + 1) * sizeof(skip_node_t*) +
              key_len + val_len;
  skip_node_t* n = (skip_node_t*)calloc(1, sz);
  if (!n) return NULL;
  n->seq     = seq;
  n->key_len = key_len;
  n->val_len = val_len;
  n->deleted = deleted;
  n->level   = level;
  if (key_len > 0)
    memcpy((char*)n + sizeof(skip_node_t) +
           (size_t)(level + 1) * sizeof(skip_node_t*), key, key_len);
  if (val_len > 0)
    memcpy((char*)n + sizeof(skip_node_t) +
           (size_t)(level + 1) * sizeof(skip_node_t*) + key_len, value, val_len);
  return n;
}

static int random_level(void) {
  int level = 0;
  while (rand() < RAND_MAX / 2 && level < MAX_LEVEL - 1)
    level++;
  return level;
}

struct kanbudb_memtable {
  skip_node_t* header;
  size_t size_limit;
  size_t used;
  int    level;
};

kanbudb_memtable_t* memtable_create(size_t max_size) {
  kanbudb_memtable_t* mt = (kanbudb_memtable_t*)calloc(1, sizeof(*mt));
  if (!mt) return NULL;
  mt->header = skip_node_create(MAX_LEVEL - 1, 0, NULL, 0, NULL, 0, 0);
  if (!mt->header) { free(mt); return NULL; }
  mt->size_limit = max_size;
  mt->used       = 0;
  mt->level      = 0;
  return mt;
}

static void skip_free(skip_node_t* h) {
  skip_node_t* x = node_forward(h)[0];
  while (x) {
    skip_node_t* next = node_forward(x)[0];
    free(x);
    x = next;
  }
  free(h);
}

void memtable_destroy(kanbudb_memtable_t* mt) {
  if (!mt) return;
  skip_free(mt->header);
  free(mt);
}

int memtable_put(kanbudb_memtable_t* mt, uint64_t seq,
                 const void* key, size_t key_len,
                 const void* value, size_t val_len) {
  skip_node_t* update[MAX_LEVEL];
  skip_node_t* x = mt->header;

  for (int i = mt->level; i >= 0; i--) {
    while (1) {
      skip_node_t* next = node_forward(x)[i];
      if (!next) break;
      const void* nk = node_key(next);
      if (key_cmp(key, key_len, nk, next->key_len) > 0)
        x = next;
      else
        break;
    }
    update[i] = x;
  }

  x = node_forward(x)[0];
  int found = (x && key_cmp(key, key_len, node_key(x), x->key_len) == 0);

  if (found) {
    x->seq     = seq;
    x->deleted = 0;
    if (val_len <= x->val_len) {
      mt->used -= x->val_len;
      mt->used += val_len;
      x->val_len = val_len;
      if (val_len > 0)
        memcpy((char*)node_value(x), value, val_len);
    } else {
      skip_node_t* new_node = skip_node_create(x->level, seq,
                                                key, key_len,
                                                value, val_len, 0);
      if (!new_node) return KANBUDB_ERR_OOM;

      size_t old_sz = sizeof(skip_node_t) +
                      (size_t)(x->level + 1) * sizeof(skip_node_t*) +
                      x->key_len + x->val_len;
      size_t new_sz = sizeof(skip_node_t) +
                      (size_t)(new_node->level + 1) * sizeof(skip_node_t*) +
                      new_node->key_len + new_node->val_len;
      mt->used -= old_sz;
      mt->used += new_sz;

      for (int i = 0; i <= mt->level; i++) {
        if (node_forward(update[i])[i] == x)
          node_forward(update[i])[i] = new_node;
      }
      node_forward(new_node)[0] = node_forward(x)[0];
      free(x);
    }
  } else {
    int lvl = random_level();
    if (lvl > mt->level) {
      for (int i = mt->level + 1; i <= lvl; i++)
        update[i] = mt->header;
      mt->level = lvl;
    }

    skip_node_t* new_node = skip_node_create(lvl, seq,
                                              key, key_len,
                                              value, val_len, 0);
    if (!new_node) return KANBUDB_ERR_OOM;

    size_t sz = sizeof(skip_node_t) +
                (size_t)(lvl + 1) * sizeof(skip_node_t*) +
                key_len + val_len;
    mt->used += sz;

    for (int i = 0; i <= lvl; i++) {
      node_forward(new_node)[i] = node_forward(update[i])[i];
      node_forward(update[i])[i] = new_node;
    }
  }

  return KANBUDB_OK;
}

int memtable_delete(kanbudb_memtable_t* mt, uint64_t seq,
                    const void* key, size_t key_len) {
  int rc = memtable_put(mt, seq, key, key_len, NULL, 0);
  if (rc != KANBUDB_OK) return rc;

  skip_node_t* x = mt->header;
  while (1) {
    skip_node_t* next = node_forward(x)[0];
    if (!next) break;
    const void* nk = node_key(next);
    if (key_cmp(key, key_len, nk, next->key_len) == 0) {
      next->deleted = 1;
      break;
    }
    if (key_cmp(key, key_len, nk, next->key_len) < 0) break;
    x = next;
  }

  return KANBUDB_OK;
}

int memtable_get(kanbudb_memtable_t* mt,
                 const void* key, size_t key_len,
                 void** out_value, size_t* out_val_len,
                 int* out_deleted) {
  skip_node_t* x = mt->header;

  while (1) {
    skip_node_t* next = node_forward(x)[0];
    if (!next) return KANBUDB_ERR_NOTFOUND;
    const void* nk = node_key(next);
    int cmp = key_cmp(key, key_len, nk, next->key_len);
    if (cmp == 0) {
      if (out_value)   *out_value   = (void*)node_value(next);
      if (out_val_len) *out_val_len = next->val_len;
      if (out_deleted) *out_deleted = next->deleted;
      return next->deleted ? KANBUDB_ERR_NOTFOUND : KANBUDB_OK;
    }
    if (cmp < 0) return KANBUDB_ERR_NOTFOUND;
    x = next;
  }
}

int memtable_is_full(kanbudb_memtable_t* mt) {
  return mt->used >= mt->size_limit ? 1 : 0;
}

size_t memtable_size(kanbudb_memtable_t* mt) {
  return mt->used;
}

int memtable_iterate(kanbudb_memtable_t* mt,
                     int (*cb)(const lsm_entry_t* entry, void* ctx),
                     void* ctx) {
  skip_node_t* x = node_forward(mt->header)[0];
  while (x) {
    lsm_entry_t e;
    e.seq     = x->seq;
    e.key     = node_key(x);
    e.key_len = x->key_len;
    e.value   = node_value(x);
    e.val_len = x->val_len;
    e.deleted = x->deleted;
    int rc = cb(&e, ctx);
    if (rc != 0) return rc;
    x = node_forward(x)[0];
  }
  return KANBUDB_OK;
}

struct kanbudb_lsm {
  char* path;
  size_t memtable_size;
  kanbudb_memtable_t* active;
  kanbudb_memtable_t* flushing;
  uint64_t next_seq;   /* for SSTable sequence numbering */
};

kanbudb_lsm_t* lsm_create(const char* path, size_t memtable_size) {
  kanbudb_lsm_t* lsm = (kanbudb_lsm_t*)calloc(1, sizeof(*lsm));
  if (!lsm) return NULL;

  lsm->path = (char*)malloc(strlen(path) + 1);
  if (!lsm->path) { free(lsm); return NULL; }
  strcpy(lsm->path, path);

  lsm->memtable_size = memtable_size;
  lsm->active = memtable_create(memtable_size);
  if (!lsm->active) { free(lsm->path); free(lsm); return NULL; }
  lsm->flushing = NULL;

  return lsm;
}

void lsm_destroy(kanbudb_lsm_t* lsm) {
  if (!lsm) return;
  memtable_destroy(lsm->active);
  memtable_destroy(lsm->flushing);
  free(lsm->path);
  free(lsm);
}

int lsm_put(kanbudb_lsm_t* lsm, uint64_t table_id,
            const void* key, size_t key_len,
            const void* value, size_t val_len) {
  (void)table_id;
  if (memtable_is_full(lsm->active)) {
    int rc = lsm_flush(lsm);
    if (rc != KANBUDB_OK) return rc;
  }
  return memtable_put(lsm->active, 0, key, key_len, value, val_len);
}

int lsm_get(kanbudb_lsm_t* lsm, uint64_t table_id,
            const void* key, size_t key_len,
            void** out_value, size_t* out_val_len) {
  // NOTE: The returned out_value pointer is valid only until the next
  // flush operation. Callers must copy data immediately if they need
  // it to persist across flushes.
  (void)table_id;
  int deleted = 0;
  int rc = memtable_get(lsm->active, key, key_len,
                         out_value, out_val_len, &deleted);
  if (rc == KANBUDB_OK) return KANBUDB_OK;
  if (deleted) return KANBUDB_ERR_NOTFOUND;
  if (lsm->flushing) {
    deleted = 0;
    rc = memtable_get(lsm->flushing, key, key_len,
                       out_value, out_val_len, &deleted);
    if (rc == KANBUDB_OK) return KANBUDB_OK;
  }
  return KANBUDB_ERR_NOTFOUND;
}

int lsm_delete(kanbudb_lsm_t* lsm, uint64_t table_id,
               const void* key, size_t key_len) {
  (void)table_id;
  if (memtable_is_full(lsm->active)) {
    int rc = lsm_flush(lsm);
    if (rc != KANBUDB_OK) return rc;
  }
  return memtable_delete(lsm->active, 0, key, key_len);
}

int lsm_flush(kanbudb_lsm_t* lsm) {
  memtable_destroy(lsm->flushing);
  lsm->flushing = lsm->active;
  lsm->active = memtable_create(lsm->memtable_size);
  if (!lsm->active) return KANBUDB_ERR_OOM;
  return KANBUDB_OK;
}

int lsm_is_full(kanbudb_lsm_t* lsm) {
  return lsm ? memtable_is_full(lsm->active) : 0;
}

uint64_t lsm_next_seq(kanbudb_lsm_t* lsm) {
  if (!lsm) return 0;
  return lsm->next_seq++;
}

int lsm_iterate_flushing(kanbudb_lsm_t* lsm,
                          int (*cb)(const lsm_entry_t* entry, void* ctx),
                          void* ctx) {
  if (!lsm || !cb || !lsm->flushing) return KANBUDB_ERR_INVAL;
  return memtable_iterate(lsm->flushing, cb, ctx);
}

void lsm_destroy_flushing(kanbudb_lsm_t* lsm) {
  if (!lsm) return;
  memtable_destroy(lsm->flushing);
  lsm->flushing = NULL;
}
