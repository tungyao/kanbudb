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
