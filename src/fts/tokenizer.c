#include "tokenizer.h"
#include <string.h>
#include <ctype.h>

#define STEM_BUF_SIZE 256
#define INITIAL_WORDS_CAP 64

struct kanbudb_tokenizer {
  int         stem_enabled;
  char**      stopwords;
  int         stopwords_count;
  char        stem_buf[STEM_BUF_SIZE];
  char**      words;
  int         words_cap;
  int         words_count;
};

static char* dup_str(const char* s, size_t len) {
  char* d = kanbudb_malloc(len + 1);
  if (!d) return NULL;
  memcpy(d, s, len);
  d[len] = '\0';
  return d;
}

static int is_token_char(int c) {
  return isalnum((unsigned char)c) || c == '\'';
}

static int has_suffix(const char* word, size_t word_len, const char* suffix) {
  size_t slen = strlen(suffix);
  if (word_len < slen) return 0;
  return memcmp(word + word_len - slen, suffix, slen) == 0;
}

static int stem_english(char* word, int len) {
  if (len < 3) return len;

  static const char* suffixes[] = {"ing", "ed", "ly", "es", "s", "er", "or"};
  static int slens[] = {3, 2, 2, 2, 1, 2, 2};

  for (int i = 0; i < 7; i++) {
    if (has_suffix(word, len, suffixes[i])) {
      int new_len = len - slens[i];
      if (new_len < 3) return len;
      if (new_len >= 2 && word[new_len - 1] == word[new_len - 2] &&
          isalpha((unsigned char)word[new_len - 1])) {
        new_len--;
      }
      word[new_len] = '\0';
      return new_len;
    }
  }
  return len;
}

static int is_stopword(kanbudb_tokenizer_t* t, const char* word) {
  for (int i = 0; i < t->stopwords_count; i++) {
    if (strcmp(word, t->stopwords[i]) == 0) return 1;
  }
  return 0;
}

kanbudb_tokenizer_t* tokenizer_create(void) {
  kanbudb_tokenizer_t* t = kanbudb_calloc(1, sizeof(kanbudb_tokenizer_t));
  if (!t) return NULL;
  t->words_cap = INITIAL_WORDS_CAP;
  t->words = kanbudb_malloc(sizeof(char*) * (size_t)t->words_cap);
  if (!t->words) { kanbudb_free(t); return NULL; }
  return t;
}

void tokenizer_destroy(kanbudb_tokenizer_t* t) {
  if (!t) return;
  for (int i = 0; i < t->words_count; i++) {
    kanbudb_free(t->words[i]);
  }
  kanbudb_free(t->words);
  for (int i = 0; i < t->stopwords_count; i++) {
    kanbudb_free(t->stopwords[i]);
  }
  kanbudb_free(t->stopwords);
  kanbudb_free(t);
}

static int add_word(kanbudb_tokenizer_t* t, char* w) {
  if (t->words_count >= t->words_cap) {
    int new_cap = t->words_cap * 2;
    char** new_words = realloc(t->words, sizeof(char*) * (size_t)new_cap);
    if (!new_words) return -1;
    t->words = new_words;
    t->words_cap = new_cap;
  }
  t->words[t->words_count++] = w;
  return 0;
}

int tokenizer_tokenize(kanbudb_tokenizer_t* t,
                       const char* text, size_t text_len,
                       kanbudb_token_t* tokens, int max_tokens) {
  if (!t || !text || max_tokens <= 0) return 0;

  int count = 0;
  size_t i = 0;
  size_t pos = 0;

  while (i < text_len) {
    while (i < text_len && !is_token_char((unsigned char)text[i])) {
      i++;
    }
    if (i >= text_len) break;

    int j = 0;
    while (i < text_len && is_token_char((unsigned char)text[i]) && j < STEM_BUF_SIZE - 1) {
      t->stem_buf[j++] = (char)tolower((unsigned char)text[i++]);
    }
    while (i < text_len && is_token_char((unsigned char)text[i])) {
      i++;
    }
    t->stem_buf[j] = '\0';

    if (j == 0) continue;
    if (is_stopword(t, t->stem_buf)) continue;

    if (t->stem_enabled) {
      j = stem_english(t->stem_buf, j);
      t->stem_buf[j] = '\0';
    }

    if (j < 2) continue;

    char* w = dup_str(t->stem_buf, (size_t)j);
    if (!w) break;
    if (add_word(t, w) != 0) { kanbudb_free(w); break; }

    tokens[count].word = w;
    tokens[count].len = (size_t)j;
    tokens[count].pos = pos++;
    count++;

    if (count >= max_tokens) break;
  }

  return count;
}

void tokenizer_set_stemmer(kanbudb_tokenizer_t* t, int enabled) {
  if (t) t->stem_enabled = enabled;
}

void tokenizer_set_stopwords(kanbudb_tokenizer_t* t,
                             const char** words, int count) {
  if (!t) return;

  for (int i = 0; i < t->stopwords_count; i++) {
    kanbudb_free(t->stopwords[i]);
  }
  kanbudb_free(t->stopwords);

  t->stopwords = NULL;
  t->stopwords_count = 0;

  if (count > 0 && words) {
    t->stopwords = kanbudb_malloc(sizeof(char*) * (size_t)count);
    if (!t->stopwords) return;
    for (int i = 0; i < count; i++) {
      t->stopwords[i] = dup_str(words[i], strlen(words[i]));
    }
    t->stopwords_count = count;
  }
}
