#include "bloom.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) do { \
  printf("  TEST: %s ... ", #name); \
  if (test_##name()) { printf("PASS\n"); tests_passed++; } \
  else { printf("FAIL\n"); tests_failed++; } \
} while(0)

static int test_fpr_within_expected(void) {
    uint32_t num_keys = 10000;
    uint8_t bits_per_key = 10;
    kanbudb_bloom_t* bf = kanbudb_bloom_create(num_keys, bits_per_key);
    if (!bf) return 0;

    for (uint32_t i = 0; i < num_keys; i++) {
        char key[32];
        snprintf(key, sizeof(key), "key_%u", i);
        kanbudb_bloom_insert(bf, key, strlen(key));
    }

    uint32_t false_positives = 0;
    uint32_t trials = 10000;
    for (uint32_t i = 0; i < trials; i++) {
        char key[32];
        snprintf(key, sizeof(key), "nokey_%u", i + 1000000);
        if (kanbudb_bloom_maybe(bf, key, strlen(key)))
            false_positives++;
    }

    kanbudb_bloom_destroy(bf);

    double fpr = (double)false_positives / (double)trials;
    if (fpr > 0.05) {
        fprintf(stderr, "  FPR too high: %.4f (expected < 0.05)\n", fpr);
        return 0;
    }
    return 1;
}

static int test_no_false_negatives(void) {
    uint32_t num_keys = 5000;
    kanbudb_bloom_t* bf = kanbudb_bloom_create(num_keys, 14);
    if (!bf) return 0;

    for (uint32_t i = 0; i < num_keys; i++) {
        char key[32];
        snprintf(key, sizeof(key), "key_%u", i);
        kanbudb_bloom_insert(bf, key, strlen(key));
    }

    for (uint32_t i = 0; i < num_keys; i++) {
        char key[32];
        snprintf(key, sizeof(key), "key_%u", i);
        if (!kanbudb_bloom_maybe(bf, key, strlen(key))) {
            fprintf(stderr, "  false negative for key '%s'\n", key);
            kanbudb_bloom_destroy(bf);
            return 0;
        }
    }

    kanbudb_bloom_destroy(bf);
    return 1;
}

static int test_serialize_roundtrip(void) {
    uint32_t num_keys = 1000;
    kanbudb_bloom_t* bf = kanbudb_bloom_create(num_keys, 10);
    if (!bf) return 0;

    for (uint32_t i = 0; i < num_keys; i++) {
        char key[32];
        snprintf(key, sizeof(key), "skey_%u", i);
        kanbudb_bloom_insert(bf, key, strlen(key));
    }

    uint32_t ser_size = kanbudb_bloom_serialized_size(bf);
    uint8_t* buf = (uint8_t*)malloc(ser_size);
    if (!buf) { kanbudb_bloom_destroy(bf); return 0; }

    kanbudb_bloom_serialize(bf, buf);
    kanbudb_bloom_destroy(bf);

    kanbudb_bloom_t* loaded = kanbudb_bloom_load(buf, ser_size);
    if (!loaded) { free(buf); return 0; }

    for (uint32_t i = 0; i < num_keys; i++) {
        char key[32];
        snprintf(key, sizeof(key), "skey_%u", i);
        if (!kanbudb_bloom_maybe(loaded, key, strlen(key))) {
            fprintf(stderr, "  false negative after serialization for '%s'\n", key);
            kanbudb_bloom_unload(loaded); free(buf); return 0;
        }
    }

    uint32_t fp = 0;
    for (uint32_t i = 0; i < 1000; i++) {
        char key[32];
        snprintf(key, sizeof(key), "fake_%u", i + 100000);
        if (kanbudb_bloom_maybe(loaded, key, strlen(key)))
            fp++;
    }

    kanbudb_bloom_unload(loaded);
    free(buf);

    double fpr = (double)fp / 1000.0;
    if (fpr > 0.05) {
        fprintf(stderr, "  FPR too high after load: %.4f\n", fpr);
        return 0;
    }
    return 1;
}

static int test_reset(void) {
    kanbudb_bloom_t* bf = kanbudb_bloom_create(100, 10);
    if (!bf) return 0;

    uint32_t inserted = 50;
    for (uint32_t i = 0; i < inserted; i++) {
        char key[16];
        snprintf(key, sizeof(key), "k%u", i);
        kanbudb_bloom_insert(bf, key, strlen(key));
    }

    for (uint32_t i = 0; i < inserted; i++) {
        char key[16];
        snprintf(key, sizeof(key), "k%u", i);
        if (!kanbudb_bloom_maybe(bf, key, strlen(key))) {
            kanbudb_bloom_destroy(bf); return 0;
        }
    }

    kanbudb_bloom_reset(bf);

    for (uint32_t i = 0; i < inserted; i++) {
        char key[16];
        snprintf(key, sizeof(key), "k%u", i);
        if (kanbudb_bloom_maybe(bf, key, strlen(key))) {
            kanbudb_bloom_destroy(bf); return 0;
        }
    }

    kanbudb_bloom_destroy(bf);
    return 1;
}

static int test_edge_cases(void) {
    if (kanbudb_bloom_create(0, 10) != NULL) return 0;
    if (kanbudb_bloom_create(100, 0) != NULL) return 0;
    if (kanbudb_bloom_load(NULL, 0) != NULL) return 0;

    {
        uint8_t small[4] = {0};
        if (kanbudb_bloom_load(small, 4) != NULL) return 0;
    }

    kanbudb_bloom_destroy(NULL);
    kanbudb_bloom_unload(NULL);
    kanbudb_bloom_reset(NULL);
    if (kanbudb_bloom_maybe(NULL, "x", 1) != 0) return 0;
    if (kanbudb_bloom_serialized_size(NULL) != 0) return 0;

    {
        kanbudb_bloom_t* bf = kanbudb_bloom_create(1, 64);
        if (!bf) return 0;
        kanbudb_bloom_insert(bf, "a", 1);
        if (!kanbudb_bloom_maybe(bf, "a", 1)) {
            kanbudb_bloom_destroy(bf); return 0;
        }
        kanbudb_bloom_insert(bf, "", 0);
        kanbudb_bloom_destroy(bf);
    }

    return 1;
}

static int test_large_key(void) {
    kanbudb_bloom_t* bf = kanbudb_bloom_create(100, 10);
    if (!bf) return 0;

    uint8_t* large = (uint8_t*)malloc(10000);
    if (!large) { kanbudb_bloom_destroy(bf); return 0; }
    memset(large, 0xAB, 10000);

    kanbudb_bloom_insert(bf, large, 10000);
    if (!kanbudb_bloom_maybe(bf, large, 10000)) {
        free(large); kanbudb_bloom_destroy(bf); return 0;
    }

    uint8_t different[10000];
    memset(different, 0xCD, 10000);
    if (kanbudb_bloom_maybe(bf, different, 10000)) {
        free(large); kanbudb_bloom_destroy(bf);
        fprintf(stderr, "  unexpected false positive for different large key\n");
        return 0;
    }

    free(large);
    kanbudb_bloom_destroy(bf);
    return 1;
}

static int test_bloom_many_insertions(void) {
    uint32_t n = 100000;
    kanbudb_bloom_t* bf = kanbudb_bloom_create(n, 10);
    if (!bf) return 0;

    for (uint32_t i = 0; i < n; i++) {
        char key[32];
        snprintf(key, sizeof(key), "bulk_key_%u", i);
        kanbudb_bloom_insert(bf, key, strlen(key));
    }

    for (uint32_t i = 0; i < n; i++) {
        char key[32];
        snprintf(key, sizeof(key), "bulk_key_%u", i);
        if (!kanbudb_bloom_maybe(bf, key, strlen(key))) {
            fprintf(stderr, "  false negative at idx %u\n", i);
            kanbudb_bloom_destroy(bf); return 0;
        }
    }

    uint32_t fp = 0;
    uint32_t trials = 5000;
    for (uint32_t i = 0; i < trials; i++) {
        char key[32];
        snprintf(key, sizeof(key), "absent_%u", i + 200000);
        if (kanbudb_bloom_maybe(bf, key, strlen(key)))
            fp++;
    }

    kanbudb_bloom_destroy(bf);

    double fpr = (double)fp / (double)trials;
    if (fpr > 0.05) {
        fprintf(stderr, "  FPR too high: %.4f\n", fpr);
        return 0;
    }
    return 1;
}

int main(void) {
    printf("bloom filter rigorous tests:\n");
    TEST(fpr_within_expected);
    TEST(no_false_negatives);
    TEST(serialize_roundtrip);
    TEST(reset);
    TEST(edge_cases);
    TEST(large_key);
    TEST(bloom_many_insertions);
    printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
