#include "fst.h"
#include <stdlib.h>
#include <string.h>
#include <limits.h>

typedef struct fst_node {
  struct fst_node* children[256];
  uint64_t         value;
  int              has_value;
} fst_node_t;

struct kanbudb_fst {
  fst_node_t* root;
  size_t      size;
  size_t      nodes;
};

static fst_node_t* node_create(void) {
  fst_node_t* node = (fst_node_t*)calloc(1, sizeof(fst_node_t));
  if (node) {
    node->has_value = 0;
  }
  return node;
}

kanbudb_fst_t* fst_create(void) {
  kanbudb_fst_t* fst = (kanbudb_fst_t*)malloc(sizeof(kanbudb_fst_t));
  if (!fst) return NULL;
  fst->root = node_create();
  if (!fst->root) {
    free(fst);
    return NULL;
  }
  fst->size = 0;
  fst->nodes = 1;
  return fst;
}

static void node_destroy(fst_node_t* node) {
  if (!node) return;
  for (int i = 0; i < 256; i++) {
    node_destroy(node->children[i]);
  }
  free(node);
}

void fst_destroy(kanbudb_fst_t* fst) {
  if (!fst) return;
  node_destroy(fst->root);
  free(fst);
}

int fst_insert(kanbudb_fst_t* fst, const char* key, uint64_t value) {
  if (!fst || !key) return KANBUDB_ERR_INVAL;
  if (strlen(key) > FST_MAX_KEY_LEN) return KANBUDB_ERR_INVAL;
  fst_node_t* node = fst->root;
  while (*key) {
    unsigned char c = (unsigned char)*key;
    if (!node->children[c]) {
      node->children[c] = node_create();
      if (!node->children[c]) return KANBUDB_ERR_OOM;
      fst->nodes++;
    }
    node = node->children[c];
    key++;
  }
  if (!node->has_value) {
    fst->size++;
  }
  node->value = value;
  node->has_value = 1;
  return KANBUDB_OK;
}

int fst_get(const kanbudb_fst_t* fst, const char* key, uint64_t* out_value) {
  if (!fst || !key) return KANBUDB_ERR_INVAL;
  fst_node_t* node = fst->root;
  while (*key) {
    unsigned char c = (unsigned char)*key;
    if (!node->children[c]) return KANBUDB_ERR_NOTFOUND;
    node = node->children[c];
    key++;
  }
  if (!node->has_value) return KANBUDB_ERR_NOTFOUND;
  if (out_value) *out_value = node->value;
  return KANBUDB_OK;
}

static void dfs_collect(fst_node_t* node, uint64_t* results, int* count, int max_results) {
  if (!node || *count >= max_results) return;
  if (node->has_value) {
    results[*count] = node->value;
    (*count)++;
  }
  for (int i = 0; i < 256 && *count < max_results; i++) {
    if (node->children[i]) {
      dfs_collect(node->children[i], results, count, max_results);
    }
  }
}

int fst_prefix_search(const kanbudb_fst_t* fst, const char* prefix,
                      uint64_t* results, int max_results) {
  if (!fst || !prefix || !results || max_results <= 0) return 0;
  fst_node_t* node = fst->root;
  while (*prefix) {
    unsigned char c = (unsigned char)*prefix;
    if (!node->children[c]) return 0;
    node = node->children[c];
    prefix++;
  }
  int count = 0;
  dfs_collect(node, results, &count, max_results);
  return count;
}

static void fuzzy_dfs(fst_node_t* node, const char* key, int max_edits,
                      int* row, int n,
                      uint64_t* results, int* count, int max_results) {
  if (!node || *count >= max_results) return;

  if (node->has_value && row[n] <= max_edits) {
    results[*count] = node->value;
    (*count)++;
  }

  for (int i = 0; i < 256 && *count < max_results; i++) {
    if (!node->children[i]) continue;

    int new_row[FST_MAX_KEY_LEN + 1];
    int row_size = n + 1;
    if (row_size > FST_MAX_KEY_LEN) row_size = FST_MAX_KEY_LEN;
    new_row[0] = row[0] + 1;

    for (int j = 1; j <= n && j <= FST_MAX_KEY_LEN; j++) {
      int cost = ((unsigned char)key[j - 1] == (unsigned char)i) ? 0 : 1;
      int del = row[j] + 1;
      int ins = new_row[j - 1] + 1;
      int sub = row[j - 1] + cost;
      int m = del < ins ? del : ins;
      new_row[j] = m < sub ? m : sub;
    }

    int min_dist = new_row[0];
    int limit = n < FST_MAX_KEY_LEN ? n : FST_MAX_KEY_LEN;
    for (int j = 1; j <= limit; j++) {
      if (new_row[j] < min_dist) min_dist = new_row[j];
    }
    if (min_dist > max_edits) continue;

    fuzzy_dfs(node->children[i], key, max_edits,
              new_row, n, results, count, max_results);
  }
}

int fst_fuzzy_search(const kanbudb_fst_t* fst, const char* key, int max_edits,
                     uint64_t* results, int max_results) {
  if (!fst || !key || !results || max_results <= 0 || max_edits < 0) return 0;

  int n = (int)strlen(key);
  if (n > FST_MAX_KEY_LEN) n = FST_MAX_KEY_LEN;
  int row[FST_MAX_KEY_LEN + 1];
  for (int j = 0; j <= n; j++) row[j] = j;

  int count = 0;
  fuzzy_dfs(fst->root, key, max_edits, row, n, results, &count, max_results);
  return count;
}

size_t fst_size(const kanbudb_fst_t* fst) {
  return fst ? fst->size : 0;
}

size_t fst_memory_used(const kanbudb_fst_t* fst) {
  if (!fst) return 0;
  return sizeof(kanbudb_fst_t) + fst->nodes * sizeof(fst_node_t);
}
