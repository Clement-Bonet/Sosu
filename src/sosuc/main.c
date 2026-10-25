#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#include "codegen.h"
#include "fmt.h"
#include "i18n.h"
#include "lsp.h"
#include "path.h"
#include "sema.h"
#include "shell.h"

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
          "  fmt     format files or packages in place (--check: only report)\n"
          "  lsp     run the language server (Language Server Protocol, on stdin and stdout)\n"
          "  i18n extract [package] [-o messages.locale.default.sosu]\n"
          "          write the template of the catalogs of translations (spec 10.7)\n"
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
    SourceFile file;
    if (!load(&file, path)) return 2;

    TokenList tokens = {0};
    DiagList diags = {0};
    Arena arena = {0};
    lex_file(&file, &tokens, &diags);
    /* an interface file shows the functions it becomes (spec §10.2) */
    Module *module = program_is_ui_file(path) ? parse_ui_module(&file, &tokens, &arena, &diags)
                                              : parse_module(&file, &tokens, &arena, &diags);
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

static bool write_file(const char *path, const char *text) {
    FILE *file = fopen(path, "wb");
    if (!file) return false;
    bool ok = fputs(text, file) >= 0;
    return fclose(file) == 0 && ok;
}

/* ---- Linking (the [link] sections of sosu.toml) ---- */

#if defined(_WIN32)
#define HOST_LINK LINK_WINDOWS
#elif defined(__APPLE__)
#define HOST_LINK LINK_MACOS
#else
#define HOST_LINK LINK_LINUX
#endif

typedef struct {
    StringList libs;
    StringList paths; /* relative to the current folder */
    StringList frameworks;
    StringList copy;
} Links;

static void add_unique(StringList *list, char *item) {
    for (size_t i = 0; i < list->count; i++) {
        if (strcmp(list->items[i], item) == 0) {
            free(item);
            return;
        }
    }
    SOSU_VEC_PUSH(list, item);
}

static char *package_path(const Package *package, const char *relative) {
    char *joined = path_join(package->root, relative);
    char *result = path_normalize(joined);
    free(joined);
    return result;
}

/* [link] and [link.<host>] of every package: a dependency's libraries reach the program. */
static void collect_links(const Program *program, Links *links) {
    memset(links, 0, sizeof *links);
    int sections[] = { LINK_ALL, HOST_LINK };
    for (size_t i = 0; i < program->packages.count; i++) {
        const Package *package = program->packages.items[i];
        for (size_t s = 0; s < 2; s++) {
            const LinkSection *link = &package->manifest.link[sections[s]];
            for (size_t j = 0; j < link->libs.count; j++) add_unique(&links->libs, sosu_strdup(link->libs.items[j]));
            for (size_t j = 0; j < link->paths.count; j++) add_unique(&links->paths, package_path(package, link->paths.items[j]));
            for (size_t j = 0; j < link->frameworks.count; j++) {
                add_unique(&links->frameworks, sosu_strdup(link->frameworks.items[j]));
            }
            for (size_t j = 0; j < link->copy.count; j++) add_unique(&links->copy, package_path(package, link->copy.items[j]));
        }
    }
}

static void free_links(Links *links) {
    StringList *lists[] = { &links->libs, &links->paths, &links->frameworks, &links->copy };
    for (size_t i = 0; i < 4; i++) {
        for (size_t j = 0; j < lists[i]->count; j++) free(lists[i]->items[j]);
        free(lists[i]->items);
    }
}

static void append(char **text, const char *fmt, const char *value) {
    char *part = sosu_format(fmt, value);
    char *joined = sosu_format("%s%s", *text, part);
    free(*text);
    free(part);
    *text = joined;
}

/* The arguments that make the C compiler link the libraries. */
static char *link_arguments(const Links *links, bool msvc) {
    char *args = sosu_strdup("");
    if (msvc) {
        if (links->libs.count == 0 && links->paths.count == 0) return args;
        append(&args, "%s", " /link");
        for (size_t i = 0; i < links->paths.count; i++) {
            char *native = shell_path(links->paths.items[i]);
            append(&args, " /LIBPATH:\"%s\"", native);
            free(native);
        }
        for (size_t i = 0; i < links->libs.count; i++) append(&args, " \"%s.lib\"", links->libs.items[i]);
        return args;
    }
    for (size_t i = 0; i < links->paths.count; i++) append(&args, " -L\"%s\"", links->paths.items[i]);
    for (size_t i = 0; i < links->libs.count; i++) append(&args, " -l%s", links->libs.items[i]);
    for (size_t i = 0; i < links->frameworks.count; i++) append(&args, " -framework %s", links->frameworks.items[i]);
    return args;
}

static bool copy_file(const char *from, const char *to) {
    FILE *in = fopen(from, "rb");
    if (!in) return false;
    FILE *out = fopen(to, "wb");
    if (!out) {
        fclose(in);
        return false;
    }
    char buffer[8192];
    size_t n;
    bool ok = true;
    while ((n = fread(buffer, 1, sizeof buffer, in)) > 0) ok &= fwrite(buffer, 1, n, out) == n;
    ok &= !ferror(in);
    fclose(in);
    return fclose(out) == 0 && ok;
}

/* The files of `copy` go next to the program (DLLs on Windows...). */
static bool copy_files(const Links *links, const char *build_dir) {
    for (size_t i = 0; i < links->copy.count; i++) {
        const char *from = links->copy.items[i];
        const char *slash = strrchr(from, '/');
        char *to = path_join(build_dir, slash ? slash + 1 : from);
        bool ok = copy_file(from, to);
        if (!ok) fprintf(stderr, "sosuc: cannot copy `%s` (listed in `copy` of a [link] section) to `%s`\n", from, to);
        free(to);
        if (!ok) return false;
    }
    return true;
}

/* Compiles the generated C file with the runtime. Returns false if the C compiler failed. */
static bool compile_c(const char *build_dir, const char *c_file, const char *exe, const char *lib, bool optimize,
                      const Links *links) {
    char *runtime = path_join(lib, "runtime");
    char *runtime_c = path_join(runtime, "sosu_rt.c");
    const char *cc = getenv("SOSU_CC");
    char *command;
#ifdef _WIN32
    if (!cc || !cc[0]) {
        /* MSVC, found like build.bat does when cl.exe is not already on the PATH */
        char *bat = path_join(build_dir, "compile.bat");
        char *dir = shell_path(build_dir);
        char *rt = shell_path(runtime);
        char *rt_c = shell_path(runtime_c);
        char *c = shell_path(c_file);
        char *out = shell_path(exe);
        char *link_args = link_arguments(links, true);
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
            "cl /nologo /std:c11 /utf-8 /W0 /Zi %s /I\"%s\" /Fo\"%s\\\\\" /Fd\"%s\\\\\" /Fe\"%s\" \"%s\" \"%s\"%s "
            ">\"%s\\cl.log\" || (type \"%s\\cl.log\" & exit /b 1)\r\n"
            "exit /b 0\r\n"
            ":no_cl\r\n"
            "echo sosuc: no C compiler found: install Visual Studio with the C++ tools, or set SOSU_CC 1>&2\r\n"
            "exit /b 2\r\n",
            optimize ? "/O2" : "/Od", rt, dir, dir, out, c, rt_c, link_args, dir, dir);
        bool written = write_file(bat, script);
        char *native_bat = shell_path(bat);
        command = written ? sosu_format("\"%s\"", native_bat) : NULL;
        free(native_bat);
        free(script);
        free(bat);
        free(dir);
        free(rt);
        free(rt_c);
        free(c);
        free(out);
        free(link_args);
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
        char *link_args = link_arguments(links, false);
        command = sosu_format("\"%s\" -std=c99 -g %s -w -I\"%s\" \"%s\" \"%s\" -o \"%s\"%s", cc && cc[0] ? cc : "cc",
                              optimize ? "-O2" : "-O0", runtime, c_file, runtime_c, exe, link_args);
        free(link_args);
    }
    int status = shell_run(command);
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
        shell_make_dir(build_dir);

        char *name = sosu_format(mode == MODE_TEST ? "%s_tests" : "%s", root->name);
        char *file = sosu_format("%s.c", name);
        char *c_file = path_join(build_dir, file);
#ifdef _WIN32
        char *exe_name = sosu_format("%s.exe", name);
#else
        char *exe_name = sosu_strdup(name);
#endif
        char *exe = path_join(build_dir, exe_name);
        Links links;
        collect_links(&program, &links);
        if (!write_file(c_file, source)) {
            fprintf(stderr, "sosuc: cannot write `%s`\n", c_file);
            status = 2;
        } else if (!compile_c(build_dir, c_file, exe, lib, options->release, &links)) {
            fprintf(stderr, "sosuc: the C compiler failed on `%s`\n", c_file);
            status = 2;
        } else if (!copy_files(&links, build_dir)) {
            status = 2;
        } else if (mode == MODE_BUILD) {
            printf("built %s\n", exe);
            status = 0;
        } else {
            char *native_exe = shell_path(exe);
            char *command = sosu_format("\"%s\"", native_exe);
            status = shell_run(command);
            free(command);
            free(native_exe);
        }
        free_links(&links);
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

/* ---- fmt ---- */

/* Formats one file. Returns 0 if it was already formatted (or has been rewritten),
   1 if it has errors or, with --check, would change. */
static int format_file(const char *path, bool check) {
    SourceFile file;
    if (!load(&file, path)) return 1;
    DiagList diags = {0};
    char *formatted = format_source(&file, &diags);
    int status = 0;
    if (!formatted) {
        diag_print_text(&diags, stderr);
        fprintf(stderr, "sosuc: `%s` is not formatted: fix its errors first\n", path);
        status = 1;
    } else if (strcmp(formatted, file.text) != 0) {
        if (check) {
            printf("would reformat %s\n", path);
            status = 1;
        } else if (write_file(path, formatted)) {
            printf("formatted %s\n", path);
        } else {
            fprintf(stderr, "sosuc: cannot write `%s`\n", path);
            status = 1;
        }
    }
    free(formatted);
    diag_free(&diags);
    source_free(&file);
    return status;
}

/* `sosuc fmt [--check] path...`: files, or package folders (the modules of the package,
   not of its dependencies). */
static int command_fmt(const char **paths, int count, bool check, const char *argv0) {
    int status = 0;
    char *lib = library_dir(argv0);
    for (int i = 0; i < count; i++) {
        const char *path = paths[i];
        if (has_suffix(path, ".ui.sosu")) {
            fprintf(stderr, "sosuc: `%s`: `.ui.sosu` files have no formatter yet\n", path);
            continue;
        }
        if (program_is_locale_file(path)) {
            fprintf(stderr, "sosuc: `%s`: a catalog of translations is not formatted\n", path);
            continue;
        }
        if (has_suffix(path, ".sosu")) {
            status |= format_file(path, check);
            continue;
        }
        Program program;
        program_load(&program, path, lib);
        if (program.fatal) {
            fprintf(stderr, "sosuc: %s\n", program.fatal);
            status = 1;
        } else {
            const Package *root = program.packages.items[0];
            for (size_t j = 0; j < program.units.count; j++) {
                const Unit *unit = program.units.items[j];
                /* interface files have no formatter yet: the one of .sosu files would break them */
                if (unit->package == root && !program_is_ui_file(unit->file.path)) {
                    status |= format_file(unit->file.path, check);
                }
            }
        }
        program_free(&program);
    }
    free(lib);
    return status;
}

/* `sosuc i18n extract [package] [-o messages.locale.default.sosu]`: the template of the
   catalogs of translations of a package (spec §10.7), on stdout or in a file. */
static int command_i18n(int argc, char **argv, const char *argv0) {
    if (argc < 1 || strcmp(argv[0], "extract") != 0) {
        fputs("usage: sosuc i18n extract [package] [-o messages.locale.default.sosu]\n", stderr);
        return 2;
    }
    const char *path = ".";
    const char *out = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            out = argv[++i];
        } else if (argv[i][0] != '-') {
            path = argv[i];
        } else {
            fprintf(stderr, "sosuc: unknown option `%s`\n", argv[i]);
            return 2;
        }
    }
    char *lib = library_dir(argv0);
    Program program;
    program_load(&program, path, lib);
    free(lib);
    if (program.fatal) {
        fprintf(stderr, "sosuc: %s\n", program.fatal);
        program_free(&program);
        return 1;
    }
    I18nTemplate *t = i18n_template_new();
    const Package *root = program.packages.items[0];
    for (size_t i = 0; i < program.units.count; i++) {
        const Unit *unit = program.units.items[i];
        if (unit->package == root) i18n_template_add(t, &unit->file, unit->file.path);
    }
    char *text = i18n_template_text(t);
    int status = 0;
    if (!out) {
        fputs(text, stdout);
    } else if (write_file(out, text)) {
        printf("%zu texts to translate written to %s\n", i18n_template_count(t), out);
    } else {
        fprintf(stderr, "sosuc: cannot write `%s`\n", out);
        status = 1;
    }
    free(text);
    i18n_template_free(t);
    program_free(&program);
    return status;
}

/* The language server: messages on stdin and stdout, each behind a `Content-Length`
   header (the Language Server Protocol). Diagnostics never go to stdout here. */
static int command_lsp(const char *argv0) {
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY); /* no \r\n translation: the lengths are in bytes */
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    char *lib = library_dir(argv0);
    LspServer *server = lsp_new(lib);
    free(lib);
    bool running = true;
    while (running) {
        size_t length = 0;
        bool has_length = false;
        char line[1024];
        for (;;) {
            if (!fgets(line, sizeof line, stdin)) {
                running = false;
                break;
            }
            if (line[0] == '\r' || line[0] == '\n') break;
            if (strncmp(line, "Content-Length:", 15) == 0) {
                length = (size_t)strtoul(line + 15, NULL, 10);
                has_length = true;
            }
        }
        if (!running || !has_length) continue;
        char *body = sosu_alloc(length + 1);
        if (fread(body, 1, length, stdin) != length) {
            free(body);
            break;
        }
        body[length] = '\0';
        LspMessages out = {0};
        running = lsp_handle(server, body, length, &out);
        for (size_t i = 0; i < out.count; i++) {
            fprintf(stdout, "Content-Length: %zu\r\n\r\n%s", strlen(out.items[i]), out.items[i]);
        }
        fflush(stdout);
        lsp_messages_free(&out);
        free(body);
    }
    int status = lsp_exit_code(server);
    lsp_free(server);
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

    if (strcmp(command, "i18n") == 0) return command_i18n(argc - 2, argv + 2, argv[0]);

    DiagFormat format = FORMAT_TEXT;
    const char *path = NULL;
    BuildOptions options = { false, { true, true, true } };
    int checks = -1;    /* --checks: -1 when not given */
    int contracts = -1; /* --contracts */
    bool is_fmt = strcmp(command, "fmt") == 0;
    bool fmt_check = false;
    const char **paths = sosu_alloc((size_t)argc * sizeof *paths); /* fmt takes several */
    int path_count = 0;
    for (int i = 2; i < argc; i++) {
        if (is_fmt && strcmp(argv[i], "--check") == 0) {
            fmt_check = true;
        } else if (is_fmt && argv[i][0] != '-') {
            paths[path_count++] = argv[i];
        } else if (strcmp(argv[i], "--format=text") == 0) {
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

    if (strcmp(command, "lsp") == 0) {
        free(paths);
        return command_lsp(argv[0]);
    }
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
    if (is_fmt) {
        if (path_count == 0) paths[path_count++] = ".";
        int status = command_fmt(paths, path_count, fmt_check, argv[0]);
        free(paths);
        return status;
    }
    free(paths);
    fprintf(stderr, "sosuc: unknown command `%s`\n", command);
    usage(stderr);
    return 2;
}
