#ifndef KANBUDB_EMBEDDING_H
#define KANBUDB_EMBEDDING_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct kanbudb_embed kanbudb_embed_t;

int  kanbudb_embed_create(uint32_t dimensions, uint32_t ngram_size,
                          kanbudb_embed_t** out);
void kanbudb_embed_destroy(kanbudb_embed_t* embed);
int  kanbudb_embed_text(const kanbudb_embed_t* embed,
                        const char* text, size_t text_len,
                        float* out_vector);
int  kanbudb_embed_batch(const kanbudb_embed_t* embed,
                         const char** texts, const size_t* text_lens,
                         uint32_t count, float* out_vectors);
uint32_t kanbudb_embed_dimensions(const kanbudb_embed_t* embed);

#ifdef __cplusplus
}
#endif

#endif /* KANBUDB_EMBEDDING_H */
