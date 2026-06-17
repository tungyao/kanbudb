cmake_minimum_required(VERSION 3.14)
project(kanbudb C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

option(KANBUDB_BUILD_TESTS "Build tests" ON)

set(KANBUDB_SOURCES
  src/util/arena.c
  src/util/page_cache.c
  src/util/fst.c
  src/util/bloom.c
  src/storage/wal.c
  src/storage/lsm.c
  src/storage/btree.c
  src/storage/compaction.c
  src/storage/sstable.c
  src/core/db.c
  src/query/query_builder.c
  src/query/row_format.c
  src/fts/tokenizer.c
  src/fts/index.c
  src/fts/parser.c
  src/fts/ranker.c
  src/vector/vector.c
  src/vector/distance.c
  src/vector/persistence.c
  src/vector/hnsw.c
  src/vector/embedding.c
  src/vector/quantize.c
  src/vector/vec_filter.c
  src/hybrid/hybrid.c
  src/multi/multi.c
)

add_library(kanbudb_static STATIC ${KANBUDB_SOURCES})
target_include_directories(kanbudb_static PUBLIC include src/util src/storage src src/vector src/hybrid src/fts)

add_library(kanbudb_shared SHARED ${KANBUDB_SOURCES})
target_include_directories(kanbudb_shared PUBLIC include src/util src/storage src src/vector src/hybrid src/fts)

find_package(Threads REQUIRED)
target_link_libraries(kanbudb_static PRIVATE Threads::Threads m)
target_link_libraries(kanbudb_shared PRIVATE Threads::Threads m)

find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_check_modules(LIBURING liburing QUIET)
  if(LIBURING_FOUND)
    target_compile_definitions(kanbudb_static PUBLIC KANBUDB_HAVE_IOURING)
    target_compile_definitions(kanbudb_shared PUBLIC KANBUDB_HAVE_IOURING)
    target_include_directories(kanbudb_static PUBLIC ${LIBURING_INCLUDE_DIRS})
    target_include_directories(kanbudb_shared PUBLIC ${LIBURING_INCLUDE_DIRS})
    target_link_libraries(kanbudb_static PUBLIC ${LIBURING_LIBRARIES})
    target_link_libraries(kanbudb_shared PUBLIC ${LIBURING_LIBRARIES})
    message(STATUS "liburing found — io_uring WAL enabled")
  endif()
endif()

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

  add_executable(test_thread_safety test/unit/test_thread_safety.c)
  target_include_directories(test_thread_safety PRIVATE src/util src/storage src)
  target_link_libraries(test_thread_safety kanbudb_static Threads::Threads)
  add_test(NAME test_thread_safety COMMAND test_thread_safety)

  add_executable(test_vector test/unit/test_vector.c)
  target_include_directories(test_vector PRIVATE src/vector)
  target_link_libraries(test_vector kanbudb_static m)
  add_test(NAME test_vector COMMAND test_vector)

  add_executable(test_hnsw test/unit/test_hnsw.c)
  target_include_directories(test_hnsw PRIVATE src/vector)
  target_link_libraries(test_hnsw kanbudb_static m)
  add_test(NAME test_hnsw COMMAND test_hnsw)

  add_executable(test_embed test/unit/test_embed.c)
  target_include_directories(test_embed PRIVATE src/vector src)
  target_link_libraries(test_embed kanbudb_static m)
  add_test(NAME test_embed COMMAND test_embed)

  add_executable(test_quantize test/unit/test_quantize.c)
  target_include_directories(test_quantize PRIVATE src/vector)
  target_link_libraries(test_quantize kanbudb_static m)
  add_test(NAME test_quantize COMMAND test_quantize)

  add_executable(test_hybrid test/unit/test_hybrid.c)
  target_include_directories(test_hybrid PRIVATE src/vector src/hybrid src/fts src)
  target_link_libraries(test_hybrid kanbudb_static m)
  add_test(NAME test_hybrid COMMAND test_hybrid)

  add_executable(test_vec_filter test/unit/test_vec_filter.c)
  target_include_directories(test_vec_filter PRIVATE src/vector)
  target_link_libraries(test_vec_filter kanbudb_static m)
  add_test(NAME test_vec_filter COMMAND test_vec_filter)

  add_executable(bench_vector test/bench/bench_vector.c)
  target_include_directories(bench_vector PRIVATE src/vector)
  target_link_libraries(bench_vector kanbudb_static m)

  add_executable(bench_basic test/bench/bench_basic.c)
  target_include_directories(bench_basic PRIVATE src)
  target_link_libraries(bench_basic kanbudb_static)

  add_executable(bench_persist test/bench/bench_persist.c)
  target_include_directories(bench_persist PRIVATE src src/storage)
  target_link_libraries(bench_persist kanbudb_static)

  add_executable(bench_thread_safety test/bench/bench_thread_safety.c)
  target_include_directories(bench_thread_safety PRIVATE src/vector)
  target_link_libraries(bench_thread_safety kanbudb_static Threads::Threads m)

  add_executable(test_bloom test/unit/test_bloom.c)
  target_include_directories(test_bloom PRIVATE src/util)
  target_link_libraries(test_bloom kanbudb_static m)
  add_test(NAME test_bloom COMMAND test_bloom)

  add_executable(test_wal_io test/unit/test_wal_io.c)
  target_include_directories(test_wal_io PRIVATE src/util src/storage)
  target_link_libraries(test_wal_io kanbudb_static)
  add_test(NAME test_wal_io COMMAND test_wal_io)

  add_executable(test_distance_simd test/unit/test_distance_simd.c)
  target_include_directories(test_distance_simd PRIVATE src/vector src/util)
  target_link_libraries(test_distance_simd kanbudb_static m)
  add_test(NAME test_distance_simd COMMAND test_distance_simd)

  add_executable(test_leveled_compact test/unit/test_leveled_compact.c)
  target_include_directories(test_leveled_compact PRIVATE src/storage src/util)
  target_link_libraries(test_leveled_compact kanbudb_static)
  add_test(NAME test_leveled_compact COMMAND test_leveled_compact)

  add_executable(test_lock_stress test/unit/test_lock_stress.c)
  target_include_directories(test_lock_stress PRIVATE src)
  target_link_libraries(test_lock_stress kanbudb_static Threads::Threads)
  add_test(NAME test_lock_stress COMMAND test_lock_stress)

  add_executable(test_multi_process test/unit/test_multi_process.c)
  target_include_directories(test_multi_process PRIVATE src)
  target_link_libraries(test_multi_process kanbudb_static)
  add_test(NAME test_multi_process COMMAND test_multi_process)

  add_executable(test_multi_stress test/unit/test_multi_stress.c)
  target_include_directories(test_multi_stress PRIVATE src)
  target_link_libraries(test_multi_stress kanbudb_static)
  add_test(NAME test_multi_stress COMMAND test_multi_stress)

  add_executable(test_sstable_bloom test/unit/test_sstable_bloom.c)
  target_include_directories(test_sstable_bloom PRIVATE src/storage src/util)
  target_link_libraries(test_sstable_bloom kanbudb_static)
  add_test(NAME test_sstable_bloom COMMAND test_sstable_bloom)
endif()

add_custom_target(amalgamate
  COMMAND ${CMAKE_COMMAND} -E env python3 tools/amalgamate.py
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)

install(TARGETS kanbudb_shared LIBRARY DESTINATION . COMPONENT python)
