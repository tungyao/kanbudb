// Test KanbuDB Go bindings.
// Run from project root:
//   export CGO_LDFLAGS="-L$(pwd)/build -lkanbudb_shared"
//   export CGO_CFLAGS="-I$(pwd)/include"
//   go run bindings/go_kanbudb/cmd/main.go

package main

import (
	"fmt"
	"os"

	"github.com/kanbudb/go-kanbudb"
)

func cleanup() {
	os.Remove("/tmp/kanbudb_test_go.wal")
	os.Remove("/tmp/kanbudb_test_go.lsm")
}

func testLifecycle() {
	db, err := kanbudb.Open("/tmp/kanbudb_test_go", nil)
	if err != nil {
		panic(err)
	}
	db.Close()
	fmt.Println("  PASS: lifecycle")
}

func testCRUD() {
	db, _ := kanbudb.Open("/tmp/kanbudb_test_go", nil)
	db.CreateTable("t1", []string{"id", "val"}, []kanbudb.ColType{kanbudb.String, kanbudb.String}, "id")

	err := db.Put("t1", "k1", []byte("hello"))
	if err != nil {
		panic(err)
	}

	val, err := db.Get("t1", "k1")
	if err != nil || string(val) != "hello" {
		panic(fmt.Sprintf("unexpected: %s %v", val, err))
	}

	val, err = db.Get("t1", "nope")
	if err != nil || val != nil {
		panic("expected nil for missing key")
	}

	db.Delete("t1", "k1")
	val, _ = db.Get("t1", "k1")
	if val != nil {
		panic("expected nil after delete")
	}

	db.Close()
	fmt.Println("  PASS: crud")
}

func testFTS() {
	db, _ := kanbudb.Open("/tmp/kanbudb_test_go", nil)
	db.CreateTable("docs", []string{"id", "body"}, []kanbudb.ColType{kanbudb.String, kanbudb.String}, "id")
	db.FtsCreateIndex("docs", "body", nil)
	db.Put("docs", "d1", []byte("hello world database"))

	rs, err := db.FtsSearch("docs", "body", "hello")
	if err != nil {
		panic(err)
	}
	rs.Close()
	db.Close()
	fmt.Println("  PASS: fts")
}

func main() {
	cleanup()
	testLifecycle()
	testCRUD()
	testFTS()
	cleanup()
	fmt.Println("\nAll Go tests passed!")
}
