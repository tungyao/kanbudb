#ifndef KANBUDB_MACROS_H
#define KANBUDB_MACROS_H

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

/* Error codes */
#define KANBUDB_OK         0
#define KANBUDB_ERR_OOM    (-1)
#define KANBUDB_ERR_NOTFOUND (-2)
#define KANBUDB_ERR_EXISTS  (-3)
#define KANBUDB_ERR_CORRUPT (-4)
#define KANBUDB_ERR_IO      (-5)
#define KANBUDB_ERR_INVAL   (-6)
#define KANBUDB_ERR_BUSY    (-7)

/* Common types */
typedef int8_t   i8;
typedef int16_t  i16;
typedef int32_t  i32;
typedef int64_t  i64;
typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef float    f32;
typedef double   f64;

/* Allocation helpers */
#define kanbudb_malloc(sz)    malloc(sz)
#define kanbudb_calloc(n, sz) calloc(n, sz)
#define kanbudb_free(p)       free(p)

/* Assertions */
#ifdef NDEBUG
  #define KANBUDB_ASSERT(cond) ((void)0)
#else
  #define KANBUDB_ASSERT(cond) assert(cond)
#endif

/* Compiler hints */
#ifdef __GNUC__
  #define KANBUDB_INLINE inline __attribute__((always_inline))
  #define KANBUDB_UNUSED  __attribute__((unused))
  #define KANBUDB_LIKELY(x)   __builtin_expect(!!(x), 1)
  #define KANBUDB_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
  #define KANBUDB_INLINE inline
  #define KANBUDB_UNUSED
  #define KANBUDB_LIKELY(x)   (x)
  #define KANBUDB_UNLIKELY(x) (x)
#endif

/* Alignment */
#define KANBUDB_ALIGNMENT 8
#define KANBUDB_ALIGN(sz) \
  (((sz) + KANBUDB_ALIGNMENT - 1) & ~(KANBUDB_ALIGNMENT - 1))

/* Min/max */
#define KANBUDB_MIN(a, b) ((a) < (b) ? (a) : (b))
#define KANBUDB_MAX(a, b) ((a) > (b) ? (a) : (b))

#endif /* KANBUDB_MACROS_H */
