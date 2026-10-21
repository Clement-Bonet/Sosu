/* Sosu runtime: included by the C code that sosuc generates (spec §11.3).
   Debug checks (spec §7.7): integer overflow, division by zero, array bounds, contracts. */
#ifndef SOSU_RT_H
#define SOSU_RT_H

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
/* Decodes the code point at *pos (valid UTF-8) and moves *pos after it: `s.chars()`. */
uint32_t sosu_next_char(sosu_str text, size_t *pos);

/* `sosuc test`: runs one test block; returns false if it failed. */
bool sosu_run_test(void (*test)(void), const char *name);
/* Prints the summary of a test run and returns the exit code. */
int sosu_test_summary(int total, int failed);

static inline void sosu_check(bool condition, const char *loc, const char *message) {
    if (!condition) sosu_fail(loc, message);
}

static inline size_t sosu_index(int64_t index, size_t length, const char *loc) {
    if (index < 0 || (uint64_t)index >= length) sosu_fail(loc, "index out of bounds");
    return (size_t)index;
}

static inline size_t sosu_uindex(uint64_t index, size_t length, const char *loc) {
    if (index >= length) sosu_fail(loc, "index out of bounds");
    return (size_t)index;
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
SOSU_UNSIGNED(u8, uint8_t, UINT8_MAX, 8)
SOSU_UNSIGNED(u16, uint16_t, UINT16_MAX, 16)
SOSU_UNSIGNED(u32, uint32_t, UINT32_MAX, 32)
SOSU_UNSIGNED(u64, uint64_t, UINT64_MAX, 64)
SOSU_UNSIGNED(usize, uint64_t, UINT64_MAX, 64)

#endif
