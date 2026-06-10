#include "arena.h"
#include <stdio.h>
#include <string.h>

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) do { \
  printf("  TEST: %s ... ", #name); \
  if (test_##name()) { printf("PASS\n"); tests_passed++; } \
  else { printf("FAIL\n"); tests_failed++; } \
} while(0)

static int test_create_destroy(void) {
  arena_t *a = arena_create(1024);
  if (!a) return 0;
  arena_destroy(a);
  return 1;
}

static int test_basic_alloc(void) {
  arena_t *a = arena_create(1024);
  if (!a) return 0;

  void *p1 = arena_alloc(a, 16);
  void *p2 = arena_alloc(a, 32);
  void *p3 = arena_alloc(a, 64);

  int ok = (p1 != NULL && p2 != NULL && p3 != NULL);
  ok = ok && (p1 != p2 && p2 != p3);
  ok = ok && (arena_used(a) == 112);

  arena_destroy(a);
  return ok;
}

static int test_alloc_zero(void) {
  arena_t *a = arena_create(1024);
  if (!a) return 0;

  void *p = arena_alloc_zero(a, 128);
  if (!p) { arena_destroy(a); return 0; }

  unsigned char *buf = (unsigned char *)p;
  int ok = 1;
  for (int i = 0; i < 128; i++) {
    if (buf[i] != 0) { ok = 0; break; }
  }

  arena_destroy(a);
  return ok;
}

static int test_reset(void) {
  arena_t *a = arena_create(1024);
  if (!a) return 0;

  arena_alloc(a, 256);
  arena_alloc(a, 256);

  if (arena_used(a) == 0) { arena_destroy(a); return 0; }

  arena_reset(a);

  int ok = (arena_used(a) == 0);

  void *p = arena_alloc(a, 64);
  ok = ok && (p != NULL);

  arena_destroy(a);
  return ok;
}

static int test_null_alloc(void) {
  void *p = arena_alloc(NULL, 16);
  return (p == NULL);
}

static int test_large_alloc(void) {
  arena_t *a = arena_create(256);
  if (!a) return 0;

  void *p = arena_alloc(a, 1024);
  if (!p) { arena_destroy(a); return 0; }

  int ok = (arena_used(a) >= 1024);

  arena_destroy(a);
  return ok;
}

int main(void) {
  printf("arena tests:\n");
  TEST(create_destroy);
  TEST(basic_alloc);
  TEST(alloc_zero);
  TEST(reset);
  TEST(null_alloc);
  TEST(large_alloc);
  printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
  return tests_failed > 0 ? 1 : 0;
}
