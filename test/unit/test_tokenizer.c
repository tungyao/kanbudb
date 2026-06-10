#include "tokenizer.h"
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
  kanbudb_tokenizer_t* t = tokenizer_create();
  if (!t) return 0;
  tokenizer_destroy(t);
  return 1;
}

static int test_basic_tokenize(void) {
  kanbudb_tokenizer_t* t = tokenizer_create();
  if (!t) return 0;

  kanbudb_token_t tokens[16];
  int n = tokenizer_tokenize(t, "hello world", 11, tokens, 16);

  if (n != 2) { tokenizer_destroy(t); return 0; }
  if (tokens[0].len != 5 || strcmp(tokens[0].word, "hello") != 0) { tokenizer_destroy(t); return 0; }
  if (tokens[1].len != 5 || strcmp(tokens[1].word, "world") != 0) { tokenizer_destroy(t); return 0; }
  if (tokens[0].pos != 0 || tokens[1].pos != 1) { tokenizer_destroy(t); return 0; }

  tokenizer_destroy(t);
  return 1;
}

static int test_stemming(void) {
  kanbudb_tokenizer_t* t = tokenizer_create();
  if (!t) return 0;
  tokenizer_set_stemmer(t, 1);

  kanbudb_token_t tokens[16];
  int n = tokenizer_tokenize(t, "running walked", 14, tokens, 16);

  if (n != 2) { tokenizer_destroy(t); return 0; }
  if (strcmp(tokens[0].word, "run") != 0) { tokenizer_destroy(t); return 0; }
  if (strcmp(tokens[1].word, "walk") != 0) { tokenizer_destroy(t); return 0; }

  tokenizer_destroy(t);
  return 1;
}

static int test_stopwords(void) {
  kanbudb_tokenizer_t* t = tokenizer_create();
  if (!t) return 0;

  const char* stopwords[] = {"the"};
  tokenizer_set_stopwords(t, stopwords, 1);

  kanbudb_token_t tokens[16];
  int n = tokenizer_tokenize(t, "the apple", 9, tokens, 16);

  if (n != 1) { tokenizer_destroy(t); return 0; }
  if (strcmp(tokens[0].word, "apple") != 0) { tokenizer_destroy(t); return 0; }

  tokenizer_destroy(t);
  return 1;
}

static int test_lowercase(void) {
  kanbudb_tokenizer_t* t = tokenizer_create();
  if (!t) return 0;

  kanbudb_token_t tokens[16];
  int n = tokenizer_tokenize(t, "Hello World", 11, tokens, 16);

  if (n != 2) { tokenizer_destroy(t); return 0; }
  if (strcmp(tokens[0].word, "hello") != 0) { tokenizer_destroy(t); return 0; }
  if (strcmp(tokens[1].word, "world") != 0) { tokenizer_destroy(t); return 0; }

  tokenizer_destroy(t);
  return 1;
}

static int test_empty_input(void) {
  kanbudb_tokenizer_t* t = tokenizer_create();
  if (!t) return 0;

  kanbudb_token_t tokens[16];
  int n = tokenizer_tokenize(t, "", 0, tokens, 16);

  if (n != 0) { tokenizer_destroy(t); return 0; }

  tokenizer_destroy(t);
  return 1;
}

int main(void) {
  printf("tokenizer tests:\n");
  TEST(create_destroy);
  TEST(basic_tokenize);
  TEST(stemming);
  TEST(stopwords);
  TEST(lowercase);
  TEST(empty_input);
  printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
  return tests_failed > 0 ? 1 : 0;
}
