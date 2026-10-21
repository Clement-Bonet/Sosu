#include "common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void out_of_memory(void) {
    fputs("sosuc: out of memory\n", stderr);
    exit(3);
}

void *sosu_alloc(size_t size) {
    void *ptr = malloc(size ? size : 1);
    if (!ptr) out_of_memory();
    return ptr;
}

void *sosu_realloc(void *ptr, size_t size) {
    void *result = realloc(ptr, size ? size : 1);
    if (!result) out_of_memory();
    return result;
}

char *sosu_strdup(const char *text) {
    size_t length = strlen(text);
    char *copy = sosu_alloc(length + 1);
    memcpy(copy, text, length + 1);
    return copy;
}

char *sosu_vformat(const char *fmt, va_list args) {
    va_list copy;
    va_copy(copy, args);
    int length = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);
    if (length < 0) length = 0;
    char *buffer = sosu_alloc((size_t)length + 1);
    vsnprintf(buffer, (size_t)length + 1, fmt, args);
    return buffer;
}

char *sosu_format(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    char *result = sosu_vformat(fmt, args);
    va_end(args);
    return result;
}
