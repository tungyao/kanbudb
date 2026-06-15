#ifndef KANBUDB_QUERY_BUILDER_H
#define KANBUDB_QUERY_BUILDER_H

#include "db.h"
#include "macros.h"

#define KANBUDB_MAX_COLS 64

/* Condition tree node for multi-condition filters */
typedef enum {
  QBCOND_LEAF,
  QBCOND_AND,
  QBCOND_OR,
  QBCOND_NOT
} qb_cond_type_t;

typedef struct qb_condition_t {
  qb_cond_type_t type;
  union {
    struct {
      char column[64];
      char op[16];
      char value[256];
      size_t value_len;
    } leaf;
    struct {
      struct qb_condition_t* left;
      struct qb_condition_t* right;
    } comb;
    struct {
      struct qb_condition_t* child;
    } neg;
  } as;
} qb_condition_t;

struct result_set_t {
  int               num_rows;
  int               current;
  int               num_cols;
  kanbudb_col_type_t col_types[KANBUDB_MAX_COLS];
  char*             col_names[KANBUDB_MAX_COLS];
  int               is_fts;
  uint64_t*         doc_ids;
  double*           scores;
  /* Row storage for query results */
  void**            row_data;
  size_t*           row_lens;
  int               row_capacity;
  int               row_count;
};

/* Recursively destroy a condition tree */
void condition_destroy(qb_condition_t* cond);

/* Add a row to result set (copies the data) */
int rs_add_row(result_set_t* rs, const void* data, size_t len);

/* Get pointer to column data from a stored row */
const void* rs_get_row_column(result_set_t* rs, int row_idx, int col, size_t* out_len);

#endif /* KANBUDB_QUERY_BUILDER_H */
