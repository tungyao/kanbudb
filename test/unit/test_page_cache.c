#include "page_cache.h"
#include "macros.h"
#include <stdio.h>
#include <string.h>

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) do { \
  printf("  TEST: %s ... ", #name); \
  if (test_##name()) { printf("PASS\n"); tests_passed++; } \
  else { printf("FAIL\n"); tests_failed++; } \
} while(0)

static int test_init_destroy(void) {
  kanbudb_page_cache_t pc;
  if (page_cache_init(&pc, 16) != KANBUDB_OK) return 0;
  page_cache_destroy(&pc);
  return 1;
}

static int test_get_miss(void) {
  kanbudb_page_cache_t pc;
  if (page_cache_init(&pc, 16) != KANBUDB_OK) return 0;
  kanbudb_page_t *p = page_cache_get(&pc, 42);
  int ok = (p == NULL);
  page_cache_destroy(&pc);
  return ok;
}

static int test_alloc_and_get(void) {
  kanbudb_page_cache_t pc;
  if (page_cache_init(&pc, 16) != KANBUDB_OK) return 0;
  kanbudb_page_t *a = page_cache_alloc(&pc, 42);
  if (!a) { page_cache_destroy(&pc); return 0; }
  int ok = (a->id == 42);
  kanbudb_page_t *b = page_cache_get(&pc, 42);
  ok = ok && (b == a);
  ok = ok && (b->refcount == 2);
  page_cache_release(&pc, b);
  page_cache_destroy(&pc);
  return ok;
}

static int test_eviction(void) {
  kanbudb_page_cache_t pc;
  if (page_cache_init(&pc, 4) != KANBUDB_OK) return 0;
  kanbudb_page_t *pages[8];
  int i, ok = 1;

  for (i = 0; i < 8; i++) {
    pages[i] = page_cache_alloc(&pc, i);
    if (!pages[i]) { ok = 0; break; }
  }
  if (!ok) { page_cache_destroy(&pc); return 0; }

  kanbudb_page_t *p0 = page_cache_get(&pc, 0);
  ok = ok && (p0 == NULL);

  kanbudb_page_t *p7 = page_cache_get(&pc, 7);
  ok = ok && (p7 != NULL);
  if (p7) page_cache_release(&pc, p7);

  kanbudb_page_t *p8 = page_cache_alloc(&pc, 8);
  ok = ok && (p8 != NULL);

  page_cache_destroy(&pc);
  return ok;
}

typedef struct {
  int count;
  uint64_t page_ids[2];
  unsigned char data[KANBUDB_PAGE_SIZE];
} write_record_t;

static void test_write_fn(void *ctx, uint64_t page_id, const unsigned char *data) {
  write_record_t *rec = (write_record_t *)ctx;
  if (rec->count < 2) rec->page_ids[rec->count] = page_id;
  rec->count++;
  memcpy(rec->data, data, KANBUDB_PAGE_SIZE);
}

static int test_flush(void) {
  kanbudb_page_cache_t pc;
  if (page_cache_init(&pc, 16) != KANBUDB_OK) return 0;

  kanbudb_page_t *p1 = page_cache_alloc(&pc, 10);
  kanbudb_page_t *p2 = page_cache_alloc(&pc, 20);
  if (!p1 || !p2) { page_cache_destroy(&pc); return 0; }

  page_cache_mark_dirty(&pc, p1);
  page_cache_mark_dirty(&pc, p2);

  write_record_t rec;
  rec.count = 0;
  memset(&rec, 0, sizeof(rec));

  page_cache_flush(&pc, test_write_fn, &rec);

  int ok = (rec.count == 2);
  ok = ok && ((rec.page_ids[0] == 10 && rec.page_ids[1] == 20) ||
              (rec.page_ids[0] == 20 && rec.page_ids[1] == 10));

  page_cache_destroy(&pc);
  return ok;
}

static int test_alloc_existing(void) {
  kanbudb_page_cache_t pc;
  if (page_cache_init(&pc, 16) != KANBUDB_OK) return 0;

  kanbudb_page_t *a = page_cache_alloc(&pc, 42);
  if (!a) { page_cache_destroy(&pc); return 0; }

  kanbudb_page_t *b = page_cache_alloc(&pc, 42);
  if (!b) { page_cache_destroy(&pc); return 0; }

  int ok = (a == b);
  ok = ok && (a->refcount == 2);

  page_cache_release(&pc, a);
  page_cache_destroy(&pc);
  return ok;
}

int main(void) {
  printf("page_cache tests:\n");
  TEST(init_destroy);
  TEST(get_miss);
  TEST(alloc_and_get);
  TEST(alloc_existing);
  TEST(eviction);
  TEST(flush);
  printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
  return tests_failed > 0 ? 1 : 0;
}
