#ifndef KANBUDB_PAGE_CACHE_H
#define KANBUDB_PAGE_CACHE_H

#include <stddef.h>
#include <stdint.h>

#define KANBUDB_PAGE_SIZE 4096

typedef struct kanbudb_page_t {
  uint64_t id;
  unsigned char data[KANBUDB_PAGE_SIZE];
  int refcount;
  int dirty;
  struct kanbudb_page_t *hash_next;
  struct kanbudb_page_t *lru_next;
  struct kanbudb_page_t *lru_prev;
} kanbudb_page_t;

typedef struct kanbudb_page_cache_t {
  kanbudb_page_t **slots;
  size_t capacity;
  size_t count;
  kanbudb_page_t lru_head;
  uint64_t hits;
  uint64_t misses;
} kanbudb_page_cache_t;

int page_cache_init(kanbudb_page_cache_t *pc, size_t max_pages);
void page_cache_destroy(kanbudb_page_cache_t *pc);
kanbudb_page_t *page_cache_get(kanbudb_page_cache_t *pc, uint64_t page_id);
kanbudb_page_t *page_cache_alloc(kanbudb_page_cache_t *pc, uint64_t page_id);
void page_cache_release(kanbudb_page_cache_t *pc, kanbudb_page_t *page);
void page_cache_mark_dirty(kanbudb_page_cache_t *pc, kanbudb_page_t *page);
void page_cache_flush(kanbudb_page_cache_t *pc,
    void (*write_fn)(void *ctx, uint64_t page_id, const unsigned char *data),
    void *ctx);

#endif /* KANBUDB_PAGE_CACHE_H */
