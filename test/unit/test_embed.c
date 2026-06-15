#include "vector.h"
#include "db.h"
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <sys/stat.h>

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) do { \
  printf("  TEST: %s ... ", #name); \
  if (test_##name()) { printf("PASS\n"); tests_passed++; } \
  else { printf("FAIL\n"); tests_failed++; } \
} while(0)

#define ASSERT(cond, msg) do { \
  if (!(cond)) { printf("\n    ASSERT FAIL: %s\n", msg); return 0; } \
} while(0)

static void rmdir_recursive(const char* path)
{
    char buf[512];
    snprintf(buf, sizeof(buf), "%s.wal", path);      unlink(buf);
    snprintf(buf, sizeof(buf), "%s.vec/meta", path);  unlink(buf);
    snprintf(buf, sizeof(buf), "%s.vec/vectors", path); unlink(buf);
    snprintf(buf, sizeof(buf), "%s.vec/ids", path);   unlink(buf);
    snprintf(buf, sizeof(buf), "%s.vec/wal", path);   unlink(buf);
    snprintf(buf, sizeof(buf), "%s.vec", path);       rmdir(buf);
    snprintf(buf, sizeof(buf), "%s.ckpt.1", path);    unlink(buf);
    snprintf(buf, sizeof(buf), "%s.system", path);    unlink(buf);
    snprintf(buf, sizeof(buf), "%s.seq", path);       unlink(buf);
    rmdir(path);
}

/* ------------------------------------------------------------------ */
/*  Embedding standalone tests                                         */
/* ------------------------------------------------------------------ */

static int test_embed_create_destroy(void)
{
    kanbudb_embed_t* e = NULL;
    int rc = kanbudb_embed_create(128, 3, &e);
    ASSERT(rc == 0 && e != NULL, "create ok");
    ASSERT(kanbudb_embed_dimensions(e) == 128, "dimensions ok");
    kanbudb_embed_destroy(e);
    return 1;
}

static int test_embed_deterministic(void)
{
    kanbudb_embed_t* e = NULL;
    kanbudb_embed_create(64, 3, &e);

    float v1[64], v2[64];
    const char* text = "hello world";
    kanbudb_embed_text(e, text, strlen(text), v1);
    kanbudb_embed_text(e, text, strlen(text), v2);

    int same = 1;
    for (int i = 0; i < 64; i++) {
        if (fabsf(v1[i] - v2[i]) > 1e-6f) { same = 0; break; }
    }
    ASSERT(same, "same text produces same vector");

    kanbudb_embed_destroy(e);
    return 1;
}

static int test_embed_different_texts(void)
{
    kanbudb_embed_t* e = NULL;
    kanbudb_embed_create(128, 3, &e);

    float v1[128], v2[128];
    kanbudb_embed_text(e, "hello", 5, v1);
    kanbudb_embed_text(e, "goodbye", 7, v2);

    int same = 1;
    for (int i = 0; i < 128; i++) {
        if (fabsf(v1[i] - v2[i]) > 1e-6f) { same = 0; break; }
    }
    ASSERT(!same, "different texts produce different vectors");

    float dot = 0;
    for (int i = 0; i < 128; i++) dot += v1[i] * v2[i];
    ASSERT(dot < 1.0f + 1e-5f, "dot product bounded");

    kanbudb_embed_destroy(e);
    return 1;
}

static int test_embed_normalized(void)
{
    kanbudb_embed_t* e = NULL;
    kanbudb_embed_create(64, 3, &e);

    float v[64];
    kanbudb_embed_text(e, "test normalization", 18, v);

    float norm = 0;
    for (int i = 0; i < 64; i++) norm += v[i] * v[i];
    ASSERT(fabsf(norm - 1.0f) < 1e-5f, "vector is unit normalized");

    kanbudb_embed_destroy(e);
    return 1;
}

static int test_embed_short_text(void)
{
    kanbudb_embed_t* e = NULL;
    kanbudb_embed_create(32, 5, &e);

    float v[32];
    int rc = kanbudb_embed_text(e, "ab", 2, v);
    ASSERT(rc == 0, "short text ok");

    float norm = 0;
    for (int i = 0; i < 32; i++) norm += v[i] * v[i];
    ASSERT(norm > 0.0f, "non-zero vector for short text");

    kanbudb_embed_destroy(e);
    return 1;
}

static int test_embed_batch(void)
{
    kanbudb_embed_t* e = NULL;
    kanbudb_embed_create(32, 3, &e);

    const char* texts[] = { "alpha", "beta", "gamma" };
    size_t lens[] = { 5, 4, 5 };
    float batch[3 * 32];
    float single[32];

    kanbudb_embed_batch(e, texts, lens, 3, batch);
    kanbudb_embed_text(e, texts[0], lens[0], single);

    int same = 1;
    for (int i = 0; i < 32; i++) {
        if (fabsf(batch[i] - single[i]) > 1e-6f) { same = 0; break; }
    }
    ASSERT(same, "batch[0] matches single");

    kanbudb_embed_destroy(e);
    return 1;
}

/* ------------------------------------------------------------------ */
/*  DB-level vector + embedding tests                                  */
/* ------------------------------------------------------------------ */

static int test_db_vec_create_search(void)
{
    const char* path = "/tmp/kanbudb_test_embed_db";
    rmdir_recursive(path);

    db_t* db = NULL;
    db_config_t cfg = { 0 };
    int rc = db_open(path, &cfg, &db);
    ASSERT(rc == KANBUDB_OK, "db_open");

    db_vec_options_t opts = KANBUDB_VEC_OPTIONS_DEFAULT;
    opts.dimension = 64;
    rc = db_vec_create_index(db, &opts);
    ASSERT(rc == KANBUDB_OK, "vec_create_index");

    rc = db_vec_insert_text(db, 1, "the quick brown fox", 19);
    ASSERT(rc == KANBUDB_OK, "insert text 1");
    rc = db_vec_insert_text(db, 2, "jumps over the lazy dog", 22);
    ASSERT(rc == KANBUDB_OK, "insert text 2");
    rc = db_vec_insert_text(db, 3, "the cat sat on the mat", 21);
    ASSERT(rc == KANBUDB_OK, "insert text 3");

    ASSERT(db_vec_count(db) == 3, "count is 3");

    kanbudb_vec_result_t results[3];
    rc = db_vec_search_text(db, "quick fox", 9, 2, results);
    ASSERT(rc == 2, "search returns 2 results");
    ASSERT(results[0].distance >= 0.0f, "distance non-negative");

    db_close(db);
    rmdir_recursive(path);
    return 1;
}

static int test_db_vec_insert_vector(void)
{
    const char* path = "/tmp/kanbudb_test_embed_vec";
    rmdir_recursive(path);

    db_t* db = NULL;
    db_config_t cfg = { 0 };
    db_open(path, &cfg, &db);

    db_vec_options_t opts = KANBUDB_VEC_OPTIONS_DEFAULT;
    opts.dimension = 4;
    db_vec_create_index(db, &opts);

    float v[4] = { 1.0f, 0.0f, 0.0f, 0.0f };
    int rc = db_vec_insert_vector(db, 10, v);
    ASSERT(rc == KANBUDB_OK, "insert raw vector");

    float q[4] = { 0.9f, 0.1f, 0.0f, 0.0f };
    kanbudb_vec_result_t results[1];
    rc = db_vec_search(db, q, 1, results);
    ASSERT(rc == 1, "search raw vector");
    ASSERT(results[0].id == 10, "found correct id");

    db_close(db);
    rmdir_recursive(path);
    return 1;
}

static int test_db_vec_delete(void)
{
    const char* path = "/tmp/kanbudb_test_embed_del";
    rmdir_recursive(path);

    db_t* db = NULL;
    db_config_t cfg = { 0 };
    db_open(path, &cfg, &db);

    db_vec_options_t opts = KANBUDB_VEC_OPTIONS_DEFAULT;
    opts.dimension = 16;
    db_vec_create_index(db, &opts);

    db_vec_insert_text(db, 1, "apple", 5);
    db_vec_insert_text(db, 2, "banana", 6);
    ASSERT(db_vec_count(db) == 2, "count before delete");

    db_vec_delete(db, 1);
    ASSERT(db_vec_count(db) >= 1, "count after delete");

    kanbudb_vec_result_t results[10];
    int n = db_vec_search_text(db, "apple", 5, 10, results);
    int found = 0;
    for (int i = 0; i < n; i++) {
        if (results[i].id == 1) found = 1;
    }
    ASSERT(!found, "deleted id not in search results");

    db_close(db);
    rmdir_recursive(path);
    return 1;
}

static int test_db_vec_batch(void)
{
    const char* path = "/tmp/kanbudb_test_embed_batch";
    rmdir_recursive(path);

    db_t* db = NULL;
    db_config_t cfg = { 0 };
    db_open(path, &cfg, &db);

    db_vec_options_t opts = KANBUDB_VEC_OPTIONS_DEFAULT;
    opts.dimension = 32;
    db_vec_create_index(db, &opts);

    uint64_t ids[] = { 1, 2, 3 };
    const char* texts[] = { "hello", "world", "test" };
    size_t lens[] = { 5, 5, 4 };
    int rc = db_vec_insert_batch(db, 3, ids, texts, lens);
    ASSERT(rc == KANBUDB_OK, "batch insert");
    ASSERT(db_vec_count(db) == 3, "batch count");

    db_close(db);
    rmdir_recursive(path);
    return 1;
}

static int test_db_vec_hnsw(void)
{
    const char* path = "/tmp/kanbudb_test_embed_hnsw";
    rmdir_recursive(path);

    db_t* db = NULL;
    db_config_t cfg = { 0 };
    db_open(path, &cfg, &db);

    db_vec_options_t opts = KANBUDB_VEC_OPTIONS_DEFAULT;
    opts.dimension = 32;
    opts.enable_hnsw = 1;
    opts.hnsw_m = 8;
    opts.hnsw_ef_construction = 50;

    kanbudb_vec_params_t vparams = KANBUDB_VEC_PARAMS_DEFAULT;
    vparams.dimension = opts.dimension;
    vparams.algo = KANBUDB_VEC_ALGO_HNSW;
    vparams.M = opts.hnsw_m;
    vparams.ef_construction = opts.hnsw_ef_construction;
    vparams.initial_capacity = 256;

    kanbudb_vec_index_t* vidx = NULL;
    int rc = kanbudb_vec_create(path, &vparams, &vidx);
    ASSERT(rc == KANBUDB_VEC_OK, "hnsw create direct");

    for (int i = 0; i < 100; i++) {
        char buf[64];
        int len = snprintf(buf, sizeof(buf), "document number %d", i);
        kanbudb_embed_t* emb = NULL;
        kanbudb_embed_create(opts.dimension, 3, &emb);
        float vec[32];
        kanbudb_embed_text(emb, buf, (size_t)len, vec);
        rc = kanbudb_vec_insert(vidx, (uint64_t)i, vec);
        kanbudb_embed_destroy(emb);
        ASSERT(rc == KANBUDB_VEC_OK, "hnsw insert");
    }

    ASSERT(kanbudb_vec_count(vidx) == 100, "hnsw count");

    kanbudb_embed_t* qemb = NULL;
    kanbudb_embed_create(opts.dimension, 3, &qemb);
    float qvec[32];
    kanbudb_embed_text(qemb, "document number 50", 18, qvec);
    kanbudb_vec_result_t results[5];
    rc = kanbudb_vec_search(vidx, qvec, 5, results);
    ASSERT(rc >= 1, "hnsw search returns results");
    kanbudb_embed_destroy(qemb);

    kanbudb_vec_close(vidx);

    db_close(db);
    rmdir_recursive(path);
    return 1;
}

int main(void)
{
    printf("=== Embedding standalone tests ===\n");
    TEST(embed_create_destroy);
    TEST(embed_deterministic);
    TEST(embed_different_texts);
    TEST(embed_normalized);
    TEST(embed_short_text);
    TEST(embed_batch);

    printf("\n=== DB vector + embedding tests ===\n");
    TEST(db_vec_create_search);
    TEST(db_vec_insert_vector);
    TEST(db_vec_delete);
    TEST(db_vec_batch);
    TEST(db_vec_hnsw);

    printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
