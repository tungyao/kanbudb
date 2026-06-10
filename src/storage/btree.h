#ifndef KANBUDB_BTREE_H
#define KANBUDB_BTREE_H

#include "macros.h"

#define BTREE_ORDER 16

typedef struct kanbudb_btree kanbudb_btree_t;

typedef struct {
  void*  key;
  size_t key_len;
  void*  value;
  size_t val_len;
} btree_kv_t;

kanbudb_btree_t* btree_create(void);
void            btree_destroy(kanbudb_btree_t* bt);
int             btree_put(kanbudb_btree_t* bt, const void* key, size_t key_len,
                          const void* value, size_t val_len);
int             btree_get(kanbudb_btree_t* bt, const void* key, size_t key_len,
                          void** out_value, size_t* out_val_len);
int             btree_delete(kanbudb_btree_t* bt, const void* key, size_t key_len);

/* Cursor for range scans */
typedef struct btree_cursor btree_cursor_t;
btree_cursor_t* btree_cursor_create(kanbudb_btree_t* bt);
int             btree_cursor_seek(btree_cursor_t* cur, const void* key, size_t key_len);
int             btree_cursor_next(btree_cursor_t* cur, btree_kv_t* out);
void            btree_cursor_destroy(btree_cursor_t* cur);

#endif
