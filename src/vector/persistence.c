#include "persistence.h"
#include "vector.h"
#include "vector_internal.h"
#include "distance.h"
#include "macros.h"
#include "hnsw.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>

/* ------------------------------------------------------------------ */
/*  WAL helpers                                                        */
/* ------------------------------------------------------------------ */

static int wal_fwrite_all(FILE* fp, const void* data, size_t size)
{
    return fwrite(data, 1, size, fp) == size ? KANBUDB_VEC_OK : KANBUDB_VEC_ERR_IO;
}

int vec_wal_open(kanbudb_vec_index_t* idx)
{
    if (!idx || !idx->path) return KANBUDB_VEC_ERR_INVAL;
    char buf[1024];
    snprintf(buf, sizeof(buf), "%s/wal", idx->path);
    idx->wal_fp = fopen(buf, "ab");
    if (!idx->wal_fp) return KANBUDB_VEC_ERR_IO;
    fseek(idx->wal_fp, 0, SEEK_END);
    idx->wal_bytes = (uint64_t)ftell(idx->wal_fp);
    return KANBUDB_VEC_OK;
}

void vec_wal_close(kanbudb_vec_index_t* idx)
{
    if (idx->wal_fp) {
        fclose(idx->wal_fp);
        idx->wal_fp = NULL;
    }
    idx->wal_bytes = 0;
}

int vec_wal_append_insert(kanbudb_vec_index_t* idx, uint64_t id,
                          const float* vector)
{
    if (!idx || !idx->wal_fp || !vector) return KANBUDB_VEC_ERR_INVAL;

    uint32_t op  = WAL_OP_INSERT;
    uint32_t dim = idx->params.dimension;

    uint64_t ts = (uint64_t)time(NULL);
    if (wal_fwrite_all(idx->wal_fp, &op, sizeof(op)) != KANBUDB_VEC_OK)
        return KANBUDB_VEC_ERR_IO;
    if (wal_fwrite_all(idx->wal_fp, &id, sizeof(id)) != KANBUDB_VEC_OK)
        return KANBUDB_VEC_ERR_IO;
    if (wal_fwrite_all(idx->wal_fp, &dim, sizeof(dim)) != KANBUDB_VEC_OK)
        return KANBUDB_VEC_ERR_IO;
    if (wal_fwrite_all(idx->wal_fp, vector, (size_t)dim * sizeof(float)) != KANBUDB_VEC_OK)
        return KANBUDB_VEC_ERR_IO;
    if (wal_fwrite_all(idx->wal_fp, &ts, sizeof(ts)) != KANBUDB_VEC_OK)
        return KANBUDB_VEC_ERR_IO;

    fflush(idx->wal_fp);
    idx->wal_bytes += sizeof(op) + sizeof(id) + sizeof(dim)
                      + (size_t)dim * sizeof(float) + sizeof(ts);
    return KANBUDB_VEC_OK;
}

int vec_wal_append_delete(kanbudb_vec_index_t* idx, uint64_t id)
{
    if (!idx || !idx->wal_fp) return KANBUDB_VEC_ERR_INVAL;

    uint32_t op = WAL_OP_DELETE;
    if (wal_fwrite_all(idx->wal_fp, &op, sizeof(op)) != KANBUDB_VEC_OK)
        return KANBUDB_VEC_ERR_IO;
    if (wal_fwrite_all(idx->wal_fp, &id, sizeof(id)) != KANBUDB_VEC_OK)
        return KANBUDB_VEC_ERR_IO;

    fflush(idx->wal_fp);
    idx->wal_bytes += sizeof(op) + sizeof(id);
    return KANBUDB_VEC_OK;
}

/* ------------------------------------------------------------------ */
/*  WAL replay                                                         */
/* ------------------------------------------------------------------ */

static int vec_wal_replay(kanbudb_vec_index_t* idx)
{
    if (!idx || !idx->path) return KANBUDB_VEC_ERR_INVAL;

    char buf[1024];
    snprintf(buf, sizeof(buf), "%s/wal", idx->path);
    FILE* fp = fopen(buf, "rb");
    if (!fp) return KANBUDB_VEC_OK;

    uint32_t dim = idx->params.dimension;
    int rc = KANBUDB_VEC_OK;

    for (;;) {
        uint32_t op;
        if (fread(&op, sizeof(op), 1, fp) != 1) break;

        if (op == WAL_OP_INSERT) {
            uint64_t id;
            uint32_t rec_dim;
            uint64_t ts;
            if (fread(&id, sizeof(id), 1, fp) != 1) break;
            if (fread(&rec_dim, sizeof(rec_dim), 1, fp) != 1) break;

            float* vector = malloc((size_t)rec_dim * sizeof(float));
            if (!vector) { rc = KANBUDB_VEC_ERR_OOM; break; }
            if (fread(vector, sizeof(float), rec_dim, fp) != rec_dim) {
                free(vector); break;
            }
            if (fread(&ts, sizeof(ts), 1, fp) != 1) {
                free(vector); break;
            }

            if (idx->algo_type == 1) {
                hnsw_insert((hnsw_index_t*)idx->algo_data, id, vector);
            } else {
                int existing = -1;
                for (uint64_t i = 0; i < idx->flat_count; i++) {
                    if (idx->flat_ids[i] == id && !idx->flat_deleted[i]) {
                        existing = (int)i; break;
                    }
                }
                if (existing >= 0) {
                    memcpy(idx->flat_vectors + (size_t)existing * dim,
                           vector, (size_t)dim * sizeof(float));
                    idx->flat_deleted[existing] = 0;
                } else {
                    if (idx->flat_count >= idx->flat_capacity) {
                        uint64_t new_cap = idx->flat_capacity
                                           ? idx->flat_capacity * 2 : 1024;
                        float* v = realloc(idx->flat_vectors,
                                           (size_t)new_cap * dim * sizeof(float));
                        uint64_t* ids = realloc(idx->flat_ids,
                                                (size_t)new_cap * sizeof(uint64_t));
                        uint8_t* del = realloc(idx->flat_deleted,
                                               (size_t)new_cap);
                        if (!v || !ids || !del) {
                            free(v); free(ids); free(del);
                            free(vector);
                            rc = KANBUDB_VEC_ERR_OOM; goto replay_done;
                        }
                        memset(del + idx->flat_capacity, 0,
                               (size_t)(new_cap - idx->flat_capacity));
                        idx->flat_vectors  = v;
                        idx->flat_ids      = ids;
                        idx->flat_deleted  = del;
                        idx->flat_capacity = new_cap;
                    }
                    memcpy(idx->flat_vectors + (size_t)idx->flat_count * dim,
                           vector, (size_t)dim * sizeof(float));
                    idx->flat_ids[idx->flat_count] = id;
                    idx->flat_deleted[idx->flat_count] = 0;
                    idx->flat_count++;
                }
            }
            free(vector);
        } else if (op == WAL_OP_DELETE) {
            uint64_t id;
            if (fread(&id, sizeof(id), 1, fp) != 1) break;

            if (idx->algo_type == 1) {
                hnsw_delete((hnsw_index_t*)idx->algo_data, id);
            } else {
                for (uint64_t i = 0; i < idx->flat_count; i++) {
                    if (idx->flat_ids[i] == id && !idx->flat_deleted[i]) {
                        idx->flat_deleted[i] = 1; break;
                    }
                }
            }
        } else if (op == WAL_OP_CHECKPOINT) {
            /* Data files are up-to-date before this point; skip. */
        } else {
            break; /* unknown or corrupt op */
        }
    }

replay_done:
    fclose(fp);
    return rc;
}

/* ------------------------------------------------------------------ */
/*  Checkpoint                                                         */
/* ------------------------------------------------------------------ */

int vec_checkpoint(kanbudb_vec_index_t* idx)
{
    if (!idx || !idx->path) return KANBUDB_VEC_ERR_INVAL;

    int rc = vec_persist_write(idx->path, idx);
    if (rc != KANBUDB_VEC_OK) return rc;

    /* Truncate WAL */
    char buf[1024];
    snprintf(buf, sizeof(buf), "%s/wal", idx->path);
    if (idx->wal_fp) {
        fclose(idx->wal_fp);
        idx->wal_fp = NULL;
    }
    idx->wal_bytes = 0;
    idx->wal_fp = fopen(buf, "ab");
    if (!idx->wal_fp) return KANBUDB_VEC_ERR_IO;

    idx->dirty = 0;
    return KANBUDB_VEC_OK;
}

/* ------------------------------------------------------------------ */
/*  Persist write helpers                                              */
/* ------------------------------------------------------------------ */

static int write_meta_vectors_ids(const char* path,
                                  uint32_t dim,
                                  kanbudb_vec_metric_t metric,
                                  kanbudb_vec_algo_t algo,
                                  uint64_t count,
                                  const float* vectors,
                                  const uint64_t* ids,
                                  uint32_t M,
                                  uint32_t ef_construction,
                                  uint32_t ef_search)
{
    char buf[1024];

    /* Meta */
    snprintf(buf, sizeof(buf), "%s/meta", path);
    FILE* f = fopen(buf, "wb");
    if (!f) return KANBUDB_VEC_ERR_IO;

    vec_file_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic           = KANBUDB_VEC_MAGIC;
    hdr.version         = KANBUDB_VEC_VERSION;
    hdr.dimension       = dim;
    hdr.metric          = (uint32_t)metric;
    hdr.count           = count;
    hdr.capacity        = count;
    hdr.algo            = (uint32_t)algo;
    hdr.M               = M;
    hdr.ef_construction = ef_construction;
    hdr.ef_search       = ef_search;

    if (fwrite(&hdr, sizeof(hdr), 1, f) != 1) { fclose(f); return KANBUDB_VEC_ERR_IO; }
    fclose(f);

    /* Vectors */
    snprintf(buf, sizeof(buf), "%s/vectors", path);
    f = fopen(buf, "wb");
    if (!f) return KANBUDB_VEC_ERR_IO;
    size_t vbytes = (size_t)count * dim * sizeof(float);
    if (vbytes > 0 && fwrite(vectors, 1, vbytes, f) != vbytes) {
        fclose(f); return KANBUDB_VEC_ERR_IO;
    }
    fclose(f);

    /* IDs */
    snprintf(buf, sizeof(buf), "%s/ids", path);
    f = fopen(buf, "wb");
    if (!f) return KANBUDB_VEC_ERR_IO;
    size_t ibytes = (size_t)count * sizeof(uint64_t);
    if (ibytes > 0 && fwrite(ids, 1, ibytes, f) != ibytes) {
        fclose(f); return KANBUDB_VEC_ERR_IO;
    }
    fclose(f);

    return KANBUDB_VEC_OK;
}

/* ------------------------------------------------------------------ */
/*  vec_persist_write — FLAT                                           */
/* ------------------------------------------------------------------ */

static int persist_write_flat(const char* path, const kanbudb_vec_index_t* idx)
{
    uint64_t active = 0;
    for (uint64_t i = 0; i < idx->flat_count; i++)
        if (!idx->flat_deleted[i]) active++;

    if (active == 0)
        return write_meta_vectors_ids(path, idx->params.dimension,
                                      idx->params.metric,
                                      KANBUDB_VEC_ALGO_FLAT,
                                      0, NULL, NULL, 0, 0, 0);

    float* vectors = malloc((size_t)active * idx->params.dimension * sizeof(float));
    uint64_t* ids  = malloc((size_t)active * sizeof(uint64_t));
    if (!vectors || !ids) { free(vectors); free(ids); return KANBUDB_VEC_ERR_OOM; }

    uint64_t pos = 0;
    for (uint64_t i = 0; i < idx->flat_count; i++) {
        if (idx->flat_deleted[i]) continue;
        memcpy(vectors + (size_t)pos * idx->params.dimension,
               idx->flat_vectors + (size_t)i * idx->params.dimension,
               (size_t)idx->params.dimension * sizeof(float));
        ids[pos++] = idx->flat_ids[i];
    }

    int rc = write_meta_vectors_ids(path, idx->params.dimension,
                                    idx->params.metric,
                                    KANBUDB_VEC_ALGO_FLAT,
                                    active, vectors, ids,
                                    0, 0, 0);
    free(vectors);
    free(ids);
    return rc;
}

/* ------------------------------------------------------------------ */
/*  vec_persist_write — HNSW                                           */
/* ------------------------------------------------------------------ */

static int persist_write_hnsw(const char* path, const kanbudb_vec_index_t* idx)
{
    hnsw_index_t* hnsw = (hnsw_index_t*)idx->algo_data;
    if (!hnsw) return KANBUDB_VEC_ERR_INVAL;

    uint64_t active = 0;
    for (int i = 0; i < hnsw->element_count; i++)
        if (!hnsw->nodes[i].deleted) active++;

    if (active == 0)
        return write_meta_vectors_ids(path, idx->params.dimension,
                                      idx->params.metric,
                                      KANBUDB_VEC_ALGO_HNSW,
                                      0, NULL, NULL,
                                      (uint32_t)hnsw->M,
                                      (uint32_t)hnsw->ef_construction,
                                      (uint32_t)hnsw->ef_search);

    float* vectors = malloc((size_t)active * idx->params.dimension * sizeof(float));
    uint64_t* ids  = malloc((size_t)active * sizeof(uint64_t));
    if (!vectors || !ids) { free(vectors); free(ids); return KANBUDB_VEC_ERR_OOM; }

    uint64_t pos = 0;
    for (int i = 0; i < hnsw->element_count; i++) {
        if (hnsw->nodes[i].deleted) continue;
        memcpy(vectors + (size_t)pos * idx->params.dimension,
               hnsw->nodes[i].vector,
               (size_t)idx->params.dimension * sizeof(float));
        ids[pos++] = hnsw->nodes[i].id;
    }

    int rc = write_meta_vectors_ids(path, idx->params.dimension,
                                    idx->params.metric,
                                    KANBUDB_VEC_ALGO_HNSW,
                                    active, vectors, ids,
                                    (uint32_t)hnsw->M,
                                    (uint32_t)hnsw->ef_construction,
                                    (uint32_t)hnsw->ef_search);
    free(vectors);
    free(ids);
    return rc;
}

int vec_persist_write(const char* path, const kanbudb_vec_index_t* idx)
{
    if (!path || !idx) return KANBUDB_VEC_ERR_INVAL;

    if (idx->algo_type == 1)
        return persist_write_hnsw(path, idx);
    return persist_write_flat(path, idx);
}

/* ------------------------------------------------------------------ */
/*  vec_persist_read                                                   */
/* ------------------------------------------------------------------ */

int vec_persist_read(const char* path, kanbudb_vec_index_t** out)
{
    if (!path || !out) return KANBUDB_VEC_ERR_INVAL;

    char buf[1024];
    snprintf(buf, sizeof(buf), "%s/meta", path);
    FILE* f = fopen(buf, "rb");
    if (!f) return KANBUDB_VEC_ERR_NOTFOUND;

    /* Read v1 fields common to all versions */
    uint32_t magic, version, dim_u32, metric_u32;
    uint64_t count, capacity;
    if (fread(&magic,   sizeof(magic),   1, f) != 1) { fclose(f); return KANBUDB_VEC_ERR_CORRUPT; }
    if (fread(&version, sizeof(version), 1, f) != 1) { fclose(f); return KANBUDB_VEC_ERR_CORRUPT; }
    if (fread(&dim_u32, sizeof(dim_u32), 1, f) != 1) { fclose(f); return KANBUDB_VEC_ERR_CORRUPT; }
    if (fread(&metric_u32, sizeof(metric_u32), 1, f) != 1) { fclose(f); return KANBUDB_VEC_ERR_CORRUPT; }
    if (fread(&count,   sizeof(count),   1, f) != 1) { fclose(f); return KANBUDB_VEC_ERR_CORRUPT; }
    if (fread(&capacity, sizeof(capacity), 1, f) != 1) { fclose(f); return KANBUDB_VEC_ERR_CORRUPT; }

    if (magic != KANBUDB_VEC_MAGIC) { fclose(f); return KANBUDB_VEC_ERR_CORRUPT; }

    uint32_t algo            = KANBUDB_VEC_ALGO_FLAT;
    uint32_t M               = 16;
    uint32_t ef_construction = 200;
    uint32_t ef_search       = 50;

    if (version >= 2) {
        if (fread(&algo,            sizeof(algo),            1, f) != 1) { fclose(f); return KANBUDB_VEC_ERR_CORRUPT; }
        if (fread(&M,               sizeof(M),               1, f) != 1) { fclose(f); return KANBUDB_VEC_ERR_CORRUPT; }
        if (fread(&ef_construction, sizeof(ef_construction), 1, f) != 1) { fclose(f); return KANBUDB_VEC_ERR_CORRUPT; }
        if (fread(&ef_search,       sizeof(ef_search),       1, f) != 1) { fclose(f); return KANBUDB_VEC_ERR_CORRUPT; }
    }
    fclose(f);

    /* Allocate index */
    kanbudb_vec_params_t params;
    params.metric             = (kanbudb_vec_metric_t)metric_u32;
    params.dimension          = dim_u32;
    params.initial_capacity   = (uint32_t)(count > 0 ? count : 1024);
    params.enable_persistence = 1;
    params.algo               = (kanbudb_vec_algo_t)algo;
    params.M                  = M;
    params.ef_construction    = ef_construction;
    params.ef_search          = ef_search;

    struct kanbudb_vec_index* idx = calloc(1, sizeof(*idx));
    if (!idx) return KANBUDB_VEC_ERR_OOM;

    idx->params    = params;
    idx->dist_func = vec_get_dist_func(params.metric);
    idx->path      = strdup(path);
    if (!idx->path) { free(idx); return KANBUDB_VEC_ERR_OOM; }

    pthread_rwlock_init(&idx->rwlock, NULL);

    int rc = KANBUDB_VEC_OK;

    if (algo == KANBUDB_VEC_ALGO_HNSW) {
        int max_elems = (int)(count > 0 ? count : 1024);
        rc = hnsw_create((int)dim_u32, (int)M, (int)ef_construction,
                         (int)ef_search, max_elems, params.metric,
                         (hnsw_index_t**)&idx->algo_data);
        if (rc != 0) {
            free(idx->path); free(idx);
            return (rc == KANBUDB_ERR_OOM) ? KANBUDB_VEC_ERR_OOM : rc;
        }
        idx->algo_type = 1;

        if (count > 0) {
            float* vectors = malloc((size_t)count * dim_u32 * sizeof(float));
            uint64_t* ids  = malloc((size_t)count * sizeof(uint64_t));
            if (!vectors || !ids) {
                free(vectors); free(ids); free(idx->path);
                hnsw_destroy((hnsw_index_t*)idx->algo_data);
                free(idx); return KANBUDB_VEC_ERR_OOM;
            }

            snprintf(buf, sizeof(buf), "%s/vectors", path);
            f = fopen(buf, "rb");
            if (!f) { free(vectors); free(ids); free(idx->path); hnsw_destroy((hnsw_index_t*)idx->algo_data); free(idx); return KANBUDB_VEC_ERR_IO; }
            size_t vbytes = (size_t)count * dim_u32 * sizeof(float);
            if (vbytes > 0 && fread(vectors, 1, vbytes, f) != vbytes) {
                fclose(f); free(vectors); free(ids); free(idx->path); hnsw_destroy((hnsw_index_t*)idx->algo_data); free(idx); return KANBUDB_VEC_ERR_CORRUPT;
            }
            fclose(f);

            snprintf(buf, sizeof(buf), "%s/ids", path);
            f = fopen(buf, "rb");
            if (!f) { free(vectors); free(ids); free(idx->path); hnsw_destroy((hnsw_index_t*)idx->algo_data); free(idx); return KANBUDB_VEC_ERR_IO; }
            size_t ibytes = (size_t)count * sizeof(uint64_t);
            if (ibytes > 0 && fread(ids, 1, ibytes, f) != ibytes) {
                fclose(f); free(vectors); free(ids); free(idx->path); hnsw_destroy((hnsw_index_t*)idx->algo_data); free(idx); return KANBUDB_VEC_ERR_CORRUPT;
            }
            fclose(f);

            for (uint64_t i = 0; i < count; i++) {
                rc = hnsw_insert((hnsw_index_t*)idx->algo_data, ids[i],
                                 vectors + (size_t)i * dim_u32);
                if (rc != 0) break;
            }

            free(vectors);
            free(ids);
        }
    } else {
        idx->algo_type = 0;
        uint64_t cap = count > 0 ? count + (count / 2) : 1024;
        if (cap < 1024) cap = 1024;

        idx->flat_count    = count;
        idx->flat_capacity = cap;

        idx->flat_vectors = calloc((size_t)cap * dim_u32, sizeof(float));
        idx->flat_ids     = calloc((size_t)cap, sizeof(uint64_t));
        idx->flat_deleted = calloc((size_t)cap, 1);
        if (!idx->flat_vectors || !idx->flat_ids || !idx->flat_deleted) {
            free(idx->flat_vectors); free(idx->flat_ids); free(idx->flat_deleted);
            free(idx->path); free(idx); return KANBUDB_VEC_ERR_OOM;
        }

        if (count > 0) {
            snprintf(buf, sizeof(buf), "%s/vectors", path);
            f = fopen(buf, "rb");
            if (!f) { free(idx->flat_vectors); free(idx->flat_ids); free(idx->flat_deleted); free(idx->path); free(idx); return KANBUDB_VEC_ERR_IO; }
            size_t vbytes = (size_t)count * dim_u32 * sizeof(float);
            if (vbytes > 0 && fread(idx->flat_vectors, 1, vbytes, f) != vbytes) {
                fclose(f); free(idx->flat_vectors); free(idx->flat_ids); free(idx->flat_deleted); free(idx->path); free(idx); return KANBUDB_VEC_ERR_CORRUPT;
            }
            fclose(f);

            snprintf(buf, sizeof(buf), "%s/ids", path);
            f = fopen(buf, "rb");
            if (!f) { free(idx->flat_vectors); free(idx->flat_ids); free(idx->flat_deleted); free(idx->path); free(idx); return KANBUDB_VEC_ERR_IO; }
            size_t ibytes = (size_t)count * sizeof(uint64_t);
            if (ibytes > 0 && fread(idx->flat_ids, 1, ibytes, f) != ibytes) {
                fclose(f); free(idx->flat_vectors); free(idx->flat_ids); free(idx->flat_deleted); free(idx->path); free(idx); return KANBUDB_VEC_ERR_CORRUPT;
            }
            fclose(f);
        }
    }

    if (rc != KANBUDB_VEC_OK) {
        kanbudb_vec_close(idx);
        return rc;
    }

    /* Open WAL and replay */
    rc = vec_wal_open(idx);
    if (rc == KANBUDB_VEC_OK)
        rc = vec_wal_replay(idx);

    if (rc == KANBUDB_VEC_OK)
        idx->dirty = 0;

    *out = idx;
    return rc;
}
