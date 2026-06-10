#include "fts/index.h"
#include "fts/parser.h"
#include "fts/ranker.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) do { \
  printf("  TEST: %s ... ", #name); \
  if (test_##name()) { printf("PASS\n"); tests_passed++; } \
  else { printf("FAIL\n"); tests_failed++; } \
} while(0)

static int test_index_create_destroy(void) {
  kanbudb_fts_index_t* idx = fts_index_create();
  if (!idx) return 0;
  fts_index_destroy(idx);
  return 1;
}

static int test_index_add_search(void) {
  kanbudb_fts_index_t* idx = fts_index_create();
  if (!idx) return 0;

  const char* terms[] = {"hello", "world"};
  const size_t term_lens[] = {5, 5};
  const size_t positions[] = {0, 1};
  int rc = fts_index_add_document(idx, terms, term_lens, positions, 2);
  if (rc != KANBUDB_OK) { fts_index_destroy(idx); return 0; }

  uint64_t results[16];
  int n = fts_index_search(idx, "hello", results, 16);
  int ok = (n == 1);
  if (ok) {
    uint64_t doc_id = results[0];
    ok = (doc_id == 0);
  }

  fts_index_destroy(idx);
  return ok;
}

static int test_parse_term(void) {
  fts_query_node_t nodes[16];
  int n = fts_query_parse("hello world", nodes, 16);
  if (n != 2) return 0;
  if (nodes[0].op != FTS_TERM) return 0;
  if (strcmp(nodes[0].text, "hello") != 0) return 0;
  if (nodes[1].op != FTS_TERM) return 0;
  if (strcmp(nodes[1].text, "world") != 0) return 0;
  return 1;
}

static int test_parse_phrase(void) {
  fts_query_node_t nodes[16];
  int n = fts_query_parse("\"hello world\"", nodes, 16);
  if (n != 1) return 0;
  if (nodes[0].op != FTS_PHRASE) return 0;
  if (strcmp(nodes[0].text, "hello world") != 0) return 0;
  return 1;
}

static int test_parse_fuzzy(void) {
  fts_query_node_t nodes[16];
  int n = fts_query_parse("hello~2", nodes, 16);
  if (n != 1) return 0;
  if (nodes[0].op != FTS_FUZZY) return 0;
  if (nodes[0].fuzzy_distance != 2) return 0;
  if (strcmp(nodes[0].text, "hello") != 0) return 0;
  return 1;
}

static int test_parse_boolean(void) {
  fts_query_node_t nodes[16];
  int n = fts_query_parse("hello AND world", nodes, 16);
  if (n != 3) return 0;
  if (nodes[0].op != FTS_TERM || strcmp(nodes[0].text, "hello") != 0) return 0;
  if (nodes[1].op != FTS_BOOLEAN_AND) return 0;
  if (nodes[2].op != FTS_TERM || strcmp(nodes[2].text, "world") != 0) return 0;
  return 1;
}

static int test_parse_boost(void) {
  fts_query_node_t nodes[16];
  int n = fts_query_parse("hello^2.5", nodes, 16);
  if (n != 1) return 0;
  if (nodes[0].op != FTS_TERM) return 0;
  if (fabs(nodes[0].boost - 2.5) > 1e-9) return 0;
  return 1;
}

static int test_parse_field(void) {
  fts_query_node_t nodes[16];
  int n = fts_query_parse("title:hello", nodes, 16);
  if (n != 1) return 0;
  if (nodes[0].op != FTS_TERM) return 0;
  if (strcmp(nodes[0].field, "title") != 0) return 0;
  if (strcmp(nodes[0].text, "hello") != 0) return 0;
  return 1;
}

static int test_bm25(void) {
  double score = bm25_score(3.0, 10.0, 8.0, 100.0, 5.0, 1.2, 0.75);
  if (!(score > 0.0)) return 0;
  if (!isfinite(score)) return 0;
  return 1;
}

int main(void) {
  printf("fts tests:\n");
  TEST(index_create_destroy);
  TEST(index_add_search);
  TEST(parse_term);
  TEST(parse_phrase);
  TEST(parse_fuzzy);
  TEST(parse_boolean);
  TEST(parse_boost);
  TEST(parse_field);
  TEST(bm25);
  printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
  return tests_failed > 0 ? 1 : 0;
}
