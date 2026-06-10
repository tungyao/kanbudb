#include "db.h"
#include "macros.h"
#include "sstable.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>

#define BENCH_DB "/tmp/kanbudb_bench_persist"
#define VALUE_SIZE 128
#define VALUE_STR "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789!@#$%^&*()_+-=[]{}|;:',.<>?/`~ "

static double now_sec(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void remove_file(const char* path) { unlink(path); }

static void cleanup_all(void) {
  char p[512];
  snprintf(p, sizeof(p), "%s.wal", BENCH_DB); remove_file(p);
  snprintf(p, sizeof(p), "%s.system", BENCH_DB); remove_file(p);
  snprintf(p, sizeof(p), "%s.seq", BENCH_DB); remove_file(p);
  for (int i = 0; i < 200; i++) {
    snprintf(p, sizeof(p), "%s.sst.0.%d", BENCH_DB, i); remove_file(p);
    snprintf(p, sizeof(p), "%s.sst.1.%d", BENCH_DB, i); remove_file(p);
    snprintf(p, sizeof(p), "%s.ckpt.%d", BENCH_DB, i); remove_file(p);
    snprintf(p, sizeof(p), "%s.ckpt.%d.tmp", BENCH_DB, i); remove_file(p);
  }
}

/* Build a deterministic value of exactly VALUE_SIZE bytes from an index */
static void make_value(char* buf, int n) {
  for (int i = 0; i < VALUE_SIZE; i++) {
    buf[i] = VALUE_STR[(n + i) % (sizeof(VALUE_STR) - 1)];
  }
}

/* Callback for file counting */
static int file_count_cb(const char* pattern_prefix, int max) {
  char p[512];
  int count = 0;
  for (int i = 0; i < max; i++) {
    snprintf(p, sizeof(p), "%s.%d", pattern_prefix, i);
    struct stat st;
    if (stat(p, &st) == 0) count++;
  }
  return count;
}

int main(void) {
  cleanup_all();

  const int N = 80000;
  const char* label = "N=80000 VALUE=128 MEMTABLE=2MB";

  db_config_t config;
  config.fsync_mode    = KANBUDB_FSYNC_NONE;
  config.cache_size    = 8 * 1024 * 1024;
  config.memtable_size = 2 * 1024 * 1024;
  config.compaction_threads = 1;

  const char* col_names[] = {"id", "data"};
  kanbudb_col_type_t col_types[] = {KANBUDB_INT32, KANBUDB_STRING};

  printf("=== KanbuDB Persistence Benchmark ===\n");
  printf("  %s\n\n", label);

  /* ═══════════════════════════════════════════════════════
     Phase 1: Sequential writes
     ═══════════════════════════════════════════════════════ */
  printf("─── Phase 1: Sequential writes ───\n");

  db_t* db = NULL;
  int rc = db_open(BENCH_DB, &config, &db);
  if (rc != KANBUDB_OK) { fprintf(stderr, "db_open failed\n"); return 1; }

  rc = db_create_table(db, "bench", col_names, col_types, 2, "id");
  if (rc != KANBUDB_OK) { fprintf(stderr, "create_table failed\n"); return 1; }

  double t0 = now_sec();
  for (int i = 0; i < N; i++) {
    char key[32];
    char val[VALUE_SIZE];
    snprintf(key, sizeof(key), "key_%d", i);
    make_value(val, i);
    rc = db_put(db, "bench", key, strlen(key) + 1, val, VALUE_SIZE);
    if (rc != KANBUDB_OK) {
      fprintf(stderr, "put failed at %d (rc=%d)\n", i, rc);
      return 1;
    }
  }
  double t1 = now_sec();

  int sst_count = file_count_cb(BENCH_DB ".sst.0", 200);
  double write_ms = (t1 - t0) * 1000;
  printf("  %d inserts: %.2f ms  (%.0f op/s, %.2f us/op)\n",
         N, write_ms, N / (t1 - t0), (t1 - t0) / N * 1e6);
  printf("  SSTable files: %d\n", sst_count);
  db_close(db);

  /* ═══════════════════════════════════════════════════════
     Phase 2: Read all (after reopen)
     ═══════════════════════════════════════════════════════ */
  printf("\n─── Phase 2: Sequential reads ───\n");

  t0 = now_sec();
  rc = db_open(BENCH_DB, &config, &db);
  t1 = now_sec();
  printf("  Reopen: %.2f ms\n", (t1 - t0) * 1000);

  t0 = now_sec();
  for (int i = 0; i < N; i++) {
    char key[32];
    snprintf(key, sizeof(key), "key_%d", i);
    void* val = NULL;
    size_t vlen = 0;
    rc = db_get(db, "bench", key, strlen(key) + 1, &val, &vlen);
    if (rc != KANBUDB_OK) {
      fprintf(stderr, "  FAIL: key %d not found (rc=%d)\n", i, rc);
      return 1;
    }
    if (vlen != VALUE_SIZE) {
      fprintf(stderr, "  FAIL: key %d wrong size %zu\n", i, vlen);
      return 1;
    }
  }
  t1 = now_sec();
  printf("  %d reads:  %.2f ms  (%.0f op/s, %.2f us/op)\n",
         N, (t1 - t0) * 1000, N / (t1 - t0), (t1 - t0) / N * 1e6);
  db_close(db);

  /* ═══════════════════════════════════════════════════════
     Phase 3: Overwrite ALL keys with new values
     ═══════════════════════════════════════════════════════ */
  printf("\n─── Phase 3: Overwrite all keys ───\n");

  rc = db_open(BENCH_DB, &config, &db);
  if (rc != KANBUDB_OK) return 1;

  t0 = now_sec();
  for (int i = 0; i < N; i++) {
    char key[32];
    char val[VALUE_SIZE];
    snprintf(key, sizeof(key), "key_%d", i);
    make_value(val, i + N);  /* Use a different base value */
    rc = db_put(db, "bench", key, strlen(key) + 1, val, VALUE_SIZE);
    if (rc != KANBUDB_OK) {
      fprintf(stderr, "overwrite failed at %d\n", i);
      return 1;
    }
  }
  t1 = now_sec();
  printf("  %d updates: %.2f ms  (%.0f op/s, %.2f us/op)\n",
         N, (t1 - t0) * 1000, N / (t1 - t0), (t1 - t0) / N * 1e6);
  db_close(db);

  /* ═══════════════════════════════════════════════════════
     Phase 4: Mixed workload (read + write random keys)
     ═══════════════════════════════════════════════════════ */
  printf("\n─── Phase 4: Random mixed workload ───\n");

  rc = db_open(BENCH_DB, &config, &db);
  if (rc != KANBUDB_OK) return 1;

  int mixed_ops = 50000;
  int reads_ok = 0, writes_ok = 0;
  t0 = now_sec();
  for (int i = 0; i < mixed_ops; i++) {
    int idx = (i * 37337 + 7777) % N;
    char key[32];
    snprintf(key, sizeof(key), "key_%d", idx);

    if (i % 2 == 0) {
      /* Read */
      void* val = NULL;
      size_t vlen = 0;
      rc = db_get(db, "bench", key, strlen(key) + 1, &val, &vlen);
      if (rc != KANBUDB_OK) {
        fprintf(stderr, "mixed read failed at %d key %s\n", i, key);
        return 1;
      }
      reads_ok++;
    } else {
      /* Write */
      char val[VALUE_SIZE];
      make_value(val, idx + 2 * N);
      rc = db_put(db, "bench", key, strlen(key) + 1, val, VALUE_SIZE);
      if (rc != KANBUDB_OK) {
        fprintf(stderr, "mixed write failed at %d\n", i);
        return 1;
      }
      (void)0;
      writes_ok++;
    }
  }
  t1 = now_sec();
  printf("  %d ops (%d reads, %d writes): %.2f ms  (%.0f op/s)\n",
         mixed_ops, reads_ok, writes_ok, (t1 - t0) * 1000,
         mixed_ops / (t1 - t0));
  db_close(db);

  (void)0;

  /* ═══════════════════════════════════════════════════════
     Phase 5: Final restart + full verify
     ═══════════════════════════════════════════════════════ */
  printf("\n─── Phase 5: Final restart + verify ───\n");

  t0 = now_sec();
  rc = db_open(BENCH_DB, &config, &db);
  t1 = now_sec();
  printf("  Reopen: %.2f ms\n", (t1 - t0) * 1000);

  /* Pre-compute which keys Phase 4 wrote */
  int* p4_written = (int*)calloc((size_t)N, sizeof(int));
  for (int j = 1; j < mixed_ops; j += 2) {
    int idx = (j * 37337 + 7777) % N;
    p4_written[idx] = 1;
  }

  t0 = now_sec();
  int verified = 0;
  for (int i = 0; i < N; i++) {
    char key[32];
    char expected[VALUE_SIZE];
    snprintf(key, sizeof(key), "key_%d", i);

    if (p4_written[i]) {
      make_value(expected, i + 2 * N);
    } else {
      make_value(expected, i + N);
    }

    void* val = NULL;
    size_t vlen = 0;
    rc = db_get(db, "bench", key, strlen(key) + 1, &val, &vlen);
    if (rc != KANBUDB_OK) {
      fprintf(stderr, "  FAIL: key %d missing after restart\n", i);
      free(p4_written);
      return 1;
    }
    if (vlen != VALUE_SIZE || memcmp(val, expected, VALUE_SIZE) != 0) {
      fprintf(stderr, "  FAIL: key %d data mismatch (phase4=%d)\n", i, p4_written[i]);
      free(p4_written);
      return 1;
    }
    verified++;
  }
  free(p4_written);
  t1 = now_sec();

  printf("  %d keys verified: %.2f ms  (%.0f op/s)\n",
         verified, (t1 - t0) * 1000, verified / (t1 - t0));

  /* ── Summary ─────────────────────────────────────────── */
  char p[512];
  snprintf(p, sizeof(p), "%s.wal", BENCH_DB);
  struct stat st;
  uint64_t wal_size = 0;
  if (stat(p, &st) == 0) wal_size = (uint64_t)st.st_size;

  snprintf(p, sizeof(p), "%s.system", BENCH_DB);
  uint64_t sys_size = 0;
  if (stat(p, &st) == 0) sys_size = (uint64_t)st.st_size;

  int final_sst = file_count_cb(BENCH_DB ".sst.0", 200);

  printf("\n=== Summary ===\n");
  printf("  WAL:        %llu bytes\n", (unsigned long long)wal_size);
  printf("  System:     %llu bytes\n", (unsigned long long)sys_size);
  printf("  SSTables:   %d files\n", final_sst);

  if (final_sst > 1) {
    printf("  SSTable seq list:\n");
    for (int i = 1; i <= final_sst; i++) {
      char p[512];
      snprintf(p, sizeof(p), "%s.sst.0.%d", BENCH_DB, i);
      struct stat st;
      if (stat(p, &st) == 0) {
        printf("    .sst.0.%d  (%lld bytes)\n", i, (long long)st.st_size);
      }
    }
  }

  printf("  ALL CHECKS PASSED ✓\n");

  db_close(db);

  (void)final_sst;

  cleanup_all();
  return 0;
}
