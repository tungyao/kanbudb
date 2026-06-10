#ifndef KANBUDB_ROW_FORMAT_H
#define KANBUDB_ROW_FORMAT_H

#include "macros.h"
#include "db.h"
#include "query_builder.h"

typedef struct {
  int   num_cols;
  i32   fixed_offsets[KANBUDB_MAX_COLS];
  i32   fixed_sizes[KANBUDB_MAX_COLS];
} row_schema_t;

void row_schema_init(row_schema_t* rs, const kanbudb_col_type_t* types, int num_cols);

const void* row_extract_column(const row_schema_t* rs, int col,
                                const void* row_data, i32 row_len, i32* out_len);

#endif
