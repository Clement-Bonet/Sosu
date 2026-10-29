#ifndef SOSU_COMMON_H
#define SOSU_COMMON_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SOSU_VERSION "0.1.1"

/* Stops sosuc with a message: memory could not be had (exit code 3). */
void sosu_out_of_memory(void);
/* Allocation helpers: they never return NULL (the compiler exits on out of memory). */
void *sosu_alloc(size_t size);
void *sosu_realloc(void *ptr, size_t size);
char *sosu_strdup(const char *text);

/* printf-style formatting into a newly allocated string. */
char *sosu_format(const char *fmt, ...);
char *sosu_vformat(const char *fmt, va_list args);

/* Appends a value to a dynamic array, i.e. any struct with `items`, `count` and `cap`. */
#define SOSU_VEC_PUSH(vec, value)                                                      \
    do {                                                                               \
        if ((vec)->count == (vec)->cap) {                                              \
            (vec)->cap = (vec)->cap ? (vec)->cap * 2 : 16;                             \
            (vec)->items = sosu_realloc((vec)->items, (vec)->cap * sizeof *(vec)->items); \
        }                                                                              \
        (vec)->items[(vec)->count++] = (value);                                        \
    } while (0)

#endif
