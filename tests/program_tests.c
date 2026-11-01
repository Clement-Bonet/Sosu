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
    const char *args; /* after `--`, for the program; NULL: none */
} Case;

static const Case cases[] = {
    { "run", "basics", "expected.txt", 0, NULL },
    { "run", "overflow", "expected.txt", 101, NULL },
    { "run", "classes", "expected.txt", 0, NULL },
    { "run", "linking", "expected.txt", 0, NULL },
    { "run", "files", "expected.txt", 0, NULL },
    { "run", "clock_random", "expected.txt", 0, NULL },
    { "run", "arrays", "expected.txt", 0, NULL },
    { "run", "copies", "expected.txt", 0, NULL },
    { "run", "screens", "expected.txt", 0, NULL },
    { "run", "titlebar", "expected.txt", 0, NULL },
    { "run", "identity", "expected.txt", 0, NULL },
    { "run", "languages", "expected.txt", 0, NULL },
    { "run", "places", "expected.txt", 0, NULL },
    { "run", "generics", "expected.txt", 0, NULL },
    { "run", "collections", "expected.txt", 0, NULL },
    { "run", "unsigned_floats", "expected.txt", 101, NULL },
    { "run", "constants", "expected.txt", 0, NULL },
    { "run", "bindgen", "expected.txt", 0, NULL },
    { "run", "lambdas", "expected.txt", 101, NULL },
    { "run", "iterators", "expected.txt", 0, NULL },
    { "run", "csources", "expected.txt", 0, NULL },
    { "run", "threads", "expected.txt", 0, NULL },
    { "run", "mathematics", "expected.txt", 0, NULL },
    { "run", "sorting", "expected.txt", 0, NULL },
    { "run", "texts", "expected.txt", 0, NULL },
    { "run", "calendar", "expected.txt", 0, NULL },
    { "run", "system", "expected.txt", 3, "first \"two words\" 3" },
    { "run", "formats", "expected.txt", 0, NULL },
    { "run", "readme", "expected.txt", 0, NULL }, /* the example of README.md */
    { "run", "hashes", "expected.txt", 0, NULL },
    { "run", "compression", "expected.txt", 0, NULL },
    { "run", "geometry", "expected.txt", 0, NULL },
    { "run --release", "overflow", "expected_release.txt", 0, NULL },
    { "run --release", "unsigned_floats", "expected_release.txt", 0, NULL },
    { "test", "testing", "expected.txt", 1, NULL },
    { "test", "checks", "expected.txt", 1, NULL }, /* the package testing (spec §8.3) */
    { "test", "images", "expected.txt", 0, NULL }, /* the package image (spec §8.2) */
    { "test", "sounds", "expected.txt", 0, NULL }, /* the package audio (spec §8.2) */
    { "test", "databases", "expected.txt", 0, NULL }, /* the packages db and sqlite (spec §8.2) */
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
        else snprintf(libs, sizeof libs, "%s/libgeom.a %s/libsosu_rt.a -lm -pthread", dir, dir); /* its .links */
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

/* The text of a file after its first `lines` lines. */
static const char *after_lines(const char *text, int lines) {
    while (text && lines-- > 0) {
        text = strchr(text, '\n');
        if (text) text++;
    }
    return text ? text : "";
}

/* `sosuc bindgen` (spec §11.6): the module of tests/programs/bindgen, written again from its header
   and checked against it by the C compiler, is the one committed (but its first lines, which name
   the system and the command); `long` is warned about. */
static void test_bindgen(const char *sosuc) {
    char command[1024];
    snprintf(command, sizeof command,
             "%s bindgen tests/programs/bindgen/c/clib.h -o build/clib.sosu --check > build/bindgen.out 2> build/bindgen.err",
             sosuc);
    CHECK(run(command) == 0, "bindgen of clib.h failed (see build/bindgen.err)");
    char *actual = read_text("build/clib.sosu");
    char *expected = read_text("tests/programs/bindgen/src/clib.sosu");
    CHECK(actual && expected && strcmp(after_lines(actual, 2), after_lines(expected, 2)) == 0,
          "bindgen of clib.h: build/clib.sosu differs from tests/programs/bindgen/src/clib.sosu");
    free(actual);
    free(expected);
    snprintf(command, sizeof command, "%s bindgen tests/programs/bindgen/c/long.h -o build/long.sosu > build/bindgen.out 2> build/bindgen.err",
             sosuc);
    CHECK(run(command) == 0, "bindgen of long.h failed");
    char *warnings = read_text("build/bindgen.err");
    CHECK(warnings && strstr(warnings, "`clib_long` uses `long`"), "bindgen of long.h: no warning about `long`");
    free(warnings);
}

/* `sosuc package` (spec §11.6): the program of tests/programs/packaged in build/package/, with its
   archive; run from another folder, it reads its asset next to itself. */
static void test_package(const char *sosuc) {
    char command[1024];
    snprintf(command, sizeof command, "%s package tests/programs/packaged > build/package.out 2> build/package.err", sosuc);
    CHECK(run(command) == 0, "sosuc package failed (see build/package.err)");
    const char *dir = "tests/programs/packaged/build/package";
#if defined(_WIN32)
    const char *archive = "packaged-windows-x64.zip";
    const char *program = "..\\tests\\programs\\packaged\\build\\package\\packaged\\packaged.exe";
#elif defined(__APPLE__)
#ifdef __aarch64__
    const char *archive = "packaged-macos-arm64.zip";
#else
    const char *archive = "packaged-macos-x64.zip";
#endif
    const char *program = "../tests/programs/packaged/build/package/Packaged.app/Contents/MacOS/packaged";
#else
    const char *archive = "packaged-linux-x64.tar.gz";
    const char *program = "../tests/programs/packaged/build/package/packaged/packaged";
#endif
    char path[512];
    snprintf(path, sizeof path, "%s/%s", dir, archive);
    FILE *file = fopen(path, "rb");
    CHECK(file != NULL, "sosuc package: no archive %s", path);
    if (file) fclose(file);
    snprintf(command, sizeof command, "cd build && %s > packaged.out", program);
    CHECK(run(command) == 0, "the packaged program failed");
    char *actual = read_text("build/packaged.out");
    CHECK(actual && strstr(actual, "Read next to the program."), "the packaged program did not find its asset: %s",
          actual ? actual : "(nothing)");
    free(actual);
}

/* A program for Android (spec §11.5): `libmain.so` for the emulator, an ELF library for x86_64.
   Skipped when this machine has no NDK, or no SDL3 for Android. */
static void test_android(const char *sosuc) {
    char command[512];
    snprintf(command, sizeof command, "%s build --target android-x64 examples/window > build/android.out 2> build/android.err",
             sosuc);
    if (run(command) != 0) {
        char *err = read_text("build/android.err");
        int missing = err && (strstr(err, "NDK was not found") || strstr(err, "SDL3"));
        if (missing) printf("android: skipped (no Android NDK, or no SDL3 for Android: see build/android.err)\n");
        CHECK(missing, "build --target android-x64 failed: %s", err ? err : "");
        free(err);
        return;
    }
    FILE *file = fopen("examples/window/build/android/x86_64/libmain.so", "rb");
    unsigned char header[20] = { 0 };
    size_t read = file ? fread(header, 1, sizeof header, file) : 0;
    if (file) fclose(file);
    CHECK(read == sizeof header && memcmp(header, "\x7f" "ELF", 4) == 0 && header[18] == 62,
          "libmain.so for android-x64 is not an ELF library for x86_64");
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
        snprintf(command, sizeof command, "%s %s tests/programs/%s%s%s > %s 2> build%s%s.err", argv[1], c->command,
                 c->program, c->args ? " -- " : "", c->args ? c->args : "", output, sep, c->program);
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
    test_bindgen(argv[1]);
    test_package(argv[1]);
    test_android(argv[1]);

    /* `assert(a == b)` shows both sides, on the error output (spec §7.3) */
    char *sides = read_text("build/checks.err");
    CHECK(sides && strstr(sides, "assertion failed: square(2) == 5\n      left: 4\n     right: 5\n") &&
              strstr(sides, "      left: \"Ada\\tLovelace\"\n") && strstr(sides, "      left: 0.30000000000000004\n"),
          "checks: assert does not show both sides: %s", sides ? sides : "(nothing)");
    free(sides);

    /* `copy` of the [link] section of tests/programs/linking */
    char *copied = read_text("tests/programs/linking/build/message.txt");
    CHECK(copied && strstr(copied, "Copied next to the program"), "linking: message.txt was not copied to build/");
    free(copied);
    return test_summary("program tests");
}
