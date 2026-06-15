# KanbuDB Embedded Database — 使用指南

## 编译

```bash
cmake -B build
cmake --build build
```

链接：`-lkanbudb_static` 或 `-lkanbudb_shared`

单文件分发：
```bash
cmake --build build --target amalgamate
# dist/kanbudb.h + dist/kanbudb.c
```

---

## 1. 数据库生命周期

```c
#include "db.h"

db_t *db;
db_config_t cfg = {
    .fsync_mode = KANBUDB_FSYNC_PERIODIC,  // NONE / PERIODIC / ALWAYS
    .cache_size = 0,                       // 0 = auto
    .memtable_size = 4 * 1024 * 1024,      // 4MB
    .compaction_threads = 1,
};
int rc = db_open("/path/to/mydb", &cfg, &db);
if (rc != KANBUDB_OK) {
    printf("open failed: %s\n", db_error_string(rc));
    return;
}

// ... 使用数据库 ...

db_close(db);
```

---

## 2. 建表

```c
const char *cols[] = {"id", "name", "age", "salary"};
kanbudb_col_type_t types[] = {
    KANBUDB_INT64,
    KANBUDB_STRING,
    KANBUDB_INT32,
    KANBUDB_DOUBLE,
};
int rc = db_create_table(db, "employees", cols, types, 4, "id");
// rc == KANBUDB_OK
// 重复建表返回 KANBUDB_ERR_EXISTS
```

支持的类型：`KANBUDB_INT32`, `KANBUDB_INT64`, `KANBUDB_FLOAT`, `KANBUDB_DOUBLE`, `KANBUDB_STRING`, `KANBUDB_BLOB`, `KANBUDB_BOOL`

---

## 3. 增删改查

### 写入

```c
// key-value 存储，按表名分区
int rc = db_put(db, "employees", "1001", 4, "{\"name\":\"Alice\",\"age\":30}", 28);
if (rc != KANBUDB_OK) { /* 处理错误 */ }
```

### 读取

```c
void *val;
size_t len;
int rc = db_get(db, "employees", "1001", 4, &val, &len);
if (rc == KANBUDB_OK) {
    printf("value=%.*s\n", (int)len, (char*)val);
    // 注意：val 指针在下一次写入前有效，建议立即拷贝
} else if (rc == KANBUDB_ERR_NOTFOUND) {
    printf("key not found\n");
}
```

### 删除

```c
int rc = db_delete(db, "employees", "1001", 4);
// rc == KANBUDB_OK 或 KANBUDB_ERR_NOTFOUND
```

---

## 4. 查询构建器

```c
query_builder_t *qb = db_query(db, "employees");
if (!qb) { /* OOM */ }

qb_filter(qb, "age", ">=", &age_val);
qb_sort(qb, "salary", 0);   // 0=asc, 1=desc
qb_limit(qb, 10);

result_set_t *rs = qb_exec(qb);
if (rs) {
    while (rs_next(rs)) {
        void *data;
        size_t len;
        rs_get_column(rs, 0, &data, &len); // id
        printf("id=%.*s\n", (int)len, (char*)data);
    }
    rs_close(rs);
}
qb_destroy(qb);
```

---

## 5. 全文搜索

### 创建索引

```c
fts_options_t fts_opts = {
    .enable_stemming = 1,
    .enable_stop_words = 1,
    .language = "english",
};
db_fts_create_index(db, "articles", "body", &fts_opts);
```

### 搜索

```c
result_set_t *rs = NULL;
int rc = db_fts_search(db, "articles", "body", "database performance", &rs);
if (rc == KANBUDB_OK && rs) {
    while (rs_next(rs)) {
        void *id, *score;
        size_t id_len, score_len;
        rs_get_column(rs, 0, &id, &id_len);    // doc_id
        rs_get_column(rs, 1, &score, &score_len); // BM25 score
        printf("doc %.*s score=%.*s\n",
               (int)id_len, (char*)id,
               (int)score_len, (char*)score);
    }
    rs_close(rs);
}
```

### 删除索引

```c
db_fts_drop_index(db, "articles", "body");
```

---

## 6. 错误处理

```c
int rc = db_put(db, "employees", "key", 3, "val", 3);
if (rc != KANBUDB_OK) {
    const char *msg = db_error_string(rc);
    int last = db_last_error(db);
    printf("error %d: %s\n", last, msg);
}
```

错误码：

| 码 | 常量 | 含义 |
|----|------|------|
| 0 | `KANBUDB_OK` | 成功 |
| -1 | `KANBUDB_ERR_OOM` | 内存不足 |
| -2 | `KANBUDB_ERR_NOTFOUND` | 未找到 |
| -3 | `KANBUDB_ERR_EXISTS` | 已存在 |
| -4 | `KANBUDB_ERR_CORRUPT` | 数据损坏 |
| -5 | `KANBUDB_ERR_IO` | I/O 错误 |
| -6 | `KANBUDB_ERR_INVAL` | 参数错误 |
| -7 | `KANBUDB_ERR_BUSY` | 忙 |

---

## 7. 完整示例

```c
#include "db.h"
#include <stdio.h>
#include <string.h>

int main(void) {
    db_t *db;

    // 打开数据库
    db_config_t cfg = {KANBUDB_FSYNC_PERIODIC, 0, 4*1024*1024, 1};
    if (db_open("/tmp/testdb", &cfg, &db) != KANBUDB_OK)
        return 1;

    // 建表
    const char *cols[] = {"id", "content"};
    kanbudb_col_type_t types[] = {KANBUDB_STRING, KANBUDB_STRING};
    db_create_table(db, "docs", cols, types, 2, "id");

    // 写入
    db_put(db, "docs", "doc1", 4, "hello world", 11);
    db_put(db, "docs", "doc2", 4, "hello database", 14);

    // 读取
    void *val; size_t len;
    if (db_get(db, "docs", "doc1", 4, &val, &len) == KANBUDB_OK)
        printf("doc1 = %.*s\n", (int)len, (char*)val);

    // FTS 搜索
    db_fts_create_index(db, "docs", "content", NULL);
    result_set_t *rs = NULL;
    if (db_fts_search(db, "docs", "content", "hello", &rs) == KANBUDB_OK) {
        while (rs_next(rs)) {
            void *id; size_t il;
            rs_get_column(rs, 0, &id, &il);
            printf("matched: %.*s\n", (int)il, (char*)id);
        }
        rs_close(rs);
    }

    db_close(db);
    return 0;
}
```

编译运行：
```bash
gcc -o example example.c -Iinclude -Lbuild -lkanbudb_static
./example
```

---

## 8. 多线程并发访问

KanbuDB 支持单写多读并发模型（v0.2.2+）：

- **读读并发**：多个线程可以同时调用 `db_get()` 和 `qb_exec()`
- **写串行化**：`db_put()` / `db_delete()` 与其他写操作互斥
- **读写互斥**：写操作会等待所有读操作完成

```c
#include <pthread.h>

// 多个线程可以并发读取
void* reader(void* arg) {
    db_t* db = (db_t*)arg;
    for (int i = 0; i < 1000; i++) {
        void* val; size_t len;
        db_get(db, "users", "key1", 4, &val, &len);
    }
    return NULL;
}

// 写操作是串行的
void* writer(void* arg) {
    db_t* db = (db_t*)arg;
    for (int i = 0; i < 1000; i++) {
        db_put(db, "users", "key2", 4, "data", 4);
    }
    return NULL;
}

// 并发使用
pthread_t r[4], w[2];
for (int i = 0; i < 4; i++) pthread_create(&r[i], NULL, reader, db);
for (int i = 0; i < 2; i++) pthread_create(&w[i], NULL, writer, db);
for (int i = 0; i < 4; i++) pthread_join(r[i], NULL);
for (int i = 0; i < 2; i++) pthread_join(w[i], NULL);
```

编译时需要链接 pthread：
```bash
gcc -o myapp myapp.c -Iinclude -Lbuild -lkanbudb_static -lpthread
```

**注意：**
- `db_get()` 返回的指针在下一次写操作后失效，建议立即拷贝
- `db_close()` 会等待所有活跃读操作完成后再销毁资源
- 不支持事务回滚或快照隔离

---

## 9. 向量搜索 (Vector Search)

向量搜索模块提供浮点向量的近似最近邻 (ANN) 搜索，支持 L2/余弦/内积三种距离度量，可选择 FLAT (暴力) 或 HNSW (分层可导航小世界) 算法。

### API 总览

```c
#include "vector.h"
```

| 函数 | 作用 |
|------|------|
| `kanbudb_vec_create` | 创建向量索引 |
| `kanbudb_vec_open` | 从持久化数据恢复索引 |
| `kanbudb_vec_close` | 关闭并释放资源 (自动 flush) |
| `kanbudb_vec_flush` | 手动触发持久化快照 |
| `kanbudb_vec_flush_interval` | 设置后台自动 flush 间隔 (ms) |
| `kanbudb_vec_destroy` | 删除索引文件 |
| `kanbudb_vec_insert` | 插入/更新向量 |
| `kanbudb_vec_insert_batch` | 批量插入 |
| `kanbudb_vec_delete` | 软删除 (标记删除) |
| `kanbudb_vec_search` | Top-K 近似最近邻搜索 |
| `kanbudb_vec_search_radius` | 半径内向量搜索 |
| `kanbudb_vec_get` | 按 ID 获取原始向量 |
| `kanbudb_vec_count` | 返回向量数量 |
| `kanbudb_vec_dimension` | 返回向量维度 |
| `kanbudb_vec_stats` | 返回统计信息 |

### 算法选择

| 算法 | 枚举值 | 插入速度 | 搜索速度 | 内存 | 适用场景 |
|------|--------|----------|----------|------|----------|
| FLAT | `KANBUDB_VEC_ALGO_FLAT` | 极快 (O(1) append) | 慢 (O(n) 扫描) | 低 | 小数据集 (<10万), 原型验证 |
| HNSW | `KANBUDB_VEC_ALGO_HNSW` | 较慢 (O(log n) 图构建) | 极快 (O(log n)) | 中高 | 大规模, 线上服务 |

### 快速开始

```c
#include "vector.h"
#include <stdio.h>

int main(void) {
    kanbudb_vec_index_t* idx = NULL;

    // 创建 FLAT 索引 (三维 L2 距离)
    kanbudb_vec_params_t params = {
        .algo = KANBUDB_VEC_ALGO_FLAT,
        .metric = KANBUDB_VEC_METRIC_L2,
        .dimension = 3,
        .initial_capacity = 0,
        .enable_persistence = 0,
    };
    if (kanbudb_vec_create(NULL, &params, &idx) != 0) return 1;

    // 插入向量
    float v1[] = {1.0f, 0.0f, 0.0f};
    float v2[] = {0.0f, 1.0f, 0.0f};
    float v3[] = {0.0f, 0.0f, 1.0f};
    kanbudb_vec_insert(idx, 1, v1);
    kanbudb_vec_insert(idx, 2, v2);
    kanbudb_vec_insert(idx, 3, v3);

    // Top-2 搜索
    float query[] = {1.0f, 0.1f, 0.0f};
    kanbudb_vec_result_t results[2];
    int n = kanbudb_vec_search(idx, query, 2, results);
    printf("Found %d results:\n", n);
    for (int i = 0; i < n; i++)
        printf("  id=%llu  distance=%.4f\n",
               (unsigned long long)results[i].id, results[i].distance);

    // 按 ID 获取向量
    float out[3];
    if (kanbudb_vec_get(idx, 1, out) == 0)
        printf("Vector id=1: [%.2f, %.2f, %.2f]\n", out[0], out[1], out[2]);

    kanbudb_vec_close(idx);
    return 0;
}
```

### HNSW 模式

```c
kanbudb_vec_params_t params = {
    .algo = KANBUDB_VEC_ALGO_HNSW,      // ← 使用 HNSW 算法
    .metric = KANBUDB_VEC_METRIC_L2,
    .dimension = 768,                    // 适合 LLM 嵌入维度
    .initial_capacity = 1000000,
    .enable_persistence = 0,
    .M = 16,                             // 每层最大连接数 (默认 16)
    .ef_construction = 200,              // 构建时搜索宽度 (默认 200)
    .ef_search = 50,                     // 查询时搜索宽度 (默认 50)
};
kanbudb_vec_create(NULL, &params, &idx);
```

**HNSW 性能调优建议：**
- `M` (8-48)：越大召回越高，内存越大。文本嵌入推荐 16-32。
- `ef_construction` (100-500)：越大构建质量越高但越慢。推荐 200。
- `ef_search` (10-2000)：越大召回越高但越慢。精确度优先用 200+，速度优先用 50。

### 距离度量

```c
KANBUDB_VEC_METRIC_L2      // 欧氏距离 (平方，非 sqrt)
KANBUDB_VEC_METRIC_COSINE  // 余弦距离 (1 - dot)，向量需归一化
KANBUDB_VEC_METRIC_IP      // 内积 (返回 -dot 实现最小堆排序)
```

### 持久化与 WAL

```c
kanbudb_vec_params_t params = {
    .algo = KANBUDB_VEC_ALGO_HNSW,
    .metric = KANBUDB_VEC_METRIC_L2,
    .dimension = 128,
    .enable_persistence = 1,  // ← 启用持久化
};

// 创建时指定存储路径
kanbudb_vec_create("/tmp/my_vec_index", &params, &idx);

// 每次 insert/delete 自动写入 WAL
kanbudb_vec_insert(idx, 1, vector_data);
kanbudb_vec_delete(idx, 2);

// 手动 flush (写入快照 + 截断 WAL)
kanbudb_vec_flush(idx);

// 设置后台自动 flush (每 5000ms)
kanbudb_vec_flush_interval(idx, 5000);

// 关闭 (自动 flush)
kanbudb_vec_close(idx);

// 重新打开 (从快照 + WAL 恢复)
kanbudb_vec_open("/tmp/my_vec_index", &idx);
```

持久化文件结构：
```
/tmp/my_vec_index/
├── meta       ← 元数据 (维度、度量、算法、参数、数量)
├── vectors    ← 原始向量二进制数据
├── ids        ← 向量 ID 数组
└── wal        ← WAL (增量操作日志，快照后截断)
```

### 批量插入 + 半径搜索

```c
// 批量插入 (FLAT 模式下单次拷贝，HNSW 下逐条插入)
uint64_t ids[] = {1, 2, 3, 4, 5};
float vectors[] = {  // 连续内存: count * dim
    0.1f, 0.2f, 0.3f,
    0.4f, 0.5f, 0.6f,
    0.7f, 0.8f, 0.9f,
    /* ... */
};
kanbudb_vec_insert_batch(idx, 5, ids, vectors);

// 半径搜索 (距离阈值内所有结果)
kanbudb_vec_result_t radius_results[100];
int n = kanbudb_vec_search_radius(idx, query, 1.5f, radius_results, 100);
```

### 多线程安全

向量模块使用和 Core DB 相同的 `pthread_rwlock_t` 单写多读并发模型：

| 操作 | 锁类型 | 说明 |
|------|--------|------|
| `vec_insert` | 写锁 | 串行化 |
| `vec_delete` | 写锁 | 串行化 |
| `vec_flush` | 写锁 | 串行化 |
| `vec_search` | 读锁 | 读读并发 |
| `vec_get` / `vec_count` | 读锁 | 读读并发 |

### 完整示例

```c
#include "vector.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <math.h>

int main(void) {
    const int dim = 128;
    const int n = 10000;

    // HNSW 索引 (推荐在大规模数据下使用)
    kanbudb_vec_params_t params = {
        .algo = KANBUDB_VEC_ALGO_HNSW,
        .metric = KANBUDB_VEC_METRIC_L2,
        .dimension = dim,
        .initial_capacity = n,
        .M = 16,
        .ef_construction = 200,
        .ef_search = 50,
    };

    kanbudb_vec_index_t* idx = NULL;
    if (kanbudb_vec_create(NULL, &params, &idx) != 0) return 1;

    // 生成随机数据并插入
    srand(42);
    for (int i = 0; i < n; i++) {
        float vec[128];
        for (int j = 0; j < dim; j++)
            vec[j] = (float)rand() / (float)RAND_MAX;
        kanbudb_vec_insert(idx, (uint64_t)(i + 1), vec);
    }
    printf("Inserted %d vectors\n", kanbudb_vec_count(idx));

    // 搜索
    float query[128];
    for (int j = 0; j < dim; j++)
        query[j] = (float)rand() / (float)RAND_MAX;

    kanbudb_vec_result_t results[10];
    int found = kanbudb_vec_search(idx, query, 10, results);

    printf("Top-10 nearest neighbors:\n");
    for (int i = 0; i < found; i++)
        printf("  [%d] id=%llu  distance=%.4f\n", i,
               (unsigned long long)results[i].id, results[i].distance);

    kanbudb_vec_close(idx);
    return 0;
}
```

编译运行：
```bash
gcc -o example example.c -Iinclude -Lbuild -lkanbudb_static -lpthread -lm
./example
```

---

## 10. 持久化与架构

### 持久化文件

| 文件 | 作用 |
|------|------|
| `<path>.wal` | Core WAL, 崩溃恢复, flush 后被截断 |
| `<path>.sst.0.N` | SSTable, memtable flush 后的排序数据 |
| `<path>.ckpt.N` | db_close 时的 B-tree 快照 |
| `<path>.system` | 表 schema 持久化 |
| `<path>.seq` | SSTable 全局单调序列号 |
| `<path>.meta` | 向量索引元数据 (维度/度量/算法) |
| `<path>.vectors` | 向量索引原始数据 |
| `<path>.ids` | 向量索引 ID 映射 |
| `<path>/wal` | 向量索引 WAL (增量操作日志) |

### 启动恢复流程

Core DB 启动恢复：
```
db_open:
  1. 加载 .system → 恢复所有表 schema
  2. 扫描 *.sst → 加载到 B-tree（按 seq 排序）
  3. wal_replay → LSM 恢复未 flush 的写入
```

向量索引启动恢复：
```
kanbudb_vec_open:
  1. 加载 meta → 恢复参数: 维度/度量/算法/数量
  2. 加载 vectors + ids → 恢复原始数据
  3. (HNSW模式) 重建图结构
  4. WAL replay → 恢复最近未 checkpoint 的增量操作
```

重启后表名自动可用, 无需重新 `db_create_table`。

### 架构图

```
┌──────────────────────────────────────────────┐
│              Public C API                    │
│  db_open / db_put / db_get / db_query / fts │
│  + vec_create / vec_insert / vec_search     │
├────────────────┬─────────────────────────────┤
│   Core DB      │    Query Builder    Vector  │
│  (table mgmt)  │   (fluent API)     Index    │
├────────┬───────┴──┬──────────────────────────┤
│  WAL   │   LSM    │       B+tree    HNSW 图  │
│(crash  │(memtable │    (cold store,  (层级     │
│recovery│ + flush) │   重建自 SSTable)  图)    │
├────────┴──────────┴──────────────────────────┤
│  SSTable + 向量持久化(meta/vectors/ids/wal)  │
├──────────────────────────────────────────────┤
│           FTS 引擎                            │
│   Tokenizer → Index → Parser → Ranker        │
├──────────────────────────────────────────────┤
│          Util 层                              │
│   Arena / Page Cache / FST / BM25 / Distance │
└──────────────────────────────────────────────┘
```

核心路径：
- 写入路径: `db_put → WAL append → LSM memtable`
- 读取路径: `db_get → LSM memtable → B+tree`
- 刷新路径: `memtable 满 → flush → SSTable → B-tree`
- 启动恢复: `db_open → .system + *.sst + WAL replay`
- FTS 路径: `文本 → tokenizer → 倒排索引 (FST) → BM25`
- **向量搜索路径: `vec_insert → WAL → 索引图` / `vec_search → 图遍历 → Top-K`**
- **向量持久化路径: `vec_flush → 快照 → meta+vectors+ids` / `vec_open → 加载 + WAL replay`**
