#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L /* clock_gettime, nanosleep */
#endif

#include "sosu_rt.h"

#include <errno.h>
#include <math.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __APPLE__
#include <TargetConditionals.h>
#endif
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

/* ---- For std.thread and std.channel (spec §5.6) ----
   Declared by std itself with `extern "C"`. A thread shares no memory with the others: it runs
   a function on a copy of a value, and threads talk through channels, which copy what they
   carry. A run-time error in a thread stops the program, as in the main thread. */

#ifdef _WIN32
typedef SRWLOCK sosu_lock;
typedef CONDITION_VARIABLE sosu_cond;
#define LOCK_INIT(l) InitializeSRWLock(l)
#define LOCK(l) AcquireSRWLockExclusive(l)
#define UNLOCK(l) ReleaseSRWLockExclusive(l)
#define COND_INIT(c) InitializeConditionVariable(c)
#define COND_WAIT(c, l) SleepConditionVariableSRW((c), (l), INFINITE, 0)
#define COND_WAKE_ALL(c) WakeAllConditionVariable(c)
#define LOCK_FREE(l) ((void)(l))
#define COND_FREE(c) ((void)(c))
#else
typedef pthread_mutex_t sosu_lock;
typedef pthread_cond_t sosu_cond;
#define LOCK_INIT(l) pthread_mutex_init((l), NULL)
#define LOCK(l) pthread_mutex_lock(l)
#define UNLOCK(l) pthread_mutex_unlock(l)
#define COND_INIT(c) pthread_cond_init((c), NULL)
#define COND_WAIT(c, l) pthread_cond_wait((c), (l))
#define COND_WAKE_ALL(c) pthread_cond_broadcast(c)
#define LOCK_FREE(l) pthread_mutex_destroy(l)
#define COND_FREE(c) pthread_cond_destroy(c)
#endif

/* Where `sosu_channel_receive` copies a value for this thread, until its next call. */
static SOSU_THREAD uint8_t *staging;
static SOSU_THREAD size_t staging_size;

typedef struct {
    void (*entry)(uint8_t *);
    uint8_t *data;
#ifdef _WIN32
    HANDLE handle;
#else
    pthread_t handle;
#endif
} sosu_thread;

#ifdef _WIN32
static DWORD WINAPI thread_main(LPVOID argument) {
#else
static void *thread_main(void *argument) {
#endif
    sosu_thread *thread = argument;
    thread->entry(thread->data);
    free(staging);
    staging = NULL;
    staging_size = 0;
    return 0;
}

/* Starts `entry(data)` on a new thread; its handle, for sosu_thread_join. */
void *sosu_thread_start(void (*entry)(uint8_t *), uint8_t *data) {
    sosu_thread *thread = malloc(sizeof *thread);
    if (!thread) sosu_fail("std.thread", "out of memory");
    thread->entry = entry;
    thread->data = data;
#ifdef _WIN32
    thread->handle = CreateThread(NULL, 0, thread_main, thread, 0, NULL);
    bool started = thread->handle != NULL;
#else
    bool started = pthread_create(&thread->handle, NULL, thread_main, thread) == 0;
#endif
    if (!started) sosu_fail("std.thread", "the system cannot start another thread");
    return thread;
}

/* Waits for the end of a thread, and releases it. */
void sosu_thread_join(void *handle) {
    sosu_thread *thread = handle;
#ifdef _WIN32
    WaitForSingleObject(thread->handle, INFINITE);
    CloseHandle(thread->handle);
#else
    pthread_join(thread->handle, NULL);
#endif
    free(thread);
}

/* A queue of `capacity` values of `size` bytes, in a ring. */
typedef struct {
    sosu_lock lock;
    sosu_cond changed; /* a value sent or received, or the channel closed */
    uint8_t *items;
    size_t size;
    size_t capacity;
    size_t head;
    size_t count;
    bool closed;
} sosu_channel;

/* A channel, as a number: its handle is a value without pointer (`Send`), given to threads. */
uint64_t sosu_channel_new(uint64_t size, uint64_t capacity) {
    sosu_channel *channel = calloc(1, sizeof *channel);
    uint8_t *items = channel && size && capacity ? malloc((size_t)(size * capacity)) : NULL;
    if (!channel || (size && capacity && !items)) sosu_fail("std.channel", "out of memory");
    channel->items = items;
    channel->size = (size_t)size;
    channel->capacity = (size_t)capacity;
    LOCK_INIT(&channel->lock);
    COND_INIT(&channel->changed);
    return (uint64_t)(uintptr_t)channel;
}

/* Copies `value` in, waiting while the channel is full. False if it is closed. */
bool sosu_channel_send(uint64_t id, const uint8_t *value) {
    sosu_channel *channel = (sosu_channel *)(uintptr_t)id;
    LOCK(&channel->lock);
    while (channel->count == channel->capacity && !channel->closed) COND_WAIT(&channel->changed, &channel->lock);
    bool sent = !channel->closed;
    if (sent) {
        size_t at = (channel->head + channel->count) % channel->capacity;
        memcpy(channel->items + at * channel->size, value, channel->size);
        channel->count++;
        COND_WAKE_ALL(&channel->changed);
    }
    UNLOCK(&channel->lock);
    return sent;
}

/* The address of a copy of the next value, waiting while the channel is empty and open; 0 when it
   is closed and empty. The copy stays until the next call of this thread. */
uint64_t sosu_channel_receive(uint64_t id) {
    sosu_channel *channel = (sosu_channel *)(uintptr_t)id;
    if (staging_size < channel->size || !staging) {
        free(staging);
        staging_size = channel->size ? channel->size : 1;
        staging = malloc(staging_size);
        if (!staging) sosu_fail("std.channel", "out of memory");
    }
    LOCK(&channel->lock);
    while (channel->count == 0 && !channel->closed) COND_WAIT(&channel->changed, &channel->lock);
    bool received = channel->count > 0;
    if (received) {
        memcpy(staging, channel->items + channel->head * channel->size, channel->size);
        channel->head = (channel->head + 1) % channel->capacity;
        channel->count--;
        COND_WAKE_ALL(&channel->changed);
    }
    UNLOCK(&channel->lock);
    return received ? (uint64_t)(uintptr_t)staging : 0;
}

/* No more values: the receivers get those left, then none; senders get false. */
void sosu_channel_close(uint64_t id) {
    sosu_channel *channel = (sosu_channel *)(uintptr_t)id;
    LOCK(&channel->lock);
    channel->closed = true;
    COND_WAKE_ALL(&channel->changed);
    UNLOCK(&channel->lock);
}

/* Releases the channel, once no thread uses it. */
void sosu_channel_free(uint64_t id) {
    sosu_channel *channel = (sosu_channel *)(uintptr_t)id;
    LOCK_FREE(&channel->lock);
    COND_FREE(&channel->changed);
    free(channel->items);
    free(channel);
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

/* ---- std.math: the C library, under names that do not take those of the module ---- */

#define SOSU_MATH1(name) double sosu_math_##name(double x) { return name(x); }
#define SOSU_MATH2(name) double sosu_math_##name(double x, double y) { return name(x, y); }
SOSU_MATH1(sqrt) SOSU_MATH1(cbrt) SOSU_MATH1(exp) SOSU_MATH1(exp2) SOSU_MATH1(log) SOSU_MATH1(log2)
SOSU_MATH1(log10) SOSU_MATH1(sin) SOSU_MATH1(cos) SOSU_MATH1(tan) SOSU_MATH1(asin) SOSU_MATH1(acos)
SOSU_MATH1(atan) SOSU_MATH1(sinh) SOSU_MATH1(cosh) SOSU_MATH1(tanh) SOSU_MATH1(floor) SOSU_MATH1(ceil)
SOSU_MATH1(round) SOSU_MATH1(trunc)
SOSU_MATH2(pow) SOSU_MATH2(atan2) SOSU_MATH2(hypot) SOSU_MATH2(fmod)
double sosu_math_infinity(void) { return HUGE_VAL; }
double sosu_math_nan(void) { return nan(""); }

/* ---- std.text: floats written and read by the C library ---- */

/* `value` with `decimals` digits after the dot, in `out`; gives the length it needs, which is
   more than `capacity` when it did not fit (nothing is written then). */
size_t sosu_format_f64(double value, uint32_t decimals, uint8_t *out, size_t capacity) {
    char small[64];
    int n = snprintf(small, sizeof small, "%.*f", (int)(decimals > 300 ? 300 : decimals), value);
    if (n < 0) return 0;
    if ((size_t)n <= capacity) {
        if ((size_t)n < sizeof small) {
            memcpy(out, small, (size_t)n);
        } else {
            char *big = malloc((size_t)n + 1);
            if (!big) return 0;
            snprintf(big, (size_t)n + 1, "%.*f", (int)(decimals > 300 ? 300 : decimals), value);
            memcpy(out, big, (size_t)n);
            free(big);
        }
    }
    return (size_t)n;
}

/* `value` with the fewest digits that read back the same (15 to 17), as `%g` writes them:
   0.1, 1e+21. Gives its length; `out` holds at least 32 bytes. */
size_t sosu_format_f64_shortest(double value, uint8_t *out) {
    char text[32];
    int n = 0;
    for (int digits = 15; digits <= 17; digits++) {
        n = snprintf(text, sizeof text, "%.*g", digits, value);
        if (strtod(text, NULL) == value) break;
    }
    if (n < 0) n = 0;
    memcpy(out, text, (size_t)n);
    return (size_t)n;
}

/* The float written in `length` bytes, already checked by std.text (digits, a dot, an exponent). */
double sosu_parse_f64(const uint8_t *bytes, size_t length) {
    char small[128];
    char *text = length < sizeof small ? small : malloc(length + 1);
    if (!text) return 0.0;
    memcpy(text, bytes, length);
    text[length] = '\0';
    double value = strtod(text, NULL);
    if (text != small) free(text);
    return value;
}

/* ---- std.os and std.fs: the program, its environment, other programs, folders and files ----
   Texts cross as UTF-8 C strings. A text given back is kept in `scratch`: the function gives
   its length (or a negative number on failure), and std copies it with `sosu_os_take` into a
   place of length + 1 bytes. On Windows, the wide functions of the system take and give
   UTF-16, converted here. Errors of std.fs are 0 (none), 1 not found, 2 permission denied,
   3 already exists, 4 folder not empty, 5 other. */

static SOSU_THREAD uint8_t *scratch;
static SOSU_THREAD size_t scratch_length;

static int64_t keep(uint8_t *bytes, size_t length) {
    free(scratch);
    scratch = bytes;
    scratch_length = length;
    return (int64_t)length;
}

static int64_t keep_copy(const char *text, size_t length) {
    uint8_t *bytes = malloc(length + 1);
    if (!bytes) return -2;
    memcpy(bytes, text, length);
    return keep(bytes, length);
}

void sosu_os_take(uint8_t *out) {
    if (scratch_length) memcpy(out, scratch, scratch_length);
    free(scratch);
    scratch = NULL;
    scratch_length = 0;
}

static int sosu_argc;
static char **sosu_argv;

void sosu_set_args(int argc, char **argv) {
    sosu_argc = argc;
    sosu_argv = argv;
}

#ifdef _WIN32

static wchar_t *wide(const uint8_t *utf8) {
    int n = MultiByteToWideChar(CP_UTF8, 0, (const char *)utf8, -1, NULL, 0);
    wchar_t *w = malloc((size_t)(n > 0 ? n : 1) * sizeof *w);
    if (!w) return NULL;
    if (n <= 0 || !MultiByteToWideChar(CP_UTF8, 0, (const char *)utf8, -1, w, n)) w[0] = 0;
    return w;
}

/* UTF-16 of `length` units (-1: up to its NUL) to UTF-8, malloc'd; its length in `*out`. */
static char *narrow(const wchar_t *w, int length, size_t *out) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, length, NULL, 0, NULL, NULL);
    char *text = malloc((size_t)(n > 0 ? n : 0) + 1);
    if (!text) return NULL;
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w, length, text, n, NULL, NULL);
    size_t size = n > 0 ? (size_t)n : 0;
    if (length < 0 && size > 0) size--; /* the NUL */
    text[size] = '\0';
    *out = size;
    return text;
}

static int32_t fs_error(DWORD code) {
    switch (code) {
    case ERROR_FILE_NOT_FOUND: case ERROR_PATH_NOT_FOUND: case ERROR_INVALID_NAME: return 1;
    case ERROR_ACCESS_DENIED: case ERROR_SHARING_VIOLATION: return 2;
    case ERROR_ALREADY_EXISTS: case ERROR_FILE_EXISTS: return 3;
    case ERROR_DIR_NOT_EMPTY: return 4;
    default: return 5;
    }
}

/* The arguments of the command line, in UTF-8, cut by the rules of the C library of
   Microsoft: the program as written, then arguments between spaces, with quotes and the
   backslashes before them. */
static char **windows_args;
static int windows_argc;

static void read_command_line(void) {
    if (windows_args) return;
    const wchar_t *line = GetCommandLineW();
    size_t cap = 8;
    windows_args = malloc(cap * sizeof *windows_args);
    wchar_t *arg = malloc((wcslen(line) + 1) * sizeof *arg);
    if (!windows_args || !arg) abort();
    const wchar_t *p = line;
    bool first = true;
    for (;;) {
        while (!first && (*p == L' ' || *p == L'\t')) p++;
        if (!*p) break;
        size_t n = 0;
        if (first) {
            if (*p == L'"') {
                for (p++; *p && *p != L'"'; p++) arg[n++] = *p;
                if (*p) p++;
            } else {
                for (; *p && *p != L' ' && *p != L'\t'; p++) arg[n++] = *p;
            }
            first = false;
        } else {
            bool quoted = false;
            while (*p && (quoted || (*p != L' ' && *p != L'\t'))) {
                size_t slashes = 0;
                while (*p == L'\\') {
                    slashes++;
                    p++;
                }
                if (*p == L'"') {
                    for (size_t k = 0; k < slashes / 2; k++) arg[n++] = L'\\';
                    if (slashes % 2) {
                        arg[n++] = L'"';
                    } else if (quoted && p[1] == L'"') {
                        arg[n++] = L'"'; /* "" in quotes: a quote */
                        p++;
                    } else {
                        quoted = !quoted;
                    }
                    p++;
                } else {
                    for (size_t k = 0; k < slashes; k++) arg[n++] = L'\\';
                    if (*p && (quoted || (*p != L' ' && *p != L'\t'))) arg[n++] = *p++;
                }
            }
        }
        if ((size_t)windows_argc == cap) {
            windows_args = realloc(windows_args, (cap *= 2) * sizeof *windows_args);
            if (!windows_args) abort();
        }
        size_t size;
        windows_args[windows_argc++] = narrow(arg, (int)n, &size);
    }
    free(arg);
}

size_t sosu_os_arg_count(void) {
    read_command_line();
    return (size_t)windows_argc;
}

int64_t sosu_os_arg(size_t index) {
    read_command_line();
    if (index >= (size_t)windows_argc || !windows_args[index]) return -1;
    return keep_copy(windows_args[index], strlen(windows_args[index]));
}

int64_t sosu_os_env(const uint8_t *name) {
    wchar_t *w = wide(name);
    if (!w) return -2;
    DWORD n = GetEnvironmentVariableW(w, NULL, 0);
    if (n == 0) {
        free(w);
        return -1;
    }
    wchar_t *value = malloc(n * sizeof *value);
    DWORD got = value ? GetEnvironmentVariableW(w, value, n) : 0;
    free(w);
    if (!value || got >= n) {
        free(value);
        return -2;
    }
    size_t size;
    char *text = narrow(value, (int)got, &size);
    free(value);
    return text ? keep((uint8_t *)text, size) : -2;
}

/* `value` NULL removes the variable. */
bool sosu_os_set_env(const uint8_t *name, const uint8_t *value) {
    wchar_t *w = wide(name), *v = value ? wide(value) : NULL;
    bool done = w && (!value || v) && SetEnvironmentVariableW(w, v);
    free(w);
    free(v);
    return done;
}

static void quote_wide(wchar_t **line, size_t *size, size_t *cap, const wchar_t *arg) {
    size_t length = wcslen(arg);
    if (*size + 2 * length + 4 > *cap) {
        *cap = 2 * (*size + 2 * length + 4);
        *line = realloc(*line, *cap * sizeof **line);
        if (!*line) abort();
    }
    wchar_t *out = *line;
    if (*size) out[(*size)++] = L' ';
    out[(*size)++] = L'"';
    for (size_t i = 0; i < length; i++) {
        if (arg[i] == L'"') {
            for (size_t k = i; k > 0 && arg[k - 1] == L'\\'; k--) out[(*size)++] = L'\\';
            out[(*size)++] = L'\\';
        }
        out[(*size)++] = arg[i];
    }
    for (size_t k = length; k > 0 && arg[k - 1] == L'\\'; k--) out[(*size)++] = L'\\';
    out[(*size)++] = L'"';
    out[*size] = 0;
}

/* Runs the program `args[0]` (looked for in PATH) with the others, and keeps what it writes
   to its output; its errors go to those of this program. -1: not found, -2: other failure. */
int64_t sosu_os_run(const uint8_t *const *args, size_t count, int32_t *code) {
    wchar_t *line = NULL;
    size_t size = 0, cap = 0;
    for (size_t i = 0; i < count; i++) {
        wchar_t *w = wide(args[i]);
        if (!w) return -2;
        quote_wide(&line, &size, &cap, w);
        free(w);
    }
    SECURITY_ATTRIBUTES inherit = { sizeof inherit, NULL, TRUE };
    HANDLE read_end, write_end;
    if (!line || !CreatePipe(&read_end, &write_end, &inherit, 0)) {
        free(line);
        return -2;
    }
    SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW start;
    memset(&start, 0, sizeof start);
    start.cb = sizeof start;
    start.dwFlags = STARTF_USESTDHANDLES;
    start.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    start.hStdOutput = write_end;
    start.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION process;
    BOOL started = CreateProcessW(NULL, line, NULL, NULL, TRUE, 0, NULL, NULL, &start, &process);
    DWORD failure = GetLastError();
    free(line);
    CloseHandle(write_end);
    if (!started) {
        CloseHandle(read_end);
        return failure == ERROR_FILE_NOT_FOUND || failure == ERROR_PATH_NOT_FOUND ? -1 : -2;
    }
    size_t length = 0, room = 4096;
    uint8_t *output = malloc(room + 1);
    DWORD got;
    while (output && ReadFile(read_end, output + length, (DWORD)(room - length), &got, NULL) && got > 0) {
        length += got;
        if (length == room) output = realloc(output, (room *= 2) + 1);
    }
    CloseHandle(read_end);
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exit_code = 0;
    GetExitCodeProcess(process.hProcess, &exit_code);
    CloseHandle(process.hProcess);
    CloseHandle(process.hThread);
    if (!output) return -2;
    *code = (int32_t)exit_code;
    return keep(output, length);
}

int32_t sosu_fs_create_dir(const uint8_t *path) {
    wchar_t *w = wide(path);
    int32_t error = !w ? 5 : CreateDirectoryW(w, NULL) ? 0 : fs_error(GetLastError());
    free(w);
    return error;
}

int32_t sosu_fs_remove_dir(const uint8_t *path) {
    wchar_t *w = wide(path);
    int32_t error = !w ? 5 : RemoveDirectoryW(w) ? 0 : fs_error(GetLastError());
    free(w);
    return error;
}

int32_t sosu_fs_remove_file(const uint8_t *path) {
    wchar_t *w = wide(path);
    int32_t error = !w ? 5 : DeleteFileW(w) ? 0 : fs_error(GetLastError());
    free(w);
    return error;
}

int32_t sosu_fs_rename(const uint8_t *from, const uint8_t *to) {
    wchar_t *a = wide(from), *b = wide(to);
    int32_t error = !a || !b ? 5 : MoveFileExW(a, b, MOVEFILE_REPLACE_EXISTING) ? 0 : fs_error(GetLastError());
    free(a);
    free(b);
    return error;
}

int32_t sosu_fs_copy(const uint8_t *from, const uint8_t *to) {
    wchar_t *a = wide(from), *b = wide(to);
    int32_t error = !a || !b ? 5 : CopyFileW(a, b, FALSE) ? 0 : fs_error(GetLastError());
    free(a);
    free(b);
    return error;
}

/* kind: 0 a file, 1 a folder, 2 something else; modified: seconds since 1970. */
int32_t sosu_fs_info(const uint8_t *path, int64_t *size, int64_t *modified, int32_t *kind) {
    wchar_t *w = wide(path);
    WIN32_FILE_ATTRIBUTE_DATA data = { 0 };
    BOOL found = w && GetFileAttributesExW(w, GetFileExInfoStandard, &data);
    DWORD failure = GetLastError();
    free(w);
    if (!found) return fs_error(failure);
    *size = (int64_t)(((uint64_t)data.nFileSizeHigh << 32) | data.nFileSizeLow);
    uint64_t ticks = ((uint64_t)data.ftLastWriteTime.dwHighDateTime << 32) | data.ftLastWriteTime.dwLowDateTime;
    *modified = (int64_t)(ticks / 10000000) - 11644473600; /* from 1601, in 100 ns */
    *kind = data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY ? 1 : 0;
    return 0;
}

/* The names in a folder, each followed by a NUL, without "." and "..". */
int64_t sosu_fs_list(const uint8_t *path, int32_t *error) {
    size_t length = strlen((const char *)path);
    uint8_t *pattern = malloc(length + 3);
    if (!pattern) return -2;
    memcpy(pattern, path, length);
    memcpy(pattern + length, "/*", 3);
    wchar_t *w = wide(pattern);
    free(pattern);
    WIN32_FIND_DATAW entry;
    HANDLE find = w ? FindFirstFileW(w, &entry) : INVALID_HANDLE_VALUE;
    free(w);
    if (find == INVALID_HANDLE_VALUE) {
        DWORD failure = GetLastError();
        if (failure == ERROR_FILE_NOT_FOUND) return keep(malloc(1), 0); /* an empty folder of a drive */
        *error = fs_error(failure);
        return -1;
    }
    size_t size = 0, room = 256;
    uint8_t *names = malloc(room);
    do {
        if (!wcscmp(entry.cFileName, L".") || !wcscmp(entry.cFileName, L"..")) continue;
        size_t n;
        char *name = narrow(entry.cFileName, -1, &n);
        if (!name || !names) break;
        while (size + n + 1 > room) names = realloc(names, room *= 2);
        if (!names) break;
        memcpy(names + size, name, n + 1);
        size += n + 1;
        free(name);
    } while (FindNextFileW(find, &entry));
    FindClose(find);
    if (!names) return -2;
    return keep(names, size);
}

int64_t sosu_fs_current_dir(void) {
    DWORD n = GetCurrentDirectoryW(0, NULL);
    wchar_t *w = n ? malloc(n * sizeof *w) : NULL;
    if (!w || !GetCurrentDirectoryW(n, w)) {
        free(w);
        return -2;
    }
    size_t size;
    char *text = narrow(w, -1, &size);
    free(w);
    for (size_t i = 0; text && i < size; i++) {
        if (text[i] == '\\') text[i] = '/';
    }
    return text ? keep((uint8_t *)text, size) : -2;
}

void *sosu_fs_open(const uint8_t *path, const uint8_t *mode) {
    wchar_t *p = wide(path), *m = wide(mode);
    FILE *file = p && m ? _wfopen(p, m) : NULL;
    free(p);
    free(m);
    return file;
}

int32_t sosu_fs_seek(void *file, int64_t offset, int32_t whence) {
    return _fseeki64(file, offset, whence == 0 ? SEEK_SET : whence == 1 ? SEEK_CUR : SEEK_END);
}

int64_t sosu_fs_tell(void *file) { return _ftelli64(file); }

#else /* POSIX */

static int32_t fs_error(int code) {
    switch (code) {
    case ENOENT: case ENOTDIR: return 1;
    case EACCES: case EPERM: case EROFS: return 2;
    case EEXIST: return 3;
    case ENOTEMPTY: return 4;
    default: return 5;
    }
}

size_t sosu_os_arg_count(void) { return sosu_argc > 0 ? (size_t)sosu_argc : 0; }

int64_t sosu_os_arg(size_t index) {
    if (index >= sosu_os_arg_count() || !sosu_argv[index]) return -1;
    return keep_copy(sosu_argv[index], strlen(sosu_argv[index]));
}

int64_t sosu_os_env(const uint8_t *name) {
    const char *value = getenv((const char *)name);
    return value ? keep_copy(value, strlen(value)) : -1;
}

bool sosu_os_set_env(const uint8_t *name, const uint8_t *value) {
    return (value ? setenv((const char *)name, (const char *)value, 1) : unsetenv((const char *)name)) == 0;
}

int64_t sosu_os_run(const uint8_t *const *args, size_t count, int32_t *code) {
#if defined(__APPLE__) && TARGET_OS_IPHONE
    (void)args;
    (void)count;
    (void)code;
    return -2; /* iOS runs no other program */
#else
    char **argv = malloc((count + 1) * sizeof *argv);
    int out[2], report[2];
    if (!argv || count == 0) {
        free(argv);
        return -2;
    }
    for (size_t i = 0; i < count; i++) argv[i] = (char *)args[i];
    argv[count] = NULL;
    if (pipe(out) != 0) {
        free(argv);
        return -2;
    }
    if (pipe(report) != 0) {
        close(out[0]);
        close(out[1]);
        free(argv);
        return -2;
    }
    fcntl(report[1], F_SETFD, FD_CLOEXEC); /* closed by a successful exec */
    pid_t child = fork();
    if (child == 0) {
        close(out[0]);
        close(report[0]);
        dup2(out[1], 1);
        close(out[1]);
        execvp(argv[0], argv);
        int failure = errno;
        ssize_t written = write(report[1], &failure, sizeof failure);
        (void)written;
        _exit(127);
    }
    free(argv);
    close(out[1]);
    close(report[1]);
    if (child < 0) {
        close(out[0]);
        close(report[0]);
        return -2;
    }
    size_t length = 0, room = 4096;
    uint8_t *output = malloc(room + 1);
    ssize_t got;
    while (output && (got = read(out[0], output + length, room - length)) != 0) {
        if (got < 0) {
            if (errno == EINTR) continue;
            break;
        }
        length += (size_t)got;
        if (length == room) output = realloc(output, (room *= 2) + 1);
    }
    close(out[0]);
    int failure = 0;
    ssize_t reported = read(report[0], &failure, sizeof failure);
    close(report[0]);
    int status = 0;
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
    }
    if (reported == (ssize_t)sizeof failure) {
        free(output);
        return failure == ENOENT ? -1 : -2;
    }
    if (!output) return -2;
    *code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
    return keep(output, length);
#endif
}

int32_t sosu_fs_create_dir(const uint8_t *path) { return mkdir((const char *)path, 0777) == 0 ? 0 : fs_error(errno); }
int32_t sosu_fs_remove_dir(const uint8_t *path) { return rmdir((const char *)path) == 0 ? 0 : fs_error(errno); }
int32_t sosu_fs_remove_file(const uint8_t *path) { return unlink((const char *)path) == 0 ? 0 : fs_error(errno); }

int32_t sosu_fs_rename(const uint8_t *from, const uint8_t *to) {
    return rename((const char *)from, (const char *)to) == 0 ? 0 : fs_error(errno);
}

int32_t sosu_fs_copy(const uint8_t *from, const uint8_t *to) {
    FILE *in = fopen((const char *)from, "rb");
    if (!in) return fs_error(errno);
    FILE *out = fopen((const char *)to, "wb");
    if (!out) {
        int32_t error = fs_error(errno);
        fclose(in);
        return error;
    }
    char buffer[16384];
    size_t n;
    int32_t error = 0;
    while ((n = fread(buffer, 1, sizeof buffer, in)) > 0) {
        if (fwrite(buffer, 1, n, out) != n) {
            error = 5;
            break;
        }
    }
    if (ferror(in)) error = 5;
    fclose(in);
    if (fclose(out) != 0) error = 5;
    return error;
}

int32_t sosu_fs_info(const uint8_t *path, int64_t *size, int64_t *modified, int32_t *kind) {
    struct stat info;
    if (stat((const char *)path, &info) != 0) return fs_error(errno);
    *size = (int64_t)info.st_size;
    *modified = (int64_t)info.st_mtime;
    *kind = S_ISREG(info.st_mode) ? 0 : S_ISDIR(info.st_mode) ? 1 : 2;
    return 0;
}

int64_t sosu_fs_list(const uint8_t *path, int32_t *error) {
    DIR *dir = opendir((const char *)path);
    if (!dir) {
        *error = fs_error(errno);
        return -1;
    }
    size_t size = 0, room = 256;
    uint8_t *names = malloc(room);
    struct dirent *entry;
    while (names && (entry = readdir(dir)) != NULL) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        size_t n = strlen(entry->d_name);
        while (names && size + n + 1 > room) names = realloc(names, room *= 2);
        if (!names) break;
        memcpy(names + size, entry->d_name, n + 1);
        size += n + 1;
    }
    closedir(dir);
    if (!names) return -2;
    return keep(names, size);
}

int64_t sosu_fs_current_dir(void) {
    size_t room = 256;
    char *text = malloc(room);
    while (text && !getcwd(text, room)) {
        if (errno != ERANGE) {
            free(text);
            return -2;
        }
        text = realloc(text, room *= 2);
    }
    return text ? keep((uint8_t *)text, strlen(text)) : -2;
}

void *sosu_fs_open(const uint8_t *path, const uint8_t *mode) { return fopen((const char *)path, (const char *)mode); }

int32_t sosu_fs_seek(void *file, int64_t offset, int32_t whence) {
    return fseeko(file, (off_t)offset, whence == 0 ? SEEK_SET : whence == 1 ? SEEK_CUR : SEEK_END);
}

int64_t sosu_fs_tell(void *file) { return (int64_t)ftello(file); }

#endif

/* std.log: a line to the error output (on Android, the log of the system, below). */
void sosu_write_stderr(const uint8_t *bytes, size_t length) {
    fwrite(bytes, 1, length, stderr);
    fflush(stderr);
}

void sosu_os_exit(int32_t code) {
    fflush(stdout);
    exit(code);
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
