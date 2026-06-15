"""Test KanbuDB Python bindings."""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "bindings", "python"))
from kanbudb import open as kanbudb_open, KanbuDBError


def _cleanup(path):
    for ext in [".wal", ".lsm", ".system"]:
        try:
            os.unlink(path + ext)
        except FileNotFoundError:
            pass


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


if __name__ == "__main__":
    test_lifecycle()
    test_create_table()
    test_crud()
    test_fts()
    test_multi_and()
    test_multi_or()
    test_not()
    test_nested_conditions()
    print("\nAll Python tests passed!")
