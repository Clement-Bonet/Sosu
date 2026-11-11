#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test.h"

/* End-to-end tests: `sosuc run` / `sosuc test` on the packages of tests/programs, which
   compiles them through the C compiler, then compares the output with expected.txt. The
   examples of the packages (examples/<name>, spec §8.3) are run the same way; their expected
   output is tests/examples/<name>.txt, so that their folders hold only the example.
   Usage (from the repository root): program_tests <path to sosuc> */

typedef struct {
    const char *command; /* with its options */
    const char *program;
    const char *expected; /* file in the program folder; NULL for an example: tests/examples/<name>.txt */
    int exit_code;
    const char *args; /* after `--`, for the program; NULL: none */
} Case;

static const Case cases[] = {
    { "run", "basics", "expected.txt", 0, NULL },
    /* read the same by sh and by cmd (an even number of quotes, so that cmd sees the `>` after
       them), then quoted again by sosuc for the program */
    { "run", "arguments", "expected.txt", 0, "\"a b\" \"c d\\\\\" \"x\\\"y\\\"z\" \"\" plain" },
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
    { "run", "kept_closures", "expected.txt", 0, NULL }, /* closures in an arena (spec §6.1, rule 5) */
    { "run", "iterators", "expected.txt", 0, NULL },
    { "run", "csources", "expected.txt", 0, NULL },
    { "run", "cppsources", "expected.txt", 0, NULL }, /* [link] cpp: C++ files of a package (spec §9.1) */
    { "run", "physics2d", "expected.txt", 0, NULL }, /* physics2d on Box2D, a [native.box2d] (spec §8.2) */
    { "run", "physics", "expected.txt", 0, NULL }, /* physics on Jolt, a [native.jolt] of C++ (spec §8.2) */
    { "run", "nav", "expected.txt", 0, NULL }, /* nav on Recast and Detour, and A* on grids (spec §8.2) */
    { "run", "threads", "expected.txt", 0, NULL },
    { "run", "thread_closure", "expected.txt", 101, NULL },
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
    { "run", "controls", "expected.txt", 0, NULL }, /* platform.gamepad, with a virtual pad (spec §8.2) */
    { "run", "inputs", "expected.txt", 0, NULL },   /* platform.actions, with devices of the test */
    { "run --release", "overflow", "expected_release.txt", 0, NULL },
    { "run --release", "unsigned_floats", "expected_release.txt", 0, NULL },
    { "test", "testing", "expected.txt", 1, NULL },
    { "test", "checks", "expected.txt", 1, NULL }, /* the package testing (spec §8.3) */
    { "test", "images", "expected.txt", 0, NULL }, /* the package image (spec §8.2) */
    { "test", "sounds", "expected.txt", 0, NULL }, /* the package audio (spec §8.2) */
    { "test", "databases", "expected.txt", 0, NULL }, /* the packages db and sqlite (spec §8.2) */
    { "run", "effects", "expected.txt", 0, NULL }, /* an interface with an effect parameter (spec §5.4) */
    { "test", "network", "expected.txt", 0, NULL }, /* the package net (spec §8.2) */
    /* the examples of the packages of 1.0 (spec §8.3) */
    { "run", "examples/checksums", NULL, 0, NULL },  /* crypto */
    { "run", "examples/archive", NULL, 0, NULL },    /* compress */
    { "run", "examples/orbit", NULL, 0, NULL },      /* math */
    { "test", "examples/wordcount", NULL, 0, NULL }, /* testing */
    { "run", "examples/thumbnail", NULL, 0, NULL },  /* image */
    { "run", "examples/bookshelf", NULL, 0, NULL },  /* db and sqlite */
    { "run", "examples/fetch", NULL, 0, NULL },      /* net, with a server of its own */
    { "run", "examples/greetings", NULL, 0, NULL },  /* i18n */
    /* the libraries of 3D together, played for five seconds by a script, without a window */
    { "run", "examples/brawl", NULL, 0, "--check" },
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

#ifdef _WIN32
/* The FILETYPE of the version resource of `path` (1: a program, 2: a DLL), found by the signature
   of its VS_FIXEDFILEINFO; 0 when there is none. */
static unsigned file_type(const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file) return 0;
    unsigned char window[40] = {0};
    unsigned type = 0;
    int c;
    while (!type && (c = fgetc(file)) != EOF) {
        memmove(window, window + 1, sizeof window - 1);
        window[sizeof window - 1] = (unsigned char)c;
        /* dwSignature 0xFEEF04BD, then eight fields, then dwFileType */
        if (memcmp(window, "\xBD\x04\xEF\xFE", 4) == 0) type = window[36] | (unsigned)window[37] << 8;
    }
    fclose(file);
    return type;
}

/* The fields of the "Details" of the properties of `path`, as Windows reads them, one per line. */
static char *version_info(const char *path) {
    char command[1024];
    snprintf(command, sizeof command,
             "powershell -NoProfile -Command \"$v = (Get-Item '%s').VersionInfo; $v.FileDescription; $v.FileVersion; "
             "$v.ProductName; $v.ProductVersion; $v.LegalCopyright; $v.OriginalFilename; "
             "($v.FileMajorPart, $v.FileMinorPart, $v.FileBuildPart, $v.FilePrivatePart) -join ','\" > build\\version.out",
             path);
    if (run(command) != 0) return NULL;
    return read_text("build/version.out");
}
#endif

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
#ifdef _WIN32
    /* the version of the DLL, in the properties of the file: from [package], without [app] */
    CHECK(file_type("tests/programs/library/build/geom.dll") == 2, "geom.dll: no version resource of a DLL");
    char *details = version_info("tests\\programs\\library\\build\\geom.dll");
    const char *expected = "geom\n0.1.0\ngeom\n0.1.0\n\ngeom.dll\n0,1,0,0\n";
    CHECK(details && strcmp(details, expected) == 0, "geom.dll: version\n--- got:\n%s--- expected:\n%s",
          details ? details : "(none)\n", expected);
    free(details);
#endif
}

/* The text of a file after its first `lines` lines. */
static const char *after_lines(const char *text, int lines) {
    while (text && lines-- > 0) {
        text = strchr(text, '\n');
        if (text) text++;
    }
    return text ? text : "";
}

/* `sosuc doc`: the reference of tests/packages/doc, a module in a subfolder, its private
   function left out. */
static void test_doc(const char *sosuc) {
    char command[512];
    snprintf(command, sizeof command, "%s doc tests/packages/doc -o build/doc.md > build/doc.out 2> build/doc.err", sosuc);
    CHECK(run(command) == 0, "sosuc doc failed (see build/doc.err)");
    char *actual = read_text("build/doc.md");
    char *expected = read_text("tests/packages/doc/expected.md");
    CHECK(actual && expected && strcmp(actual, expected) == 0, "sosuc doc: output differs\n--- got:\n%s--- expected:\n%s",
          actual ? actual : "(none)\n", expected ? expected : "(none)\n");
    free(actual);
    free(expected);
    snprintf(command, sizeof command, "%s doc tests/packages/doc --html > build/doc.out 2> build/doc.err", sosuc);
    CHECK(run(command) == 2, "sosuc doc: an unknown option is refused");
}

/* `sosuc bindgen` (spec §11.6): the module of tests/programs/bindgen, written again from its header
   and checked against it by the C compiler, is the one committed (but its first lines, which name
   the system and the command); `long` is warned about. */
/* A pointer read after the `free()` of its arena stops a debug build (spec §7.7): an access
   violation, whose code depends on the system; what was printed before is lost with it. */
static void test_freed_arena(const char *sosuc) {
    char command[1024];
    snprintf(command, sizeof command, "%s run tests/programs/freed_arena > build/freed_arena.out 2> build/freed_arena.err",
             sosuc);
    CHECK(run(command) != 0, "run freed_arena: the pointer into a freed arena was read without stopping");
    char *said = read_text("build/freed_arena.err");
    CHECK(said && strstr(said, "sosu: a pointer into an arena was used after the free() of the arena"),
          "run freed_arena: the stop does not say why (see build/freed_arena.err)");
    free(said);
}

/* A recursion without end stops the program with a message, and the code 101 (review R3), in the
   thread that started it and in one of std.thread. */
static void test_stack_overflow(const char *sosuc) {
    static const char *const programs[] = { "stack_overflow", "thread_overflow" };
    for (size_t i = 0; i < sizeof programs / sizeof programs[0]; i++) {
        const char *name = programs[i];
        char command[1024];
        snprintf(command, sizeof command, "%s run tests/programs/%s > build/%s.out 2> build/%s.err", sosuc, name, name, name);
        CHECK(run(command) == 101, "run %s: the overflow does not stop with the code 101", name);
        char path[256];
        snprintf(path, sizeof path, "build/%s.err", name);
        char *said = read_text(path);
        CHECK(said && strstr(said, "sosu: stack overflow"), "run %s: the stop does not say why (see %s)", name, path);
        free(said);
    }
}

/* The package gpu (spec §8.2): scenes drawn without a window and read back, and a computation.
   A machine without a GPU driver (Vulkan, Direct3D 12, Metal) makes the program exit with 77:
   the test is skipped, and says so. The shaders it loads must be up to date. */
static void test_gpu(const char *sosuc) {
    char command[1024];
    snprintf(command, sizeof command, "%s shaders tests/programs/gpu --check > build/gpu_shaders.out 2>&1", sosuc);
    CHECK(run(command) == 0, "the shaders of tests/programs/gpu are out of date (see build/gpu_shaders.out)");
    /* the shaders of gpu.overlay, carried by the package in C */
    snprintf(command, sizeof command, "%s shaders packages/gpu --c native/shaders.c --check > build/gpu_shaders.out 2>&1",
             sosuc);
    CHECK(run(command) == 0, "packages/gpu/native/shaders.c is out of date (see build/gpu_shaders.out)");
    snprintf(command, sizeof command, "%s run tests/programs/gpu > build/gpu.out 2> build/gpu.err", sosuc);
    int status = run(command);
    if (status == 77) {
        char *said = read_text("build/gpu.out");
        printf("gpu: skipped, %s", said ? said : "no device\n");
        free(said);
        return;
    }
    CHECK(status == 0, "run gpu: exit code %d (see build/gpu.out and build/gpu.err)", status);
    char *actual = read_text("build/gpu.out");
    char *expected = read_text("tests/programs/gpu/expected.txt");
    CHECK(actual && expected && strcmp(actual, expected) == 0, "run gpu: output differs\n--- got:\n%s--- expected:\n%s",
          actual ? actual : "(none)\n", expected ? expected : "(none)\n");
    free(actual);
    free(expected);

    /* a handle used after its release stops the program, even when its line was taken again */
    snprintf(command, sizeof command, "%s run tests/programs/gpu_released > build/gpu_released.out 2> build/gpu_released.err",
             sosuc);
    status = run(command);
    CHECK(status == 101, "run gpu_released: exit code %d, expected 101 (see build/gpu_released.err)", status);
    char *said = read_text("build/gpu_released.err");
    CHECK(said && strstr(said, "assertion failed: s.alive && s.generation == generation"),
          "run gpu_released: the stop does not say why (see build/gpu_released.err)");
    free(said);
    char *printed = read_text("build/gpu_released.out");
    CHECK(printed && !strstr(printed, "not reached"), "run gpu_released: the program went on after the dead handle");
    free(printed);

    /* render3d: a lit scene with its shadow, read back; its shaders carried in C */
    snprintf(command, sizeof command,
             "%s shaders packages/render3d --c native/render3d_shaders.c --check > build/gpu_shaders.out 2>&1", sosuc);
    CHECK(run(command) == 0, "packages/render3d/native/render3d_shaders.c is out of date (see build/gpu_shaders.out)");
    snprintf(command, sizeof command, "%s run tests/programs/render3d > build/render3d.out 2> build/render3d.err", sosuc);
    status = run(command);
    CHECK(status == 0, "run render3d: exit code %d (see build/render3d.out and build/render3d.err)", status);
    actual = read_text("build/render3d.out");
    expected = read_text("tests/programs/render3d/expected.txt");
    CHECK(actual && expected && strcmp(actual, expected) == 0, "run render3d: output differs\n--- got:\n%s--- expected:\n%s",
          actual ? actual : "(none)\n", expected ? expected : "(none)\n");
    free(actual);
    free(expected);

    /* particles: drawn over a scene of render3d, hidden by what is nearer; its shaders carried in C */
    snprintf(command, sizeof command,
             "%s shaders packages/particles --c native/particles_shaders.c --check > build/gpu_shaders.out 2>&1", sosuc);
    CHECK(run(command) == 0, "packages/particles/native/particles_shaders.c is out of date (see build/gpu_shaders.out)");
    snprintf(command, sizeof command, "%s run tests/programs/particles > build/particles.out 2> build/particles.err", sosuc);
    status = run(command);
    CHECK(status == 0, "run particles: exit code %d (see build/particles.out and build/particles.err)", status);
    actual = read_text("build/particles.out");
    expected = read_text("tests/programs/particles/expected.txt");
    CHECK(actual && expected && strcmp(actual, expected) == 0, "run particles: output differs\n--- got:\n%s--- expected:\n%s",
          actual ? actual : "(none)\n", expected ? expected : "(none)\n");
    free(actual);
    free(expected);

    /* assets: files loaded by threads, failures, a release, and a texture loaded again */
    snprintf(command, sizeof command, "%s run tests/programs/assets > build/assets.out 2> build/assets.err", sosuc);
    status = run(command);
    CHECK(status == 0, "run assets: exit code %d (see build/assets.out and build/assets.err)", status);
    actual = read_text("build/assets.out");
    expected = read_text("tests/programs/assets/expected.txt");
    CHECK(actual && expected && strcmp(actual, expected) == 0, "run assets: output differs\n--- got:\n%s--- expected:\n%s",
          actual ? actual : "(none)\n", expected ? expected : "(none)\n");
    free(actual);
    free(expected);
}

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

    /* a header nested beyond any stack: what is too deep is left out, and the rest is read */
    FILE *deep = fopen("build/deep.h", "w");
    CHECK(deep != NULL, "cannot write build/deep.h");
    if (!deep) return;
    fputs("#define DEEP ", deep);
    for (int i = 0; i < 300000; i++) fputc('(', deep);
    fputc('1', deep);
    for (int i = 0; i < 300000; i++) fputc(')', deep);
    fputs("\nint ", deep);
    for (int i = 0; i < 100000; i++) fputc('(', deep);
    fputs("*x", deep);
    for (int i = 0; i < 100000; i++) fputc(')', deep);
    fputs(";\n", deep);
    for (int i = 0; i < 20000; i++) fputs("struct A { ", deep);
    fputs("int x; ", deep);
    for (int i = 0; i < 20000; i++) fputs("} a; ", deep);
    fputs("\nint after(int x);\n", deep);
    fclose(deep);
    snprintf(command, sizeof command, "%s bindgen build/deep.h -o build/deep.sosu > build/bindgen.out 2> build/bindgen.err", sosuc);
    CHECK(run(command) == 0, "bindgen of deep.h failed (see build/bindgen.err)");
    char *deep_module = read_text("build/deep.sosu");
    CHECK(deep_module && strstr(deep_module, "after"), "bindgen of deep.h: what follows is read");
    free(deep_module);
}

/* The guides say what they show (docs/QUICK_GUIDE.md, docs/GUIDE.md): each block after a mark is a package of its own. `run` compiles
   and runs it, and the `text` block after it is what it prints; `check` and `test` must pass;
   `error E0000` must be refused with that code first. */
static void test_guide(const char *sosuc, const char *name, const char *path_of_guide, int minimum) {
    char *guide = read_text(path_of_guide);
    CHECK(guide != NULL, "cannot read %s", path_of_guide);
    if (!guide) return;
    int count = 0;
    for (char *at = strstr(guide, "<!-- "); at; at = strstr(at + 1, "<!-- ")) {
        char kind[32] = "";
        char *line_end = strchr(at, '\n');
        if (!line_end || sscanf(at, "<!-- %31[^-]-->", kind) != 1) continue;
        char *code = strstr(line_end, "```csharp\n");
        if (code != line_end + 1) continue;
        code += strlen("```csharp\n");
        char *end = strstr(code, "```\n");
        if (!end) break;
        count++;

        char dir[64], path[96], command[1024];
        snprintf(dir, sizeof dir, "build/%s%d", name, count);
        snprintf(path, sizeof path, "mkdir \"build/%s%d/src\" 2> nul", name, count);
#ifdef _WIN32
        for (char *c = path; *c; c++) {
            if (*c == '/') *c = '\\';
        }
#else
        snprintf(path, sizeof path, "mkdir -p %s/src", dir);
#endif
        run(path);
        snprintf(path, sizeof path, "%s/sosu.toml", dir);
        FILE *file = fopen(path, "w");
        CHECK(file != NULL, "cannot write %s", path);
        if (!file) break;
        fprintf(file, "[package]\nname = \"%s%d\"\nversion = \"0.1.0\"\n", name, count);
        fclose(file);
        snprintf(path, sizeof path, "%s/src/main.sosu", dir);
        file = fopen(path, "w");
        CHECK(file != NULL, "cannot write %s", path);
        if (!file) break;
        fwrite(code, 1, (size_t)(end - code), file);
        *end = '\0';
        if (!strstr(code, "i32 main(")) fputs("\ni32 main() {\n    return 0;\n}\n", file);
        *end = '`';
        fclose(file);

        bool refused = strncmp(kind, "error ", 6) == 0;
        const char *verb = strncmp(kind, "run", 3) == 0 ? "run" : strncmp(kind, "test", 4) == 0 ? "test" : "check";
        snprintf(command, sizeof command, "%s %s %s > %s/out.txt 2> %s/err.txt", sosuc, verb, dir, dir, dir);
        int status = run(command);
        snprintf(path, sizeof path, "%s/%s", dir, refused ? "err.txt" : "out.txt");
        char *output = read_text(path);
        if (refused) {
            char want[32];
            sscanf(kind + 6, "%31s", want);
            const char *first = output ? strstr(output, "error[") : NULL;
            CHECK(status != 0 && first && strncmp(first + 6, want, strlen(want)) == 0, "%s block %d: not refused with %s", path_of_guide, count, want);
        } else {
            CHECK(status == 0, "%s block %d (%s): failed, see %s", path_of_guide, count, verb, dir);
            const char *after = end + strlen("```\n");
            if (strcmp(verb, "run") == 0 && strncmp(after, "\n```text\n", 9) == 0) {
                const char *text = after + 9;
                size_t length = (size_t)(strstr(text, "```\n") - text);
                CHECK(output && strlen(output) == length && strncmp(output, text, length) == 0,
                      "%s block %d prints something else, see %s", path_of_guide, count, dir);
            }
        }
        free(output);
    }
    CHECK(count >= minimum, "only %d blocks found in %s", count, path_of_guide);
    free(guide);
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
#ifdef _WIN32
    /* the version of the program and the texts of [app], in the properties of the file */
    const char *exe = "tests\\programs\\packaged\\build\\package\\packaged\\packaged.exe";
    CHECK(file_type(exe) == 1, "packaged.exe: no version resource of a program");
    char *details = version_info(exe);
    const char *expected = "A packaged test program\n0.1.0\nPackaged\n0.1.0\nCopyright 2026 The Sosu authors\n"
                           "packaged.exe\n0,1,0,0\n";
    CHECK(details && strcmp(details, expected) == 0, "packaged.exe: version\n--- got:\n%s--- expected:\n%s",
          details ? details : "(none)\n", expected);
    free(details);
#endif
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

static void write_file(const char *path, const char *text) {
    FILE *file = fopen(path, "wb");
    CHECK(file != NULL, "cannot write %s", path);
    if (!file) return;
    fputs(text, file);
    fclose(file);
}

/* git in build/native_test/lib */
static bool lib_git(const char *args) {
    char command[512];
    snprintf(command, sizeof command,
             "git -C build/native_test/lib -c user.name=test -c user.email=test@example.com %s > build/native_git.out 2>&1",
             args);
    return run(command) == 0;
}

static void write_app_manifest(const char *tag, const char *c) {
    char text[1024];
    snprintf(text, sizeof text,
             "[package]\nname = \"app\"\nversion = \"0.1.0\"\n\n"
             "# C and C++ files of a Git repository\n"
             "[native.lib]\ngit = \"../lib\"\ntag = \"%s\"\nc = [\"%s\"]\ncpp = [\"cpp/\"]\ninclude = [\"include\"]\n"
             "defines = [\"LIB_BASE=100\"]\n",
             tag, c);
    write_file("build/native_test/app/sosu.toml", text);
}

/* [native.<name>] (spec §9.1): the C and C++ files of a Git repository, cloned at a tag and kept in
   sosu.lock; `dir/ **` (no space) takes those of the subfolders (two files of one name among them), `dir/` those
   of a folder; a path above the clone is refused. `sosuc deps upgrade` moves the tag to the newest
   version, not to a pre-release. */
static void test_native(const char *sosuc) {
#ifdef _WIN32
    run("rmdir /s /q build\\native_test > NUL 2>&1");
    run("mkdir build\\native_test\\lib\\src\\a build\\native_test\\lib\\src\\b build\\native_test\\lib\\cpp "
        "build\\native_test\\lib\\include build\\native_test\\app\\src");
#else
    run("rm -rf build/native_test");
    run("mkdir -p build/native_test/lib/src/a build/native_test/lib/src/b build/native_test/lib/cpp "
        "build/native_test/lib/include build/native_test/app/src");
#endif
    write_file("build/native_test/lib/include/lib.h", "int lib_a(void);\nint lib_b(void);\n");
    write_file("build/native_test/lib/src/a/part.c", "#include \"lib.h\"\nint lib_a(void) { return LIB_BASE + 1; }\n");
    write_file("build/native_test/lib/src/b/part.c", "#include \"lib.h\"\nint lib_b(void) { return 2; }\n");
    write_file("build/native_test/lib/cpp/version.cpp",
               "#include <string>\nextern \"C\" int lib_version(void) { return (int)std::string(\"v1\").size(); }\n");
    write_file("build/native_test/app/src/main.sosu",
               "import std.io;\n\nextern \"C\" {\n    i32 lib_a();\n    i32 lib_b();\n    i32 lib_version();\n}\n\n"
               "i32 main() uses io {\n    unsafe {\n        io.print(\"{} {} {}\", lib_a(), lib_b(), lib_version());\n    }\n"
               "    return 0;\n}\n");
    bool ready = lib_git("init --quiet") && lib_git("add .") && lib_git("commit --quiet -m one") && lib_git("tag v1.0.0");
    CHECK(ready, "native: cannot create the test repository (is git installed?)");
    if (!ready) return;

    char command[1024];
    write_app_manifest("v1.0.0", "src/**");
    snprintf(command, sizeof command, "%s run build/native_test/app > build/native.out 2> build/native.err", sosuc);
    CHECK(run(command) == 0, "native: run failed (see build/native.err)");
    char *out = read_text("build/native.out");
    CHECK(out && strcmp(out, "101 2 2\n") == 0, "native: output `%s`, expected `101 2 2`", out ? out : "(none)");
    free(out);
    char *lock = read_text("build/native_test/app/sosu.lock");
    CHECK(lock && strstr(lock, "\nnative.lib ../lib tag v1.0.0 "), "native: sosu.lock\n%s", lock ? lock : "(none)");
    free(lock);

    /* newer tags: a pre-release is skipped */
    write_file("build/native_test/lib/src/b/part.c", "#include \"lib.h\"\nint lib_b(void) { return 3; }\n");
    CHECK(lib_git("commit --quiet -am two") && lib_git("tag v1.2.0") && lib_git("tag v2.0-beta") && lib_git("tag nightly"),
          "native: second commit");
    snprintf(command, sizeof command, "%s deps upgrade build/native_test/app > build/native_deps.out 2> build/native.err",
             sosuc);
    CHECK(run(command) == 0, "native: deps upgrade failed (see build/native.err)");
    char *manifest = read_text("build/native_test/app/sosu.toml");
    CHECK(manifest && strstr(manifest, "tag = \"v1.2.0\""), "native: deps upgrade wrote\n%s", manifest ? manifest : "");
    CHECK(manifest && strstr(manifest, "# C and C++ files of a Git repository\n"), "native: the comments are kept");
    free(manifest);
    snprintf(command, sizeof command, "%s deps upgrade build/native_test/app > build/native_deps.out 2> build/native.err",
             sosuc);
    CHECK(run(command) == 0, "native: deps upgrade, already the newest");
    out = read_text("build/native_deps.out");
    CHECK(out && strstr(out, "lib: v1.2.0 is the newest"), "native: deps upgrade said `%s`", out ? out : "");
    free(out);
    snprintf(command, sizeof command, "%s run build/native_test/app > build/native.out 2> build/native.err", sosuc);
    CHECK(run(command) == 0, "native: run after the upgrade failed (see build/native.err)");
    out = read_text("build/native.out");
    CHECK(out && strcmp(out, "101 3 2\n") == 0, "native: output after the upgrade `%s`", out ? out : "(none)");
    free(out);
    lock = read_text("build/native_test/app/sosu.lock");
    CHECK(lock && strstr(lock, "\nnative.lib ../lib tag v1.2.0 "), "native: sosu.lock upgraded\n%s", lock ? lock : "");
    free(lock);
    snprintf(command, sizeof command, "%s deps upgrade build/native_test/app other > build/native_deps.out 2> build/native.err",
             sosuc);
    CHECK(run(command) == 2, "native: deps upgrade of an unknown name is refused");

    /* a path above the clone */
    write_app_manifest("v1.2.0", "../src/**");
    snprintf(command, sizeof command, "%s build build/native_test/app > build/native.out 2> build/native.err", sosuc);
    CHECK(run(command) != 0, "native: a path above the repository was accepted");
    char *err = read_text("build/native.err");
    CHECK(err && strstr(err, "leaves its repository"), "native: the refusal says `%s`", err ? err : "");
    free(err);
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
        /* an example is named by its folder, examples/<name>; a test program by its name */
        const char *slash = strchr(c->program, '/');
        const char *name = slash ? slash + 1 : c->program;
        char command[1024], output[256], expected_path[256], folder[256];
        if (slash) {
            snprintf(folder, sizeof folder, "%s", c->program);
            snprintf(expected_path, sizeof expected_path, "tests/examples/%s.txt", name);
        } else {
            snprintf(folder, sizeof folder, "tests/programs/%s", c->program);
            snprintf(expected_path, sizeof expected_path, "tests/programs/%s/%s", c->program, c->expected);
        }
        snprintf(output, sizeof output, "build%s%s.out", sep, name);
        snprintf(command, sizeof command, "%s %s %s%s%s > %s 2> build%s%s.err", argv[1], c->command, folder,
                 c->args ? " -- " : "", c->args ? c->args : "", output, sep, name);
        int status = run(command);
        CHECK(status == c->exit_code, "%s %s: exit code %d, expected %d (see build/%s.err)", c->command, c->program,
              status, c->exit_code, name);

        char *actual = read_text(output);
        char *expected = read_text(expected_path);
        CHECK(actual && expected && strcmp(actual, expected) == 0, "%s %s: output differs\n--- got:\n%s--- expected:\n%s",
              c->command, c->program, actual ? actual : "(none)\n", expected ? expected : "(none)\n");
        free(actual);
        free(expected);
    }

    test_library(argv[1]);
    test_bindgen(argv[1]);
    test_gpu(argv[1]);
    test_doc(argv[1]);
    test_guide(argv[1], "quick", "docs/QUICK_GUIDE.md", 30);
    test_guide(argv[1], "guide", "docs/GUIDE.md", 55);
    test_freed_arena(argv[1]);
    test_stack_overflow(argv[1]);
    test_package(argv[1]);
    test_android(argv[1]);
    test_native(argv[1]);

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
