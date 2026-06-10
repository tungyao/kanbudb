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
};

#endif /* KANBUDB_QUERY_BUILDER_H */
