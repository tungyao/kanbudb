#ifndef KANBUDB_QUERY_BUILDER_H
#define KANBUDB_QUERY_BUILDER_H

#include "db.h"
#include "macros.h"

#define KANBUDB_MAX_COLS 64

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

/* Add a row to result set (copies the data) */
int rs_add_row(result_set_t* rs, const void* data, size_t len);

/* Get pointer to column data from a stored row */
const void* rs_get_row_column(result_set_t* rs, int row_idx, int col, size_t* out_len);

#endif /* KANBUDB_QUERY_BUILDER_H */
