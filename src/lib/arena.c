#include "arena.h"

#include <stdlib.h>
#include <string.h>

#define BLOCK_SIZE (64 * 1024)

/* C99 has no max_align_t. */
typedef union {
    long double ld;
    long long ll;
    double d;
    void *p;
} MaxAlign;

struct ArenaBlock {
    ArenaBlock *next;
    size_t used;
    size_t cap;
    MaxAlign data[];
};

void *arena_alloc(Arena *arena, size_t size) {
    size_t align = sizeof(MaxAlign);
    size = (size + align - 1) / align * align;
    if (!arena->head || arena->head->cap - arena->head->used < size) {
        size_t cap = size > BLOCK_SIZE ? size : BLOCK_SIZE;
        ArenaBlock *block = sosu_alloc(sizeof(ArenaBlock) + cap);
        block->next = arena->head;
        block->used = 0;
        block->cap = cap;
        arena->head = block;
    }
    void *ptr = (char *)arena->head->data + arena->head->used;
    arena->head->used += size;
    memset(ptr, 0, size);
    return ptr;
}

void *arena_copy(Arena *arena, const void *data, size_t size) {
    if (size == 0) return NULL;
    void *ptr = arena_alloc(arena, size);
    memcpy(ptr, data, size);
    return ptr;
}

void arena_free(Arena *arena) {
    ArenaBlock *block = arena->head;
    while (block) {
        ArenaBlock *next = block->next;
        free(block);
        block = next;
    }
    arena->head = NULL;
}
