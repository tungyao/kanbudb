#ifndef KANBUDB_QUANTIZE_H
#define KANBUDB_QUANTIZE_H

#include <stddef.h>
#include <stdint.h>
#include "db.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct kanbudb_quantizer kanbudb_quantizer_t;

int  kanbudb_quant_create(const kanbudb_quant_params_t* params,
                          kanbudb_quantizer_t** out);
void kanbudb_quant_destroy(kanbudb_quantizer_t* q);

int kanbudb_quant_train(kanbudb_quantizer_t* q,
                        const float* vectors, uint32_t count);

int kanbudb_quant_encode(const kanbudb_quantizer_t* q,
                         const float* vector,
                         uint8_t* out_code, size_t* out_code_len);

int kanbudb_quant_decode(const kanbudb_quantizer_t* q,
                         const uint8_t* code, size_t code_len,
                         float* out_vector);

float kanbudb_quant_distance(const kanbudb_quantizer_t* q,
                             const uint8_t* code_a, size_t code_len_a,
                             const uint8_t* code_b, size_t code_len_b);

uint32_t kanbudb_quant_code_size(const kanbudb_quantizer_t* q);

#ifdef __cplusplus
}
#endif

#endif /* KANBUDB_QUANTIZE_H */
