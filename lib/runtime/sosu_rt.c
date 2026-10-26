#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L /* clock_gettime, nanosleep */
#endif

#include "sosu_rt.h"

#include <errno.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

/* Variables of each thread: a library is called from several threads at once (spec §9.2). */
#if defined(_MSC_VER)
#define SOSU_THREAD __declspec(thread)
#elif defined(__GNUC__) || defined(__clang__)
#define SOSU_THREAD __thread
#else
#define SOSU_THREAD _Thread_local
#endif

/* The guard of the running test or export: a run-time error jumps back to it. A test prints
   the message; an export keeps it for `<package>_error()`. */
static SOSU_THREAD jmp_buf *current_guard;
static SOSU_THREAD bool guard_prints;
static SOSU_THREAD char last_error[512];
static SOSU_THREAD bool has_error;

/* ---- For std.time and std.random ----
   Declared by std itself with `extern "C"`: the clock and sleeping differ by system. */

uint64_t sosu_monotonic_ns(void) {
#ifdef _WIN32
    static LARGE_INTEGER frequency;
    LARGE_INTEGER counter;
    if (frequency.QuadPart == 0) QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&counter);
    uint64_t seconds = (uint64_t)(counter.QuadPart / frequency.QuadPart);
    uint64_t rest = (uint64_t)(counter.QuadPart % frequency.QuadPart);
    return seconds * 1000000000u + rest * 1000000000u / (uint64_t)frequency.QuadPart;
#else
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000000u + (uint64_t)now.tv_nsec;
#endif
}

void sosu_sleep_ms(uint32_t ms) {
#ifdef _WIN32
    Sleep(ms);
#else
    struct timespec wait = { (time_t)(ms / 1000), (long)(ms % 1000) * 1000000L };
    while (nanosleep(&wait, &wait) != 0 && errno == EINTR) {
    }
#endif
}

int64_t sosu_unix_seconds(void) {
    return (int64_t)time(NULL);
}

/* A seed that differs from one run to the next: the clocks, and addresses that the
   system places at random. Not for cryptography. */
uint64_t sosu_random_seed(void) {
    static uint64_t calls;
    int local = 0;
    uint64_t seed = sosu_monotonic_ns() ^ ((uint64_t)time(NULL) << 32);
    seed ^= (uint64_t)(uintptr_t)&local ^ ((uint64_t)(uintptr_t)&calls << 16);
    seed ^= ++calls * 0x9E3779B97F4A7C15u;
    return seed;
}

void sosu_fail(const char *loc, const char *message) {
    fflush(stdout);
    if (current_guard) {
        if (guard_prints) fprintf(stderr, "    %s: %s\n", loc, message);
        snprintf(last_error, sizeof last_error, "%s: %s", loc, message);
        has_error = true;
        longjmp(*current_guard, 1);
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

sosu_arena sosu_arena_copy(const sosu_arena *arena, const char *loc) {
    if (!arena->state) sosu_fail(loc, "arena used after `free`");
    return sosu_arena_new(arena->state->block_size, loc);
}

/* `arena` may lie in one of its own blocks (an object that owns the arena holding it, spec
   §9.2): it is marked freed before its blocks are. */
void sosu_arena_free(sosu_arena *arena) {
    sosu_arena_state *state = arena->state;
    if (!state) return;
    arena->state = NULL;
    sosu_block *block = state->head;
    while (block) {
        sosu_block *next = block->next;
        free(block);
        block = next;
    }
    free(state);
}

void *sosu_arena_alloc_array(sosu_arena *arena, size_t size, uint64_t count, const char *loc) {
    if (count != 0 && size > SIZE_MAX / count) sosu_fail(loc, "allocation too large");
    return sosu_arena_alloc(arena, size * (size_t)count, loc);
}

/* ---- Text ---- */

uint8_t *sosu_str_to_cstr(sosu_arena *arena, sosu_str text, const char *loc) {
    uint8_t *copy = sosu_arena_alloc(arena, text.len + 1, loc); /* zeroed: ends with NUL */
    if (text.len) memcpy(copy, text.ptr, text.len);
    return copy;
}

bool sosu_utf8_valid(const uint8_t *s, size_t length) {
    size_t i = 0;
    while (i < length) {
        uint32_t c = s[i];
        size_t n;
        uint32_t min;
        if (c < 0x80) { i++; continue; }
        else if ((c & 0xE0) == 0xC0) { n = 2; c &= 0x1F; min = 0x80; }
        else if ((c & 0xF0) == 0xE0) { n = 3; c &= 0x0F; min = 0x800; }
        else if ((c & 0xF8) == 0xF0) { n = 4; c &= 0x07; min = 0x10000; }
        else return false;
        if (length - i < n) return false;
        for (size_t k = 1; k < n; k++) {
            if ((s[i + k] & 0xC0) != 0x80) return false;
            c = (c << 6) | (s[i + k] & 0x3F);
        }
        if (c < min || c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF)) return false;
        i += n;
    }
    return true;
}

int32_t sosu_errno(void) { return (int32_t)errno; }

int64_t sosu_file_size(void *file) {
    FILE *f = file;
    long start = ftell(f);
    if (start < 0 || fseek(f, 0, SEEK_END) != 0) return -1;
    long end = ftell(f);
    if (fseek(f, start, SEEK_SET) != 0) return -1;
    return end < 0 ? -1 : (int64_t)(end - start);
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
    current_guard = &here;
    guard_prints = true;
    if (setjmp(here) == 0) {
        test();
        passed = true;
    }
    current_guard = NULL;
    guard_prints = false;
    printf("test %s ... %s\n", name, passed ? "ok" : "FAILED");
    return passed;
}

jmp_buf *sosu_guard_enter(jmp_buf *here) {
    jmp_buf *outer = current_guard;
    current_guard = here;
    has_error = false;
    return outer;
}

void sosu_guard_leave(jmp_buf *outer) { current_guard = outer; }

const char *sosu_last_error(void) { return has_error ? last_error : NULL; }

int sosu_test_summary(int total, int failed) {
    printf("\n%d test%s, %d passed, %d failed\n", total, total == 1 ? "" : "s", total - failed, failed);
    return failed ? 1 : 0;
}

bool sosu_str_eq(sosu_str a, sosu_str b) {
    return a.len == b.len && (a.len == 0 || memcmp(a.ptr, b.ptr, a.len) == 0);
}

/* hash(x) (spec §5.4): the same value for equal values, spread over 64 bits (not for cryptography). */
uint64_t sosu_hash_u64(uint64_t x) {
    x ^= x >> 33;
    x *= 0xFF51AFD7ED558CCDULL;
    x ^= x >> 33;
    x *= 0xC4CEB9FE1A85EC53ULL;
    return x ^ (x >> 33);
}

uint64_t sosu_hash_f64(double x) {
    if (x == 0.0) x = 0.0; /* -0.0 == 0.0 */
    uint64_t bits;
    memcpy(&bits, &x, sizeof bits);
    return sosu_hash_u64(bits);
}

uint64_t sosu_hash_str(sosu_str text) {
    uint64_t h = 0xCBF29CE484222325ULL; /* FNV-1a */
    for (size_t i = 0; i < text.len; i++) {
        h ^= text.ptr[i];
        h *= 0x100000001B3ULL;
    }
    return sosu_hash_u64(h);
}

/* The order of two texts, byte by byte (the order of their code points, in UTF-8). */
int sosu_str_cmp(sosu_str a, sosu_str b) {
    size_t n = a.len < b.len ? a.len : b.len;
    int order = n ? memcmp(a.ptr, b.ptr, n) : 0;
    if (order) return order;
    return a.len < b.len ? -1 : a.len > b.len;
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

/* ---- Android (spec §11.5) ----
   An application has no console: what the program prints (io.print, the run-time errors) goes
   to the log of the system, read with `adb logcat -s sosu`. A thread reads standard output and
   standard error through a pipe, line by line. */

#ifdef __ANDROID__
#include <android/log.h>
#include <pthread.h>
#include <unistd.h>

static int log_pipe[2];

static void *log_lines(void *unused) {
    (void)unused;
    char line[1024];
    size_t used = 0;
    char c;
    while (read(log_pipe[0], &c, 1) == 1) {
        if (c != '\n' && used + 1 < sizeof line) {
            line[used++] = c;
            continue;
        }
        line[used] = '\0';
        __android_log_write(ANDROID_LOG_INFO, "sosu", line);
        used = 0;
    }
    return NULL;
}

__attribute__((constructor)) static void log_output(void) {
    if (pipe(log_pipe) != 0) return;
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    dup2(log_pipe[1], STDOUT_FILENO);
    dup2(log_pipe[1], STDERR_FILENO);
    pthread_t reader;
    if (pthread_create(&reader, NULL, log_lines, NULL) == 0) pthread_detach(reader);
}
#endif
