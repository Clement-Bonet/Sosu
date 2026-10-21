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
