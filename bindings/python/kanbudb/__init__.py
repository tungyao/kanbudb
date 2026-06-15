"""KanbuDB Embedded Database — Python bindings (ctypes)

Usage:
    import kanbudb

    db = kanbudb.open("/tmp/testdb")
    db.create_table("users", {"id": "int64", "name": "string", "age": "int32"}, pk="id")
    db.put("users", "u1", {"name": "Alice", "age": 30})
    row = db.get("users", "u1")
    for row in db.query("users").filter("age", ">", 25).exec():
        print(row)
    db.close()
"""

import ctypes
import os
import struct
from typing import Optional, Any

# ── Load shared library ──────────────────────────────────────────────

_lib_path = os.environ.get("KANBUDB_LIB", "")
if not _lib_path or not os.path.exists(_lib_path):
    candidates = [
        os.path.join(os.path.dirname(__file__), "libkanbudb_shared.so"),
        os.path.join(os.path.dirname(__file__), "libkanbudb_shared.dylib"),
        os.path.join(os.path.dirname(__file__), "libkanbudb_shared.dll"),
        os.path.join(os.path.dirname(__file__), "..", "..", "..", "build", "libkanbudb_shared.so"),
        os.path.join(os.path.dirname(__file__), "..", "..", "..", "build", "libkanbudb_shared.dylib"),
    ]
    _lib_path = next((p for p in candidates if os.path.exists(p)), "")
if not _lib_path:
    raise RuntimeError(f"KanbuDB library not found. Set KANBUDB_LIB env var.")

_lib = ctypes.cdll.LoadLibrary(_lib_path)

# ── C types ──────────────────────────────────────────────────────────

class _DbConfig(ctypes.Structure):
    _fields_ = [
        ("fsync_mode", ctypes.c_int),
        ("cache_size", ctypes.c_size_t),
        ("memtable_size", ctypes.c_size_t),
        ("compaction_threads", ctypes.c_int),
    ]

class _FtsOptions(ctypes.Structure):
    _fields_ = [
        ("enable_stemming", ctypes.c_int),
        ("enable_stop_words", ctypes.c_int),
        ("language", ctypes.c_char_p),
    ]

class _VecParams(ctypes.Structure):
    _fields_ = [
        ("algo", ctypes.c_int),
        ("metric", ctypes.c_int),
        ("dimension", ctypes.c_uint32),
        ("initial_capacity", ctypes.c_uint32),
        ("enable_persistence", ctypes.c_int),
        ("M", ctypes.c_uint32),
        ("ef_construction", ctypes.c_uint32),
        ("ef_search", ctypes.c_uint32),
    ]

class _VecResult(ctypes.Structure):
    _fields_ = [
        ("id", ctypes.c_uint64),
        ("distance", ctypes.c_float),
    ]

VEC_ALGO_FLAT = 0
VEC_ALGO_HNSW = 1

VEC_METRIC_L2 = 0
VEC_METRIC_COSINE = 1
VEC_METRIC_IP = 2

# ── Error codes ──────────────────────────────────────────────────────

KANBUDB_OK = 0
KANBUDB_ERR_OOM = -1
KANBUDB_ERR_NOTFOUND = -2
KANBUDB_ERR_EXISTS = -3
KANBUDB_ERR_CORRUPT = -4
KANBUDB_ERR_IO = -5
KANBUDB_ERR_INVAL = -6
KANBUDB_ERR_BUSY = -7

ERROR_STRINGS = {
    0: "ok", -1: "out of memory", -2: "not found", -3: "already exists",
    -4: "corrupt data", -5: "I/O error", -6: "invalid argument", -7: "busy",
}

COL_TYPES = {
    "int32": 0, "int64": 1, "float": 2, "double": 3,
    "string": 4, "blob": 5, "bool": 6,
}

_FSYNC_NONE = 0
_FSYNC_PERIODIC = 1
_FSYNC_ALWAYS = 2

# ── C function signatures ────────────────────────────────────────────

_lib.db_open.argtypes = [ctypes.c_char_p, ctypes.POINTER(_DbConfig), ctypes.POINTER(ctypes.c_void_p)]
_lib.db_open.restype = ctypes.c_int

_lib.db_close.argtypes = [ctypes.c_void_p]
_lib.db_close.restype = ctypes.c_int

_lib.db_last_error.argtypes = [ctypes.c_void_p]
_lib.db_last_error.restype = ctypes.c_int

_lib.db_error_string.argtypes = [ctypes.c_int]
_lib.db_error_string.restype = ctypes.c_char_p

_lib.db_create_table.argtypes = [
    ctypes.c_void_p, ctypes.c_char_p,
    ctypes.POINTER(ctypes.c_char_p), ctypes.POINTER(ctypes.c_int),
    ctypes.c_int, ctypes.c_char_p,
]
_lib.db_create_table.restype = ctypes.c_int

_lib.db_put.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_size_t]
_lib.db_put.restype = ctypes.c_int

_lib.db_get.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_void_p), ctypes.POINTER(ctypes.c_size_t)]
_lib.db_get.restype = ctypes.c_int

_lib.db_delete.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_size_t]
_lib.db_delete.restype = ctypes.c_int

_lib.db_query.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
_lib.db_query.restype = ctypes.c_void_p

_lib.qb_filter.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_void_p]
_lib.qb_filter.restype = ctypes.c_int

_lib.qb_sort.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int]
_lib.qb_sort.restype = ctypes.c_int

_lib.qb_limit.argtypes = [ctypes.c_void_p, ctypes.c_int]
_lib.qb_limit.restype = ctypes.c_int

_lib.qb_join.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_char_p]
_lib.qb_join.restype = ctypes.c_int

_lib.qb_exec.argtypes = [ctypes.c_void_p]
_lib.qb_exec.restype = ctypes.c_void_p

_lib.qb_destroy.argtypes = [ctypes.c_void_p]
_lib.qb_destroy.restype = None

_lib.rs_next.argtypes = [ctypes.c_void_p]
_lib.rs_next.restype = ctypes.c_int

_lib.rs_get_column.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.POINTER(ctypes.c_void_p), ctypes.POINTER(ctypes.c_size_t)]
_lib.rs_get_column.restype = ctypes.c_int

_lib.rs_get_column_type.argtypes = [ctypes.c_void_p, ctypes.c_int]
_lib.rs_get_column_type.restype = ctypes.c_int

_lib.rs_num_columns.argtypes = [ctypes.c_void_p]
_lib.rs_num_columns.restype = ctypes.c_int

_lib.rs_close.argtypes = [ctypes.c_void_p]
_lib.rs_close.restype = None

_lib.db_fts_create_index.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p, ctypes.POINTER(_FtsOptions)]
_lib.db_fts_create_index.restype = ctypes.c_int

_lib.db_fts_search.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_char_p, ctypes.POINTER(ctypes.c_void_p)]
_lib.db_fts_search.restype = ctypes.c_int

_lib.db_fts_drop_index.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p]
_lib.db_fts_drop_index.restype = ctypes.c_int

_lib.qb_cond.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_void_p]
_lib.qb_cond.restype = ctypes.c_void_p

_lib.qb_cond_and.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p]
_lib.qb_cond_and.restype = ctypes.c_void_p

_lib.qb_cond_or.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p]
_lib.qb_cond_or.restype = ctypes.c_void_p

_lib.qb_cond_not.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
_lib.qb_cond_not.restype = ctypes.c_void_p

_lib.qb_where.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
_lib.qb_where.restype = ctypes.c_int

# ── Vector function signatures ───────────────────────────────────────

_lib.kanbudb_vec_create.argtypes = [
    ctypes.c_char_p, ctypes.POINTER(_VecParams), ctypes.POINTER(ctypes.c_void_p),
]
_lib.kanbudb_vec_create.restype = ctypes.c_int

_lib.kanbudb_vec_open.argtypes = [ctypes.c_char_p, ctypes.POINTER(ctypes.c_void_p)]
_lib.kanbudb_vec_open.restype = ctypes.c_int

_lib.kanbudb_vec_close.argtypes = [ctypes.c_void_p]
_lib.kanbudb_vec_close.restype = ctypes.c_int

_lib.kanbudb_vec_insert.argtypes = [ctypes.c_void_p, ctypes.c_uint64, ctypes.POINTER(ctypes.c_float)]
_lib.kanbudb_vec_insert.restype = ctypes.c_int

_lib.kanbudb_vec_delete.argtypes = [ctypes.c_void_p, ctypes.c_uint64]
_lib.kanbudb_vec_delete.restype = ctypes.c_int

_lib.kanbudb_vec_search.argtypes = [
    ctypes.c_void_p, ctypes.POINTER(ctypes.c_float), ctypes.c_uint32,
    ctypes.POINTER(_VecResult),
]
_lib.kanbudb_vec_search.restype = ctypes.c_int

_lib.kanbudb_vec_count.argtypes = [ctypes.c_void_p]
_lib.kanbudb_vec_count.restype = ctypes.c_int

_lib.kanbudb_vec_dimension.argtypes = [ctypes.c_void_p]
_lib.kanbudb_vec_dimension.restype = ctypes.c_int


# ── High-level Python API ────────────────────────────────────────────

class KanbuDBError(Exception):
    def __init__(self, code: int, msg: str = ""):
        self.code = code
        self.msg = msg or ERROR_STRINGS.get(code, f"unknown error {code}")
        super().__init__(f"[{code}] {self.msg}")


def _check(rc: int) -> None:
    if rc != KANBUDB_OK:
        raise KanbuDBError(rc)


class _ResultSet:
    def __init__(self, ptr: int, db_ref):
        self._ptr = ctypes.c_void_p(ptr)
        self._db_ref = db_ref
        self._num_cols = _lib.rs_num_columns(self._ptr) if ptr else 0

    def __iter__(self):
        return self

    def __next__(self) -> dict:
        if not self._ptr.value or not _lib.rs_next(self._ptr):
            raise StopIteration
        row = {}
        for i in range(self._num_cols):
            data_ptr = ctypes.c_void_p()
            data_len = ctypes.c_size_t()
            _lib.rs_get_column(self._ptr, i, ctypes.byref(data_ptr), ctypes.byref(data_len))
            if data_ptr.value:
                row[i] = ctypes.string_at(data_ptr, data_len.value)
            else:
                row[i] = None
        return row

    def close(self):
        if self._ptr.value:
            _lib.rs_close(self._ptr)
            self._ptr.value = 0

    def __del__(self):
        self.close()


class _QueryBuilder:
    def __init__(self, db_ptr, table: str):
        self._ptr = _lib.db_query(db_ptr, table.encode())
        self._db_ref = db_ptr

    def filter(self, column: str, op: str, value) -> "_QueryBuilder":
        v = str(value).encode()
        _check(_lib.qb_filter(self._ptr, column.encode(), op.encode(), ctypes.c_char_p(v)))
        return self

    def sort(self, column: str, ascending: bool = True) -> "_QueryBuilder":
        _check(_lib.qb_sort(self._ptr, column.encode(), 1 if ascending else 0))
        return self

    def limit(self, n: int) -> "_QueryBuilder":
        _check(_lib.qb_limit(self._ptr, n))
        return self

    def join(self, table: str, on_local: str, on_foreign: str) -> "_QueryBuilder":
        _check(_lib.qb_join(self._ptr, table.encode(), on_local.encode(), on_foreign.encode()))
        return self

    def where(self, cond: "Condition") -> "_QueryBuilder":
        _check(_lib.qb_where(self._ptr, cond._ptr))
        return self

    def exec(self) -> _ResultSet:
        rs_ptr = _lib.qb_exec(self._ptr)
        return _ResultSet(rs_ptr, self._db_ref)

    def close(self):
        if self._ptr:
            _lib.qb_destroy(self._ptr)
            self._ptr = None

    def __del__(self):
        self.close()


class Condition:
    """Condition tree node for multi-condition filters.

    Supports Python operators:
        & (AND), | (OR), ~ (NOT)

    Usage:
        c1 = Condition("age", ">", 18)
        c2 = Condition("name", "=", "alice")
        root = c1 & c2          # AND
        root = c1 | c2          # OR
        root = ~c1              # NOT
        db.query("users").where(root).exec()
    """

    def __init__(self, column: str, op: str, value):
        v = str(value).encode()
        self._ptr = ctypes.c_void_p(
            _lib.qb_cond(None, column.encode(), op.encode(), ctypes.c_char_p(v))
        )

    @classmethod
    def _from_ptr(cls, ptr):
        cond = cls.__new__(cls)
        cond._ptr = ptr
        return cond

    def __and__(self, other: "Condition") -> "Condition":
        return Condition._from_ptr(
            ctypes.c_void_p(_lib.qb_cond_and(None, self._ptr, other._ptr))
        )

    def __or__(self, other: "Condition") -> "Condition":
        return Condition._from_ptr(
            ctypes.c_void_p(_lib.qb_cond_or(None, self._ptr, other._ptr))
        )

    def __invert__(self) -> "Condition":
        return Condition._from_ptr(
            ctypes.c_void_p(_lib.qb_cond_not(None, self._ptr))
        )


class Database:
    """KanbuDB embedded database handle."""

    def __init__(self, path: str, *, fsync: str = "periodic",
                 cache_size: int = 0, memtable_size: int = 4 * 1024 * 1024):
        fsync_map = {"none": 0, "periodic": 1, "always": 2}
        cfg = _DbConfig(fsync_map.get(fsync, 1), cache_size, memtable_size, 1)
        self._ptr = ctypes.c_void_p()
        rc = _lib.db_open(path.encode(), ctypes.byref(cfg), ctypes.byref(self._ptr))
        if rc != KANBUDB_OK:
            raise KanbuDBError(rc)
        self._table_schemas: dict[str, dict[str, str]] = {}

    # ── Schema ────────────────────────────────────────────────────

    def create_table(self, name: str, columns: dict[str, str], pk: str):
        """columns: {"col_name": "int64|string|int32|float|double|blob|bool", ...}"""
        col_names = (ctypes.c_char_p * len(columns))()
        col_types = (ctypes.c_int * len(columns))()
        for i, (cname, ctype) in enumerate(columns.items()):
            col_names[i] = cname.encode()
            col_types[i] = COL_TYPES[ctype]
        rc = _lib.db_create_table(self._ptr, name.encode(), col_names, col_types, len(columns), pk.encode())
        _check(rc)
        self._table_schemas[name] = dict(columns)

    def put_row(self, table: str, key: str, values: dict[str, Any]):
        """Put a row with properly encoded column values for query/filter/condition support."""
        schema = self._table_schemas.get(table)
        if not schema:
            raise KanbuDBError(KANBUDB_ERR_INVAL, f"unknown table '{table}'")
        buf = bytearray()
        for col_name, col_type_str in schema.items():
            val = values.get(col_name)
            ctype = COL_TYPES[col_type_str]
            if ctype == COL_TYPES["int32"]:
                buf.extend(struct.pack("<i", int(val)))
            elif ctype == COL_TYPES["int64"]:
                buf.extend(struct.pack("<q", int(val)))
            elif ctype == COL_TYPES["float"]:
                buf.extend(struct.pack("<f", float(val)))
            elif ctype == COL_TYPES["double"]:
                buf.extend(struct.pack("<d", float(val)))
            elif ctype == COL_TYPES["bool"]:
                buf.extend(struct.pack("<B", 1 if val else 0))
            else:
                s = str(val).encode()
                buf.extend(struct.pack("<I", len(s)))
                buf.extend(s)
        rc = _lib.db_put(self._ptr, table.encode(), key.encode(), len(key),
                         bytes(buf), len(buf))
        _check(rc)

    # ── CRUD ──────────────────────────────────────────────────────

    def put(self, table: str, key: str, value: Any):
        """value is serialized with str().encode() for simplicity."""
        serialized = str(value).encode()
        rc = _lib.db_put(self._ptr, table.encode(), key.encode(), len(key), serialized, len(serialized))
        _check(rc)

    def get(self, table: str, key: str) -> Optional[bytes]:
        val_ptr = ctypes.c_void_p()
        val_len = ctypes.c_size_t()
        rc = _lib.db_get(self._ptr, table.encode(), key.encode(), len(key),
                         ctypes.byref(val_ptr), ctypes.byref(val_len))
        if rc == KANBUDB_ERR_NOTFOUND:
            return None
        _check(rc)
        return ctypes.string_at(val_ptr, val_len.value)

    def delete(self, table: str, key: str) -> bool:
        rc = _lib.db_delete(self._ptr, table.encode(), key.encode(), len(key))
        if rc == KANBUDB_ERR_NOTFOUND:
            return False
        _check(rc)
        return True

    # ── Query ─────────────────────────────────────────────────────

    def query(self, table: str) -> _QueryBuilder:
        return _QueryBuilder(self._ptr, table)

    # ── Full-text search ──────────────────────────────────────────

    def fts_create_index(self, table: str, column: str, *,
                         stemming: bool = True, stop_words: bool = True,
                         language: str = "english"):
        opts = _FtsOptions(1 if stemming else 0, 1 if stop_words else 0, language.encode())
        rc = _lib.db_fts_create_index(self._ptr, table.encode(), column.encode(), ctypes.byref(opts))
        _check(rc)

    def fts_search(self, table: str, column: str, query: str) -> _ResultSet:
        rs_ptr = ctypes.c_void_p()
        rc = _lib.db_fts_search(self._ptr, table.encode(), column.encode(),
                                query.encode(), ctypes.byref(rs_ptr))
        if rc != KANBUDB_OK:
            raise KanbuDBError(rc)
        return _ResultSet(rs_ptr.value, self._ptr)

    def fts_drop_index(self, table: str, column: str):
        rc = _lib.db_fts_drop_index(self._ptr, table.encode(), column.encode())
        _check(rc)

    # ── Vector Index ──────────────────────────────────────────────

    def vec_create(self, path: str, params: dict) -> int:
        """Create a vector index.

        Args:
            path: File path for the index (or None for in-memory).
            params: dict with keys:
                - metric: "l2", "cosine", or "ip"
                - dimension: int
                - algo: "flat" or "hnsw" (optional, default "flat")
                - initial_capacity: int (optional)
                - M: int (HNSW, optional)
                - ef_construction: int (HNSW, optional)
                - ef_search: int (HNSW, optional)

        Returns:
            Opaque index handle (int pointer value).
        """
        metric_map = {"l2": VEC_METRIC_L2, "cosine": VEC_METRIC_COSINE, "ip": VEC_METRIC_IP}
        algo_map = {"flat": VEC_ALGO_FLAT, "hnsw": VEC_ALGO_HNSW}
        vp = _VecParams(
            algo=algo_map.get(params.get("algo", "flat"), VEC_ALGO_FLAT),
            metric=metric_map.get(params.get("metric", "l2"), VEC_METRIC_L2),
            dimension=params.get("dimension", 0),
            initial_capacity=params.get("initial_capacity", 0),
            enable_persistence=1 if path else 0,
            M=params.get("M", 16),
            ef_construction=params.get("ef_construction", 200),
            ef_search=params.get("ef_search", 50),
        )
        out = ctypes.c_void_p()
        rc = _lib.kanbudb_vec_create(
            path.encode() if path else None, ctypes.byref(vp), ctypes.byref(out)
        )
        _check(rc)
        return out.value

    def vec_open(self, path: str) -> int:
        """Open an existing vector index from disk."""
        out = ctypes.c_void_p()
        rc = _lib.kanbudb_vec_open(path.encode(), ctypes.byref(out))
        _check(rc)
        return out.value

    def vec_close(self, idx: int):
        """Close a vector index handle."""
        _check(_lib.kanbudb_vec_close(ctypes.c_void_p(idx)))

    def vec_insert(self, idx: int, vid: int, vector: list):
        """Insert a vector into the index.

        Args:
            idx: Index handle from vec_create/vec_open.
            vid: Unique 64-bit ID for the vector.
            vector: List of floats (length must match dimension).
        """
        arr = (ctypes.c_float * len(vector))(*vector)
        _check(_lib.kanbudb_vec_insert(ctypes.c_void_p(idx), ctypes.c_uint64(vid), arr))

    def vec_delete(self, idx: int, vid: int):
        """Delete a vector by ID."""
        _check(_lib.kanbudb_vec_delete(ctypes.c_void_p(idx), ctypes.c_uint64(vid)))

    def vec_search(self, idx: int, query: list, k: int = 10):
        """Search for nearest neighbors.

        Args:
            idx: Index handle.
            query: List of floats (query vector).
            k: Number of nearest neighbors to return.

        Returns:
            List of (id, distance) tuples, sorted by distance ascending.
        """
        qarr = (ctypes.c_float * len(query))(*query)
        results = (_VecResult * k)()
        n = _lib.kanbudb_vec_search(ctypes.c_void_p(idx), qarr, ctypes.c_uint32(k), results)
        if n < 0:
            raise KanbuDBError(n)
        return [(results[i].id, results[i].distance) for i in range(n)]

    def vec_count(self, idx: int) -> int:
        """Return the number of vectors in the index."""
        return _lib.kanbudb_vec_count(ctypes.c_void_p(idx))

    def vec_dimension(self, idx: int) -> int:
        """Return the dimension of vectors in the index."""
        return _lib.kanbudb_vec_dimension(ctypes.c_void_p(idx))

    # ── Lifecycle ─────────────────────────────────────────────────

    def close(self):
        if self._ptr and self._ptr.value:
            _lib.db_close(self._ptr)
            self._ptr.value = 0

    def __enter__(self):
        return self

    def __exit__(self, *args):
        self.close()

    def __del__(self):
        self.close()


def open(path: str, **kw) -> Database:
    """Open (or create) a KanbuDB database.

    Args:
        path: Database file path (creates path.wal + path.lsm).
        fsync: "none", "periodic" (default), or "always".
        cache_size: Page cache size in bytes (0 = auto).
        memtable_size: Memtable size in bytes (default 4MB).

    Returns:
        Database instance.
    """
    return Database(path, **kw)
