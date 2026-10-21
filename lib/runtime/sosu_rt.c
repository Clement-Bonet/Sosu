#include "sosu_rt.h"

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static jmp_buf *current_test;

void sosu_fail(const char *loc, const char *message) {
    fflush(stdout);
    if (current_test) {
        fprintf(stderr, "    %s: %s\n", loc, message);
        longjmp(*current_test, 1);
    }
    fprintf(stderr, "sosu: %s: %s\n", loc, message);
    exit(101);
}

/* ---- Arenas ---- */

typedef union {
    long double ld;
    long long ll;
    double d;
    void *p;
} sosu_max_align;

typedef struct sosu_block {
    struct sosu_block *next;
    size_t used;
    size_t cap;
    sosu_max_align data[];
} sosu_block;

struct sosu_arena_state {
    sosu_block *head;
    size_t block_size;
};

sosu_arena sosu_arena_new(uint64_t block_size, const char *loc) {
    sosu_arena arena;
    arena.state = malloc(sizeof *arena.state);
    if (!arena.state) sosu_fail(loc, "out of memory");
    arena.state->head = NULL;
    arena.state->block_size = block_size ? (size_t)block_size : 4096;
    return arena;
}

void *sosu_arena_try_alloc(sosu_arena *arena, size_t size) {
    sosu_arena_state *state = arena->state;
    if (!state) return NULL;
    size_t align = sizeof(sosu_max_align);
    size = (size + align - 1) / align * align;
    if (size == 0) size = align;
    if (!state->head || state->head->cap - state->head->used < size) {
        size_t cap = size > state->block_size ? size : state->block_size;
        sosu_block *block = malloc(sizeof(sosu_block) + cap);
        if (!block) return NULL;
        block->next = state->head;
        block->used = 0;
        block->cap = cap;
        state->head = block;
    }
    void *memory = (char *)state->head->data + state->head->used;
    state->head->used += size;
    memset(memory, 0, size);
    return memory;
}

void *sosu_arena_alloc(sosu_arena *arena, size_t size, const char *loc) {
    if (!arena->state) sosu_fail(loc, "arena used after `free`");
    void *memory = sosu_arena_try_alloc(arena, size);
    if (!memory) sosu_fail(loc, "out of memory");
    return memory;
}

void sosu_arena_free(sosu_arena *arena) {
    if (!arena->state) return;
    sosu_block *block = arena->state->head;
    while (block) {
        sosu_block *next = block->next;
        free(block);
        block = next;
    }
    free(arena->state);
    arena->state = NULL;
}

/* ---- Classes ---- */

const void *sosu_find_interface(const sosu_class *klass, const void *id) {
    for (const sosu_class *c = klass; c; c = c->parent) {
        for (size_t i = 0; i < c->interface_count; i++) {
            if (c->interfaces[i].id == id) return c->interfaces[i].table;
        }
    }
    sosu_fail("?", "interface not implemented"); /* impossible: checked at compile time */
    return NULL;
}

bool sosu_instance_of(const sosu_class *klass, const sosu_class *target) {
    for (const sosu_class *c = klass; c; c = c->parent) {
        if (c == target) return true;
    }
    return false;
}

bool sosu_run_test(void (*test)(void), const char *name) {
    jmp_buf here;
    bool passed = false;
    current_test = &here;
    if (setjmp(here) == 0) {
        test();
        passed = true;
    }
    current_test = NULL;
    printf("test %s ... %s\n", name, passed ? "ok" : "FAILED");
    return passed;
}

int sosu_test_summary(int total, int failed) {
    printf("\n%d test%s, %d passed, %d failed\n", total, total == 1 ? "" : "s", total - failed, failed);
    return failed ? 1 : 0;
}

bool sosu_str_eq(sosu_str a, sosu_str b) {
    return a.len == b.len && (a.len == 0 || memcmp(a.ptr, b.ptr, a.len) == 0);
}

uint32_t sosu_next_char(sosu_str text, size_t *pos) {
    const uint8_t *s = text.ptr + *pos;
    uint32_t c;
    size_t n;
    if (s[0] < 0x80) { c = s[0]; n = 1; }
    else if (s[0] < 0xE0) { c = s[0] & 0x1F; n = 2; }
    else if (s[0] < 0xF0) { c = s[0] & 0x0F; n = 3; }
    else { c = s[0] & 0x07; n = 4; }
    for (size_t i = 1; i < n; i++) c = (c << 6) | (s[i] & 0x3F);
    *pos += n;
    return c;
}

void sosu_print_str(sosu_str text) { fwrite(text.ptr, 1, text.len, stdout); }
void sosu_print_i64(int64_t value) { printf("%lld", (long long)value); }
void sosu_print_u64(uint64_t value) { printf("%llu", (unsigned long long)value); }
void sosu_print_bool(bool value) { fputs(value ? "true" : "false", stdout); }
void sosu_print_end(void) { putchar('\n'); }

/* Shortest usual form, always with a dot so that a float reads as a float: 25.0, 3.14159. */
void sosu_print_f64(double value) {
    char text[64];
    snprintf(text, sizeof text, "%.15g", value);
    fputs(text, stdout);
    if (!strpbrk(text, ".eni")) fputs(".0", stdout); /* not 1e+20, inf or nan */
}

void sosu_print_char(uint32_t c) {
    unsigned char bytes[4];
    int n;
    if (c < 0x80) {
        bytes[0] = (unsigned char)c;
        n = 1;
    } else if (c < 0x800) {
        bytes[0] = (unsigned char)(0xC0 | (c >> 6));
        bytes[1] = (unsigned char)(0x80 | (c & 0x3F));
        n = 2;
    } else if (c < 0x10000) {
        bytes[0] = (unsigned char)(0xE0 | (c >> 12));
        bytes[1] = (unsigned char)(0x80 | ((c >> 6) & 0x3F));
        bytes[2] = (unsigned char)(0x80 | (c & 0x3F));
        n = 3;
    } else {
        bytes[0] = (unsigned char)(0xF0 | (c >> 18));
        bytes[1] = (unsigned char)(0x80 | ((c >> 12) & 0x3F));
        bytes[2] = (unsigned char)(0x80 | ((c >> 6) & 0x3F));
        bytes[3] = (unsigned char)(0x80 | (c & 0x3F));
        n = 4;
    }
    fwrite(bytes, 1, (size_t)n, stdout);
}
