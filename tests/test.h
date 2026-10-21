#ifndef SOSU_TEST_H
#define SOSU_TEST_H

/* Minimal test framework: each test program is a single translation unit. */

#include <stdio.h>

static int checks;
static int failures;

#define CHECK(cond, ...)                                              \
    do {                                                              \
        checks++;                                                     \
        if (!(cond)) {                                                \
            failures++;                                               \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);      \
            fprintf(stderr, __VA_ARGS__);                             \
            fputc('\n', stderr);                                      \
        }                                                             \
    } while (0)

static int test_summary(const char *name) {
    printf("%s: %d checks, %d failures\n", name, checks, failures);
    return failures ? 1 : 0;
}

#endif
