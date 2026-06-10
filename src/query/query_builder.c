#include "query_builder.h"
#include "db.h"
#include "macros.h"
#include "core/db.h"
#include "lsm.h"
#include "row_format.h"

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

int rs_add_row(result_set_t* rs, const void* data, size_t len) {
  if (!rs) return KANBUDB_ERR_INVAL;
  if (rs->row_count >= rs->row_capacity) {
    int new_cap = rs->row_capacity ? rs->row_capacity * 2 : 64;
    void** new_data = (void**)realloc(rs->row_data, (size_t)new_cap * sizeof(void*));
    size_t* new_lens = (size_t*)realloc(rs->row_lens, (size_t)new_cap * sizeof(size_t));
    if (!new_data || !new_lens) return KANBUDB_ERR_OOM;
    rs->row_data = new_data;
    rs->row_lens = new_lens;
    rs->row_capacity = new_cap;
  }
  void* copy = malloc(len);
  if (!copy) return KANBUDB_ERR_OOM;
  memcpy(copy, data, len);
  rs->row_data[rs->row_count] = copy;
  rs->row_lens[rs->row_count] = len;
  rs->row_count++;
  return KANBUDB_OK;
}

const void* rs_get_row_column(result_set_t* rs, int row_idx, int col, size_t* out_len) {
  if (!rs || row_idx < 0 || row_idx >= rs->row_count || col < 0 || col >= rs->num_cols) {
    if (out_len) *out_len = 0;
    return NULL;
  }
  row_schema_t schema;
  row_schema_init(&schema, rs->col_types, rs->num_cols);
  i32 len;
  const void* ptr = row_extract_column(&schema, col, rs->row_data[row_idx],
                                         (i32)rs->row_lens[row_idx], &len);
  if (out_len) *out_len = (size_t)len;
  return ptr;
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
  for (int i = 0; i < rs->row_count; i++) {
    free(rs->row_data[i]);
  }
  free(rs->row_data);
  free(rs->row_lens);
  free(rs);
}
