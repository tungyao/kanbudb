/* KanbuDB Embedded Database - Amalgamated */

/* === core/db.c === */

#include "db.h"
#include "macros.h"
#include "core/db.h"
#include "query/query_builder.h"
#include "lsm.h"
#include "btree.h"
#include "wal.h"
#include "fts/index.h"
#include "fts/tokenizer.h"
#include "fts/parser.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const db_config_t default_config = {
  KANBUDB_FSYNC_NONE,
  65536,
  65536,
  1
};

static int find_table(struct kanbudb_db* db, const char* name) {
  for (int i = 0; i < db->num_tables; i++) {
    if (strcmp(db->tables[i].name, name) == 0)
      return i;
  }
  return -1;
}

int db_open(const char* path, const db_config_t* config, db_t** out) {
  if (!path || !out) return KANBUDB_ERR_INVAL;

  struct kanbudb_db* db = (struct kanbudb_db*)calloc(1, sizeof(*db));
  if (!db) return KANBUDB_ERR_OOM;

  db->path = (char*)malloc(strlen(path) + 1);
  if (!db->path) { free(db); return KANBUDB_ERR_OOM; }
  strcpy(db->path, path);

  if (config) {
    db->config = *config;
  } else {
    db->config = default_config;
  }

  char wal_path[512];
  snprintf(wal_path, sizeof(wal_path), "%s.wal", path);

  db->wal = wal_create(wal_path, db->config.fsync_mode);
  if (!db->wal) { free(db->path); free(db); return KANBUDB_ERR_IO; }

  char lsm_path[512];
  snprintf(lsm_path, sizeof(lsm_path), "%s.lsm", path);

  db->lsm = lsm_create(lsm_path, db->config.memtable_size);
  if (!db->lsm) {
    wal_destroy(db->wal);
    free(db->path);
    free(db);
    return KANBUDB_ERR_OOM;
  }

  db->btree = btree_create();
  if (!db->btree) {
    lsm_destroy(db->lsm);
    wal_destroy(db->wal);
    free(db->path);
    free(db);
    return KANBUDB_ERR_OOM;
  }

  db->fts_index = fts_index_create();
  if (!db->fts_index) {
    btree_destroy(db->btree);
    lsm_destroy(db->lsm);
    wal_destroy(db->wal);
    free(db->path);
    free(db);
    return KANBUDB_ERR_OOM;
  }

  db->num_tables = 0;
  db->last_error = KANBUDB_OK;

  *out = db;
  return KANBUDB_OK;
}

int db_close(db_t* db) {
  if (!db) return KANBUDB_ERR_INVAL;
  struct kanbudb_db* internal = (struct kanbudb_db*)db;

  for (int i = 0; i < internal->num_tables; i++) {
    kanbudb_table_t* t = &internal->tables[i];
    if (t->col_names) {
      for (int j = 0; j < t->num_cols; j++) {
        free(t->col_names[j]);
      }
      free(t->col_names);
    }
    free(t->col_types);
  }

  if (internal->fts_index) fts_index_destroy(internal->fts_index);
  btree_destroy(internal->btree);
  lsm_destroy(internal->lsm);
  wal_destroy(internal->wal);
  free(internal->path);
  free(internal);
  return KANBUDB_OK;
}

int db_last_error(db_t* db) {
  if (!db) return KANBUDB_ERR_INVAL;
  return ((struct kanbudb_db*)db)->last_error;
}

const char* db_error_string(int err) {
  switch (err) {
    case KANBUDB_OK:         return "success";
    case KANBUDB_ERR_OOM:    return "out of memory";
    case KANBUDB_ERR_NOTFOUND: return "not found";
    case KANBUDB_ERR_EXISTS: return "already exists";
    case KANBUDB_ERR_CORRUPT: return "corrupt data";
    case KANBUDB_ERR_IO:     return "I/O error";
    case KANBUDB_ERR_INVAL:  return "invalid argument";
    case KANBUDB_ERR_BUSY:   return "busy";
    default:                return "unknown error";
  }
}

int db_create_table(db_t* db, const char* table_name,
                    const char** col_names, const kanbudb_col_type_t* col_types,
                    int num_columns, const char* primary_key) {
  if (!db || !table_name || !col_names || !col_types || num_columns <= 0) {
    return KANBUDB_ERR_INVAL;
  }

  struct kanbudb_db* internal = (struct kanbudb_db*)db;

  if (find_table(internal, table_name) >= 0) {
    internal->last_error = KANBUDB_ERR_EXISTS;
    return KANBUDB_ERR_EXISTS;
  }

  if (internal->num_tables >= KANBUDB_MAX_TABLES) {
    internal->last_error = KANBUDB_ERR_OOM;
    return KANBUDB_ERR_OOM;
  }

  kanbudb_table_t* t = &internal->tables[internal->num_tables];

  size_t nlen = strlen(table_name);
  if (nlen >= sizeof(t->name)) nlen = sizeof(t->name) - 1;
  memcpy(t->name, table_name, nlen);
  t->name[nlen] = '\0';

  t->num_cols = num_columns;

  t->col_types = (kanbudb_col_type_t*)malloc((size_t)num_columns * sizeof(kanbudb_col_type_t));
  if (!t->col_types) { internal->last_error = KANBUDB_ERR_OOM; return KANBUDB_ERR_OOM; }
  memcpy(t->col_types, col_types, (size_t)num_columns * sizeof(kanbudb_col_type_t));

  t->col_names = (char**)malloc((size_t)num_columns * sizeof(char*));
  if (!t->col_names) { free(t->col_types); internal->last_error = KANBUDB_ERR_OOM; return KANBUDB_ERR_OOM; }

  for (int i = 0; i < num_columns; i++) {
    t->col_names[i] = (char*)malloc(strlen(col_names[i]) + 1);
    if (!t->col_names[i]) {
      for (int j = 0; j < i; j++) free(t->col_names[j]);
      free(t->col_names);
      free(t->col_types);
      internal->last_error = KANBUDB_ERR_OOM;
      return KANBUDB_ERR_OOM;
    }
    strcpy(t->col_names[i], col_names[i]);
  }

  t->primary_key_idx = -1;
  if (primary_key) {
    for (int i = 0; i < num_columns; i++) {
      if (strcmp(col_names[i], primary_key) == 0) {
        t->primary_key_idx = i;
        break;
      }
    }
  }

  t->id = (uint64_t)(internal->num_tables + 1);
  internal->num_tables++;

  internal->last_error = KANBUDB_OK;
  return KANBUDB_OK;
}

int db_put(db_t* db, const char* table, const char* key, size_t key_len,
           const void* value, size_t value_len) {
  if (!db || !table || !key) return KANBUDB_ERR_INVAL;

  struct kanbudb_db* internal = (struct kanbudb_db*)db;
  int idx = find_table(internal, table);
  if (idx < 0) { internal->last_error = KANBUDB_ERR_NOTFOUND; return KANBUDB_ERR_NOTFOUND; }

  uint64_t table_id = internal->tables[idx].id;

  int rc = wal_append(internal->wal, KANBUDB_WAL_PUT, table_id,
                       key, key_len, value, value_len);
  if (rc != KANBUDB_OK) { internal->last_error = rc; return rc; }

  rc = lsm_put(internal->lsm, table_id, key, key_len, value, value_len);
  if (rc != KANBUDB_OK) { internal->last_error = rc; return rc; }

  internal->last_error = KANBUDB_OK;
  return KANBUDB_OK;
}

int db_get(db_t* db, const char* table, const char* key, size_t key_len,
           void** value, size_t* value_len) {
  if (!db || !table || !key || !value || !value_len) return KANBUDB_ERR_INVAL;

  struct kanbudb_db* internal = (struct kanbudb_db*)db;
  int idx = find_table(internal, table);
  if (idx < 0) { internal->last_error = KANBUDB_ERR_NOTFOUND; return KANBUDB_ERR_NOTFOUND; }

  int rc = lsm_get(internal->lsm, internal->tables[idx].id,
                    key, key_len, value, value_len);
  if (rc == KANBUDB_OK) { internal->last_error = KANBUDB_OK; return KANBUDB_OK; }

  rc = btree_get(internal->btree, key, key_len, value, value_len);
  if (rc == KANBUDB_OK) { internal->last_error = KANBUDB_OK; return KANBUDB_OK; }

  internal->last_error = KANBUDB_ERR_NOTFOUND;
  return KANBUDB_ERR_NOTFOUND;
}

int db_delete(db_t* db, const char* table, const char* key, size_t key_len) {
  if (!db || !table || !key) return KANBUDB_ERR_INVAL;

  struct kanbudb_db* internal = (struct kanbudb_db*)db;
  int idx = find_table(internal, table);
  if (idx < 0) { internal->last_error = KANBUDB_ERR_NOTFOUND; return KANBUDB_ERR_NOTFOUND; }

  uint64_t table_id = internal->tables[idx].id;

  int rc = wal_append(internal->wal, KANBUDB_WAL_DELETE, table_id,
                       key, key_len, NULL, 0);
  if (rc != KANBUDB_OK) { internal->last_error = rc; return rc; }

  rc = lsm_delete(internal->lsm, table_id, key, key_len);
  if (rc != KANBUDB_OK) { internal->last_error = rc; return rc; }

  internal->last_error = KANBUDB_OK;
  return KANBUDB_OK;
}

int db_fts_create_index(db_t* db, const char* table, const char* column,
                        const fts_options_t* opts) {
  (void)opts;
  if (!db || !table || !column) return KANBUDB_ERR_INVAL;
  struct kanbudb_db* internal = (struct kanbudb_db*)db;
  if (find_table(internal, table) < 0) {
    internal->last_error = KANBUDB_ERR_NOTFOUND;
    return KANBUDB_ERR_NOTFOUND;
  }
  internal->last_error = KANBUDB_OK;
  return KANBUDB_OK;
}

int db_fts_drop_index(db_t* db, const char* table, const char* column) {
  if (!db || !table || !column) return KANBUDB_ERR_INVAL;
  struct kanbudb_db* internal = (struct kanbudb_db*)db;
  internal->last_error = KANBUDB_OK;
  return KANBUDB_OK;
}

int db_fts_search(db_t* db, const char* table, const char* column,
                  const char* query, result_set_t** out) {
  if (!db || !table || !column || !query || !out) return KANBUDB_ERR_INVAL;
  struct kanbudb_db* internal = (struct kanbudb_db*)db;

  if (find_table(internal, table) < 0) {
    internal->last_error = KANBUDB_ERR_NOTFOUND;
    return KANBUDB_ERR_NOTFOUND;
  }

  if (!internal->fts_index) {
    internal->last_error = KANBUDB_ERR_INVAL;
    return KANBUDB_ERR_INVAL;
  }

  fts_query_node_t nodes[64];
  int num_nodes = fts_query_parse(query, nodes, 64);
  if (num_nodes <= 0) {
    internal->last_error = KANBUDB_ERR_INVAL;
    return KANBUDB_ERR_INVAL;
  }

  uint64_t doc_id_buf[1024];
  int num_doc_ids = 0;

  for (int i = 0; i < num_nodes; i++) {
    if (nodes[i].op == FTS_TERM || nodes[i].op == FTS_FUZZY) {
      uint64_t results[256];
      int n;
      if (nodes[i].op == FTS_FUZZY) {
        int max_edits = nodes[i].fuzzy_distance > 0 ? nodes[i].fuzzy_distance : 1;
        n = fts_index_search_fuzzy(internal->fts_index, nodes[i].text,
                                    max_edits, results, 256);
      } else {
        n = fts_index_search(internal->fts_index, nodes[i].text,
                              results, 256);
      }
      for (int j = 0; j < n && num_doc_ids < 1024; j++) {
        int already = 0;
        for (int k = 0; k < num_doc_ids; k++) {
          if (doc_id_buf[k] == results[j]) { already = 1; break; }
        }
        if (!already) {
          doc_id_buf[num_doc_ids++] = results[j];
        }
      }
    }
  }

  result_set_t* rs = (result_set_t*)calloc(1, sizeof(*rs));
  if (!rs) { internal->last_error = KANBUDB_ERR_OOM; return KANBUDB_ERR_OOM; }

  rs->is_fts = 1;
  rs->num_rows = num_doc_ids;
  rs->num_cols = 2;
  rs->col_types[0] = KANBUDB_INT64;
  rs->col_types[1] = KANBUDB_DOUBLE;
  rs->col_names[0] = (char*)"doc_id";
  rs->col_names[1] = (char*)"score";

  if (num_doc_ids > 0) {
    rs->doc_ids = (uint64_t*)malloc((size_t)num_doc_ids * sizeof(uint64_t));
    rs->scores = (double*)calloc((size_t)num_doc_ids, sizeof(double));
    if (!rs->doc_ids || !rs->scores) {
      free(rs->doc_ids);
      free(rs->scores);
      free(rs);
      internal->last_error = KANBUDB_ERR_OOM;
      return KANBUDB_ERR_OOM;
    }
    memcpy(rs->doc_ids, doc_id_buf, (size_t)num_doc_ids * sizeof(uint64_t));
  } else {
    rs->doc_ids = NULL;
    rs->scores = NULL;
  }

  rs->current = -1;

  *out = rs;
  internal->last_error = KANBUDB_OK;
  return KANBUDB_OK;
}

/* === fts/index.c === */

#include "fts/index.h"
#include <stdlib.h>
#include <string.h>

kanbudb_fts_index_t* fts_index_create(void) {
  kanbudb_fts_index_t* idx = kanbudb_malloc(sizeof(kanbudb_fts_index_t));
  if (!idx) return NULL;
  idx->term_dict = fst_create();
  if (!idx->term_dict) {
    kanbudb_free(idx);
    return NULL;
  }
  idx->next_doc_id = 0;
  idx->total_docs = 0;
  idx->total_terms = 0;
  return idx;
}

void fts_index_destroy(kanbudb_fts_index_t* idx) {
  if (!idx) return;
  fst_destroy(idx->term_dict);
  kanbudb_free(idx);
}

int fts_index_add_document(kanbudb_fts_index_t* idx, const char** terms,
                           const size_t* term_lens, const size_t* positions,
                           int num_terms) {
  (void)term_lens;
  if (!idx || !terms || num_terms < 0) return KANBUDB_ERR_INVAL;
  uint64_t doc_id = idx->next_doc_id++;
  idx->total_docs++;
  for (int i = 0; i < num_terms; i++) {
    if (!terms[i]) continue;
    uint64_t encoded = (doc_id << 16) | (uint64_t)(positions ? positions[i] : 0);
    int rc = fst_insert(idx->term_dict, terms[i], encoded);
    if (rc != KANBUDB_OK) return rc;
    idx->total_terms++;
  }
  return KANBUDB_OK;
}

int fts_index_search(const kanbudb_fts_index_t* idx, const char* term,
                     uint64_t* results, int max_results) {
  if (!idx || !term || !results || max_results <= 0) return 0;
  uint64_t encoded;
  int rc = fst_get(idx->term_dict, term, &encoded);
  if (rc != KANBUDB_OK) return 0;
  results[0] = encoded >> 16;
  return 1;
}

int fts_index_search_fuzzy(const kanbudb_fts_index_t* idx, const char* term,
                           int max_edits, uint64_t* results, int max_results) {
  if (!idx || !term || !results || max_results <= 0 || max_edits < 0) return 0;
  uint64_t encoded[256];
  int n = fst_fuzzy_search(idx->term_dict, term, max_edits, encoded, max_results > 256 ? 256 : max_results);
  for (int i = 0; i < n && i < max_results; i++) {
    results[i] = encoded[i] >> 16;
  }
  return n;
}

/* === fts/parser.c === */

#include "fts/parser.h"
#include <string.h>
#include <ctype.h>
#include <stdlib.h>

static int is_boolean_op(const char* s, int len) {
  if (len == 3 && strncasecmp(s, "AND", 3) == 0) return FTS_BOOLEAN_AND;
  if (len == 2 && strncasecmp(s, "OR", 2) == 0) return FTS_BOOLEAN_OR;
  if (len == 3 && strncasecmp(s, "NOT", 3) == 0) return FTS_BOOLEAN_NOT;
  return 0;
}

int fts_query_parse(const char* query, fts_query_node_t* nodes, int max_nodes) {
  if (!query || !nodes || max_nodes <= 0) return 0;
  int count = 0;
  const char* p = query;

  while (*p && count < max_nodes) {
    while (*p && isspace((unsigned char)*p)) p++;
    if (!*p) break;

    if (*p == '"') {
      p++;
      fts_query_node_t* node = &nodes[count];
      memset(node, 0, sizeof(fts_query_node_t));
      node->op = FTS_PHRASE;
      node->boost = 1.0;
      int i = 0;
      while (*p && *p != '"' && i < (int)sizeof(node->text) - 1) {
        node->text[i++] = *p++;
      }
      node->text[i] = '\0';
      if (*p == '"') p++;
      count++;
    } else if (*p == '[') {
      p++;
      fts_query_node_t* node = &nodes[count];
      memset(node, 0, sizeof(fts_query_node_t));
      node->op = FTS_RANGE;
      node->boost = 1.0;
      while (*p && isspace((unsigned char)*p)) p++;
      int i = 0;
      while (*p && *p != 'T' && *p != 't' && i < (int)sizeof(node->text) - 1) {
        node->text[i++] = *p++;
      }
      node->text[i] = '\0';
      if ((*p == 'T' || *p == 't') && *(p+1) == 'O' && (*(p+2) == 'T' || *(p+2) == 'o' || *(p+2) == 't' || *(p+2) == 'o'))
        p += 3;
      else if (*p) p++;
      while (*p && isspace((unsigned char)*p)) p++;
      i = 0;
      while (*p && *p != ']' && i < (int)sizeof(node->text2) - 1) {
        node->text2[i++] = *p++;
      }
      node->text2[i] = '\0';
      if (*p == ']') p++;
      count++;
    } else {
      const char* start = p;
      while (*p && !isspace((unsigned char)*p) && *p != '"') p++;
      int len = (int)(p - start);

      int op_type = is_boolean_op(start, len);
      if (op_type && count > 0 && count < max_nodes) {
        fts_query_node_t* node = &nodes[count];
        memset(node, 0, sizeof(fts_query_node_t));
        node->op = (fts_query_op_t)op_type;
        node->boost = 1.0;
        count++;
        continue;
      }

      fts_query_node_t* node = &nodes[count];
      memset(node, 0, sizeof(fts_query_node_t));
      node->op = FTS_TERM;
      node->boost = 1.0;

      int has_field = 0;
      int field_end = -1;
      for (int i = 0; i < len; i++) {
        if (start[i] == ':') {
          has_field = 1;
          field_end = i;
          break;
        }
      }

      if (has_field) {
        int fld_len = field_end;
        if (fld_len > 63) fld_len = 63;
        memcpy(node->field, start, fld_len);
        node->field[fld_len] = '\0';
        start += field_end + 1;
        len -= field_end + 1;
      }

      int is_fuzzy = 0;
      int fuzzy_dist = 0;
      for (int i = 0; i < len; i++) {
        if (start[i] == '~') {
          is_fuzzy = 1;
          fuzzy_dist = 0;
          for (int j = i + 1; j < len; j++) {
            if (start[j] >= '0' && start[j] <= '9')
              fuzzy_dist = fuzzy_dist * 10 + (start[j] - '0');
            else
              break;
          }
          len = i;
          break;
        }
      }

      if (is_fuzzy) {
        node->op = FTS_FUZZY;
        node->fuzzy_distance = fuzzy_dist;
      }

      double boost = 1.0;
      for (int i = 0; i < len; i++) {
        if (start[i] == '^') {
          boost = atof(start + i + 1);
          len = i;
          break;
        }
      }
      if (boost != 1.0 && !is_fuzzy) {
        /* we set op back to FTS_TERM but keep boost */
      }
      node->boost = boost;

      int text_len = len;
      if (text_len > (int)sizeof(node->text) - 1) text_len = (int)sizeof(node->text) - 1;
      memcpy(node->text, start, text_len);
      node->text[text_len] = '\0';
      count++;
    }
  }
  return count;
}

/* === fts/ranker.c === */

#include "fts/ranker.h"
#include <math.h>

double bm25_score(double term_freq, double doc_len, double avg_dl,
                  double num_docs, double doc_freq,
                  double k1, double b) {
  if (doc_freq <= 0.0 || num_docs <= 0.0) return 0.0;
  double idf = log(1.0 + (num_docs - doc_freq + 0.5) / (doc_freq + 0.5));
  double tf = term_freq * (k1 + 1.0) / (term_freq + k1 * (1.0 - b + b * doc_len / avg_dl));
  return idf * tf;
}

/* === fts/tokenizer.c === */

#include "tokenizer.h"
#include <string.h>
#include <ctype.h>

#define STEM_BUF_SIZE 256
#define INITIAL_WORDS_CAP 64

struct kanbudb_tokenizer {
  int         stem_enabled;
  char**      stopwords;
  int         stopwords_count;
  char        stem_buf[STEM_BUF_SIZE];
  char**      words;
  int         words_cap;
  int         words_count;
};

static char* dup_str(const char* s, size_t len) {
  char* d = kanbudb_malloc(len + 1);
  if (!d) return NULL;
  memcpy(d, s, len);
  d[len] = '\0';
  return d;
}

static int is_token_char(int c) {
  return isalnum((unsigned char)c) || c == '\'';
}

static int has_suffix(const char* word, size_t word_len, const char* suffix) {
  size_t slen = strlen(suffix);
  if (word_len < slen) return 0;
  return memcmp(word + word_len - slen, suffix, slen) == 0;
}

static int stem_english(char* word, int len) {
  if (len < 3) return len;

  static const char* suffixes[] = {"ing", "ed", "ly", "es", "s", "er", "or"};
  static int slens[] = {3, 2, 2, 2, 1, 2, 2};

  for (int i = 0; i < 7; i++) {
    if (has_suffix(word, len, suffixes[i])) {
      int new_len = len - slens[i];
      if (new_len < 3) return len;
      if (new_len >= 2 && word[new_len - 1] == word[new_len - 2] &&
          isalpha((unsigned char)word[new_len - 1])) {
        new_len--;
      }
      word[new_len] = '\0';
      return new_len;
    }
  }
  return len;
}

static int is_stopword(kanbudb_tokenizer_t* t, const char* word) {
  for (int i = 0; i < t->stopwords_count; i++) {
    if (strcmp(word, t->stopwords[i]) == 0) return 1;
  }
  return 0;
}

kanbudb_tokenizer_t* tokenizer_create(void) {
  kanbudb_tokenizer_t* t = kanbudb_calloc(1, sizeof(kanbudb_tokenizer_t));
  if (!t) return NULL;
  t->words_cap = INITIAL_WORDS_CAP;
  t->words = kanbudb_malloc(sizeof(char*) * (size_t)t->words_cap);
  if (!t->words) { kanbudb_free(t); return NULL; }
  return t;
}

void tokenizer_destroy(kanbudb_tokenizer_t* t) {
  if (!t) return;
  for (int i = 0; i < t->words_count; i++) {
    kanbudb_free(t->words[i]);
  }
  kanbudb_free(t->words);
  for (int i = 0; i < t->stopwords_count; i++) {
    kanbudb_free(t->stopwords[i]);
  }
  kanbudb_free(t->stopwords);
  kanbudb_free(t);
}

static int add_word(kanbudb_tokenizer_t* t, char* w) {
  if (t->words_count >= t->words_cap) {
    int new_cap = t->words_cap * 2;
    char** new_words = realloc(t->words, sizeof(char*) * (size_t)new_cap);
    if (!new_words) return -1;
    t->words = new_words;
    t->words_cap = new_cap;
  }
  t->words[t->words_count++] = w;
  return 0;
}

int tokenizer_tokenize(kanbudb_tokenizer_t* t,
                       const char* text, size_t text_len,
                       kanbudb_token_t* tokens, int max_tokens) {
  if (!t || !text || max_tokens <= 0) return 0;

  int count = 0;
  size_t i = 0;
  size_t pos = 0;

  while (i < text_len) {
    while (i < text_len && !is_token_char((unsigned char)text[i])) {
      i++;
    }
    if (i >= text_len) break;

    int j = 0;
    while (i < text_len && is_token_char((unsigned char)text[i]) && j < STEM_BUF_SIZE - 1) {
      t->stem_buf[j++] = (char)tolower((unsigned char)text[i++]);
    }
    while (i < text_len && is_token_char((unsigned char)text[i])) {
      i++;
    }
    t->stem_buf[j] = '\0';

    if (j == 0) continue;
    if (is_stopword(t, t->stem_buf)) continue;

    if (t->stem_enabled) {
      j = stem_english(t->stem_buf, j);
      t->stem_buf[j] = '\0';
    }

    if (j < 2) continue;

    char* w = dup_str(t->stem_buf, (size_t)j);
    if (!w) break;
    if (add_word(t, w) != 0) { kanbudb_free(w); break; }

    tokens[count].word = w;
    tokens[count].len = (size_t)j;
    tokens[count].pos = pos++;
    count++;

    if (count >= max_tokens) break;
  }

  return count;
}

void tokenizer_set_stemmer(kanbudb_tokenizer_t* t, int enabled) {
  if (t) t->stem_enabled = enabled;
}

void tokenizer_set_stopwords(kanbudb_tokenizer_t* t,
                             const char** words, int count) {
  if (!t) return;

  for (int i = 0; i < t->stopwords_count; i++) {
    kanbudb_free(t->stopwords[i]);
  }
  kanbudb_free(t->stopwords);

  t->stopwords = NULL;
  t->stopwords_count = 0;

  if (count > 0 && words) {
    t->stopwords = kanbudb_malloc(sizeof(char*) * (size_t)count);
    if (!t->stopwords) return;
    for (int i = 0; i < count; i++) {
      t->stopwords[i] = dup_str(words[i], strlen(words[i]));
    }
    t->stopwords_count = count;
  }
}

/* === query/query_builder.c === */

#include "query_builder.h"
#include "db.h"
#include "macros.h"
#include "core/db.h"
#include "lsm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct query_builder_t {
  struct kanbudb_db* db;
  char     table[64];
  char     filter_column[64];
  char     filter_op[16];
  char     filter_value[256];
  size_t   filter_value_len;
  int      has_filter;
  char     sort_column[64];
  int      sort_ascending;
  int      has_sort;
  int      limit;
  int      has_limit;
  char     join_table[64];
  char     join_on_local[64];
  char     join_on_foreign[64];
  int      has_join;
};



query_builder_t* db_query(db_t* db, const char* table) {
  if (!db || !table) return NULL;

  struct query_builder_t* qb;
  qb = (struct query_builder_t*)calloc(1, sizeof(*qb));
  if (!qb) return NULL;

  qb->db = (struct kanbudb_db*)db;

  size_t n = strlen(table);
  if (n >= sizeof(qb->table)) n = sizeof(qb->table) - 1;
  memcpy(qb->table, table, n);
  qb->table[n] = '\0';

  qb->limit = 0;
  qb->has_filter = 0;
  qb->has_sort = 0;
  qb->has_limit = 0;
  qb->has_join = 0;

  return qb;
}

int qb_from(query_builder_t* qb, const char* table) {
  if (!qb || !table) return KANBUDB_ERR_INVAL;

  size_t n = strlen(table);
  if (n >= sizeof(qb->table)) n = sizeof(qb->table) - 1;
  memcpy(qb->table, table, n);
  qb->table[n] = '\0';

  return KANBUDB_OK;
}

int qb_filter(query_builder_t* qb, const char* column,
              const char* op, const void* value) {
  if (!qb || !column || !op || !value) return KANBUDB_ERR_INVAL;

  size_t n = strlen(column);
  if (n >= sizeof(qb->filter_column)) n = sizeof(qb->filter_column) - 1;
  memcpy(qb->filter_column, column, n);
  qb->filter_column[n] = '\0';

  n = strlen(op);
  if (n >= sizeof(qb->filter_op)) n = sizeof(qb->filter_op) - 1;
  memcpy(qb->filter_op, op, n);
  qb->filter_op[n] = '\0';

  /* Store value as string for v1 simplicity */
  snprintf(qb->filter_value, sizeof(qb->filter_value), "%s", (const char*)value);
  qb->filter_value_len = strlen(qb->filter_value);
  qb->has_filter = 1;

  return KANBUDB_OK;
}

int qb_sort(query_builder_t* qb, const char* column, int ascending) {
  if (!qb || !column) return KANBUDB_ERR_INVAL;

  size_t n = strlen(column);
  if (n >= sizeof(qb->sort_column)) n = sizeof(qb->sort_column) - 1;
  memcpy(qb->sort_column, column, n);
  qb->sort_column[n] = '\0';

  qb->sort_ascending = ascending;
  qb->has_sort = 1;

  return KANBUDB_OK;
}

int qb_limit(query_builder_t* qb, int limit) {
  if (!qb || limit < 0) return KANBUDB_ERR_INVAL;

  qb->limit = limit;
  qb->has_limit = 1;

  return KANBUDB_OK;
}

int qb_join(query_builder_t* qb, const char* table,
            const char* on_local, const char* on_foreign) {
  if (!qb || !table || !on_local || !on_foreign) return KANBUDB_ERR_INVAL;

  size_t n = strlen(table);
  if (n >= sizeof(qb->join_table)) n = sizeof(qb->join_table) - 1;
  memcpy(qb->join_table, table, n);
  qb->join_table[n] = '\0';

  n = strlen(on_local);
  if (n >= sizeof(qb->join_on_local)) n = sizeof(qb->join_on_local) - 1;
  memcpy(qb->join_on_local, on_local, n);
  qb->join_on_local[n] = '\0';

  n = strlen(on_foreign);
  if (n >= sizeof(qb->join_on_foreign)) n = sizeof(qb->join_on_foreign) - 1;
  memcpy(qb->join_on_foreign, on_foreign, n);
  qb->join_on_foreign[n] = '\0';

  qb->has_join = 1;

  return KANBUDB_OK;
}

result_set_t* qb_exec(query_builder_t* qb) {
  if (!qb) return NULL;

  result_set_t* rs = (result_set_t*)calloc(1, sizeof(*rs));
  if (!rs) return NULL;

  /* v1 stub: return empty result set */
  rs->num_rows = 0;
  rs->current = -1;
  rs->num_cols = 0;

  /* Look up the table to populate column metadata */
  struct kanbudb_db* internal = qb->db;
  for (int i = 0; i < internal->num_tables; i++) {
    if (strcmp(internal->tables[i].name, qb->table) == 0) {
      kanbudb_table_t* t = &internal->tables[i];
      rs->num_cols = t->num_cols;
      for (int j = 0; j < t->num_cols && j < KANBUDB_MAX_COLS; j++) {
        rs->col_types[j] = t->col_types[j];
        rs->col_names[j] = t->col_names[j];
      }
      break;
    }
  }

  return rs;
}

void qb_destroy(query_builder_t* qb) {
  free(qb);
}

int rs_next(result_set_t* rs) {
  if (!rs) return 0;

  rs->current++;
  if (rs->current >= rs->num_rows) {
    rs->current = rs->num_rows;
    return 0;
  }

  return 1;
}

int rs_get_column(result_set_t* rs, int col, void** data, size_t* len) {
  if (!rs || !data || !len) return KANBUDB_ERR_INVAL;
  if (col < 0 || col >= rs->num_cols) return KANBUDB_ERR_INVAL;

  if (rs->current < 0 || rs->current >= rs->num_rows) {
    *data = NULL;
    *len = 0;
    return KANBUDB_OK;
  }

  if (rs->is_fts) {
    if (col == 0) {
      *data = (void*)&rs->doc_ids[rs->current];
      *len = sizeof(uint64_t);
    } else if (col == 1) {
      *data = (void*)&rs->scores[rs->current];
      *len = sizeof(double);
    } else {
      *data = NULL;
      *len = 0;
    }
    return KANBUDB_OK;
  }

  /* v1 stub: no data rows for non-FTS */
  *data = NULL;
  *len = 0;

  return KANBUDB_OK;
}

kanbudb_col_type_t rs_get_column_type(result_set_t* rs, int col) {
  if (!rs || col < 0 || col >= rs->num_cols) return KANBUDB_INT32;

  return rs->col_types[col];
}

int rs_num_columns(result_set_t* rs) {
  if (!rs) return 0;

  return rs->num_cols;
}

void rs_close(result_set_t* rs) {
  if (!rs) return;
  if (rs->is_fts) {
    free(rs->doc_ids);
    free(rs->scores);
  }
  free(rs);
}

/* === storage/btree.c === */

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

int btree_delete(kanbudb_btree_t* bt, const void* key, size_t key_len) {
  (void)bt;
  (void)key;
  (void)key_len;
  return KANBUDB_ERR_NOTFOUND;
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

/* === storage/compaction.c === */

#include "compaction.h"
#include <stdlib.h>
#include <string.h>

struct kanbudb_compactor {
  int placeholder;
};

kanbudb_compactor_t* compactor_create(void) {
  kanbudb_compactor_t* c = (kanbudb_compactor_t*)calloc(1, sizeof(*c));
  if (!c) return NULL;
  c->placeholder = 0;
  return c;
}

void compactor_destroy(kanbudb_compactor_t* c) {
  if (!c) return;
  free(c);
}

int compactor_compact(kanbudb_compactor_t* c,
                      const uint8_t* sstable_data, size_t sstable_len,
                      uint8_t** out_btree_data, size_t* out_btree_len) {
  (void)c;
  if (!sstable_data || !out_btree_data || !out_btree_len)
    return KANBUDB_ERR_INVAL;

  uint8_t* copy = (uint8_t*)malloc(sstable_len);
  if (!copy) return KANBUDB_ERR_OOM;

  memcpy(copy, sstable_data, sstable_len);
  *out_btree_data = copy;
  *out_btree_len = sstable_len;
  return KANBUDB_OK;
}

/* === storage/lsm.c === */

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

/* === storage/wal.c === */

#include "wal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define KANBUDB_WAL_MAGIC  0x4845524D4553ULL
#define KANBUDB_WAL_VERSION 1
#define KANBUDB_WAL_PERIODIC_THRESHOLD 1000

typedef struct __attribute__((packed)) {
  uint64_t magic;
  uint32_t version;
  uint64_t seq;
} wal_header_t;

struct kanbudb_wal {
  FILE* file;
  char* path;
  int fsync_mode;
  uint64_t seq;
  int periodic_count;
};

kanbudb_wal_t* wal_create(const char* path, int fsync_mode) {
  kanbudb_wal_t* wal = (kanbudb_wal_t*)calloc(1, sizeof(*wal));
  if (!wal) return NULL;

  wal->path = (char*)malloc(strlen(path) + 1);
  if (!wal->path) { free(wal); return NULL; }
  strcpy(wal->path, path);
  wal->fsync_mode = fsync_mode;

  FILE* f = fopen(path, "a+b");
  if (!f) { free(wal->path); free(wal); return NULL; }

  fseek(f, 0, SEEK_END);
  long size = ftell(f);

  if (size == 0) {
    uint64_t magic = KANBUDB_WAL_MAGIC;
    uint32_t version = KANBUDB_WAL_VERSION;
    uint64_t seq = 0;
    if (fwrite(&magic, sizeof(magic), 1, f) != 1 ||
        fwrite(&version, sizeof(version), 1, f) != 1 ||
        fwrite(&seq, sizeof(seq), 1, f) != 1) {
      fclose(f);
      free(wal->path);
      free(wal);
      return NULL;
    }
    wal->seq = 0;
    fflush(f);
  } else {
    fseek(f, 0, SEEK_SET);
    uint64_t magic;
    uint32_t version;
    if (fread(&magic, sizeof(magic), 1, f) != 1 ||
        fread(&version, sizeof(version), 1, f) != 1 ||
        magic != KANBUDB_WAL_MAGIC ||
        version != KANBUDB_WAL_VERSION) {
      fclose(f);
      free(wal->path);
      free(wal);
      return NULL;
    }
    if (fread(&wal->seq, sizeof(wal->seq), 1, f) != 1) {
      fclose(f);
      free(wal->path);
      free(wal);
      return NULL;
    }
    fseek(f, 0, SEEK_END);
  }

  wal->file = f;
  return wal;
}

void wal_destroy(kanbudb_wal_t* wal) {
  if (!wal) return;
  if (wal->file) fclose(wal->file);
  free(wal->path);
  free(wal);
}

int wal_append(kanbudb_wal_t* wal, int op,
               uint64_t table_id, const void* key, size_t key_len,
               const void* value, size_t val_len) {
  wal->seq++;
  uint64_t seq = wal->seq;
  uint8_t op_u8 = (op == KANBUDB_WAL_DELETE) ? 1 : 0;
  uint64_t key_len_u64 = key_len;
  uint64_t val_len_u64 = val_len;

  if (fwrite(&seq, sizeof(seq), 1, wal->file) != 1) return KANBUDB_ERR_IO;
  if (fwrite(&op_u8, sizeof(op_u8), 1, wal->file) != 1) return KANBUDB_ERR_IO;
  if (fwrite(&table_id, sizeof(table_id), 1, wal->file) != 1) return KANBUDB_ERR_IO;
  if (fwrite(&key_len_u64, sizeof(key_len_u64), 1, wal->file) != 1) return KANBUDB_ERR_IO;
  if (fwrite(&val_len_u64, sizeof(val_len_u64), 1, wal->file) != 1) return KANBUDB_ERR_IO;
  if (fwrite(key, 1, key_len, wal->file) != key_len) return KANBUDB_ERR_IO;
  if (op_u8 == 0 && value && val_len > 0) {
    if (fwrite(value, 1, val_len, wal->file) != val_len) return KANBUDB_ERR_IO;
  }

  if (wal->fsync_mode == 2) {
    fflush(wal->file);
  } else if (wal->fsync_mode == 1) {
    wal->periodic_count++;
    if (wal->periodic_count >= KANBUDB_WAL_PERIODIC_THRESHOLD) {
      fflush(wal->file);
      wal->periodic_count = 0;
    }
  }

  return KANBUDB_OK;
}

int wal_sync(kanbudb_wal_t* wal) {
  if (fflush(wal->file) != 0) return KANBUDB_ERR_IO;
  wal->periodic_count = 0;
  return KANBUDB_OK;
}

int wal_replay(kanbudb_wal_t* wal,
               int (*callback)(int op, uint64_t table_id,
                               const void* key, size_t key_len,
                               const void* value, size_t val_len,
                               void* ctx),
               void* ctx) {
  fseek(wal->file, sizeof(wal_header_t), SEEK_SET);

  uint64_t seq;
  uint8_t op_u8;
  uint64_t table_id;
  uint64_t key_len;
  uint64_t val_len;

  while (1) {
    if (fread(&seq, sizeof(seq), 1, wal->file) != 1) break;
    if (fread(&op_u8, sizeof(op_u8), 1, wal->file) != 1) break;
    if (fread(&table_id, sizeof(table_id), 1, wal->file) != 1) break;
    if (fread(&key_len, sizeof(key_len), 1, wal->file) != 1) break;
    if (fread(&val_len, sizeof(val_len), 1, wal->file) != 1) break;

    if (key_len > 1048576 || val_len > 1048576) return KANBUDB_ERR_CORRUPT;

    unsigned char* key = (unsigned char*)malloc(key_len);
    if (!key) return KANBUDB_ERR_OOM;
    if (fread(key, 1, key_len, wal->file) != key_len) {
      free(key);
      return KANBUDB_ERR_CORRUPT;
    }

    unsigned char* value = NULL;
    if (op_u8 == 0 && val_len > 0) {
      value = (unsigned char*)malloc(val_len);
      if (!value) { free(key); return KANBUDB_ERR_OOM; }
      if (fread(value, 1, val_len, wal->file) != val_len) {
        free(key);
        free(value);
        return KANBUDB_ERR_CORRUPT;
      }
    }

    wal->seq = seq;

    if (callback) {
      int rc = callback((op_u8 == 0) ? KANBUDB_WAL_PUT : KANBUDB_WAL_DELETE,
                        table_id, key, key_len, value, val_len, ctx);
      if (rc != 0) {
        free(key);
        free(value);
        return rc;
      }
    }

    free(key);
    free(value);
  }

  fseek(wal->file, 0, SEEK_END);
  return KANBUDB_OK;
}

uint64_t wal_last_seq(kanbudb_wal_t* wal) {
  return wal->seq;
}

/* === util/arena.c === */

#include "arena.h"
#include "macros.h"

typedef struct block_t {
  struct block_t *next;
  size_t capacity;
  size_t offset;
} block_t;

struct arena_t {
  block_t *head;
  block_t *current;
  size_t block_size;
  size_t used;
};

static block_t *block_create(size_t capacity) {
  block_t *b = (block_t *)kanbudb_malloc(sizeof(block_t) + capacity);
  if (KANBUDB_UNLIKELY(!b)) return NULL;
  b->next = NULL;
  b->capacity = capacity;
  b->offset = 0;
  return b;
}

arena_t *arena_create(size_t block_size) {
  arena_t *a = (arena_t *)kanbudb_malloc(sizeof(arena_t));
  if (KANBUDB_UNLIKELY(!a)) return NULL;

  size_t actual_block = block_size > 0 ? block_size : 8192;
  block_t *b = block_create(actual_block);
  if (KANBUDB_UNLIKELY(!b)) {
    kanbudb_free(a);
    return NULL;
  }

  a->head = b;
  a->current = b;
  a->block_size = actual_block;
  a->used = 0;
  return a;
}

void arena_destroy(arena_t *a) {
  if (KANBUDB_UNLIKELY(!a)) return;
  block_t *b = a->head;
  while (b) {
    block_t *next = b->next;
    kanbudb_free(b);
    b = next;
  }
  kanbudb_free(a);
}

void *arena_alloc(arena_t *a, size_t size) {
  if (KANBUDB_UNLIKELY(!a || size == 0)) return NULL;

  size = KANBUDB_ALIGN(size);
  block_t *b = a->current;

  if (KANBUDB_UNLIKELY(b->offset + size > b->capacity)) {
    size_t new_cap = KANBUDB_MAX(a->block_size, size);
    block_t *newb = block_create(new_cap);
    if (KANBUDB_UNLIKELY(!newb)) return NULL;
    b->next = newb;
    a->current = newb;
    b = newb;
  }

  void *ptr = (char *)b + sizeof(block_t) + b->offset;
  b->offset += size;
  a->used += size;
  return ptr;
}

void *arena_alloc_zero(arena_t *a, size_t size) {
  void *ptr = arena_alloc(a, size);
  if (KANBUDB_LIKELY(ptr)) {
    memset(ptr, 0, size);
  }
  return ptr;
}

void arena_reset(arena_t *a) {
  if (KANBUDB_UNLIKELY(!a)) return;
  block_t *b = a->head;
  while (b) {
    b->offset = 0;
    b = b->next;
  }
  a->current = a->head;
  a->used = 0;
}

size_t arena_used(arena_t *a) {
  return a ? a->used : 0;
}

/* === util/fst.c === */

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

/* === util/page_cache.c === */

#include "page_cache.h"
#include "macros.h"

int page_cache_init(kanbudb_page_cache_t *pc, size_t max_pages) {
  if (max_pages == 0) return KANBUDB_ERR_INVAL;
  pc->slots = (kanbudb_page_t **)kanbudb_calloc(max_pages, sizeof(kanbudb_page_t *));
  pc->capacity = max_pages;
  pc->count = 0;
  pc->lru_head.lru_next = &pc->lru_head;
  pc->lru_head.lru_prev = &pc->lru_head;
  pc->hits = 0;
  pc->misses = 0;
  return KANBUDB_OK;
}

void page_cache_destroy(kanbudb_page_cache_t *pc) {
  for (size_t i = 0; i < pc->capacity; i++) {
    kanbudb_page_t *page = pc->slots[i];
    while (page) {
      kanbudb_page_t *next = page->hash_next;
      kanbudb_free(page);
      page = next;
    }
  }
  kanbudb_free(pc->slots);
  pc->slots = NULL;
  pc->capacity = 0;
  pc->count = 0;
}

static void lru_remove(kanbudb_page_cache_t *pc, kanbudb_page_t *page) {
  (void)pc;
  page->lru_prev->lru_next = page->lru_next;
  page->lru_next->lru_prev = page->lru_prev;
}

static void lru_push_front(kanbudb_page_cache_t *pc, kanbudb_page_t *page) {
  page->lru_next = pc->lru_head.lru_next;
  page->lru_prev = &pc->lru_head;
  pc->lru_head.lru_next->lru_prev = page;
  pc->lru_head.lru_next = page;
}

kanbudb_page_t *page_cache_get(kanbudb_page_cache_t *pc, uint64_t page_id) {
  size_t idx = page_id % pc->capacity;
  kanbudb_page_t *page = pc->slots[idx];
  while (page) {
    if (page->id == page_id) {
      lru_remove(pc, page);
      lru_push_front(pc, page);
      page->refcount++;
      pc->hits++;
      return page;
    }
    page = page->hash_next;
  }
  pc->misses++;
  return NULL;
}

kanbudb_page_t *page_cache_alloc(kanbudb_page_cache_t *pc, uint64_t page_id) {
  size_t idx = page_id % pc->capacity;
  kanbudb_page_t *page = pc->slots[idx];
  while (page) {
    if (page->id == page_id) {
      lru_remove(pc, page);
      lru_push_front(pc, page);
      page->refcount++;
      return page;
    }
    page = page->hash_next;
  }

  if (pc->count >= pc->capacity) {
    kanbudb_page_t *evict = pc->lru_head.lru_prev;
    while (evict != &pc->lru_head && evict->refcount > 1) {
      evict = evict->lru_prev;
    }
    if (evict == &pc->lru_head) {
      return NULL;
    }
    lru_remove(pc, evict);

    size_t evict_idx = evict->id % pc->capacity;
    kanbudb_page_t **pp = &pc->slots[evict_idx];
    while (*pp) {
      if (*pp == evict) {
        *pp = evict->hash_next;
        break;
      }
      pp = &(*pp)->hash_next;
    }
    kanbudb_free(evict);
    pc->count--;
  }

  kanbudb_page_t *new_page = (kanbudb_page_t *)kanbudb_malloc(sizeof(kanbudb_page_t));
  if (KANBUDB_UNLIKELY(!new_page)) return NULL;

  new_page->id = page_id;
  memset(new_page->data, 0, KANBUDB_PAGE_SIZE);
  new_page->refcount = 1;
  new_page->dirty = 0;
  new_page->hash_next = NULL;
  new_page->lru_next = NULL;
  new_page->lru_prev = NULL;

  new_page->hash_next = pc->slots[idx];
  pc->slots[idx] = new_page;

  lru_push_front(pc, new_page);
  pc->count++;
  return new_page;
}

void page_cache_release(kanbudb_page_cache_t *pc, kanbudb_page_t *page) {
  (void)pc;
  if (page) page->refcount--;
}

void page_cache_mark_dirty(kanbudb_page_cache_t *pc, kanbudb_page_t *page) {
  (void)pc;
  if (page) page->dirty = 1;
}

void page_cache_flush(kanbudb_page_cache_t *pc,
    void (*write_fn)(void *ctx, uint64_t page_id, const unsigned char *data),
    void *ctx) {
  for (size_t i = 0; i < pc->capacity; i++) {
    kanbudb_page_t *page = pc->slots[i];
    while (page) {
      if (page->dirty) {
        write_fn(ctx, page->id, page->data);
        page->dirty = 0;
      }
      page = page->hash_next;
    }
  }
}
