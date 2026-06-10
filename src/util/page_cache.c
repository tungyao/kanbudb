#include "page_cache.h"
#include "macros.h"

int page_cache_init(kanbudb_page_cache_t *pc, size_t max_pages) {
  if (max_pages == 0) return KANBUDB_ERR_INVAL;
  pc->slots = (kanbudb_page_t **)kanbudb_calloc(max_pages, sizeof(kanbudb_page_t *));
  pc->capacity = max_pages;
  pc->count = 0;
  pc->lru_head.lru_next = &pc->lru_head;
  pc->lru_head.lru_prev = &pc->lru_head;
  pc->hits = 0;
  pc->misses = 0;
  return KANBUDB_OK;
}

void page_cache_destroy(kanbudb_page_cache_t *pc) {
  for (size_t i = 0; i < pc->capacity; i++) {
    kanbudb_page_t *page = pc->slots[i];
    while (page) {
      kanbudb_page_t *next = page->hash_next;
      kanbudb_free(page);
      page = next;
    }
  }
  kanbudb_free(pc->slots);
  pc->slots = NULL;
  pc->capacity = 0;
  pc->count = 0;
}

static void lru_remove(kanbudb_page_cache_t *pc, kanbudb_page_t *page) {
  (void)pc;
  page->lru_prev->lru_next = page->lru_next;
  page->lru_next->lru_prev = page->lru_prev;
}

static void lru_push_front(kanbudb_page_cache_t *pc, kanbudb_page_t *page) {
  page->lru_next = pc->lru_head.lru_next;
  page->lru_prev = &pc->lru_head;
  pc->lru_head.lru_next->lru_prev = page;
  pc->lru_head.lru_next = page;
}

kanbudb_page_t *page_cache_get(kanbudb_page_cache_t *pc, uint64_t page_id) {
  size_t idx = page_id % pc->capacity;
  kanbudb_page_t *page = pc->slots[idx];
  while (page) {
    if (page->id == page_id) {
      lru_remove(pc, page);
      lru_push_front(pc, page);
      page->refcount++;
      pc->hits++;
      return page;
    }
    page = page->hash_next;
  }
  pc->misses++;
  return NULL;
}

kanbudb_page_t *page_cache_alloc(kanbudb_page_cache_t *pc, uint64_t page_id) {
  size_t idx = page_id % pc->capacity;
  kanbudb_page_t *page = pc->slots[idx];
  while (page) {
    if (page->id == page_id) {
      lru_remove(pc, page);
      lru_push_front(pc, page);
      page->refcount++;
      return page;
    }
    page = page->hash_next;
  }

  if (pc->count >= pc->capacity) {
    kanbudb_page_t *evict = pc->lru_head.lru_prev;
    while (evict != &pc->lru_head && evict->refcount > 1) {
      evict = evict->lru_prev;
    }
    if (evict == &pc->lru_head) {
      return NULL;
    }
    lru_remove(pc, evict);

    size_t evict_idx = evict->id % pc->capacity;
    kanbudb_page_t **pp = &pc->slots[evict_idx];
    while (*pp) {
      if (*pp == evict) {
        *pp = evict->hash_next;
        break;
      }
      pp = &(*pp)->hash_next;
    }
    kanbudb_free(evict);
    pc->count--;
  }

  kanbudb_page_t *new_page = (kanbudb_page_t *)kanbudb_malloc(sizeof(kanbudb_page_t));
  if (KANBUDB_UNLIKELY(!new_page)) return NULL;

  new_page->id = page_id;
  memset(new_page->data, 0, KANBUDB_PAGE_SIZE);
  new_page->refcount = 1;
  new_page->dirty = 0;
  new_page->hash_next = NULL;
  new_page->lru_next = NULL;
  new_page->lru_prev = NULL;

  new_page->hash_next = pc->slots[idx];
  pc->slots[idx] = new_page;

  lru_push_front(pc, new_page);
  pc->count++;
  return new_page;
}

void page_cache_release(kanbudb_page_cache_t *pc, kanbudb_page_t *page) {
  (void)pc;
  if (page) page->refcount--;
}

void page_cache_mark_dirty(kanbudb_page_cache_t *pc, kanbudb_page_t *page) {
  (void)pc;
  if (page) page->dirty = 1;
}

void page_cache_flush(kanbudb_page_cache_t *pc,
    void (*write_fn)(void *ctx, uint64_t page_id, const unsigned char *data),
    void *ctx) {
  for (size_t i = 0; i < pc->capacity; i++) {
    kanbudb_page_t *page = pc->slots[i];
    while (page) {
      if (page->dirty) {
        write_fn(ctx, page->id, page->data);
        page->dirty = 0;
      }
      page = page->hash_next;
    }
  }
}
