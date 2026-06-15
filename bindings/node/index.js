// KanbuDB Embedded Database — Node.js bindings (koffi)
//
// Usage:
//   const kanbudb = require('./index');
//   const db = kanbudb.open('/tmp/testdb');
//   db.createTable('users', { id: 'int64', name: 'string', age: 'int32' }, 'id');
//   db.put('users', 'u1', Buffer.from('{"name":"Alice"}'));
//   const val = db.get('users', 'u1');
//   console.log(val?.toString());
//   db.close();

'use strict';

const koffi = require('koffi');

const libPath = process.env.KANBUDB_LIB || (
  process.platform === 'darwin'
    ? `${__dirname}/../../build/libkanbudb_shared.dylib`
    : `${__dirname}/../../build/libkanbudb_shared.so`
);
const lib = koffi.load(libPath);

// ── Type definitions ────────────────────────────────────────────

const INT = koffi.types.int;
const SIZET = koffi.types.size_t;
const STR = koffi.types.string;
const PTR_VOID = koffi.pointer(koffi.types.void);
const OUT_PTR_VOID = koffi.out(PTR_VOID);
const PTR_SIZET = koffi.pointer(SIZET);
const PTR_STR = koffi.pointer(STR);
const PTR_INT = koffi.pointer(INT);
const PTR_DBCFG = koffi.pointer(koffi.struct('db_config_t', {
  fsync_mode: INT,
  cache_size: SIZET,
  memtable_size: SIZET,
  compaction_threads: INT,
}));
const PTR_FTSOPTS = koffi.pointer(koffi.struct('fts_options_t', {
  enable_stemming: INT,
  enable_stop_words: INT,
  language: STR,
}));

const PTR_VECPARAMS = koffi.pointer(koffi.struct('kanbudb_vec_params_t', {
  algo: INT,
  metric: INT,
  dimension: koffi.types.uint32,
  initial_capacity: koffi.types.uint32,
  enable_persistence: INT,
  M: koffi.types.uint32,
  ef_construction: koffi.types.uint32,
  ef_search: koffi.types.uint32,
}));

const PTR_VECRESULT = koffi.pointer(koffi.struct('kanbudb_vec_result_t', {
  id: koffi.types.uint64,
  distance: koffi.types.float,
}));

const ERR_CODES = {
  0: 'ok', '-1': 'out of memory', '-2': 'not found', '-3': 'already exists',
  '-4': 'corrupt data', '-5': 'I/O error', '-6': 'invalid argument', '-7': 'busy',
};

const COL_TYPES = { int32: 0, int64: 1, float: 2, double: 3, string: 4, blob: 5, bool: 6 };

class KanbuDBError extends Error {
  constructor(code) {
    const msg = ERR_CODES[code] || `unknown error ${code}`;
    super(`[${code}] ${msg}`);
    this.code = code;
    this.name = 'KanbuDBError';
  }
}

function check(rc) {
  if (rc !== 0) throw new KanbuDBError(rc);
}

// ── C function bindings ─────────────────────────────────────────

const db_open = lib.func('db_open', INT, [STR, PTR_DBCFG, OUT_PTR_VOID]);
const db_close = lib.func('db_close', INT, [PTR_VOID]);
const db_last_error = lib.func('db_last_error', INT, [PTR_VOID]);
const db_error_string = lib.func('db_error_string', STR, [INT]);

const db_create_table = lib.func('db_create_table', INT,
  [PTR_VOID, STR, PTR_STR, PTR_INT, INT, STR]);

const db_put = lib.func('db_put', INT, [PTR_VOID, STR, STR, SIZET, PTR_VOID, SIZET]);
const db_get = lib.func('db_get', INT, [PTR_VOID, STR, STR, SIZET, OUT_PTR_VOID, PTR_SIZET]);
const db_delete = lib.func('db_delete', INT, [PTR_VOID, STR, STR, SIZET]);

const db_query = lib.func('db_query', PTR_VOID, [PTR_VOID, STR]);
const qb_filter = lib.func('qb_filter', INT, [PTR_VOID, STR, STR, PTR_VOID]);
const qb_sort = lib.func('qb_sort', INT, [PTR_VOID, STR, INT]);
const qb_limit = lib.func('qb_limit', INT, [PTR_VOID, INT]);
const qb_join = lib.func('qb_join', INT, [PTR_VOID, STR, STR, STR]);
const qb_exec = lib.func('qb_exec', PTR_VOID, [PTR_VOID]);
const qb_destroy = lib.func('qb_destroy', koffi.types.void, [PTR_VOID]);

const rs_next = lib.func('rs_next', INT, [PTR_VOID]);
const rs_get_column = lib.func('rs_get_column', INT, [PTR_VOID, INT, OUT_PTR_VOID, PTR_SIZET]);
const rs_get_column_type = lib.func('rs_get_column_type', INT, [PTR_VOID, INT]);
const rs_num_columns = lib.func('rs_num_columns', INT, [PTR_VOID]);
const rs_close = lib.func('rs_close', koffi.types.void, [PTR_VOID]);

const db_fts_index = lib.func('db_fts_create_index', INT, [PTR_VOID, STR, STR, PTR_FTSOPTS]);
const db_fts_search = lib.func('db_fts_search', INT, [PTR_VOID, STR, STR, STR, OUT_PTR_VOID]);
const db_fts_drop = lib.func('db_fts_drop_index', INT, [PTR_VOID, STR, STR]);

// ── Vector function bindings ────────────────────────────────────────

const vec_create = lib.func('kanbudb_vec_create', INT, [STR, PTR_VECPARAMS, OUT_PTR_VOID]);
const vec_open = lib.func('kanbudb_vec_open', INT, [STR, OUT_PTR_VOID]);
const vec_close = lib.func('kanbudb_vec_close', INT, [PTR_VOID]);
const vec_insert = lib.func('kanbudb_vec_insert', INT, [PTR_VOID, koffi.types.uint64, koffi.pointer(koffi.types.float)]);
const vec_delete = lib.func('kanbudb_vec_delete', INT, [PTR_VOID, koffi.types.uint64]);
const vec_search = lib.func('kanbudb_vec_search', INT, [PTR_VOID, koffi.pointer(koffi.types.float), koffi.types.uint32, PTR_VECRESULT]);
const vec_count = lib.func('kanbudb_vec_count', INT, [PTR_VOID]);
const vec_dimension = lib.func('kanbudb_vec_dimension', INT, [PTR_VOID]);

const qb_cond = lib.func('qb_cond', PTR_VOID, [PTR_VOID, STR, STR, PTR_VOID]);
const qb_cond_and = lib.func('qb_cond_and', PTR_VOID, [PTR_VOID, PTR_VOID, PTR_VOID]);
const qb_cond_or = lib.func('qb_cond_or', PTR_VOID, [PTR_VOID, PTR_VOID, PTR_VOID]);
const qb_cond_not = lib.func('qb_cond_not', PTR_VOID, [PTR_VOID, PTR_VOID]);
const qb_where = lib.func('qb_where', INT, [PTR_VOID, PTR_VOID]);

// ── Helpers ─────────────────────────────────────────────────────

function decodePtr(buf) {
  return koffi.decode(buf, PTR_VOID);
}

function decodeSize(buf) {
  return koffi.decode(buf, SIZET);
}

function readBufFromExt(extPtr, len) {
  const arr = koffi.view(extPtr, len);
  return Buffer.from(arr);
}

function allocBuf(size) {
  return Buffer.alloc(size);
}

// ── ResultSet ───────────────────────────────────────────────────

class ResultSet {
  constructor(ptr) {
    this.ptr = ptr;
    this.numCols = rs_num_columns(ptr);
  }

  next() {
    if (!this.ptr) return false;
    return rs_next(this.ptr) !== 0;
  }

  getColumn(idx) {
    const dataPtr = allocBuf(8);
    const lenPtr = allocBuf(8);
    if (rs_get_column(this.ptr, idx, dataPtr, lenPtr) !== 0) return null;
    const len = decodeSize(lenPtr);
    const ext = decodePtr(dataPtr);
    const buf = readBufFromExt(ext, len);
    const ct = rs_get_column_type(this.ptr, idx);
    const typeName = ['int32', 'int64', 'float', 'double', 'string', 'blob', 'bool'][ct] || 'blob';
    if (typeName === 'int32') return buf.readInt32LE(0);
    if (typeName === 'int64') return Number(buf.readBigInt64LE(0));
    if (typeName === 'float') return buf.readFloatLE(0);
    if (typeName === 'double') return buf.readDoubleLE(0);
    if (typeName === 'string') return buf.toString('utf8', 0, len);
    if (typeName === 'bool') return buf.readInt32LE(0) !== 0;
    return buf.subarray(0, len);
  }

  close() {
    if (this.ptr) {
      rs_close(this.ptr);
      this.ptr = null;
    }
  }

  [Symbol.iterator]() {
    return {
      rs: this,
      next: () => {
        if (!this.next()) return { done: true };
        const row = {};
        for (let i = 0; i < this.numCols; i++) row[i] = this.getColumn(i);
        return { value: row, done: false };
      },
    };
  }
}

// ── Condition ───────────────────────────────────────────────────

class Condition {
  constructor(column, op, value) {
    const valBuf = Buffer.from(String(value) + '\0');
    this.ptr = qb_cond(null, column, op, valBuf);
  }

  static and(left, right) {
    const ptr = qb_cond_and(null, left.ptr, right.ptr);
    const c = Object.create(Condition.prototype);
    c.ptr = ptr;
    return c;
  }

  static or(left, right) {
    const ptr = qb_cond_or(null, left.ptr, right.ptr);
    const c = Object.create(Condition.prototype);
    c.ptr = ptr;
    return c;
  }

  static not(child) {
    const ptr = qb_cond_not(null, child.ptr);
    const c = Object.create(Condition.prototype);
    c.ptr = ptr;
    return c;
  }
}

// ── Database ────────────────────────────────────────────────────

class Database {
  constructor(path, opts = {}) {
    const cfg = {
      fsync_mode: { none: 0, periodic: 1, always: 2 }[opts.fsync] ?? 1,
      cache_size: opts.cacheSize ?? 0,
      memtable_size: opts.memtableSize ?? 4 * 1024 * 1024,
      compaction_threads: opts.compactionThreads ?? 1,
    };

    const ptrOut = allocBuf(8);
    const rc = db_open(path, cfg, ptrOut);
    check(rc);
    this.ptr = decodePtr(ptrOut);
    this._schemas = {};
  }

  createTable(name, columns, pk) {
    const colNames = Object.keys(columns);
    const colTypes = Object.values(columns).map(t => COL_TYPES[t]);
    const colTypesBuf = allocBuf(colTypes.length * 4);
    for (let i = 0; i < colTypes.length; i++) colTypesBuf.writeInt32LE(colTypes[i], i * 4);
    const rc = db_create_table(this.ptr, name, colNames, colTypesBuf, colNames.length, pk);
    check(rc);
    this._schemas[name] = Object.assign({}, columns);
  }

  putRow(table, key, values) {
    const schema = this._schemas[table];
    if (!schema) throw new KanbuDBError(-6, `unknown table '${table}'`);

    const colNames = Object.keys(schema);
    const parts = [];
    for (const col of colNames) {
      const ct = schema[col];
      const val = values[col];
      const typeIdx = COL_TYPES[ct];
      const buf = allocBuf(64);
      let len = 0;
      if (typeIdx === 0) { buf.writeInt32LE(Number(val), 0); len = 4; }
      else if (typeIdx === 1) { buf.writeBigInt64LE(BigInt(val), 0); len = 8; }
      else if (typeIdx === 2) { buf.writeFloatLE(Number(val), 0); len = 4; }
      else if (typeIdx === 3) { buf.writeDoubleLE(Number(val), 0); len = 8; }
      else if (typeIdx === 6) { buf.writeInt8(val ? 1 : 0, 0); len = 1; }
      else {
        const s = Buffer.from(String(val), 'utf8');
        const h = allocBuf(4);
        h.writeUInt32LE(s.length, 0);
        parts.push(h);
        parts.push(s);
        continue;
      }
      parts.push(buf.subarray(0, len));
    }
    const rowData = Buffer.concat(parts);
    const rc = db_put(this.ptr, table, key, Buffer.byteLength(key), rowData, rowData.length);
    check(rc);
  }

  put(table, key, value) {
    const valBuf = Buffer.isBuffer(value) ? value : Buffer.from(String(value));
    const rc = db_put(this.ptr, table, key, Buffer.byteLength(key), valBuf, valBuf.length);
    check(rc);
  }

  get(table, key) {
    const dataPtr = allocBuf(8);
    const lenPtr = allocBuf(8);
    const rc = db_get(this.ptr, table, key, Buffer.byteLength(key), dataPtr, lenPtr);
    if (rc === -2) return null;
    check(rc);
    const len = decodeSize(lenPtr);
    const ext = decodePtr(dataPtr);
    return readBufFromExt(ext, len);
  }

  delete(table, key) {
    const rc = db_delete(this.ptr, table, key, Buffer.byteLength(key));
    if (rc === -2) return false;
    check(rc);
    return true;
  }

  query(table) {
    return new QueryBuilder(this, table);
  }

  ftsCreateIndex(table, column, opts = {}) {
    const fopts = {
      enable_stemming: opts.stemming !== false ? 1 : 0,
      enable_stop_words: opts.stopWords !== false ? 1 : 0,
      language: opts.language || 'english',
    };
    const rc = db_fts_index(this.ptr, table, column, fopts);
    check(rc);
  }

  ftsSearch(table, column, query) {
    const outPtr = allocBuf(8);
    const rc = db_fts_search(this.ptr, table, column, query, outPtr);
    check(rc);
    const ptr = decodePtr(outPtr);
    return ptr ? new ResultSet(ptr) : null;
  }

  ftsDropIndex(table, column) {
    const rc = db_fts_drop(this.ptr, table, column);
    check(rc);
  }

  // ── Vector Index ─────────────────────────────────────────────────

  vecCreate(path, params) {
    const vp = {
      algo: { flat: 0, hnsw: 1 }[params.algo || 'flat'] ?? 0,
      metric: { l2: 0, cosine: 1, ip: 2 }[params.metric || 'l2'] ?? 0,
      dimension: params.dimension || 0,
      initial_capacity: params.initialCapacity || 0,
      enable_persistence: path ? 1 : 0,
      M: params.M || 16,
      ef_construction: params.efConstruction || 200,
      ef_search: params.efSearch || 50,
    };
    const ptrOut = allocBuf(8);
    const rc = vec_create(path, vp, ptrOut);
    check(rc);
    return decodePtr(ptrOut);
  }

  vecOpen(path) {
    const ptrOut = allocBuf(8);
    const rc = vec_open(path, ptrOut);
    check(rc);
    return decodePtr(ptrOut);
  }

  vecClose(handle) {
    const rc = vec_close(handle);
    check(rc);
  }

  vecInsert(handle, id, floatBuf) {
    const rc = vec_insert(handle, BigInt(id), floatBuf);
    check(rc);
  }

  vecDelete(handle, id) {
    const rc = vec_delete(handle, BigInt(id));
    check(rc);
  }

  vecSearch(handle, queryBuf, k) {
    const results = new koffi.StructResultBuffer('kanbudb_vec_result_t', k);
    const n = vec_search(handle, queryBuf, k, results);
    if (n < 0) throw new KanbuDBError(n);
    const out = [];
    for (let i = 0; i < n; i++) {
      const r = results.get(i);
      out.push({ id: Number(r.id), distance: r.distance });
    }
    return out;
  }

  vecCount(handle) {
    return vec_count(handle);
  }

  vecDimension(handle) {
    return vec_dimension(handle);
  }

  close() {
    if (this.ptr) {
      db_close(this.ptr);
      this.ptr = null;
    }
  }
}

// ── QueryBuilder ────────────────────────────────────────────────

class QueryBuilder {
  constructor(db, table) {
    this.ptr = db_query(db.ptr, table);
    this._db = db;
  }

  filter(column, op, value) {
    const v = Buffer.from(String(value) + '\0');
    qb_filter(this.ptr, column, op, v);
    return this;
  }

  sort(column, asc = true) {
    qb_sort(this.ptr, column, asc ? 1 : 0);
    return this;
  }

  limit(n) {
    qb_limit(this.ptr, n);
    return this;
  }

  join(table, onLocal, onForeign) {
    qb_join(this.ptr, table, onLocal, onForeign);
    return this;
  }

  where(cond) {
    qb_where(this.ptr, cond.ptr);
    return this;
  }

  exec() {
    const ptr = qb_exec(this.ptr);
    return ptr ? new ResultSet(ptr) : null;
  }

  destroy() {
    if (this.ptr) {
      qb_destroy(this.ptr);
      this.ptr = null;
    }
  }
}

// ── Public API ──────────────────────────────────────────────────

module.exports = {
  open: (path, opts) => new Database(path, opts),
  Database,
  Condition,
  KanbuDBError,
};
