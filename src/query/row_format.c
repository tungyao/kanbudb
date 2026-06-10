#include "row_format.h"

void row_schema_init(row_schema_t* rs, const kanbudb_col_type_t* types, int num_cols) {
  if (!rs) return;
  rs->num_cols = num_cols;
  i32 offset = 0;
  for (int i = 0; i < num_cols && i < KANBUDB_MAX_COLS; i++) {
    switch (types[i]) {
      case KANBUDB_INT32:  rs->fixed_offsets[i] = offset; rs->fixed_sizes[i] = 4;  offset += 4; break;
      case KANBUDB_INT64:  rs->fixed_offsets[i] = offset; rs->fixed_sizes[i] = 8;  offset += 8; break;
      case KANBUDB_FLOAT:  rs->fixed_offsets[i] = offset; rs->fixed_sizes[i] = 4;  offset += 4; break;
      case KANBUDB_DOUBLE: rs->fixed_offsets[i] = offset; rs->fixed_sizes[i] = 8;  offset += 8; break;
      case KANBUDB_BOOL:   rs->fixed_offsets[i] = offset; rs->fixed_sizes[i] = 1;  offset += 1; break;
      default:
        rs->fixed_offsets[i] = -1; rs->fixed_sizes[i] = 0;
        offset += 4;
        break;
    }
  }
}

const void* row_extract_column(const row_schema_t* rs, int col,
                                const void* row_data, i32 row_len, i32* out_len) {
  if (!rs || col < 0 || col >= rs->num_cols || !row_data || !out_len) return NULL;

  const u8* data = (const u8*)row_data;
  i32 offset = 0;

  for (int i = 0; i < col; i++) {
    if (rs->fixed_sizes[i] > 0) {
      offset += rs->fixed_sizes[i];
    } else {
      if (offset + 4 > row_len) return NULL;
      u32 slen;
      memcpy(&slen, data + offset, 4);
      offset += 4 + (i32)slen;
    }
  }

  if (rs->fixed_sizes[col] > 0) {
    if (offset + rs->fixed_sizes[col] > row_len) return NULL;
    *out_len = rs->fixed_sizes[col];
    return data + offset;
  }

  if (offset + 4 > row_len) return NULL;
  u32 slen;
  memcpy(&slen, data + offset, 4);
  *out_len = (i32)slen;
  return data + offset + 4;
}

