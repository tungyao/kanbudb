#ifndef KANBUDB_QUANTIZE_INTERNAL_H
#define KANBUDB_QUANTIZE_INTERNAL_H

#include "quantize.h"

struct kanbudb_quantizer {
    kanbudb_quant_params_t params;
    float* sq_min;
    float* sq_range;
    int    sq_trained;
    float** pq_codebooks;
    uint32_t pq_sub_dim;
    int     pq_trained;
};

#endif /* KANBUDB_QUANTIZE_INTERNAL_H */
