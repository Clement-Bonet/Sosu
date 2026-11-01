/* The time of sosuc on large programs (work 5 of spec §13.1). A package of many modules, each
   the same block of Sosu written many times over (structs, enums and `match`, results and `try`,
   generics, classes with virtual methods, lambdas, loops), is written in build/bench/, then
   loaded, checked and turned into C in this process, as `sosuc build` does before the C
   compiler. Each size is measured three times, and the shortest kept.

   The program is also made four times larger, in modules and then in one module: the time
   should grow about as much (linearly). Growing more than SCALE_LIMIT times fails, which an
   algorithm that is quadratic in the size of the program does (16 times). The times go to the
   output, and to the summary of the job on GitHub ($GITHUB_STEP_SUMMARY).

   Run from the repository root, where the standard library is in lib/: `make bench`. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "codegen.h"
#include "shell.h"

#define SCALE_LIMIT 8.0

/* One block; `@` is replaced by its number, so that each block has names of its own. */
static const char *block =
    "struct Point@ {\n"
    "    i32 x;\n"
    "    i32 y;\n"
    "\n"
    "    i32 sum() {\n"
    "        return x + y;\n"
    "    }\n"
    "}\n"
    "\n"
    "enum Shape@ {\n"
    "    Circle(i32 r),\n"
    "    Rect(i32 w, i32 h),\n"
    "    Empty,\n"
    "}\n"
    "\n"
    "i32 area@(Shape@ s) {\n"
    "    return match (s) {\n"
    "        Circle(r) => 3 * r * r,\n"
    "        Rect(w, h) => w * h,\n"
    "        Empty => 0,\n"
    "    };\n"
    "}\n"
    "\n"
    "enum Fail@ { Negative, TooLarge }\n"
    "\n"
    "i32!Fail@ checked@(i32 n) {\n"
    "    if (n < 0) {\n"
    "        return err(Negative);\n"
    "    }\n"
    "    if (n > 1000000) {\n"
    "        return err(TooLarge);\n"
    "    }\n"
    "    return ok(n);\n"
    "}\n"
    "\n"
    "i32!Fail@ doubled@(i32 n) {\n"
    "    i32 v = try checked@(n);\n"
    "    return ok(v * 2);\n"
    "}\n"
    "\n"
    "T largest@<T: Ordered>(T[] items) {\n"
    "    mut T best = items[0];\n"
    "    for (T x in items) {\n"
    "        if (x > best) {\n"
    "            best = x;\n"
    "        }\n"
    "    }\n"
    "    return best;\n"
    "}\n"
    "\n"
    "class Counter@ {\n"
    "    mut i32 count;\n"
    "\n"
    "    void init(i32 start) {\n"
    "        count = start;\n"
    "    }\n"
    "\n"
    "    virtual i32 step() {\n"
    "        count += 1;\n"
    "        return count;\n"
    "    }\n"
    "}\n"
    "\n"
    "class Doubler@ : Counter@ {\n"
    "    void init() {\n"
    "        base.init(1);\n"
    "    }\n"
    "\n"
    "    override i32 step() {\n"
    "        count *= 2;\n"
    "        return count;\n"
    "    }\n"
    "}\n"
    "\n"
    "i32 apply@(fn(i32) -> i32 f, i32 x) {\n"
    "    return f(x);\n"
    "}\n"
    "\n"
    "i32 run@(Arena* arena, i32 seed) {\n"
    "    Point@ p = Point@{ x: seed, y: 2 };\n"
    "    mut i32 total = p.sum() + area@(Rect(seed, 3)) + area@(Circle(2)) + area@(Empty);\n"
    "    i32 twice = match (doubled@(seed)) {\n"
    "        ok(v) => v,\n"
    "        err(e) => 0,\n"
    "    };\n"
    "    total += twice;\n"
    "    mut i32[4] values = [seed, 7, 3, 9];\n"
    "    total += largest@(values[0..4]);\n"
    "    Doubler@* d = arena.new(Doubler@);\n"
    "    total += d.step() + d.step();\n"
    "    i32 k = seed;\n"
    "    total += apply@((i32 x) => x + k, 5);\n"
    "    for (i32 i in 0..10) {\n"
    "        total += i % 3 == 0 ? i : -i;\n"
    "    }\n"
    "    return total;\n"
    "}\n"
    "\n";

static size_t lines_of(const char *text) {
    size_t lines = 0;
    for (const char *c = text; *c; c++) lines += *c == '\n';
    return lines;
}

static bool write_text(const char *path, const char *text) {
    FILE *file = fopen(path, "wb");
    if (!file) return false;
    bool ok = fputs(text, file) >= 0;
    return fclose(file) == 0 && ok;
}

static void append(char **text, size_t *size, size_t *cap, const char *part) {
    size_t length = strlen(part);
    if (*size + length + 1 > *cap) {
        *cap = 2 * (*size + length + 1);
        *text = sosu_realloc(*text, *cap);
    }
    memcpy(*text + *size, part, length + 1);
    *size += length;
}

/* The package `bench` in `dir`: `modules` modules of `blocks` blocks each. Returns its lines. */
static size_t write_package(const char *dir, int modules, int blocks) {
    shell_remove_dir(dir);
    char *src = sosu_format("%s/src", dir);
    shell_make_dir(src);
    char *manifest = sosu_format("%s/sosu.toml", dir);
    write_text(manifest, "[package]\nname = \"bench\"\nversion = \"0.1.0\"\n");
    free(manifest);
    size_t lines = 0;
    char *main_text = NULL;
    size_t main_size = 0, main_cap = 0;
    append(&main_text, &main_size, &main_cap, "import std.io;\n");
    for (int m = 0; m < modules; m++) {
        char *line = sosu_format("import bench.m%d;\n", m);
        append(&main_text, &main_size, &main_cap, line);
        free(line);
        char *text = NULL;
        size_t size = 0, cap = 0;
        char *run = sosu_strdup("pub i32 run(Arena* arena, i32 seed) {\n    mut i32 total = 0;\n");
        for (int b = 0; b < blocks; b++) {
            char number[16];
            snprintf(number, sizeof number, "%d", b);
            for (const char *c = block; *c; c++) {
                char one[2] = { *c, '\0' };
                append(&text, &size, &cap, *c == '@' ? number : one);
            }
            char *call = sosu_format("%s    total += run%d(arena, seed + %d);\n", run, b, b);
            free(run);
            run = call;
        }
        append(&text, &size, &cap, run);
        append(&text, &size, &cap, "    return total;\n}\n");
        free(run);
        char *path = sosu_format("%s/m%d.sosu", src, m);
        write_text(path, text);
        lines += lines_of(text);
        free(path);
        free(text);
    }
    append(&main_text, &main_size, &main_cap,
           "\ni32 main() uses io {\n    mut Arena arena = Arena.new(64 * KB);\n    defer arena.free();\n    mut i32 total = 0;\n");
    for (int m = 0; m < modules; m++) {
        char *line = sosu_format("    total += m%d.run(&arena, %d);\n", m, m);
        append(&main_text, &main_size, &main_cap, line);
        free(line);
    }
    append(&main_text, &main_size, &main_cap, "    io.print(\"{}\", total);\n    return 0;\n}\n");
    char *main_path = sosu_format("%s/main.sosu", src);
    write_text(main_path, main_text);
    lines += lines_of(main_text);
    free(main_path);
    free(main_text);
    free(src);
    return lines;
}

typedef struct {
    double load, check, generate; /* seconds of processor time */
} Times;

static double seconds_since(clock_t start) {
    return (double)(clock() - start) / CLOCKS_PER_SEC;
}

/* Loads, checks and generates the package in `dir`; false if it has an error. */
static bool measure_once(const char *dir, Times *times) {
    Program program;
    Sema sema = {0};
    clock_t start = clock();
    bool ok = program_load(&program, dir, "lib");
    times->load = seconds_since(start);
    start = clock();
    if (ok) ok = sema_check(&sema, &program);
    times->check = seconds_since(start);
    start = clock();
    char *c = ok ? codegen_program(&sema, &program, CODEGEN_PROGRAM, (CodegenOptions){ true, true, true }) : NULL;
    times->generate = seconds_since(start);
    if (!c) {
        if (program.fatal) fprintf(stderr, "bench: %s\n", program.fatal);
        diag_print_text(&program.diags, stderr);
    }
    free(c);
    sema_free(&sema);
    program_free(&program);
    return c != NULL;
}

static double total(const Times *t) { return t->load + t->check + t->generate; }

/* The shortest of three runs, whose lines are `lines`. */
static bool measure(const char *name, const char *dir, size_t lines, Times *best, FILE *summary) {
    for (int i = 0; i < 3; i++) {
        Times times;
        if (!measure_once(dir, &times)) {
            fprintf(stderr, "bench: the program of `%s` does not compile\n", name);
            return false;
        }
        if (i == 0 || total(&times) < total(best)) *best = times;
    }
    double all = total(best);
    printf("%-26s %7zu lines  load %6.3f s  check %6.3f s  C %6.3f s  total %6.3f s  %8.0f lines/s\n", name, lines,
           best->load, best->check, best->generate, all, all > 0 ? (double)lines / all : 0.0);
    if (summary) {
        fprintf(summary, "| %s | %zu | %.3f | %.3f | %.3f | %.3f | %.0f |\n", name, lines, best->load, best->check,
                best->generate, all, all > 0 ? (double)lines / all : 0.0);
    }
    return true;
}

/* Measures `modules` x `blocks`, then four times as much: true if the time grew less than
   SCALE_LIMIT times. */
static bool scale(const char *what, int modules, int blocks, int more_modules, int more_blocks, FILE *summary) {
    const char *dir = "build/bench";
    char *small_name = sosu_format("%s, %d x %d", what, modules, blocks);
    char *large_name = sosu_format("%s, %d x %d", what, more_modules, more_blocks);
    Times small = { 0, 0, 0 }, large = { 0, 0, 0 };
    size_t small_lines = write_package(dir, modules, blocks);
    bool ok = measure(small_name, dir, small_lines, &small, summary);
    size_t large_lines = ok ? write_package(dir, more_modules, more_blocks) : 0;
    ok = ok && measure(large_name, dir, large_lines, &large, summary);
    if (ok) {
        double ratio = total(&small) > 0 ? total(&large) / total(&small) : 0.0;
        double size_ratio = (double)large_lines / (double)small_lines;
        printf("  %.1f times the lines: %.1f times the time\n", size_ratio, ratio);
        if (ratio > SCALE_LIMIT) printf("  more than %.0f times: an algorithm grows faster than the program\n", SCALE_LIMIT);
        if (summary) fprintf(summary, "| %.1f times the lines | | | | %.1f times the time | |\n", size_ratio, ratio);
        ok = ratio <= SCALE_LIMIT;
    }
    free(small_name);
    free(large_name);
    return ok;
}

int main(int argc, char **argv) {
    if (argc == 3) { /* `bench <modules> <blocks>`: that size only */
        Times times = { 0, 0, 0 };
        size_t lines = write_package("build/bench", atoi(argv[1]), atoi(argv[2]));
        bool ok = measure("the program", "build/bench", lines, &times, NULL);
        shell_remove_dir("build/bench");
        return ok ? 0 : 1;
    }
    const char *summary_path = getenv("GITHUB_STEP_SUMMARY");
    FILE *summary = summary_path && summary_path[0] ? fopen(summary_path, "a") : NULL;
    if (summary) {
        fputs("### The time of sosuc (tests/bench.c)\n\n| program | lines | load (s) | check (s) | C (s) | total (s) "
              "| lines/s |\n|---|---|---|---|---|---|---|\n",
              summary);
    }
    bool ok = scale("modules", 10, 20, 40, 20, summary);
    ok = scale("one module", 1, 50, 1, 200, summary) && ok;
    if (summary) fclose(summary);
    shell_remove_dir("build/bench");
    return ok ? 0 : 1;
}
