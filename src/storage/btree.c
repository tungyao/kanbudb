#include "btree.h"
#include <string.h>
#include <stdlib.h>

#define MAX_KEYS BTREE_ORDER
#define HALF_KEYS (BTREE_ORDER / 2)
#define ALLOC_SZ (2 * BTREE_ORDER)

typedef struct btree_node {
  int is_leaf;
  int num_keys;
  void** keys;
  size_t* key_lens;
  void** values;
  size_t* val_lens;
  struct btree_node** children;
  struct btree_node* next;
} btree_node_t;

struct kanbudb_btree {
  btree_node_t* root;
};

struct btree_cursor {
  btree_kv_t* items;
  int count;
  int pos;
};

static int key_cmp(const void* a, size_t alen, const void* b, size_t blen) {
  size_t min = alen < blen ? alen : blen;
  int r = memcmp(a, b, min);
  if (r) return r;
  if (alen < blen) return -1;
  if (alen > blen) return 1;
  return 0;
}

static btree_node_t* node_create(int is_leaf) {
  btree_node_t* n = (btree_node_t*)calloc(1, sizeof(*n));
  if (!n) return NULL;
  n->is_leaf = is_leaf;
  n->num_keys = 0;
  n->keys = (void**)calloc(ALLOC_SZ, sizeof(void*));
  if (!n->keys) { free(n); return NULL; }
  n->key_lens = (size_t*)calloc(ALLOC_SZ, sizeof(size_t));
  if (!n->key_lens) { free(n->keys); free(n); return NULL; }
  n->values = (void**)calloc(ALLOC_SZ, sizeof(void*));
  if (!n->values) { free(n->key_lens); free(n->keys); free(n); return NULL; }
  n->val_lens = (size_t*)calloc(ALLOC_SZ, sizeof(size_t));
  if (!n->val_lens) { free(n->values); free(n->key_lens); free(n->keys); free(n); return NULL; }
  if (!is_leaf) {
    n->children = (btree_node_t**)calloc(ALLOC_SZ + 1, sizeof(btree_node_t*));
    if (!n->children) { free(n->val_lens); free(n->values); free(n->key_lens); free(n->keys); free(n); return NULL; }
  }
  n->next = NULL;
  return n;
}

static void node_destroy(btree_node_t* n) {
  if (!n) return;
  for (int i = 0; i < n->num_keys; i++) {
    if (n->keys[i]) free(n->keys[i]);
    if (n->is_leaf && n->values[i]) free(n->values[i]);
  }
  if (!n->is_leaf) {
    for (int i = 0; i <= n->num_keys; i++) {
      node_destroy(n->children[i]);
    }
    free(n->children);
  }
  free(n->keys);
  free(n->key_lens);
  free(n->values);
  free(n->val_lens);
  free(n);
}

static void* key_dup(const void* key, size_t key_len) {
  void* k = malloc(key_len);
  if (k) memcpy(k, key, key_len);
  return k;
}

static void* val_dup(const void* val, size_t val_len) {
  if (!val) return NULL;
  void* v = malloc(val_len);
  if (v) memcpy(v, val, val_len);
  return v;
}

static int split_child(btree_node_t* parent, int idx) {
  btree_node_t* child = parent->children[idx];
  btree_node_t* new_node = node_create(child->is_leaf);
  if (!new_node) return KANBUDB_ERR_OOM;

  int mid = HALF_KEYS;

  if (child->is_leaf) {
    new_node->num_keys = MAX_KEYS - mid;
    for (int j = 0; j < new_node->num_keys; j++) {
      new_node->keys[j] = child->keys[mid + j];
      new_node->key_lens[j] = child->key_lens[mid + j];
      new_node->values[j] = child->values[mid + j];
      new_node->val_lens[j] = child->val_lens[mid + j];
    }
    child->num_keys = mid;

    for (int j = parent->num_keys; j > idx; j--) {
      parent->keys[j] = parent->keys[j - 1];
      parent->key_lens[j] = parent->key_lens[j - 1];
      parent->children[j + 1] = parent->children[j];
    }

    parent->keys[idx] = key_dup(new_node->keys[0], new_node->key_lens[0]);
    parent->key_lens[idx] = new_node->key_lens[0];
    parent->children[idx + 1] = new_node;
    parent->num_keys++;

    new_node->next = child->next;
    child->next = new_node;
  } else {
    new_node->num_keys = MAX_KEYS - mid - 1;
    for (int j = 0; j < new_node->num_keys; j++) {
      new_node->keys[j] = child->keys[mid + 1 + j];
      new_node->key_lens[j] = child->key_lens[mid + 1 + j];
      new_node->children[j] = child->children[mid + 1 + j];
    }
    new_node->children[new_node->num_keys] = child->children[MAX_KEYS];
    child->num_keys = mid;

    for (int j = parent->num_keys; j > idx; j--) {
      parent->keys[j] = parent->keys[j - 1];
      parent->key_lens[j] = parent->key_lens[j - 1];
      parent->children[j + 1] = parent->children[j];
    }

    parent->keys[idx] = child->keys[mid];
    parent->key_lens[idx] = child->key_lens[mid];
    parent->children[idx + 1] = new_node;
    parent->num_keys++;
  }
  return KANBUDB_OK;
}

static int node_insert_nonfull(btree_node_t* n,
                                const void* key, size_t key_len,
                                const void* value, size_t val_len) {
  int i = n->num_keys - 1;

  if (n->is_leaf) {
    /* Check if key already exists — update it */
    for (int j = 0; j < n->num_keys; j++) {
      if (key_cmp(key, key_len, n->keys[j], n->key_lens[j]) == 0) {
        free(n->values[j]);
        n->values[j] = val_dup(value, val_len);
        n->val_lens[j] = val_len;
        return KANBUDB_OK;
      }
    }
    /* Not found — insert in sorted order */
    while (i >= 0 && key_cmp(key, key_len, n->keys[i], n->key_lens[i]) < 0) {
      n->keys[i + 1] = n->keys[i];
      n->key_lens[i + 1] = n->key_lens[i];
      n->values[i + 1] = n->values[i];
      n->val_lens[i + 1] = n->val_lens[i];
      i--;
    }
    i++;
    n->keys[i] = key_dup(key, key_len);
    n->key_lens[i] = key_len;
    n->values[i] = val_dup(value, val_len);
    n->val_lens[i] = val_len;
    n->num_keys++;
  } else {
    while (i >= 0 && key_cmp(key, key_len, n->keys[i], n->key_lens[i]) < 0) {
      i--;
    }
    i++;
    if (n->children[i]->num_keys == MAX_KEYS) {
      int rc = split_child(n, i);
      if (rc != KANBUDB_OK) return rc;
      if (key_cmp(key, key_len, n->keys[i], n->key_lens[i]) > 0) {
        i++;
      }
    }
    return node_insert_nonfull(n->children[i], key, key_len, value, val_len);
  }
  return KANBUDB_OK;
}

static int node_search(btree_node_t* n,
                        const void* key, size_t key_len,
                        void** out_value, size_t* out_val_len) {
  int i = 0;
  while (i < n->num_keys && key_cmp(key, key_len, n->keys[i], n->key_lens[i]) >= 0) {
    i++;
  }

  if (n->is_leaf) {
    if (i > 0 && key_cmp(key, key_len, n->keys[i - 1], n->key_lens[i - 1]) == 0) {
      if (out_value) *out_value = n->values[i - 1];
      if (out_val_len) *out_val_len = n->val_lens[i - 1];
      return KANBUDB_OK;
    }
    return KANBUDB_ERR_NOTFOUND;
  }

  return node_search(n->children[i], key, key_len, out_value, out_val_len);
}

kanbudb_btree_t* btree_create(void) {
  kanbudb_btree_t* bt = (kanbudb_btree_t*)calloc(1, sizeof(*bt));
  if (!bt) return NULL;
  bt->root = node_create(1);
  if (!bt->root) { free(bt); return NULL; }
  return bt;
}

int btree_build_sorted(kanbudb_btree_t* bt,
                        const btree_kv_t* items, int num_items) {
    if (!bt || (!items && num_items > 0)) return KANBUDB_ERR_INVAL;
    for (int i = 0; i < num_items; i++) {
        int rc = btree_put(bt, items[i].key, items[i].key_len,
                           items[i].value, items[i].val_len);
        if (rc != KANBUDB_OK) return rc;
    }
    return KANBUDB_OK;
}

void btree_destroy(kanbudb_btree_t* bt) {
  if (!bt) return;
  node_destroy(bt->root);
  free(bt);
}

int btree_put(kanbudb_btree_t* bt, const void* key, size_t key_len,
              const void* value, size_t val_len) {
  if (!bt || !key) return KANBUDB_ERR_INVAL;

  if (bt->root->num_keys == MAX_KEYS) {
    btree_node_t* new_root = node_create(0);
    if (!new_root) return KANBUDB_ERR_OOM;
    new_root->children[0] = bt->root;
    bt->root = new_root;
    int rc = split_child(new_root, 0);
    if (rc != KANBUDB_OK) return rc;
  }

  return node_insert_nonfull(bt->root, key, key_len, value, val_len);
}

int btree_get(kanbudb_btree_t* bt, const void* key, size_t key_len,
              void** out_value, size_t* out_val_len) {
  if (!bt || !key) return KANBUDB_ERR_INVAL;
  return node_search(bt->root, key, key_len, out_value, out_val_len);
}

/* Recursive deletion helper — deletes key from the subtree rooted at n.
 * Only deletes from leaf nodes (data-bearing). Internal node keys
 * (separators) are left untouched — this is a simple embedded DB,
 * not a production-grade B-tree with full rebalancing. */
static int node_delete(btree_node_t* n, const void* key, size_t key_len) {
  if (!n) return KANBUDB_ERR_NOTFOUND;

  if (n->is_leaf) {
    /* Linear scan to find the key */
    for (int i = 0; i < n->num_keys; i++) {
      if (key_cmp(key, key_len, n->keys[i], n->key_lens[i]) == 0) {
        /* Found — free and shift */
        free(n->keys[i]);
        free(n->values[i]);
        for (int j = i; j < n->num_keys - 1; j++) {
          n->keys[j]    = n->keys[j + 1];
          n->key_lens[j]= n->key_lens[j + 1];
          n->values[j]  = n->values[j + 1];
          n->val_lens[j]= n->val_lens[j + 1];
        }
        n->num_keys--;
        return KANBUDB_OK;
      }
    }
    return KANBUDB_ERR_NOTFOUND;
  } else {
    /* Internal node — recurse into appropriate child */
    int i = 0;
    while (i < n->num_keys && key_cmp(key, key_len, n->keys[i], n->key_lens[i]) >= 0) {
      i++;
    }
    return node_delete(n->children[i], key, key_len);
  }
}

int btree_delete(kanbudb_btree_t* bt, const void* key, size_t key_len) {
  if (!bt || !key) return KANBUDB_ERR_INVAL;
  return node_delete(bt->root, key, key_len);
}

btree_cursor_t* btree_cursor_create(kanbudb_btree_t* bt) {
  if (!bt) return NULL;
  btree_cursor_t* cur = (btree_cursor_t*)calloc(1, sizeof(*cur));
  if (!cur) return NULL;

  int cap = 64;
  cur->items = (btree_kv_t*)malloc((size_t)cap * sizeof(btree_kv_t));
  if (!cur->items) { free(cur); return NULL; }
  cur->count = 0;
  cur->pos = 0;

  btree_node_t* n = bt->root;
  while (n && !n->is_leaf) {
    n = n->children[0];
  }
  while (n) {
    for (int i = 0; i < n->num_keys; i++) {
      if (cur->count >= cap) {
        cap *= 2;
        btree_kv_t* new_items = (btree_kv_t*)realloc(cur->items, (size_t)cap * sizeof(btree_kv_t));
        if (!new_items) { btree_cursor_destroy(cur); return NULL; }
        cur->items = new_items;
      }
      cur->items[cur->count].key = malloc(n->key_lens[i]);
      if (cur->items[cur->count].key) {
        memcpy(cur->items[cur->count].key, n->keys[i], n->key_lens[i]);
      }
      cur->items[cur->count].key_len = n->key_lens[i];
      cur->items[cur->count].value = malloc(n->val_lens[i]);
      if (cur->items[cur->count].value) {
        memcpy(cur->items[cur->count].value, n->values[i], n->val_lens[i]);
      }
      cur->items[cur->count].val_len = n->val_lens[i];
      cur->count++;
    }
    n = n->next;
  }

  return cur;
}

int btree_cursor_seek(btree_cursor_t* cur, const void* key, size_t key_len) {
  if (!cur) return KANBUDB_ERR_INVAL;
  if (!key) {
    cur->pos = 0;
    return KANBUDB_OK;
  }
  int lo = 0, hi = cur->count - 1;
  while (lo <= hi) {
    int mid = (lo + hi) / 2;
    int cmp = key_cmp(key, key_len, cur->items[mid].key, cur->items[mid].key_len);
    if (cmp == 0) {
      cur->pos = mid;
      return KANBUDB_OK;
    } else if (cmp < 0) {
      hi = mid - 1;
    } else {
      lo = mid + 1;
    }
  }
  cur->pos = lo;
  return KANBUDB_OK;
}

int btree_cursor_next(btree_cursor_t* cur, btree_kv_t* out) {
  if (!cur || !out) return KANBUDB_ERR_INVAL;
  if (cur->pos >= cur->count) return KANBUDB_ERR_NOTFOUND;
  *out = cur->items[cur->pos];
  cur->pos++;
  return KANBUDB_OK;
}

void btree_cursor_destroy(btree_cursor_t* cur) {
  if (!cur) return;
  if (cur->items) {
    for (int i = 0; i < cur->count; i++) {
      free(cur->items[i].key);
      free(cur->items[i].value);
    }
    free(cur->items);
  }
  free(cur);
}
