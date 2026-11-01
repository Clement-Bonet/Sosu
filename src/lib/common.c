#include "common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void sosu_out_of_memory(void) {
    fputs("sosuc: out of memory\n", stderr);
    exit(3);
}

void *sosu_alloc(size_t size) {
    void *ptr = malloc(size ? size : 1);
    if (!ptr) sosu_out_of_memory();
    return ptr;
}

void *sosu_realloc(void *ptr, size_t size) {
    void *result = realloc(ptr, size ? size : 1);
    if (!result) sosu_out_of_memory();
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

static size_t slot_of(const PtrMap *map, const void *key) {
    uintptr_t h = (uintptr_t)key;
    h ^= h >> 17;
    h *= (uintptr_t)0x9E3779B97F4A7C15ull;
    h ^= h >> 29;
    size_t slot = (size_t)h & (map->cap - 1);
    while (map->keys[slot] && map->keys[slot] != key) slot = (slot + 1) & (map->cap - 1);
    return slot;
}

size_t *ptrmap_get(const PtrMap *map, const void *key) {
    if (!map->cap) return NULL;
    size_t slot = slot_of(map, key);
    return map->keys[slot] ? &map->values[slot] : NULL;
}

void ptrmap_put(PtrMap *map, const void *key, size_t value) {
    if (2 * (map->count + 1) > map->cap) { /* half full at most */
        PtrMap bigger = { NULL, NULL, map->cap ? 2 * map->cap : 64, 0 };
        bigger.keys = sosu_alloc(bigger.cap * sizeof *bigger.keys);
        bigger.values = sosu_alloc(bigger.cap * sizeof *bigger.values);
        memset((void *)bigger.keys, 0, bigger.cap * sizeof *bigger.keys);
        for (size_t i = 0; i < map->cap; i++) {
            if (map->keys[i]) ptrmap_put(&bigger, map->keys[i], map->values[i]);
        }
        ptrmap_free(map);
        *map = bigger;
    }
    size_t slot = slot_of(map, key);
    if (!map->keys[slot]) map->count++;
    map->keys[slot] = key;
    map->values[slot] = value;
}

void ptrmap_free(PtrMap *map) {
    free((void *)map->keys);
    free(map->values);
    memset(map, 0, sizeof *map);
}
