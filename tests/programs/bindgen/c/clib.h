/* Functions of the C library, declared again here so that `sosuc bindgen` binds them (it binds the
   headers of a folder it is given, not those of the system): tests/programs/bindgen (spec §11.6). */
#ifndef CLIB_H
#define CLIB_H

#include <stddef.h>
#include <stdint.h>

#define CLIB_VERSION 3
#define CLIB_FLAG_READ 0x01u
#define CLIB_FLAG_WRITE (1u << 4)
#define CLIB_FLAGS (CLIB_FLAG_READ | CLIB_FLAG_WRITE)
#define CLIB_NAME "clib"
#define CLIB_RATIO 0.5f
#define CLIB_HALF 0.5
#define CLIB_BIG 0x100000000ULL
#define CLIB_NEGATIVE (-2)
#define CLIB_CHAR 'A'
#define CLIB_SIZE ((uint8_t)300)
#define CLIB_MAX(a, b) ((a) > (b) ? (a) : (b))
#define CLIB_HERE __LINE__
#ifdef CLIB_NOT_DEFINED
#define CLIB_BRANCH 1
#else
#define CLIB_BRANCH 2
#endif

typedef struct clib_window clib_window;

typedef enum clib_mode {
    CLIB_MODE_A,
    CLIB_MODE_B = 5,
    CLIB_MODE_C,
    CLIB_MODE_LAST = CLIB_MODE_C + CLIB_VERSION
} clib_mode;

typedef struct clib_point {
    int x, y;
} clib_point;

typedef struct clib_rect {
    clib_point origin;
    float w, h;
    uint8_t flags[3];
    const char *name;
    double grid[2][3];
} clib_rect;

typedef union clib_value {
    int i;
    double d;
    char bytes[12];
} clib_value;

typedef struct clib_bits {
    unsigned a : 3;
    unsigned b : 5;
    int c;
} clib_bits;

typedef struct clib_event {
    uint32_t type;
    union {
        struct { int x, y; } motion;
        uint8_t padding[24];
    };
} clib_event;

typedef int (*clib_compare)(const void *a, const void *b);

typedef struct clib_sorter {
    clib_compare compare;
    void *data;
    size_t size;
    _Bool done;
} clib_sorter;

size_t strlen(const char *text);
int abs(int value);
void *memset(void *bytes, int value, size_t count);
void qsort(void *items, size_t count, size_t size, clib_compare compare);
double floor(double x);
int printf(const char *format, ...);
static inline int clib_twice(int x) { return x * 2; }
clib_window *clib_open(const char *title, int w, int h, uint64_t flags);
int clib_area(clib_rect rect, clib_mode mode, int match);

#endif
