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

## 8. 持久化与架构

### 持久化文件

| 文件 | 作用 |
|------|------|
| `<path>.wal` | WAL, 崩溃恢复, flush 后被截断 |
| `<path>.sst.0.N` | SSTable, memtable flush 后的排序数据 |
| `<path>.ckpt.N` | db_close 时的 B-tree 快照 |
| `<path>.system` | 表 schema 持久化 |
| `<path>.seq` | SSTable 全局单调序列号 |

### 启动恢复流程

```
db_open:
  1. 加载 .system → 恢复所有表 schema
  2. 扫描 *.sst → 加载到 B-tree（按 seq 排序）
  3. wal_replay → LSM 恢复未 flush 的写入
```

重启后表名自动可用, 无需重新 `db_create_table`。

### 架构图

```
┌──────────────────────────────────────────────┐
│              Public C API                    │
│  db_open / db_put / db_get / db_query / fts │
├────────────────┬─────────────────────────────┤
│   Core DB      │    Query Builder            │
│  (table mgmt)  │   (fluent API)              │
├────────┬───────┴──┬──────────────────────────┤
│  WAL   │   LSM    │       B+tree             │
│(crash  │(memtable │    (cold store,           │
│recovery│ + flush) │   重建自 SSTable)         │
├────────┴──────────┴──────────────────────────┤
│  SSTable                                     │
│  (持久化排序键值, 稀疏索引, CRC校验)          │
├──────────────────────────────────────────────┤
│           FTS 引擎                            │
│   Tokenizer → Index → Parser → Ranker        │
├──────────────────────────────────────────────┤
│          Util 层                              │
│   Arena / Page Cache / FST / BM25            │
└──────────────────────────────────────────────┘
```

- 写入路径: `db_put → WAL append → LSM memtable`
- 读取路径: `db_get → LSM memtable → B+tree`
- 刷新路径: `memtable 满 → flush → SSTable → B-tree`
- 启动恢复: `db_open → .system + *.sst + WAL replay`
- FTS 路径: `文本 → tokenizer → 倒排索引 (FST) → BM25`
