#ifndef SOSU_ARENA_H
#define SOSU_ARENA_H

#include "common.h"

/* Bump allocator for compiler data (AST nodes...): everything is freed at once. */
typedef struct ArenaBlock ArenaBlock;

typedef struct {
    ArenaBlock *head;
} Arena;

/* Returns zeroed memory, aligned for any type. */
void *arena_alloc(Arena *arena, size_t size);
/* Copies `size` bytes into the arena; returns NULL when size is 0. */
void *arena_copy(Arena *arena, const void *data, size_t size);
void arena_free(Arena *arena);

#endif
