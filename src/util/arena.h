#ifndef KANBUDB_ARENA_H
#define KANBUDB_ARENA_H

#include <stddef.h>

typedef struct arena_t arena_t;

arena_t *arena_create(size_t block_size);
void arena_destroy(arena_t *a);
void *arena_alloc(arena_t *a, size_t size);
void *arena_alloc_zero(arena_t *a, size_t size);
void arena_reset(arena_t *a);
size_t arena_used(arena_t *a);

#endif /* KANBUDB_ARENA_H */
