# KanbuDB — Embedded Database

KanbuDB is an embedded C99 database with ACID-like durability, B+tree/SSTable storage, query builder, and full-text search.

## Features

- **Embedded** — single library, no server process
- **Durable** — WAL + periodic/always fsync modes
- **LSM + B+tree** — fast writes via memtable, cold reads via SSTable/B+tree
- **Fluent query builder** — filter, sort, limit, join
- **Full-text search** — tokenizer → FST inverted index → BM25 ranking
- **Multi-language** — Python (ctypes), Node.js (koffi), Go (cgo)

## Quick Start (C)

```c
#include "db.h"

db_t *db;
db_open("/tmp/mydb", NULL, &db);

db_create_table(db, "users",
    (const char*[]){"id", "name", "age"},
    (kanbudb_col_type_t[]){KANBUDB_STRING, KANBUDB_STRING, KANBUDB_INT32},
    3, "id");

db_put(db, "users", "u1", 2, "hello", 5);

void *val; size_t len;
db_get(db, "users", "u1", 2, &val, &len);

db_close(db);
```

## Install

### Python (PyPI)

```bash
pip install kanbudb
```

```python
import kanbudb
db = kanbudb.open("/tmp/mydb")
db.create_table("users", {"id": "string", "name": "string"}, pk="id")
db.put("users", "u1", {"name": "Alice"})
print(db.get("users", "u1"))
db.close()
```

### Node.js (npm)

```bash
npm install kanbudb-db
```

```js
const kanbudb = require('kanbudb-db');
const db = kanbudb.open('/tmp/mydb');
db.createTable('users', { id: 'string', name: 'string' }, 'id');
db.put('users', 'u1', Buffer.from('hello'));
console.log(db.get('users', 'u1')?.toString());
db.close();
```

### Go (module)

```bash
go get github.com/kanbudb/go-kanbudb
```

```go
import "github.com/kanbudb/go-kanbudb"

db, _ := kanbudb.Open("/tmp/mydb", nil)
db.CreateTable("users", []string{"id","name"},
    []kanbudb.ColType{kanbudb.String, kanbudb.String}, "id")
db.Put("users", "u1", []byte("hello"))
val, _ := db.Get("users", "u1")
db.Close()
```

## Build from Source

```bash
cmake -B build
cmake --build build
# → build/libkanbudb_shared.so + build/libkanbudb_static.a
```

### Single-file distribution

```bash
cmake --build build --target amalgamate
# → dist/kanbudb.c + dist/kanbudb.h
```

## Architecture

```
┌─────────────────────────────────────────────┐
│  Python / Node.js / Go bindings             │
├────────────────┬────────────────────────────┤
│  db_open/put/get/query/fts                  │
├────────────────┼────────────────────────────┤
│  Core DB       │  Query Builder             │
├────────┬───────┴──┬─────────────────────────┤
│  WAL   │  LSM     │  B+tree                 │
├────────┴──────────┴─────────────────────────┤
│  SSTable (sorted key-value, sparse index)   │
├─────────────────────────────────────────────┤
│  FTS: Tokenizer → FST Index → BM25 Ranker  │
└─────────────────────────────────────────────┘
```

## Documentation

- [Usage Guide](USAGE.md) — C API walkthrough
- [API Reference](API.md) — full C function reference

## License

MIT
