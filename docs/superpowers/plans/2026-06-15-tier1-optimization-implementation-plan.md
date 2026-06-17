# Tier 1 优化实现计划

> 日期: 2026-06-15
> 状态: ✅ **全部完成** — 6/6 Step 已实现，22/22 测试通过

---

## Step 1: Bloom Filter ✅ 已完成

**状态**: 完成并合入，全量 22 测试通过，零泄漏。

### 改动文件
| 文件 | 改动 |
|------|------|
| `src/util/bloom.h` | 新增 — Bloom Filter API |
| `src/util/bloom.c` | 新增 — 实现 (MurmurHash64A + double hashing) |
| `src/storage/sstable.h` | SSTABLE_VERSION → 2, header 增加 `bloom_offset` 字段 |
| `src/storage/sstable.c` | Writer: 在 index block 后写入 bloom filter; Reader: open 时从 mmap 区域加载; get 时预检查 |
| `src/storage/compaction.c` | 修复 sstable_writer 生命周期泄漏（finish 成功时未 destroy） |
| `CMakeLists.txt` | 加入 `src/util/bloom.c` |

### 设计决策
- **bits-per-key**: 10 ≈ 1% false positive rate
- **Hash**: MurmurHash64A finalizer（快于 xxhash，对短 KV key 友好）
- **Double hashing**: h1(key) + i × h2(key) mod m（Kirsch-Mitzenmacher 2008）
- **写入位置**: index block 之后、CRC footer 之前（v1 reader 跳过 `bloom_offset=0` 即兼容）

### 效果（理论）
- 读放大: `O(N_sstables)` → `O(FPR × N_sstables)` ≈ `O(0.01 × N)`
- 对于 256 个 SSTable 的极端情况，99% 的非命中查找直接返回

---

## Step 2: mmap 零拷贝 SSTable 读取 ✅ 已完成

### 实现
- `sstable_reader_open`: `open()` + `mmap()` 替代 `fopen()` + `fread()`
- `sstable_reader_close`: `munmap()` + `close()` 替代 `fclose()`
- `sstable_reader_scan`: 回调直接传递 mmap 区域指针（`btree_put` 内部 `key_dup/val_dup` 已做拷贝，安全）
- `sstable_reader_get`: 返回 mmap 区域内部指针（零拷贝）
- Index 解析: 指向 mmap 区域（无 `malloc` + `memcpy`）

### 已解决

- **SEGV**: 采用方案 A，改 `test_sstable.c` + `test_compaction.c` 去掉 `free(mmap_ptr)` ✅
- **mmap include**: `#include <sys/mman.h>` 已加 ✅

---

## Step 3: SIMD PQ 距离计算 ✅ 已完成

### 现状

`quantize.c` 中 `pq_distance` 对标量循环：
```c
for (uint32_t d = 0; d < sub_dim; d++) {
    float diff = ca[d] - cb[d];
    sum += diff * diff;
}
```

### 方案

在 `pq_distance` 中添加 AVX2 加速路径，运行时 CPU 检测 dispatch。

```c
static float pq_distance_avx2(const kanbudb_quantizer_t* q,
                               const uint8_t* a, const uint8_t* b) {
    uint32_t nsub = q->params.pq_subspaces;
    uint32_t sub_dim = q->pq_sub_dim;
    __m256 sum = _mm256_setzero_ps();
    
    for (uint32_t s = 0; s < nsub; s++) {
        const float* ca = q->pq_codebooks[s] + a[s] * sub_dim;
        const float* cb = q->pq_codebooks[s] + b[s] * sub_dim;
        uint32_t d = 0;
        for (; d + 8 <= sub_dim; d += 8) {
            __m256 va = _mm256_loadu_ps(ca + d);
            __m256 vb = _mm256_loadu_ps(cb + d);
            __m256 diff = _mm256_sub_ps(va, vb);
            sum = _mm256_fmadd_ps(diff, diff, sum);
        }
        // Tail handling for remaining dims...
    }
    // Horizontal sum...
    return result;
}
```

运行时 dispatch: 用 `__builtin_cpu_supports("avx2")` + `__builtin_cpu_supports("avx512f")` 选择最佳路径，无需编译时 `#ifdef`。

### 改动范围
- `src/vector/quantize.c`: 新增 `pq_distance_avx2` + dispatch 逻辑
- `src/vector/quantize.h`: 无接口变化

### 预期效果
- sub_dim 对齐到 8: **3-5x** 加速
- sub_dim 不对齐: 仍然 **1.5-2x**（AVX2 处理对齐部分 + 标量处理尾部）

---

## Step 4: io_uring 异步 WAL ✅ 已完成

### 现状

`wal_append` 使用 `fwrite` + 条件 `fflush`。每次 `db_put` 阻塞在文件 I/O 上。

### 方案

将 WAL 从 `FILE*` 改为 `io_uring` 异步 I/O：

```
当前: db_put → wal_append(fwrite+fflush) → 阻塞 → 返回
io_uring: db_put → wal_append(submit SQE) → 立即返回
          后台: io_uring_wait_cqe → 完成回调
```

### 关键设计

```c
struct kanbudb_wal {
    // io_uring context
    struct io_uring ring;
    int use_io_uring;  // 0 = fallback to stdio
    
    // Write buffer for batching
    uint8_t* buf;
    size_t buf_len;
    size_t buf_cap;
    
    // Fallback stdio (for Linux < 5.1 or non-Linux)
    FILE* file;
    char* path;
    int fsync_mode;
    uint64_t seq;
};
```

**写路径**:
1. 序列化 entry 到内部 buffer
2. buffer 满或显式 `wal_sync` → 提交 `IORING_OP_WRITEV` SQE
3. 定期调用 `io_uring_submit_and_wait` 收割完成事件
4. `wal_sync` → 提交 `IORING_OP_FSYNC` SQE

**IORING_SETUP 标志**: `IORING_SETUP_COOP_TASKRUN`（减少 IPI）、`IORING_SETUP_SINGLE_ISSUER`（单线程提交者优化）

**Fallback**: 检测 `io_uring_queue_init()` 失败 → 退回到 `fwrite`/`fflush` 模式

### 改动范围
- `src/storage/wal.h`: 无接口变化（内部实现替换）
- `src/storage/wal.c`: 重写 `struct kanbudb_wal`；`wal_create` 初始化 io_uring；`wal_append` 提交 SQE；`wal_truncate` 重建；`wal_destroy` 清理 io_uring

### 预期效果
- `fsync_mode=ALWAYS`: **3-10x** 吞吐（批量 fsync 替代每 op fsync）
- `fsync_mode=NONE`: 主要是减少 syscall 开销

---

## Step 5: Leveled Compaction ✅ 已完成

### 现状

所有 flush 出来的 SSTable 在同一层（文件命名 `.sst.0.SEQ`），compaction 做 N→1 简单合并。读放大随文件数线性增长。

### 方案

实现 Leveled Compaction（LevelDB/RocksDB 风格），组织为 L0→L1→…→L7：

```
L0: flush 结果，文件间 key 重叠（无序），每个文件单独
L1: key 不重叠，有序，固定大小 ~10MB
L2: key 不重叠，有序，~100MB
L3: ~1GB
L4: ~10GB
...
```

### 文件命名

```
.sst.LEVEL.SEQ
例: kanbudb.sst.0.42   (L0, seq=42)
    kanbudb.sst.1.100  (L1, seq=100)
```

### Compaction 触发

| 条件 | 动作 |
|------|------|
| L0 文件数 > `level0_file_num_compaction_trigger` (默认 4) | 选 L0 文件 + L1 重叠 → merge |
| Ln 总大小 > `max_bytes_for_level[n]` | 选 Ln 文件 + Ln+1 重叠 → merge |
| 文件含大量 tombstone（可选项） | GC compaction |

### 数据结构

```c
typedef struct {
    int    level;
    char** files;
    int    num_files;
    uint64_t total_size;
} compaction_level_t;

typedef struct {
    compaction_level_t levels[8];  // L0..L7
    // Per-level size targets
    uint64_t max_bytes[8];
} compaction_manager_t;
```

### 改动范围

| 文件 | 改动 |
|------|------|
| `src/storage/compaction.h` | 新增 `compaction_level_t`, `compaction_manager_t`, `compaction_pick()`, `compaction_register_file()`, `compaction_run_level()` |
| `src/storage/compaction.c` | 重写为 leveled merge：多路归并（N 个文件 → 1 个输出层）、tombstone GC、level 大小追踪 |
| `src/storage/sstable.c` | `sstable_scan_dir` 增加 level 过滤参数 |
| `src/core/db.h` | `struct kanbudb_db` 增加 `compaction_manager_t` 字段、后台 compaction 线程 id |
| `src/core/db.c` | `db_flush_memtable` → 写入 L0; `db_open` → 按 level 序加载; 后台线程执行 compaction_pick + compaction_run |
| `include/db.h` | 可选暴露 compaction 配置参数 |

### 预期效果
- 读放大: `O(N_sstables)` → `O(level_count × files_per_level)` ≈ `O(8 × 2)`
- 写放大: 略微增加（leveled vs classic trade-off）
- 空间放大: 减少（tombstone 更早被 GC）

---

## Step 6: 细粒度锁分拆 ✅ 已完成

### 现状

`struct kanbudb_db` 一个 `pthread_rwlock_t` 保护所有操作。`db_put`（写 WAL、写 LSM）和 `db_get`（读 B-tree）互相串行。

### 方案

分拆为 4 把锁：

| 锁名 | 保护对象 | 类型 | 获取时机 |
|------|---------|------|---------|
| `table_lock` | `tables[]`, `num_tables`, schema | rwlock | create_table, find_table, 写 schema |
| `lsm_lock` | LSM memtable (active + flushing) | rwlock | put/delete/get 操作 LSM 时 |
| `btree_lock` | B-tree 全部节点 | rwlock | put/get/delete 操作 B-tree 时 |
| `wal_lock` | WAL append (序列化点) | mutex | 每次 wal_append |

### 锁获取矩阵

```
db_create_table:  table_lock(w)
db_put:           table_lock(r) → wal_lock(m) → lsm_lock(w) → btree_lock(w)
db_get:           table_lock(r) → lsm_lock(r) → btree_lock(r)
db_delete:        table_lock(r) → wal_lock(m) → lsm_lock(w) → btree_lock(w)
db_close:         table_lock(w) + lsm_lock(w) + btree_lock(w) + wal_lock(m)
db_flush_memtable: lsm_lock(w) + btree_lock(w) + ...
db_compact:        btree_lock(w) + ...
```

### ⚠️ 锁顺序（避免死锁）

固定全序: `table_lock < wal_lock < lsm_lock < btree_lock`
任何路径必须按此顺序加锁。

### 改动范围
- `src/core/db.h`: 替换 `pthread_rwlock_t rwlock` 为 4 个锁字段
- `src/core/db.c`: 每个操作替换为精确锁选择

### 预期效果
- 多线程读不同表: **不再互锁**
- 读-写并发: 读不阻塞写（仅 `lsm_lock` 和 `btree_lock` 的读写互斥）
- compaction/flush 不阻塞普通查询（除 B-tree 写锁外）

---

## 执行顺序（更新版）

考虑到依赖关系和实现风险：

```
Step 1: Bloom Filter          ✅ DONE
Step 2: mmap SSTable Reader   ✅ DONE
Step 3: SIMD PQ Distance      ✅ DONE
Step 4: io_uring WAL          ✅ DONE
Step 5: Leveled Compaction    ✅ DONE
Step 6: 细粒度锁分拆          ✅ DONE
```

## 各步骤风险评级

| 步骤 | 风险 | 结果 |
|------|------|------|
| Step 1 | 低 | ✅ 通过 |
| Step 2 | 中 | ✅ 通过 (修复了测试中 free(mmap_ptr) 的 SEGV) |
| Step 3 | 低 | ✅ 通过 |
| Step 4 | 中 | ✅ 通过 (io_uring init 失败自动降级到 stdio) |
| Step 5 | 高 | ✅ 通过 (全流程改造：flush、open、close、后台线程) |
| Step 6 | 中 | ✅ 通过 (固定锁顺序 table<wal<lsm<btree 防死锁) |

---

## 测试策略

| 步骤 | 验证结果 |
|------|---------|
| Step 1 | ✅ 全量 ctest 22/22 |
| Step 2 | ✅ ctest 22/22 (修复测试 free SEGV 后) |
| Step 3 | ✅ `test_quantize` 20/20, PQ 精度一致 |
| Step 4 | ✅ `test_wal` 4/4, `test_persistence` 4/4, `test_db` 7/7 |
| Step 5 | ✅ `test_compaction` 2/2, `test_persistence` 4/4, `test_e2e` 2/2 |
| Step 6 | ✅ `test_thread_safety` 2/2, 全量 22/22 |
