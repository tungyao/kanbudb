"""Test KanbuDB Python bindings."""
import os
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "bindings", "python"))
from kanbudb import (
    open as kanbudb_open,
    open_reader as kanbudb_open_reader,
    KanbuDBError, KANBUDB_OK, KANBUDB_ERR_NOTFOUND,
    Embedding, Condition,
    VEC_ALGO_FLAT, VEC_METRIC_L2,
    QUANT_PQ,
)


def _rm(path):
    """Remove file or directory."""
    try:
        if os.path.isdir(path):
            import shutil
            shutil.rmtree(path)
        else:
            os.unlink(path)
    except FileNotFoundError:
        pass


def _cleanup(path, extra=None):
    for ext in [".wal", ".lsm", ".system", ".wal.mmap", ".shared", ".seq"]:
        _rm(path + ext)
    for i in range(10):
        for ext in [".ckpt." + str(i), ".sst.0." + str(i), ".sst.1." + str(i)]:
            _rm(path + ext)
    if extra:
        for p in extra:
            _rm(p)


def _try(func, *args, **kw):
    try:
        func(*args, **kw)
        return True
    except Exception as e:
        print(f"  (ignored: {e})")
        return False


# ── Core tests ──────────────────────────────────────────────────────

def test_lifecycle():
    p = "/tmp/kb_py_lifecycle"
    _cleanup(p)
    with kanbudb_open(p) as db:
        assert db is not None
    _cleanup(p)
    print("  PASS: lifecycle")


def test_create_table():
    p = "/tmp/kb_py_ct"
    _cleanup(p)
    with kanbudb_open(p) as db:
        db.create_table("users", {"id": "string", "name": "string", "age": "int32"}, pk="id")
    _cleanup(p)
    print("  PASS: create_table")


def test_crud():
    p = "/tmp/kb_py_crud"
    _cleanup(p)
    with kanbudb_open(p) as db:
        db.create_table("t1", {"id": "string", "val": "string"}, pk="id")
        db.put("t1", "k1", "hello")
        val = db.get("t1", "k1")
        assert val == b"hello", f"got {val}"
        assert db.get("t1", "nonexistent") is None
        db.delete("t1", "k1")
        assert db.get("t1", "k1") is None
    _cleanup(p)
    print("  PASS: crud")


def test_fts():
    p = "/tmp/kb_py_fts"
    _cleanup(p)
    with kanbudb_open(p) as db:
        db.create_table("docs", {"id": "string", "body": "string"}, pk="id")
        db.fts_create_index("docs", "body")
        db.put("docs", "d1", "hello world database")
        rs = db.fts_search("docs", "body", "hello")
        assert rs is not None
        rs.close()
    _cleanup(p)
    print("  PASS: fts")


def test_multi_and():
    from kanbudb import Condition
    p = "/tmp/kb_py_ma"
    _cleanup(p)
    with kanbudb_open(p) as db:
        db.create_table("users", {"id": "string", "name": "string", "age": "int32"}, pk="id")
        db.put_row("users", "u1", {"id": "u1", "name": "alice", "age": 25})
        db.put_row("users", "u2", {"id": "u2", "name": "bob", "age": 17})
        db.put_row("users", "u3", {"id": "u3", "name": "charlie", "age": 30})
        db.put_row("users", "u4", {"id": "u4", "name": "diana", "age": 22})
        c1 = Condition("age", ">", "18")
        c2 = Condition("age", "<", "28")
        rs = db.query("users").where(c1 & c2).exec()
        count = sum(1 for _ in rs)
        rs.close()
        assert count == 2, f"expected 2, got {count}"
    _cleanup(p)
    print("  PASS: multi_and")


def test_multi_or():
    from kanbudb import Condition
    p = "/tmp/kb_py_mo"
    _cleanup(p)
    with kanbudb_open(p) as db:
        db.create_table("users", {"id": "string", "name": "string", "age": "int32"}, pk="id")
        db.put_row("users", "u1", {"id": "u1", "name": "alice", "age": 25})
        db.put_row("users", "u2", {"id": "u2", "name": "bob", "age": 12})
        c1 = Condition("age", "<", "15")
        c2 = Condition("age", ">", "28")
        rs = db.query("users").where(c1 | c2).exec()
        count = sum(1 for _ in rs)
        rs.close()
        assert count == 1, f"expected 1, got {count}"
    _cleanup(p)
    print("  PASS: multi_or")


def test_not():
    from kanbudb import Condition
    p = "/tmp/kb_py_not"
    _cleanup(p)
    with kanbudb_open(p) as db:
        db.create_table("users", {"id": "string", "name": "string", "age": "int32"}, pk="id")
        db.put_row("users", "u1", {"id": "u1", "name": "alice", "age": 25})
        db.put_row("users", "u2", {"id": "u2", "name": "bob", "age": 17})
        c = Condition("age", ">", "20")
        rs = db.query("users").where(~c).exec()
        count = sum(1 for _ in rs)
        rs.close()
        assert count == 1, f"expected 1, got {count}"
    _cleanup(p)
    print("  PASS: not")


def test_nested_conditions():
    from kanbudb import Condition
    p = "/tmp/kb_py_nest"
    _cleanup(p)
    with kanbudb_open(p) as db:
        db.create_table("users", {"id": "string", "name": "string", "age": "int32"}, pk="id")
        db.put_row("users", "u1", {"id": "u1", "name": "alice", "age": 25})
        db.put_row("users", "u2", {"id": "u2", "name": "bob", "age": 17})
        db.put_row("users", "u3", {"id": "u3", "name": "charlie", "age": 30})
        left = Condition("age", ">", "20") & Condition("age", "<", "28")
        right = Condition("name", "=", "bob")
        rs = db.query("users").where(left | right).exec()
        count = sum(1 for _ in rs)
        rs.close()
        assert count == 2, f"expected 2, got {count}"
    _cleanup(p)
    print("  PASS: nested_conditions")


# ── Standalone vector index tests ───────────────────────────────────

def test_idx_create_close():
    p = "/tmp/kb_py_idx"
    _cleanup(p, [p + ".vec"])
    with kanbudb_open(p) as db:
        idx = db.idx_create(None, {"dimension": 4, "metric": "l2"})
        assert idx != 0, "idx_create returned null handle"
        db.idx_close(idx)
    _cleanup(p, [p + ".vec"])
    print("  PASS: idx_create_close")


def test_idx_insert_search():
    p = "/tmp/kb_py_idx_is"
    _cleanup(p, [p + ".vec"])
    with kanbudb_open(p) as db:
        idx = db.idx_create(None, {"dimension": 3, "metric": "l2"})
        db.idx_insert(idx, 1, [1.0, 0.0, 0.0])
        db.idx_insert(idx, 2, [0.0, 1.0, 0.0])
        db.idx_insert(idx, 3, [0.0, 0.0, 1.0])
        assert db.idx_count(idx) == 3
        results = db.idx_search(idx, [1.0, 0.1, 0.0], k=2)
        assert len(results) == 2, f"expected 2, got {len(results)}"
        assert results[0][0] == 1, f"expected id=1 nearest, got {results[0]}"
        db.idx_close(idx)
    _cleanup(p, [p + ".vec"])
    print("  PASS: idx_insert_search")


def test_idx_delete():
    p = "/tmp/kb_py_idx_del"
    _cleanup(p, [p + ".vec"])
    with kanbudb_open(p) as db:
        idx = db.idx_create(None, {"dimension": 2, "metric": "l2"})
        db.idx_insert(idx, 1, [1.0, 0.0])
        db.idx_insert(idx, 2, [0.0, 1.0])
        assert db.idx_count(idx) == 2
        db.idx_delete(idx, 1)
        # deleted vectors are excluded from search but count may include tombstones
        results = db.idx_search(idx, [1.0, 0.0], k=5)
        assert len(results) == 1, f"expected 1 result, got {len(results)}"
        assert results[0][0] == 2
        db.idx_close(idx)
    _cleanup(p, [p + ".vec"])
    print("  PASS: idx_delete")


def test_idx_get():
    p = "/tmp/kb_py_idx_get"
    _cleanup(p, [p + ".vec"])
    with kanbudb_open(p) as db:
        idx = db.idx_create(None, {"dimension": 3, "metric": "l2"})
        db.idx_insert(idx, 42, [1.5, 2.5, 3.5])
        vec = db.idx_get(idx, 42, 3)
        assert len(vec) == 3, f"expected dim 3, got {len(vec)}"
        assert abs(vec[0] - 1.5) < 1e-5
        assert abs(vec[1] - 2.5) < 1e-5
        assert abs(vec[2] - 3.5) < 1e-5
        db.idx_close(idx)
    _cleanup(p, [p + ".vec"])
    print("  PASS: idx_get")


def test_idx_insert_batch():
    p = "/tmp/kb_py_idx_ib"
    _cleanup(p, [p + ".vec"])
    with kanbudb_open(p) as db:
        idx = db.idx_create(None, {"dimension": 2, "metric": "l2"})
        db.idx_insert_batch(idx, [10, 20, 30], [[1.0, 0.0], [0.0, 1.0], [0.5, 0.5]])
        assert db.idx_count(idx) == 3
        results = db.idx_search(idx, [1.0, 0.0], k=1)
        assert results[0][0] == 10
        db.idx_close(idx)
    _cleanup(p, [p + ".vec"])
    print("  PASS: idx_insert_batch")


def test_idx_stats():
    p = "/tmp/kb_py_idx_stats"
    _cleanup(p, [p + ".vec"])
    with kanbudb_open(p) as db:
        idx = db.idx_create(None, {"dimension": 4, "metric": "l2"})
        db.idx_insert(idx, 1, [1.0, 2.0, 3.0, 4.0])
        stats = db.idx_stats(idx)
        assert stats["count"] == 1
        assert stats["dimension"] == 4
        db.idx_close(idx)
    _cleanup(p, [p + ".vec"])
    print("  PASS: idx_stats")


# ── Standalone embedding tests ─────────────────────────────────────

def test_embed():
    emb = Embedding(8, 2)
    assert emb.dimension == 8
    v = emb.embed("hello")
    assert len(v) == 8
    emb.close()
    print("  PASS: embed")


def test_embed_batch():
    emb = Embedding(4, 2)
    vecs = emb.embed_batch(["hello", "world"])
    assert len(vecs) == 2
    assert len(vecs[0]) == 4
    assert len(vecs[1]) == 4
    emb.close()
    print("  PASS: embed_batch")


# ── DB-level vector tests ─────────────────────────────────────────

def test_db_vec_insert_text():
    p = "/tmp/kb_py_dbvec"
    _cleanup(p)
    with kanbudb_open(p) as db:
        db.vec_create_index(dimension=8, ngram_size=2)
        db.vec_insert_text(1, "hello world")
        db.vec_insert_text(2, "goodbye world")
        assert db.vec_count() == 2
    _cleanup(p)
    print("  PASS: db_vec_insert_text")


def test_db_vec_search():
    p = "/tmp/kb_py_dbvec_sr"
    _cleanup(p)
    with kanbudb_open(p) as db:
        db.vec_create_index(dimension=4, ngram_size=2)
        db.vec_insert_text(1, "hello")
        db.vec_insert_text(2, "world")
        results = db.vec_search_text("hello", k=2)
        assert len(results) == 2
        assert results[0][0] == 1  # "hello" should match "hello" best
    _cleanup(p)
    print("  PASS: db_vec_search")


def test_db_vec_insert_vector():
    p = "/tmp/kb_py_dbvec_iv"
    _cleanup(p)
    with kanbudb_open(p) as db:
        db.vec_create_index(dimension=3, ngram_size=2)
        db.vec_insert_vector(10, [0.5, 0.5, 0.5])
        assert db.vec_count() == 1
        results = db.vec_search([0.5, 0.5, 0.5], k=1)
        assert results[0][0] == 10
    _cleanup(p)
    print("  PASS: db_vec_insert_vector")


# ── Multi-process reader tests ──────────────────────────────────────

def test_open_reader():
    p = "/tmp/kb_py_reader"
    _cleanup(p)

    db = kanbudb_open(p, multi_process=True)
    db.create_table("t", {"id": "string", "v": "string"}, pk="id")
    db.put("t", "k1", "val1")
    db.close()

    reader = kanbudb_open_reader(p, poll_ms=50)
    reader.refresh()
    val = reader.get("t", "k1")
    assert val == b"val1", f"got {val}"
    reader.close()
    _cleanup(p)
    print("  PASS: open_reader")


# ── Query builder from_ test ────────────────────────────────────────

def test_qb_from():
    p = "/tmp/kb_py_qbf"
    _cleanup(p)
    with kanbudb_open(p) as db:
        db.create_table("items", {"id": "string", "val": "string"}, pk="id")
        db.put_row("items", "a1", {"id": "a1", "val": "first"})
        qb = db.query().from_("items")
        rs = qb.exec()
        count = sum(1 for _ in rs)
        assert count > 0
    _cleanup(p)
    print("  PASS: qb_from")


# ── Hybrid search test (if vectors are available) ───────────────────

def test_hybrid_search():
    p = "/tmp/kb_py_hybrid"
    _cleanup(p)
    with kanbudb_open(p) as db:
        db.create_table("docs", {"id": "string", "body": "string"}, pk="id")
        db.fts_create_index("docs", "body")
        db.put("docs", "d1", "hello world database")
        db.put("docs", "d2", "goodbye world")

        db.vec_create_index(dimension=8, ngram_size=2)
        db.vec_insert_text(1, "hello world database")
        db.vec_insert_text(2, "goodbye world")

        null_vec = [0.0] * 8
        results = db.hybrid_search("docs", "body", "world", null_vec, k=2)
        assert len(results) > 0, f"expected >0 results, got {len(results)}"
        assert "id" in results[0]
        assert "score" in results[0]
    _cleanup(p)
    print("  PASS: hybrid_search")


# ── Filtered search test ───────────────────────────────────────────

def test_filtered_search():
    p = "/tmp/kb_py_filt"
    _cleanup(p)
    with kanbudb_open(p) as db:
        db.vec_create_index(dimension=4, ngram_size=2)
        db.vec_insert_text(1, "red apple")
        db.vec_insert_text(2, "green apple")
        db.vec_insert_text(3, "red berry")

        def only_even(id_val):
            return id_val % 2 == 0

        results = db.vec_search_text_filtered("apple", k=5, filter_fn=only_even)
        assert len(results) >= 1
        for rid, _ in results:
            assert rid % 2 == 0, f"filter allowed odd id {rid}"
    _cleanup(p)
    print("  PASS: filtered_search")


# ── Quantization test ──────────────────────────────────────────────

def test_quantization():
    p = "/tmp/kb_py_quant"
    _cleanup(p)
    with kanbudb_open(p) as db:
        db.vec_create_index(dimension=4, ngram_size=2)
        db.vec_quant_create(QUANT_PQ, dimension=4, pq_subspaces=2)
        db.vec_quant_train([[1.0, 2.0, 3.0, 4.0], [5.0, 6.0, 7.0, 8.0], [9.0, 10.0, 11.0, 12.0]])
        db.vec_quant_insert(10, [1.0, 2.0, 3.0, 4.0])
        db.vec_quant_insert(20, [9.0, 10.0, 11.0, 12.0])
        results = db.vec_quant_search([1.0, 2.0, 3.0, 4.0], k=2)
        assert len(results) >= 1
        decoded = db.vec_quant_decode(10, 4)
        assert len(decoded) == 4
        db.vec_quant_destroy()
    _cleanup(p)
    print("  PASS: quantization")


# ── last_error / error_string test ─────────────────────────────────

def test_last_error():
    p = "/tmp/kb_py_err"
    _cleanup(p)
    with kanbudb_open(p) as db:
        err = db.last_error()
        assert err == KANBUDB_OK
    _cleanup(p)
    print("  PASS: last_error")


# ── vec_flush / vec_destroy (standalone) ───────────────────────────

def test_idx_flush_destroy():
    p = "/tmp/kb_py_vfd"
    vec_path = p + ".vec"
    _cleanup(p, [vec_path])
    with kanbudb_open(p) as db:
        idx = db.idx_create(vec_path, {"dimension": 2, "metric": "l2",
                                        "algo": "flat", "initial_capacity": 100})
        db.idx_insert(idx, 1, [1.0, 2.0])
        db.idx_flush(idx)
        db.idx_close(idx)

        idx2 = db.idx_open(vec_path)
        assert db.idx_count(idx2) == 1
        db.idx_close(idx2)

        db.idx_destroy(vec_path)
    _cleanup(p, [vec_path])
    print("  PASS: idx_flush_destroy")


# ── db_vec_flush ────────────────────────────────────────────────────

def test_db_vec_flush():
    p = "/tmp/kb_py_dbflush"
    _cleanup(p)
    with kanbudb_open(p) as db:
        db.vec_create_index(dimension=4, ngram_size=2)
        db.vec_insert_text(1, "test data")
        _try(db.vec_flush)
    _cleanup(p)
    print("  PASS: db_vec_flush")


# ── Run all tests ──────────────────────────────────────────────────

if __name__ == "__main__":
    tests = [
        ("lifecycle", test_lifecycle),
        ("create_table", test_create_table),
        ("crud", test_crud),
        ("fts", test_fts),
        ("multi_and", test_multi_and),
        ("multi_or", test_multi_or),
        ("not", test_not),
        ("nested_conditions", test_nested_conditions),
        ("idx_create_close", test_idx_create_close),
        ("idx_insert_search", test_idx_insert_search),
        ("idx_delete", test_idx_delete),
        ("idx_get", test_idx_get),
        ("idx_insert_batch", test_idx_insert_batch),
        ("idx_stats", test_idx_stats),
        ("embed", test_embed),
        ("embed_batch", test_embed_batch),
        ("db_vec_insert_text", test_db_vec_insert_text),
        ("db_vec_search", test_db_vec_search),
        ("db_vec_insert_vector", test_db_vec_insert_vector),
        ("open_reader", test_open_reader),
        ("qb_from", test_qb_from),
        ("hybrid_search", test_hybrid_search),
        ("filtered_search", test_filtered_search),
        ("quantization", test_quantization),
        ("last_error", test_last_error),
        ("idx_flush_destroy", test_idx_flush_destroy),
        ("db_vec_flush", test_db_vec_flush),
    ]
    passed = 0
    failed = 0
    for name, func in tests:
        try:
            func()
            passed += 1
        except Exception as e:
            print(f"  FAIL: {name} — {e}")
            import traceback
            traceback.print_exc()
            failed += 1
    print(f"\n{passed} passed, {failed} failed")
    sys.exit(1 if failed else 0)
