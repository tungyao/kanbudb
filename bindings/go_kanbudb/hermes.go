// Package kanbudb provides Go bindings for the KanbuDB Embedded Database.
//
// Build the C library first:
//
//	cd kanbudb && cmake -B build && cmake --build build
//
// Then use with CGo:
//
//	export CGO_LDFLAGS="-L$(pwd)/build -lkanbudb_shared"
//	export CGO_CFLAGS="-I$(pwd)/include"
//	go run main.go
package kanbudb

/*
#cgo LDFLAGS: -lkanbudb_shared
#cgo CFLAGS: -I${SRCDIR}/../../include

#include "db.h"
#include <stdlib.h>
*/
import "C"
import (
	"fmt"
	"sync"
	"unsafe"
)

type ColType int

const (
	Int32  ColType = C.KANBUDB_INT32
	Int64  ColType = C.KANBUDB_INT64
	Float  ColType = C.KANBUDB_FLOAT
	Double ColType = C.KANBUDB_DOUBLE
	String ColType = C.KANBUDB_STRING
	Blob   ColType = C.KANBUDB_BLOB
	Bool   ColType = C.KANBUDB_BOOL
)

type FsyncMode int

const (
	FsyncNone     FsyncMode = C.KANBUDB_FSYNC_NONE
	FsyncPeriodic FsyncMode = C.KANBUDB_FSYNC_PERIODIC
	FsyncAlways   FsyncMode = C.KANBUDB_FSYNC_ALWAYS
)

type Config struct {
	FsyncMode         FsyncMode
	CacheSize         int
	MemtableSize      int
	CompactionThreads int
}

type Err struct {
	Code int
	Msg  string
}

func (e *Err) Error() string {
	return fmt.Sprintf("kanbudb: [%d] %s", e.Code, e.Msg)
}

func kanbudbError(rc C.int) error {
	if rc == 0 {
		return nil
	}
	msg := C.GoString(C.db_error_string(rc))
	return &Err{Code: int(rc), Msg: msg}
}

type Database struct {
	ptr  *C.db_t
	mu   sync.Mutex
	open bool
}

func Open(path string, cfg *Config) (*Database, error) {
	cpath := C.CString(path)
	defer C.free(unsafe.Pointer(cpath))

	var ccfg *C.db_config_t
	if cfg != nil {
		ccfg = &C.db_config_t{
			fsync_mode:        C.kanbudb_fsync_mode_t(cfg.FsyncMode),
			cache_size:        C.size_t(cfg.CacheSize),
			memtable_size:     C.size_t(cfg.MemtableSize),
			compaction_threads: C.int(cfg.CompactionThreads),
		}
	}

	var out *C.db_t
	rc := C.db_open(cpath, ccfg, &out)
	if rc != 0 {
		return nil, kanbudbError(rc)
	}
	return &Database{ptr: out, open: true}, nil
}

func (db *Database) Close() error {
	db.mu.Lock()
	defer db.mu.Unlock()
	if !db.open {
		return nil
	}
	rc := C.db_close(db.ptr)
	db.open = false
	return kanbudbError(rc)
}

func (db *Database) CreateTable(name string, colNames []string, colTypes []ColType, pk string) error {
	db.mu.Lock()
	defer db.mu.Unlock()

	cname := C.CString(name)
	defer C.free(unsafe.Pointer(cname))

	n := len(colNames)
	cnames := make([]*C.char, n)
	ctypes := make([]C.kanbudb_col_type_t, n)
	for i := range colNames {
		cnames[i] = C.CString(colNames[i])
		defer C.free(unsafe.Pointer(cnames[i]))
		ctypes[i] = C.kanbudb_col_type_t(colTypes[i])
	}

	cpk := C.CString(pk)
	defer C.free(unsafe.Pointer(cpk))

	rc := C.db_create_table(db.ptr, cname, &cnames[0], &ctypes[0], C.int(n), cpk)
	return kanbudbError(rc)
}

func (db *Database) Put(table, key string, value []byte) error {
	db.mu.Lock()
	defer db.mu.Unlock()

	ctable := C.CString(table)
	defer C.free(unsafe.Pointer(ctable))
	ckey := C.CString(key)
	defer C.free(unsafe.Pointer(ckey))

	var vptr unsafe.Pointer
	vlen := C.size_t(0)
	if len(value) > 0 {
		vptr = unsafe.Pointer(&value[0])
		vlen = C.size_t(len(value))
	}

	rc := C.db_put(db.ptr, ctable, ckey, C.size_t(len(key)), vptr, vlen)
	return kanbudbError(rc)
}

func (db *Database) Get(table, key string) ([]byte, error) {
	db.mu.Lock()
	defer db.mu.Unlock()

	ctable := C.CString(table)
	defer C.free(unsafe.Pointer(ctable))
	ckey := C.CString(key)
	defer C.free(unsafe.Pointer(ckey))

	var val unsafe.Pointer
	var valLen C.size_t
	rc := C.db_get(db.ptr, ctable, ckey, C.size_t(len(key)), &val, &valLen)
	if rc == C.KANBUDB_ERR_NOTFOUND {
		return nil, nil
	}
	if rc != 0 {
		return nil, kanbudbError(rc)
	}
	return C.GoBytes(val, C.int(valLen)), nil
}

func (db *Database) Delete(table, key string) (bool, error) {
	db.mu.Lock()
	defer db.mu.Unlock()

	ctable := C.CString(table)
	defer C.free(unsafe.Pointer(ctable))
	ckey := C.CString(key)
	defer C.free(unsafe.Pointer(ckey))

	rc := C.db_delete(db.ptr, ctable, ckey, C.size_t(len(key)))
	if rc == C.KANBUDB_ERR_NOTFOUND {
		return false, nil
	}
	if rc != 0 {
		return false, kanbudbError(rc)
	}
	return true, nil
}

func (db *Database) Query(table string) *QueryBuilder {
	return &QueryBuilder{
		db:    db,
		table: table,
	}
}

type QueryBuilder struct {
	db    *Database
	table string
	qptr  *C.query_builder_t
	built bool
}

func (qb *QueryBuilder) getPtr() *C.query_builder_t {
	if !qb.built {
		ctable := C.CString(qb.table)
		defer C.free(unsafe.Pointer(ctable))
		qb.qptr = C.db_query(qb.db.ptr, ctable)
		qb.built = true
	}
	return qb.qptr
}

func (qb *QueryBuilder) Filter(column, op string, value interface{}) *QueryBuilder {
	v := fmt.Sprintf("%v", value)
	cv := C.CString(v)
	defer C.free(unsafe.Pointer(cv))
	C.qb_filter(qb.getPtr(), C.CString(column), C.CString(op), unsafe.Pointer(cv))
	return qb
}

func (qb *QueryBuilder) Sort(column string, asc bool) *QueryBuilder {
	ascInt := 0
	if asc {
		ascInt = 1
	}
	C.qb_sort(qb.getPtr(), C.CString(column), C.int(ascInt))
	return qb
}

func (qb *QueryBuilder) Limit(n int) *QueryBuilder {
	C.qb_limit(qb.getPtr(), C.int(n))
	return qb
}

func (qb *QueryBuilder) Join(table, onLocal, onForeign string) *QueryBuilder {
	C.qb_join(qb.getPtr(), C.CString(table), C.CString(onLocal), C.CString(onForeign))
	return qb
}

func (qb *QueryBuilder) Exec() (*ResultSet, error) {
	rs := C.qb_exec(qb.getPtr())
	if rs == nil {
		return nil, fmt.Errorf("kanbudb: query execution returned nil")
	}
	return &ResultSet{ptr: rs, db: qb.db}, nil
}

func (qb *QueryBuilder) Close() {
	if qb.qptr != nil {
		C.qb_destroy(qb.qptr)
		qb.qptr = nil
	}
}

type ResultSet struct {
	ptr    *C.result_set_t
	db     *Database
	closed bool
}

func (rs *ResultSet) Next() bool {
	return C.rs_next(rs.ptr) != 0
}

func (rs *ResultSet) Column(idx int) ([]byte, error) {
	var data unsafe.Pointer
	var length C.size_t
	rc := C.rs_get_column(rs.ptr, C.int(idx), &data, &length)
	if rc != 0 {
		return nil, kanbudbError(rc)
	}
	if data == nil {
		return nil, nil
	}
	return C.GoBytes(data, C.int(length)), nil
}

func (rs *ResultSet) NumColumns() int {
	return int(C.rs_num_columns(rs.ptr))
}

func (rs *ResultSet) Close() {
	if !rs.closed && rs.ptr != nil {
		C.rs_close(rs.ptr)
		rs.closed = true
	}
}

type FtsOptions struct {
	Stemming  bool
	StopWords bool
	Language  string
}

func (db *Database) FtsCreateIndex(table, column string, opts *FtsOptions) error {
	db.mu.Lock()
	defer db.mu.Unlock()

	ctable := C.CString(table)
	defer C.free(unsafe.Pointer(ctable))
	ccol := C.CString(column)
	defer C.free(unsafe.Pointer(ccol))

	var copts *C.fts_options_t
	if opts != nil {
		clang := C.CString(opts.Language)
		defer C.free(unsafe.Pointer(clang))
		copts = &C.fts_options_t{
			enable_stemming:   b2c(opts.Stemming),
			enable_stop_words: b2c(opts.StopWords),
			language:          clang,
		}
	}

	rc := C.db_fts_create_index(db.ptr, ctable, ccol, copts)
	return kanbudbError(rc)
}

func (db *Database) FtsSearch(table, column, query string) (*ResultSet, error) {
	db.mu.Lock()
	defer db.mu.Unlock()

	ctable := C.CString(table)
	defer C.free(unsafe.Pointer(ctable))
	ccol := C.CString(column)
	defer C.free(unsafe.Pointer(ccol))
	cquery := C.CString(query)
	defer C.free(unsafe.Pointer(cquery))

	var out *C.result_set_t
	rc := C.db_fts_search(db.ptr, ctable, ccol, cquery, &out)
	if rc != 0 {
		return nil, kanbudbError(rc)
	}
	return &ResultSet{ptr: out, db: db}, nil
}

func (db *Database) FtsDropIndex(table, column string) error {
	db.mu.Lock()
	defer db.mu.Unlock()

	rc := C.db_fts_drop_index(db.ptr, C.CString(table), C.CString(column))
	return kanbudbError(rc)
}

func b2c(b bool) C.int {
	if b {
		return 1
	}
	return 0
}
