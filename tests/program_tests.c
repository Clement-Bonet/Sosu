#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test.h"

/* End-to-end tests: `sosuc run` / `sosuc test` on the packages of tests/programs, which
   compiles them through the C compiler, then compares the output with expected.txt.
   Usage (from the repository root): program_tests <path to sosuc> */

typedef struct {
    const char *command; /* with its options */
    const char *program;
    const char *expected; /* file in the program folder */
    int exit_code;
} Case;

static const Case cases[] = {
    { "run", "basics", "expected.txt", 0 },
    { "run", "overflow", "expected.txt", 101 },
    { "run", "classes", "expected.txt", 0 },
    { "run", "linking", "expected.txt", 0 },
    { "run", "files", "expected.txt", 0 },
    { "run", "clock_random", "expected.txt", 0 },
    { "run", "arrays", "expected.txt", 0 },
    { "run", "copies", "expected.txt", 0 },
    { "run", "screens", "expected.txt", 0 },
    { "run", "titlebar", "expected.txt", 0 },
    { "run", "identity", "expected.txt", 0 },
    { "run", "languages", "expected.txt", 0 },
    { "run", "places", "expected.txt", 0 },
    { "run", "generics", "expected.txt", 0 },
    { "run", "collections", "expected.txt", 0 },
    { "run", "unsigned_floats", "expected.txt", 101 },
    { "run --release", "overflow", "expected_release.txt", 0 },
    { "run --release", "unsigned_floats", "expected_release.txt", 0 },
    { "test", "testing", "expected.txt", 1 },
};

/* Reads a file in text mode, so that line endings are the same on every system. */
static char *read_text(const char *path) {
    FILE *file = fopen(path, "r");
    if (!file) return NULL;
    size_t size = 0, cap = 1024;
    char *text = malloc(cap);
    int c;
    while ((c = fgetc(file)) != EOF) {
        if (size + 1 >= cap) text = realloc(text, cap *= 2);
        text[size++] = (char)c;
    }
    text[size] = '\0';
    fclose(file);
    return text;
}

static int run(const char *command) {
    fflush(stdout);
    int status = system(command);
#ifndef _WIN32
    if (status != -1) status = (status >> 8) & 0xFF;
#endif
    return status;
}

/* A library for other languages (spec §9.2): built static, then shared, each used by a C host
   compiled with the compiler of these tests (cl, or cc). */
static void test_library(const char *sosuc) {
    for (int shared = 0; shared < 2; shared++) {
        char command[1024];
        snprintf(command, sizeof command, "%s build %s tests/programs/library > build/library.out 2> build/library.err",
                 sosuc, shared ? "--lib=shared" : "--lib");
        CHECK(run(command) == 0, "build %s library: failed (see build/library.err)", shared ? "a shared" : "a static");
#ifdef _MSC_VER
        const char *dir = "tests\\programs\\library\\build";
        snprintf(command, sizeof command,
                 "cl /nologo /W3 /I%s /Fe%s\\host%d.exe /Fo%s\\ tests\\programs\\library\\host\\host.c %s > build\\host.log",
                 dir, dir, shared, dir, shared ? "tests\\programs\\library\\build\\geom.dll.lib"
                                               : "tests\\programs\\library\\build\\geom.lib tests\\programs\\library\\build\\sosu_rt.lib");
        CHECK(run(command) == 0, "the host of the library does not compile (see build/host.log)");
        snprintf(command, sizeof command, "%s\\host%d.exe > build\\library_host.out", dir, shared);
#else
        const char *dir = "tests/programs/library/build";
#ifdef __APPLE__
        const char *shared_lib = "libgeom.dylib";
#else
        const char *shared_lib = "libgeom.so";
#endif
        char libs[256];
        if (shared) snprintf(libs, sizeof libs, "%s/%s", dir, shared_lib);
        else snprintf(libs, sizeof libs, "%s/libgeom.a %s/libsosu_rt.a", dir, dir);
        snprintf(command, sizeof command, "cc -std=c99 -I%s tests/programs/library/host/host.c %s -o %s/host%d > build/host.log 2>&1",
                 dir, libs, dir, shared);
        CHECK(run(command) == 0, "the host of the library does not compile (see build/host.log)");
        snprintf(command, sizeof command, "%s/host%d > build/library_host.out", dir, shared);
#endif
        CHECK(run(command) == 0, "the host of the %s library failed", shared ? "shared" : "static");
        char *actual = read_text("build/library_host.out");
        char *expected = read_text("tests/programs/library/expected.txt");
        CHECK(actual && expected && strcmp(actual, expected) == 0, "the host of the %s library: output differs\n--- got:\n%s",
              shared ? "shared" : "static", actual ? actual : "(none)\n");
        free(actual);
        free(expected);
    }
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fputs("usage: program_tests <path to sosuc>\n", stderr);
        return 2;
    }
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        const Case *c = &cases[i];
#ifdef _WIN32
        const char *sep = "\\";
#else
        const char *sep = "/";
#endif
        char command[512], output[256], expected_path[256];
        snprintf(output, sizeof output, "build%s%s.out", sep, c->program);
        snprintf(command, sizeof command, "%s %s tests/programs/%s > %s 2> build%s%s.err", argv[1], c->command,
                 c->program, output, sep, c->program);
        int status = run(command);
        CHECK(status == c->exit_code, "%s %s: exit code %d, expected %d (see build/%s.err)", c->command, c->program,
              status, c->exit_code, c->program);

        snprintf(expected_path, sizeof expected_path, "tests/programs/%s/%s", c->program, c->expected);
        char *actual = read_text(output);
        char *expected = read_text(expected_path);
        CHECK(actual && expected && strcmp(actual, expected) == 0, "%s %s: output differs\n--- got:\n%s--- expected:\n%s",
              c->command, c->program, actual ? actual : "(none)\n", expected ? expected : "(none)\n");
        free(actual);
        free(expected);
    }

    test_library(argv[1]);

    /* `copy` of the [link] section of tests/programs/linking */
    char *copied = read_text("tests/programs/linking/build/message.txt");
    CHECK(copied && strstr(copied, "Copied next to the program"), "linking: message.txt was not copied to build/");
    free(copied);
    return test_summary("program tests");
}
