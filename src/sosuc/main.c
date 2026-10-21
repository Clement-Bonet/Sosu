#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "codegen.h"
#include "path.h"
#include "sema.h"

typedef enum {
    FORMAT_TEXT,
    FORMAT_JSON,
} DiagFormat;

static void usage(FILE *out) {
    fputs("usage: sosuc <command> [options] [path]\n"
          "\n"
          "commands:\n"
          "  check   check a package (default: current folder), its dependencies and modules\n"
          "  lex     print the tokens of a file (debugging)\n"
          "  parse   print the syntax tree of a file (debugging)\n"
          "  build   compile a package into build/<name> (through C)\n"
          "  run     compile and run a package\n"
          "  test    compile and run the test blocks of a package\n"
          "  fmt     format source files (not implemented yet)\n"
          "\n"
          "options:\n"
          "  --format=text|json   diagnostics format (default: text)\n"
          "  --release            optimized build: no overflow checks, no contracts (spec 7.7)\n"
          "  --checks=on|off      index checks (on by default, also in release)\n"
          "  --contracts=check|off  requires / ensures (checked by default, off in release)\n"
          "  --version            print the compiler version\n",
          out);
}

static void print_diagnostics(const DiagList *diags, DiagFormat format) {
    if (format == FORMAT_JSON) {
        diag_print_json(diags, stderr);
        return;
    }
    diag_print_text(diags, stderr);
    if (diags->error_count > 0) {
        fprintf(stderr, "sosuc: %zu error%s\n", diags->error_count, diags->error_count == 1 ? "" : "s");
    }
}

static bool load(SourceFile *file, const char *path) {
    if (source_load(file, path)) return true;
    fprintf(stderr, "sosuc: cannot read `%s`: %s\n", path, strerror(errno));
    return false;
}

static bool has_suffix(const char *text, const char *suffix) {
    size_t length = strlen(text);
    size_t suffix_length = strlen(suffix);
    return length >= suffix_length && strcmp(text + length - suffix_length, suffix) == 0;
}

static int command_lex(const char *path, DiagFormat format) {
    SourceFile file;
    if (!load(&file, path)) return 2;

    TokenList tokens = {0};
    DiagList diags = {0};
    bool ok = lex_file(&file, &tokens, &diags);

    for (size_t i = 0; i < tokens.count; i++) {
        const Token *t = &tokens.items[i];
        size_t line, column;
        source_position(&file, t->offset, &line, &column);
        printf("%4zu:%-4zu %-12s %.*s", line, column, token_kind_name(t->kind), (int)t->length, file.text + t->offset);
        if (t->kind == TOK_INT) printf("  (%llu)", (unsigned long long)t->int_value);
        if (t->kind == TOK_CHAR) printf("  (U+%04X)", (unsigned)t->char_value);
        putchar('\n');
    }
    print_diagnostics(&diags, format);

    diag_free(&diags);
    token_list_free(&tokens);
    source_free(&file);
    return ok ? 0 : 1;
}

static int command_parse(const char *path, DiagFormat format) {
    if (has_suffix(path, ".ui.sosu")) {
        fprintf(stderr, "sosuc: `.ui.sosu` files are translated by the `ui` package, which does not exist yet\n");
        return 2;
    }
    SourceFile file;
    if (!load(&file, path)) return 2;

    TokenList tokens = {0};
    DiagList diags = {0};
    Arena arena = {0};
    lex_file(&file, &tokens, &diags);
    Module *module = parse_module(&file, &tokens, &arena, &diags);
    ast_print_module(module, &file, stdout);
    print_diagnostics(&diags, format);
    int status = diags.error_count ? 1 : 0;

    arena_free(&arena);
    diag_free(&diags);
    token_list_free(&tokens);
    source_free(&file);
    return status;
}

/* Folder of the `core` and `std` packages: $SOSU_HOME/lib, or ../lib next to sosuc. */
static char *library_dir(const char *argv0) {
    const char *home = getenv("SOSU_HOME");
    if (home && home[0]) return path_join(home, "lib");
    char *bin = path_dirname(argv0);
    char *lib = path_join(bin, "../lib");
    free(bin);
    return lib;
}

static int command_check(const char *dir, const char *argv0, DiagFormat format) {
    char *lib = library_dir(argv0);
    Program program;
    bool ok = program_load(&program, dir, lib);
    free(lib);
    /* Semantic analysis only runs on a program that parsed without errors, to avoid cascades. */
    if (ok) {
        Sema sema;
        ok = sema_check(&sema, &program);
        sema_free(&sema);
    }
    if (program.fatal) fprintf(stderr, "sosuc: %s\n", program.fatal);
    print_diagnostics(&program.diags, format);
    if (ok) {
        printf("checked %zu module%s in %zu package%s\n", program.units.count, program.units.count == 1 ? "" : "s",
               program.packages.count, program.packages.count == 1 ? "" : "s");
    }
    int status = program.fatal ? 2 : ok ? 0 : 1;
    program_free(&program);
    return status;
}

/* ---- build, run, test ---- */

typedef enum {
    MODE_BUILD,
    MODE_RUN,
    MODE_TEST,
} BuildMode;

typedef struct {
    bool release; /* optimized C, no overflow checks or contracts by default */
    CodegenOptions checks;
} BuildOptions;

/* A path in the form the system shell expects. */
static char *native_path(const char *path) {
    char *result = sosu_strdup(path);
#ifdef _WIN32
    for (char *c = result; *c; c++) {
        if (*c == '/') *c = '\\';
    }
#endif
    return result;
}

static int run_command(const char *command) {
    fflush(stdout);
    fflush(stderr);
    int status = system(command);
#ifndef _WIN32
    if (status != -1) status = (status >> 8) & 0xFF; /* exit code from the wait status */
#endif
    return status;
}

static bool write_file(const char *path, const char *text) {
    FILE *file = fopen(path, "wb");
    if (!file) return false;
    bool ok = fputs(text, file) >= 0;
    return fclose(file) == 0 && ok;
}

/* Compiles the generated C file with the runtime. Returns false if the C compiler failed. */
static bool compile_c(const char *build_dir, const char *c_file, const char *exe, const char *lib, bool optimize) {
    char *runtime = path_join(lib, "runtime");
    char *runtime_c = path_join(runtime, "sosu_rt.c");
    const char *cc = getenv("SOSU_CC");
    char *command;
#ifdef _WIN32
    if (!cc || !cc[0]) {
        /* MSVC, found like build.bat does when cl.exe is not already on the PATH */
        char *bat = path_join(build_dir, "compile.bat");
        char *dir = native_path(build_dir);
        char *rt = native_path(runtime);
        char *rt_c = native_path(runtime_c);
        char *c = native_path(c_file);
        char *out = native_path(exe);
        char *script = sosu_format(
            "@echo off\r\n"
            "where cl >nul 2>nul && goto :have_cl\r\n"
            "set \"VSWHERE=%%ProgramFiles(x86)%%\\Microsoft Visual Studio\\Installer\\vswhere.exe\"\r\n"
            "if not exist \"%%VSWHERE%%\" goto :no_cl\r\n"
            "for /f \"usebackq tokens=*\" %%%%i in (`\"%%VSWHERE%%\" -latest -products * -requires "
            "Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set \"VSDIR=%%%%i\"\r\n"
            "if not defined VSDIR goto :no_cl\r\n"
            "set \"PATH=%%PATH%%;%%ProgramFiles(x86)%%\\Microsoft Visual Studio\\Installer\"\r\n"
            "call \"%%VSDIR%%\\VC\\Auxiliary\\Build\\vcvars64.bat\" >nul || goto :no_cl\r\n"
            ":have_cl\r\n"
            "cl /nologo /std:c11 /utf-8 /W0 /Zi %s /I\"%s\" /Fo\"%s\\\\\" /Fd\"%s\\\\\" /Fe\"%s\" \"%s\" \"%s\" "
            ">\"%s\\cl.log\" || (type \"%s\\cl.log\" & exit /b 1)\r\n"
            "exit /b 0\r\n"
            ":no_cl\r\n"
            "echo sosuc: no C compiler found: install Visual Studio with the C++ tools, or set SOSU_CC 1>&2\r\n"
            "exit /b 2\r\n",
            optimize ? "/O2" : "/Od", rt, dir, dir, out, c, rt_c, dir, dir);
        bool written = write_file(bat, script);
        char *native_bat = native_path(bat);
        command = written ? sosu_format("\"%s\"", native_bat) : NULL;
        free(native_bat);
        free(script);
        free(bat);
        free(dir);
        free(rt);
        free(rt_c);
        free(c);
        free(out);
        if (!command) {
            fprintf(stderr, "sosuc: cannot write in `%s`\n", build_dir);
            free(runtime);
            free(runtime_c);
            return false;
        }
    } else
#endif
    {
        (void)build_dir;
        command = sosu_format("\"%s\" -std=c99 -g %s -w -I\"%s\" \"%s\" \"%s\" -o \"%s\"", cc && cc[0] ? cc : "cc", optimize ? "-O2" : "-O0", runtime,
                              c_file, runtime_c, exe);
    }
    int status = run_command(command);
    free(command);
    free(runtime);
    free(runtime_c);
    return status == 0;
}

static int command_build(const char *dir, const char *argv0, DiagFormat format, BuildMode mode, const BuildOptions *options) {
    char *lib = library_dir(argv0);
    Program program;
    Sema sema = {0};
    char *source = NULL;
    bool ok = program_load(&program, dir, lib);
    if (ok) ok = sema_check(&sema, &program);
    if (ok) {
        source = codegen_program(&sema, &program, mode == MODE_TEST ? CODEGEN_TESTS : CODEGEN_PROGRAM, options->checks);
        ok = source != NULL;
    }
    if (program.fatal) fprintf(stderr, "sosuc: %s\n", program.fatal);
    print_diagnostics(&program.diags, format);

    int status = program.fatal ? 2 : 1;
    if (ok) {
        const Package *root = program.packages.items[0];
        char *build_dir = path_join(root->root, "build");
        char *native_dir = native_path(build_dir);
#ifdef _WIN32
        char *mkdir = sosu_format("if not exist \"%s\" mkdir \"%s\"", native_dir, native_dir);
#else
        char *mkdir = sosu_format("mkdir -p \"%s\"", native_dir);
#endif
        run_command(mkdir);
        free(mkdir);
        free(native_dir);

        char *name = sosu_format(mode == MODE_TEST ? "%s_tests" : "%s", root->name);
        char *file = sosu_format("%s.c", name);
        char *c_file = path_join(build_dir, file);
#ifdef _WIN32
        char *exe_name = sosu_format("%s.exe", name);
#else
        char *exe_name = sosu_strdup(name);
#endif
        char *exe = path_join(build_dir, exe_name);
        if (!write_file(c_file, source)) {
            fprintf(stderr, "sosuc: cannot write `%s`\n", c_file);
            status = 2;
        } else if (!compile_c(build_dir, c_file, exe, lib, options->release)) {
            fprintf(stderr, "sosuc: the C compiler failed on `%s`\n", c_file);
            status = 2;
        } else if (mode == MODE_BUILD) {
            printf("built %s\n", exe);
            status = 0;
        } else {
            char *native_exe = native_path(exe);
            char *command = sosu_format("\"%s\"", native_exe);
            status = run_command(command);
            free(command);
            free(native_exe);
        }
        free(exe);
        free(exe_name);
        free(c_file);
        free(file);
        free(name);
        free(build_dir);
    }
    free(source);
    free(lib);
    sema_free(&sema);
    program_free(&program);
    return status;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        usage(stderr);
        return 2;
    }
    const char *command = argv[1];
    if (strcmp(command, "--version") == 0) {
        printf("sosuc %s\n", SOSU_VERSION);
        return 0;
    }
    if (strcmp(command, "--help") == 0 || strcmp(command, "-h") == 0) {
        usage(stdout);
        return 0;
    }

    DiagFormat format = FORMAT_TEXT;
    const char *path = NULL;
    BuildOptions options = { false, { true, true, true } };
    int checks = -1;    /* --checks: -1 when not given */
    int contracts = -1; /* --contracts */
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--format=text") == 0) {
            format = FORMAT_TEXT;
        } else if (strcmp(argv[i], "--format=json") == 0) {
            format = FORMAT_JSON;
        } else if (strcmp(argv[i], "--release") == 0) {
            options.release = true;
        } else if (strcmp(argv[i], "--checks=on") == 0 || strcmp(argv[i], "--checks=off") == 0) {
            checks = argv[i][10] == 'n';
        } else if (strcmp(argv[i], "--contracts=check") == 0 || strcmp(argv[i], "--contracts=off") == 0) {
            contracts = argv[i][12] == 'c';
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "sosuc: unknown option `%s`\n", argv[i]);
            return 2;
        } else if (path) {
            fprintf(stderr, "sosuc: only one file is supported for now\n");
            return 2;
        } else {
            path = argv[i];
        }
    }

    /* spec §7.7: release keeps the bounds checks only */
    options.checks.overflow_checks = !options.release;
    options.checks.contracts = contracts >= 0 ? contracts == 1 : !options.release;
    options.checks.bounds_checks = checks != 0;

    if (strcmp(command, "check") == 0) return command_check(path ? path : ".", argv[0], format);
    if (strcmp(command, "lex") == 0 || strcmp(command, "parse") == 0) {
        if (!path) {
            fputs("sosuc: missing file\n", stderr);
            return 2;
        }
        return command[0] == 'l' ? command_lex(path, format) : command_parse(path, format);
    }
    if (strcmp(command, "build") == 0) return command_build(path ? path : ".", argv[0], format, MODE_BUILD, &options);
    if (strcmp(command, "run") == 0) return command_build(path ? path : ".", argv[0], format, MODE_RUN, &options);
    if (strcmp(command, "test") == 0) return command_build(path ? path : ".", argv[0], format, MODE_TEST, &options);
    if (strcmp(command, "fmt") == 0) {
        fprintf(stderr, "sosuc: `%s` is not implemented yet\n", command);
        return 2;
    }
    fprintf(stderr, "sosuc: unknown command `%s`\n", command);
    usage(stderr);
    return 2;
}
