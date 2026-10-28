/* Sosu runtime: included by the C code that sosuc generates (spec §11.3).
   Debug checks (spec §7.7): integer overflow, division by zero, array bounds, contracts. */
#ifndef SOSU_RT_H
#define SOSU_RT_H

#include <setjmp.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    const uint8_t *ptr;
    size_t len; /* in bytes */
} sosu_str;

#define SOSU_STR(literal, length) ((sosu_str){ (const uint8_t *)(literal), (length) })

/* Stops the program (or the current test) with a message: `loc` is "file:line:column". */
void sosu_fail(const char *loc, const char *message);

/* Arenas (spec §6): blocks of memory freed at once. Allocations are zeroed. */
typedef struct sosu_arena_state sosu_arena_state;
typedef struct {
    sosu_arena_state *state;
} sosu_arena;

sosu_arena sosu_arena_new(uint64_t block_size, const char *loc);
void *sosu_arena_alloc(sosu_arena *arena, size_t size, const char *loc); /* stops the program if out of memory */
void *sosu_arena_try_alloc(sosu_arena *arena, size_t size);             /* NULL if out of memory */
void sosu_arena_free(sosu_arena *arena);
/* arena.copy(): a new, empty arena with the same block size */
sosu_arena sosu_arena_copy(const sosu_arena *arena, const char *loc);
/* `count` elements of `size` bytes (arena.alloc_slice); stops the program on overflow. */
void *sosu_arena_alloc_array(sosu_arena *arena, size_t size, uint64_t count, const char *loc);

/* Text (spec §4.1) */
uint8_t *sosu_str_to_cstr(sosu_arena *arena, sosu_str text, const char *loc); /* s.to_cstr(arena) */
bool sosu_utf8_valid(const uint8_t *bytes, size_t length);                   /* str.from_bytes */

/* sosu_rt.c also defines `sosu_errno` and `sosu_file_size` for std.io, which declares them
   itself with `extern "C"` (with its own types): they are not declared here, so that the
   two declarations cannot conflict. */

/* Classes (spec §5.2). Every object starts with a pointer to its class's table of virtual
   methods, which itself starts with this description of the class. */
typedef struct {
    const void *id;    /* address identifying an interface */
    const void *table; /* its methods for this class */
} sosu_iface;

typedef struct sosu_class sosu_class;
struct sosu_class {
    const sosu_class *parent;
    const char *name;
    const sosu_iface *interfaces;
    size_t interface_count;
};

#define SOSU_CLASS_OF(object) (*(const sosu_class *const *)(object))

const void *sosu_find_interface(const sosu_class *klass, const void *id);
bool sosu_instance_of(const sosu_class *klass, const sosu_class *target);

/* io.print */
void sosu_print_str(sosu_str text);
void sosu_print_i64(int64_t value);
void sosu_print_u64(uint64_t value);
void sosu_print_f64(double value);
void sosu_print_bool(bool value);
void sosu_print_char(uint32_t value);
void sosu_print_end(void);

bool sosu_str_eq(sosu_str a, sosu_str b);
int sosu_str_cmp(sosu_str a, sosu_str b);
uint64_t sosu_hash_u64(uint64_t x);
uint64_t sosu_hash_f64(double x);
uint64_t sosu_hash_str(sosu_str text);
/* Decodes the code point at *pos (valid UTF-8) and moves *pos after it: `s.chars()`. */
uint32_t sosu_next_char(sosu_str text, size_t *pos);

/* `sosuc test`: runs one test block; returns false if it failed. */
bool sosu_run_test(void (*test)(void), const char *name);

/* Exports of a library (spec §9.2): each runs under a guard, so that a run-time error returns
   to the host instead of stopping its process. `sosu_last_error` gives the message of the
   last call of this thread, or NULL if it succeeded. */
jmp_buf *sosu_guard_enter(jmp_buf *here);
void sosu_guard_leave(jmp_buf *outer);
const char *sosu_last_error(void);
#if defined(SOSU_SHARED) && defined(_WIN32)
#define SOSU_EXPORT __declspec(dllexport)
#elif defined(SOSU_SHARED)
#define SOSU_EXPORT __attribute__((visibility("default")))
#else
#define SOSU_EXPORT
#endif
/* Prints the summary of a test run and returns the exit code. */
int sosu_test_summary(int total, int failed);

static inline void sosu_check(bool condition, const char *loc, const char *message) {
    if (!condition) sosu_fail(loc, message);
}

/* Values of function type (spec §5.5): the code, and the variables a lambda captured (NULL
   for a function, or a lambda that captures nothing, whose code is then a plain C function).
   A field and a C function hold the code alone, a C function pointer. */
typedef void (*sosu_code)(void);
typedef struct {
    sosu_code code;
    void *env;
} sosu_fn;

static inline sosu_fn sosu_fn_of(sosu_code code) {
    sosu_fn f = { code, NULL };
    return f;
}

/* A value that leaves its function (returned, stored...) must not have captured variables. */
static inline sosu_fn sosu_fn_check(sosu_fn f, const char *loc) {
    if (f.env) sosu_fail(loc, "a lambda that captures variables cannot leave the function that creates it");
    return f;
}

static inline sosu_code sosu_fn_code(sosu_fn f, const char *loc) {
    return sosu_fn_check(f, loc).code;
}

static inline size_t sosu_index(int64_t index, size_t length, const char *loc) {
    if (index < 0 || (uint64_t)index >= length) sosu_fail(loc, "index out of bounds");
    return (size_t)index;
}

static inline size_t sosu_uindex(uint64_t index, size_t length, const char *loc) {
    if (index >= length) sosu_fail(loc, "index out of bounds");
    return (size_t)index;
}

/* x[start..end]: start <= end <= length */
static inline void sosu_check_view(uint64_t start, uint64_t end, size_t length, const char *loc) {
    if (start > end) sosu_fail(loc, "view `x[a..b]` with a > b");
    if (end > length) sosu_fail(loc, "view out of bounds");
}

#define SOSU_SIGNED(T, C, MIN, MAX, BITS)                                                       \
    static inline C sosu_add_##T(C a, C b, const char *loc) {                                    \
        if ((b > 0 && a > MAX - b) || (b < 0 && a < MIN - b)) sosu_fail(loc, "integer overflow in `+`"); \
        return (C)(a + b);                                                                       \
    }                                                                                            \
    static inline C sosu_sub_##T(C a, C b, const char *loc) {                                    \
        if ((b < 0 && a > MAX + b) || (b > 0 && a < MIN + b)) sosu_fail(loc, "integer overflow in `-`"); \
        return (C)(a - b);                                                                       \
    }                                                                                            \
    static inline C sosu_mul_##T(C a, C b, const char *loc) {                                    \
        bool overflow = a > 0 ? (b > 0 ? a > MAX / b : b < MIN / a)                              \
                              : (b > 0 ? a < MIN / b : (a != 0 && b < MAX / a));                 \
        if (overflow) sosu_fail(loc, "integer overflow in `*`");                                 \
        return (C)(a * b);                                                                       \
    }                                                                                            \
    static inline C sosu_div_##T(C a, C b, const char *loc) {                                    \
        if (b == 0) sosu_fail(loc, "division by zero");                                          \
        if (a == MIN && b == -1) sosu_fail(loc, "integer overflow in `/`");                      \
        return (C)(a / b);                                                                       \
    }                                                                                            \
    static inline C sosu_rem_##T(C a, C b, const char *loc) {                                    \
        if (b == 0) sosu_fail(loc, "division by zero");                                          \
        if (b == -1) return 0;                                                                   \
        return (C)(a % b);                                                                       \
    }                                                                                            \
    static inline C sosu_neg_##T(C a, const char *loc) {                                         \
        if (a == MIN) sosu_fail(loc, "integer overflow in `-`");                                 \
        return (C)(-a);                                                                          \
    }                                                                                            \
    static inline C sosu_shl_##T(C a, int64_t n, const char *loc) {                              \
        if (n < 0 || n >= BITS) sosu_fail(loc, "shift amount out of range");                     \
        return (C)((uint64_t)a << n);                                                            \
    }                                                                                            \
    static inline C sosu_shr_##T(C a, int64_t n, const char *loc) {                              \
        if (n < 0 || n >= BITS) sosu_fail(loc, "shift amount out of range");                     \
        return (C)(a >> n);                                                                      \
    }                                                                                            \
    static inline C sosu_f2i_##T(double x, const char *loc) {                                    \
        if (!(x >= (double)MIN && x < (double)MAX + 1.0)) sosu_fail(loc, "float out of range in `as`"); \
        return (C)x;                                                                             \
    }

#define SOSU_UNSIGNED(T, C, MAX, BITS)                                                          \
    static inline C sosu_add_##T(C a, C b, const char *loc) {                                    \
        if (a > MAX - b) sosu_fail(loc, "integer overflow in `+`");                              \
        return (C)(a + b);                                                                       \
    }                                                                                            \
    static inline C sosu_sub_##T(C a, C b, const char *loc) {                                    \
        if (a < b) sosu_fail(loc, "integer overflow in `-`");                                    \
        return (C)(a - b);                                                                       \
    }                                                                                            \
    static inline C sosu_mul_##T(C a, C b, const char *loc) {                                    \
        if (b != 0 && a > MAX / b) sosu_fail(loc, "integer overflow in `*`");                    \
        return (C)(a * b);                                                                       \
    }                                                                                            \
    static inline C sosu_div_##T(C a, C b, const char *loc) {                                    \
        if (b == 0) sosu_fail(loc, "division by zero");                                          \
        return (C)(a / b);                                                                       \
    }                                                                                            \
    static inline C sosu_rem_##T(C a, C b, const char *loc) {                                    \
        if (b == 0) sosu_fail(loc, "division by zero");                                          \
        return (C)(a % b);                                                                       \
    }                                                                                            \
    static inline C sosu_shl_##T(C a, int64_t n, const char *loc) {                              \
        if (n < 0 || n >= BITS) sosu_fail(loc, "shift amount out of range");                     \
        return (C)((uint64_t)a << n);                                                            \
    }                                                                                            \
    static inline C sosu_shr_##T(C a, int64_t n, const char *loc) {                              \
        if (n < 0 || n >= BITS) sosu_fail(loc, "shift amount out of range");                     \
        return (C)(a >> n);                                                                      \
    }                                                                                            \
    static inline C sosu_f2i_##T(double x, const char *loc) {                                    \
        if (!(x >= 0.0 && x < (double)MAX + 1.0)) sosu_fail(loc, "float out of range in `as`");  \
        return (C)x;                                                                             \
    }

SOSU_SIGNED(i8, int8_t, INT8_MIN, INT8_MAX, 8)
SOSU_SIGNED(i16, int16_t, INT16_MIN, INT16_MAX, 16)
SOSU_SIGNED(i32, int32_t, INT32_MIN, INT32_MAX, 32)
SOSU_SIGNED(i64, int64_t, INT64_MIN, INT64_MAX, 64)
SOSU_SIGNED(isize, int64_t, INT64_MIN, INT64_MAX, 64)
SOSU_UNSIGNED(ui8, uint8_t, UINT8_MAX, 8)
SOSU_UNSIGNED(ui16, uint16_t, UINT16_MAX, 16)
SOSU_UNSIGNED(ui32, uint32_t, UINT32_MAX, 32)
SOSU_UNSIGNED(ui64, uint64_t, UINT64_MAX, 64)
SOSU_UNSIGNED(usize, uint64_t, UINT64_MAX, 64)

/* uf32 and uf64 are never negative: checked where overflows are (debug). NaN fails too. */
static inline float sosu_check_uf32(float x, const char *loc) {
    if (!(x >= 0.0f)) sosu_fail(loc, "negative value for an unsigned float");
    return x;
}
static inline double sosu_check_uf64(double x, const char *loc) {
    if (!(x >= 0.0)) sosu_fail(loc, "negative value for an unsigned float");
    return x;
}

#endif
