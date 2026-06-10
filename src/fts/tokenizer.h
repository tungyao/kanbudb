#ifndef KANBUDB_TOKENIZER_H
#define KANBUDB_TOKENIZER_H

#include "macros.h"

typedef struct {
  const char* word;
  size_t      len;
  size_t      pos;
} kanbudb_token_t;

typedef struct kanbudb_tokenizer kanbudb_tokenizer_t;

kanbudb_tokenizer_t* tokenizer_create(void);
void                tokenizer_destroy(kanbudb_tokenizer_t* t);
int                 tokenizer_tokenize(kanbudb_tokenizer_t* t,
                                       const char* text, size_t text_len,
                                       kanbudb_token_t* tokens, int max_tokens);
void tokenizer_set_stemmer(kanbudb_tokenizer_t* t, int enabled);
void tokenizer_set_stopwords(kanbudb_tokenizer_t* t,
                             const char** words, int count);

#endif /* KANBUDB_TOKENIZER_H */
