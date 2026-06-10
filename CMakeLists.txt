cmake_minimum_required(VERSION 3.14)
project(kanbudb C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

option(KANBUDB_BUILD_TESTS "Build tests" ON)

set(KANBUDB_SOURCES
  src/util/arena.c
  src/util/page_cache.c
  src/util/fst.c
  src/storage/wal.c
  src/storage/lsm.c
  src/storage/btree.c
  src/storage/compaction.c
  src/storage/sstable.c
  src/core/db.c
  src/query/query_builder.c
  src/fts/tokenizer.c
  src/fts/index.c
  src/fts/parser.c
  src/fts/ranker.c
)

add_library(kanbudb_static STATIC ${KANBUDB_SOURCES})
target_include_directories(kanbudb_static PUBLIC include src/util src/storage src)

add_library(kanbudb_shared SHARED ${KANBUDB_SOURCES})
target_include_directories(kanbudb_shared PUBLIC include src/util src/storage src)

if(KANBUDB_BUILD_TESTS)
  enable_testing()
  add_executable(test_arena test/unit/test_arena.c)
  target_include_directories(test_arena PRIVATE src/util)
  target_link_libraries(test_arena kanbudb_static)
  add_test(NAME test_arena COMMAND test_arena)

  add_executable(test_page_cache test/unit/test_page_cache.c)
  target_include_directories(test_page_cache PRIVATE src/util)
  target_link_libraries(test_page_cache kanbudb_static)
  add_test(NAME test_page_cache COMMAND test_page_cache)

  add_executable(test_fst test/unit/test_fst.c)
  target_include_directories(test_fst PRIVATE src/util)
  target_link_libraries(test_fst kanbudb_static)
  add_test(NAME test_fst COMMAND test_fst)

  add_executable(test_wal test/unit/test_wal.c)
  target_include_directories(test_wal PRIVATE src/util src/storage)
  target_link_libraries(test_wal kanbudb_static)
  add_test(NAME test_wal COMMAND test_wal)

  add_executable(test_lsm test/unit/test_lsm.c)
  target_include_directories(test_lsm PRIVATE src/util src/storage)
  target_link_libraries(test_lsm kanbudb_static)
  add_test(NAME test_lsm COMMAND test_lsm)

  add_executable(test_btree test/unit/test_btree.c)
  target_include_directories(test_btree PRIVATE src/util src/storage)
  target_link_libraries(test_btree kanbudb_static)
  add_test(NAME test_btree COMMAND test_btree)

  add_executable(test_db test/unit/test_db.c)
  target_include_directories(test_db PRIVATE src/util src/storage src)
  target_link_libraries(test_db kanbudb_static)
  add_test(NAME test_db COMMAND test_db)

  add_executable(test_query test/unit/test_query.c)
  target_include_directories(test_query PRIVATE src/util src/storage src)
  target_link_libraries(test_query kanbudb_static)
  add_test(NAME test_query COMMAND test_query)

  add_executable(test_tokenizer test/unit/test_tokenizer.c)
  target_include_directories(test_tokenizer PRIVATE src/fts src/util)
  target_link_libraries(test_tokenizer kanbudb_static)
  add_test(NAME test_tokenizer COMMAND test_tokenizer)

  add_executable(test_fts test/unit/test_fts.c)
  target_include_directories(test_fts PRIVATE src/fts src/util)
  target_link_libraries(test_fts kanbudb_static m)
  add_test(NAME test_fts COMMAND test_fts)

  add_executable(test_fts_integration test/unit/test_fts_integration.c)
  target_include_directories(test_fts_integration PRIVATE src/util src/storage src)
  target_link_libraries(test_fts_integration kanbudb_static)
  add_test(NAME test_fts_integration COMMAND test_fts_integration)

  add_executable(test_sstable test/unit/test_sstable.c)
  target_include_directories(test_sstable PRIVATE src/storage src/util)
  target_link_libraries(test_sstable kanbudb_static)
  add_test(NAME test_sstable COMMAND test_sstable)

  add_executable(test_compaction test/unit/test_compaction.c)
  target_include_directories(test_compaction PRIVATE src/storage src/util)
  target_link_libraries(test_compaction kanbudb_static)
  add_test(NAME test_compaction COMMAND test_compaction)

  add_executable(test_persistence test/integration/test_persistence.c)
  target_include_directories(test_persistence PRIVATE src)
  target_link_libraries(test_persistence kanbudb_static)
  add_test(NAME test_persistence COMMAND test_persistence)

  add_executable(test_e2e test/integration/test_e2e.c)
  target_include_directories(test_e2e PRIVATE src)
  target_link_libraries(test_e2e kanbudb_static)
  add_test(NAME test_e2e COMMAND test_e2e)

  add_executable(bench_basic test/bench/bench_basic.c)
  target_include_directories(bench_basic PRIVATE src)
  target_link_libraries(bench_basic kanbudb_static)
endif()

add_custom_target(amalgamate
  COMMAND ${CMAKE_COMMAND} -E env python3 tools/amalgamate.py
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)
