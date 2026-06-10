"""Test KanbuDB Python bindings."""
import os
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "bindings", "python"))
from kanbudb import open as kanbudb_open, KanbuDBError


def test_lifecycle():
    with kanbudb_open("/tmp/kanbudb_test_py") as db:
        assert db is not None
    print("  PASS: lifecycle")


def test_create_table():
    with kanbudb_open("/tmp/kanbudb_test_py") as db:
        db.create_table("users", {"id": "string", "name": "string", "age": "int32"}, pk="id")
    print("  PASS: create_table")


def test_crud():
    with kanbudb_open("/tmp/kanbudb_test_py") as db:
        db.create_table("t1", {"id": "string", "val": "string"}, pk="id")
        db.put("t1", "k1", "hello")
        val = db.get("t1", "k1")
        assert val == b"hello", f"got {val}"
        assert db.get("t1", "nonexistent") is None
        db.delete("t1", "k1")
        assert db.get("t1", "k1") is None
    print("  PASS: crud")


def test_fts():
    with kanbudb_open("/tmp/kanbudb_test_py") as db:
        db.create_table("docs", {"id": "string", "body": "string"}, pk="id")
        db.fts_create_index("docs", "body")
        db.put("docs", "d1", "hello world database")
        rs = db.fts_search("docs", "body", "hello")
        assert rs is not None
        rs.close()
    print("  PASS: fts")


def cleanup():
    for f in ["/tmp/kanbudb_test_py.wal", "/tmp/kanbudb_test_py.lsm"]:
        try:
            os.unlink(f)
        except FileNotFoundError:
            pass


if __name__ == "__main__":
    cleanup()
    test_lifecycle()
    test_create_table()
    test_crud()
    test_fts()
    cleanup()
    print("\nAll Python tests passed!")
