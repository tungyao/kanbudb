#include "compaction.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) do { \
  printf("  TEST: %s ... ", #name); \
  if (test_##name()) { printf("PASS\n"); tests_passed++; } \
  else { printf("FAIL\n"); tests_failed++; } \
} while(0)

static int test_create_destroy(void) {
  kanbudb_compactor_t* c = compactor_create();
  if (!c) return 0;
  compactor_destroy(c);
  return 1;
}

static int test_compact_copies_data(void) {
  kanbudb_compactor_t* c = compactor_create();
  if (!c) return 0;

  const uint8_t input[] = {0x01, 0x02, 0x03, 0x04, 0x05};
  size_t input_len = sizeof(input);

  uint8_t* output = NULL;
  size_t output_len = 0;

  int rc = compactor_compact(c, input, input_len, &output, &output_len);
  if (rc != KANBUDB_OK) { compactor_destroy(c); return 0; }
  if (output_len != input_len) { free(output); compactor_destroy(c); return 0; }
  if (memcmp(output, input, input_len) != 0) { free(output); compactor_destroy(c); return 0; }

  free(output);
  compactor_destroy(c);
  return 1;
}

static int test_compact_empty(void) {
  kanbudb_compactor_t* c = compactor_create();
  if (!c) return 0;

  uint8_t* output = NULL;
  size_t output_len = 0;

  int rc = compactor_compact(c, (const uint8_t*)"", 0, &output, &output_len);
  if (rc != KANBUDB_OK) { compactor_destroy(c); return 0; }
  if (output_len != 0) { free(output); compactor_destroy(c); return 0; }

  if (output) {
    free(output);
  }
  compactor_destroy(c);
  return 1;
}

static int test_compact_null_checks(void) {
  kanbudb_compactor_t* c = compactor_create();
  if (!c) return 0;

  uint8_t input[] = {0x01};
  uint8_t* output = NULL;
  size_t output_len = 0;

  int rc = compactor_compact(c, NULL, 1, &output, &output_len);
  if (rc == KANBUDB_OK) { compactor_destroy(c); return 0; }

  rc = compactor_compact(c, input, 1, NULL, &output_len);
  if (rc == KANBUDB_OK) { compactor_destroy(c); return 0; }

  rc = compactor_compact(c, input, 1, &output, NULL);
  if (rc == KANBUDB_OK) { compactor_destroy(c); return 0; }

  compactor_destroy(c);
  return 1;
}

int main(void) {
  printf("compaction tests:\n");
  TEST(create_destroy);
  TEST(compact_copies_data);
  TEST(compact_empty);
  TEST(compact_null_checks);
  printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
  return tests_failed > 0 ? 1 : 0;
}
