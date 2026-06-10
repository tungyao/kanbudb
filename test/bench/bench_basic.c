#include "db.h"
#include "macros.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

#define BENCH_DB_PATH "/tmp/kanbudb_bench"
#define N 10000

static void cleanup(void) {
  char path[256];
  snprintf(path, sizeof(path), "%s.wal", BENCH_DB_PATH);
  unlink(path);
  snprintf(path, sizeof(path), "%s.lsm", BENCH_DB_PATH);
  unlink(path);
}

static double now_sec(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

int main(void) {
  cleanup();

  db_config_t config;
  config.fsync_mode = KANBUDB_FSYNC_NONE;
  config.cache_size = 4 * 1024 * 1024;
  config.memtable_size = 4 * 1024 * 1024;
  config.compaction_threads = 1;

  db_t* db = NULL;
  int rc = db_open(BENCH_DB_PATH, &config, &db);
  if (rc != KANBUDB_OK) {
    fprintf(stderr, "Failed to open db\n");
    return 1;
  }

  const char* col_names[] = {"id", "data"};
  kanbudb_col_type_t col_types[] = {KANBUDB_INT32, KANBUDB_STRING};
  rc = db_create_table(db, "bench", col_names, col_types, 2, "id");
  if (rc != KANBUDB_OK) {
    fprintf(stderr, "Failed to create table\n");
    db_close(db);
    return 1;
  }

  double t0 = now_sec();
  for (int i = 0; i < N; i++) {
    char key[32];
    char val[64];
    snprintf(key, sizeof(key), "key_%d", i);
    snprintf(val, sizeof(val), "value_%d_%s", i, "payload_data_here");
    rc = db_put(db, "bench", key, strlen(key) + 1, val, strlen(val) + 1);
    if (rc != KANBUDB_OK) {
      fprintf(stderr, "Failed to put key %d\n", i);
      db_close(db);
      return 1;
    }
  }
  double t1 = now_sec();

  double t2 = now_sec();
  for (int i = 0; i < N; i++) {
    char key[32];
    snprintf(key, sizeof(key), "key_%d", i);
    void* out_val = NULL;
    size_t out_len = 0;
    rc = db_get(db, "bench", key, strlen(key) + 1, &out_val, &out_len);
    if (rc != KANBUDB_OK) {
      fprintf(stderr, "Failed to get key %d\n", i);
      db_close(db);
      return 1;
    }
  }
  double t3 = now_sec();

  double put_avg_us = (t1 - t0) / N * 1e6;
  double get_avg_us = (t3 - t2) / N * 1e6;

  printf("Benchmark results (N=%d):\n", N);
  printf("  Insert: %.2f us avg (%.2f ms total)\n", put_avg_us, (t1 - t0) * 1000);
  printf("  Read:   %.2f us avg (%.2f ms total)\n", get_avg_us, (t3 - t2) * 1000);
  printf("  Total:  %.2f ms\n", (t1 - t0 + t3 - t2) * 1000);

  db_close(db);
  cleanup();
  return 0;
}
