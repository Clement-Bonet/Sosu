#if !defined(_WIN32) && !defined(_XOPEN_SOURCE)
#define _XOPEN_SOURCE 700 /* realpath */
#endif

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#include "bindgen.h"
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
          "  package a release of a program in build/package, with its files, and its archive (spec 11.6);\n"
          "          --target android (an APK) or ios-sim (an application for the simulator, on a Mac) (11.5)\n"
          "  fmt     format files or packages in place (--check: only report)\n"
          "  bindgen <header.h> [-I dir]... [-D NAME[=value]]... [--prefix P]... [-o file.sosu] [--check]\n"
          "          write the Sosu declarations of a C header (spec 11.6)\n"
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

/* Folder of the `core` and `std` packages: $SOSU_HOME/lib, or ../lib next to sosuc, wherever it
   was run from (`sosuc` alone was found in PATH, as an editor runs it). */
static char *library_dir(const char *argv0) {
    const char *home = getenv("SOSU_HOME");
    if (home && home[0]) return path_join(home, "lib");
    char *program = path_of_program(argv0);
    char *bin = path_dirname(program);
    free(program);
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

typedef struct Target Target; /* spec §11.5, below */

typedef struct {
    bool release; /* optimized C, no overflow checks or contracts by default */
    int lib;      /* --lib: 1 a static library, 2 a shared one (spec §9.2); 0 a program */
    const Target *target; /* --target: a phone, or this desktop */
    CodegenOptions checks;
    char *run_args; /* `sosuc run ... -- args`: given to the program, quoted (spec §8.2, std.os) */
} BuildOptions;

static bool write_file(const char *path, const char *text) {
    FILE *file = fopen(path, "wb");
    if (!file) return false;
    bool ok = fputs(text, file) >= 0;
    return fclose(file) == 0 && ok;
}

/* ---- Targets (spec §11.5) ---- */

typedef enum {
    TARGET_HOST,    /* the desktop system sosuc runs on */
    TARGET_DESKTOP, /* another desktop: another architecture, or another system (spec §11.6) */
    TARGET_ANDROID,
    TARGET_IOS,
} TargetOs;

struct Target {
    TargetOs os;
    const char *name;   /* as given to --target */
    const char *abi;    /* Android: the folder of jniLibs; iOS: the slice of an .xcframework */
    const char *triple; /* Android: the prefix of the clang of the NDK */
    bool simulator;
    const char *system; /* a desktop: windows, linux, macos */
    const char *arch;   /* a desktop: x64, arm64, universal (macOS: both) */
};

static const Target host_target = { TARGET_HOST, "host", "", "", false, NULL, NULL };

/* The desktops (spec §11.6); the triple is the one of clang, for another system. */
static const Target desktop_targets[] = {
    { TARGET_DESKTOP, "windows-x64", "", "x86_64-w64-windows-gnu", false, "windows", "x64" },
    { TARGET_DESKTOP, "windows-arm64", "", "aarch64-w64-windows-gnu", false, "windows", "arm64" },
    { TARGET_DESKTOP, "linux-x64", "", "x86_64-linux-gnu", false, "linux", "x64" },
    { TARGET_DESKTOP, "linux-arm64", "", "aarch64-linux-gnu", false, "linux", "arm64" },
    { TARGET_DESKTOP, "macos-x64", "", "x86_64-apple-macos11", false, "macos", "x64" },
    { TARGET_DESKTOP, "macos-arm64", "", "arm64-apple-macos11", false, "macos", "arm64" },
    { TARGET_DESKTOP, "macos-universal", "", "", false, "macos", "universal" },
};

static const Target mobile_targets[] = {
    { TARGET_ANDROID, "android-arm64", "arm64-v8a", "aarch64-linux-android", false, NULL, NULL },
    { TARGET_ANDROID, "android-x64", "x86_64", "x86_64-linux-android", false, NULL, NULL },
    { TARGET_ANDROID, "android", "", "", false, NULL, NULL }, /* sosuc package: every ABI */
    { TARGET_IOS, "ios-arm64", "ios-arm64", "arm64-apple-ios", false, NULL, NULL },
    { TARGET_IOS, "ios-sim", "ios-arm64_x86_64-simulator", "arm64-apple-ios-simulator", true, NULL, NULL },
};

/* The minimum version of Android: its API level, in the name of the clang of the NDK. */
#define ANDROID_API 24

/* The target named `name`, or NULL after reporting why there is none. */
static const char *host_system(void);

static const Target *find_target(const char *name) {
    for (size_t i = 0; i < sizeof mobile_targets / sizeof *mobile_targets; i++) {
        if (strcmp(mobile_targets[i].name, name) == 0) return &mobile_targets[i];
    }
    if (strcmp(name, host_system()) == 0) return &host_target;
    for (size_t i = 0; i < sizeof desktop_targets / sizeof *desktop_targets; i++) {
        if (strcmp(desktop_targets[i].name, name) == 0) return &desktop_targets[i];
    }
    fprintf(stderr,
            "sosuc: unknown target `%s`: the targets are windows-x64, windows-arm64, linux-x64, linux-arm64, macos-x64, "
            "macos-arm64, macos-universal, android-arm64, android-x64, ios-arm64 and ios-sim (spec 11.1)\n",
            name);
    return NULL;
}

/* The system of a desktop target (`windows`...), and its architecture (`x64`...). */
static const char *target_system(const Target *target) {
    if (target->system) return target->system;
    const char *host = host_system();
    return strncmp(host, "windows", 7) == 0 ? "windows" : strncmp(host, "macos", 5) == 0 ? "macos" : "linux";
}

static const char *target_arch(const Target *target) {
    if (target->arch) return target->arch;
    const char *dash = strchr(host_system(), '-');
    return dash ? dash + 1 : "x64";
}

/* The folder of the Android NDK: ANDROID_NDK_HOME, ANDROID_NDK_ROOT, or the newest version in the
   `ndk` folder of the SDK (ANDROID_HOME, ANDROID_SDK_ROOT, or where Android Studio puts it). */
static char *find_ndk(void) {
    const char *direct[] = { "ANDROID_NDK_HOME", "ANDROID_NDK_ROOT" };
    for (size_t i = 0; i < 2; i++) {
        const char *value = getenv(direct[i]);
        if (value && value[0]) return sosu_strdup(value);
    }
    char *sdks[3] = { NULL, NULL, NULL };
    const char *home = getenv("ANDROID_HOME");
    const char *root = getenv("ANDROID_SDK_ROOT");
    if (home && home[0]) sdks[0] = sosu_strdup(home);
    if (root && root[0]) sdks[1] = sosu_strdup(root);
#if defined(_WIN32)
    const char *local = getenv("LOCALAPPDATA");
    if (local) sdks[2] = sosu_format("%s/Android/Sdk", local);
#elif defined(__APPLE__)
    const char *user = getenv("HOME");
    if (user) sdks[2] = sosu_format("%s/Library/Android/sdk", user);
#else
    const char *user = getenv("HOME");
    if (user) sdks[2] = sosu_format("%s/Android/Sdk", user);
#endif
    char *found = NULL;
    for (size_t i = 0; i < 3 && !found; i++) {
        if (!sdks[i]) continue;
        char *versions = path_join(sdks[i], "ndk");
        char *native = shell_path(versions);
        char *last = shell_last_entry(native);
        if (last) found = path_join(versions, last);
        free(last);
        free(native);
        free(versions);
    }
    for (size_t i = 0; i < 3; i++) free(sdks[i]);
    return found;
}

/* The clang of the NDK for `target`, or NULL after reporting that there is no NDK. */
static char *android_clang(const Target *target) {
    char *ndk = find_ndk();
    if (!ndk) {
        fprintf(stderr, "sosuc: the Android NDK was not found: install it with the SDK Manager of Android Studio "
                        "(SDK Tools, NDK), or set ANDROID_NDK_HOME\n");
        return NULL;
    }
#if defined(_WIN32)
    const char *host = "windows-x86_64";
    const char *suffix = ".cmd";
#elif defined(__APPLE__)
    const char *host = "darwin-x86_64";
    const char *suffix = "";
#else
    const char *host = "linux-x86_64";
    const char *suffix = "";
#endif
    char *clang = sosu_format("%s/toolchains/llvm/prebuilt/%s/bin/%s%d-clang%s", ndk, host, target->triple, ANDROID_API,
                              suffix);
    free(ndk);
    if (!shell_file_exists(clang)) {
        fprintf(stderr, "sosuc: `%s` was not found: the NDK seems incomplete\n", clang);
        free(clang);
        return NULL;
    }
    return clang;
}

/* ---- Linking (the [link] sections of sosu.toml) ---- */

typedef struct {
    StringList libs;
    StringList paths; /* relative to the current folder */
    StringList frameworks;
    StringList copy;
    StringList sources;  /* C files compiled with the program ([link] c), and their folders and defines */
    StringList includes;
    StringList defines;
    bool window; /* a package is for programs with windows: no console in release (Windows) */
    char *resource; /* Windows: the icon of the program, compiled (sosuc package), or NULL */
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

/* A path of a [link] section, `{abi}` replaced by the folder of the architecture of a phone
   (`arm64-v8a`...: `paths = ["vendor/SDL3-android/lib/{abi}"]`, spec §11.5), and `{arch}` by the
   architecture of a desktop (`x64`, `arm64`: `vendor/SDL3/lib/{arch}`, spec §11.6). */
static char *link_path(const Package *package, const char *relative, const Target *target) {
    char *text = sosu_strdup(relative);
    const char *words[] = { "{abi}", "{arch}" };
    const char *values[] = { target->abi, target->os == TARGET_HOST || target->os == TARGET_DESKTOP ? target_arch(target) : "" };
    for (size_t i = 0; i < 2; i++) {
        const char *at = strstr(text, words[i]);
        if (!at) continue;
        char *replaced = sosu_format("%.*s%s%s", (int)(at - text), text, values[i], at + strlen(words[i]));
        free(text);
        text = replaced;
    }
    char *result = package_path(package, text);
    free(text);
    return result;
}

/* The [link] section of a desktop system, and of one of its architectures (-1: none). */
static int link_section_of(const char *system, const char *arch, bool with_arch) {
    for (int i = LINK_WINDOWS; i < LINK_SECTION_COUNT; i++) {
        const char *name = link_section_names[i];
        size_t n = strlen(system);
        if (strncmp(name, system, n) != 0) continue;
        if (!with_arch && name[n] == 0) return i;
        if (with_arch && name[n] == '-' && strcmp(name + n + 1, arch) == 0) return i;
    }
    return -1;
}

/* [link] and [link.<target>] of every package (the host, or `android`, `ios`): a dependency's
   libraries reach the program. */
static void collect_links(const Program *program, Links *links, const Target *target) {
    memset(links, 0, sizeof *links);
    bool desktop = target->os == TARGET_HOST || target->os == TARGET_DESKTOP;
    int own = target->os == TARGET_ANDROID ? LINK_ANDROID : target->os == TARGET_IOS ? LINK_IOS
            : link_section_of(target_system(target), "", false);
    int arch = desktop ? link_section_of(target_system(target), target_arch(target), true) : -1;
    int sections[] = { LINK_ALL, own, arch };
    for (size_t i = 0; i < program->packages.count; i++) {
        const Package *package = program->packages.items[i];
        for (size_t s = 0; s < 3; s++) {
            if (sections[s] < 0) continue;
            const LinkSection *link = &package->manifest.link[sections[s]];
            for (size_t j = 0; j < link->libs.count; j++) add_unique(&links->libs, sosu_strdup(link->libs.items[j]));
            for (size_t j = 0; j < link->paths.count; j++) {
                add_unique(&links->paths, link_path(package, link->paths.items[j], target));
            }
            for (size_t j = 0; j < link->frameworks.count; j++) {
                add_unique(&links->frameworks, sosu_strdup(link->frameworks.items[j]));
            }
            for (size_t j = 0; j < link->copy.count; j++) add_unique(&links->copy, link_path(package, link->copy.items[j], target));
            for (size_t j = 0; j < link->c.count; j++) add_unique(&links->sources, link_path(package, link->c.items[j], target));
            for (size_t j = 0; j < link->include.count; j++) {
                add_unique(&links->includes, link_path(package, link->include.items[j], target));
            }
            for (size_t j = 0; j < link->defines.count; j++) add_unique(&links->defines, sosu_strdup(link->defines.items[j]));
            if (link->subsystem && strcmp(link->subsystem, "window") == 0) links->window = true;
        }
    }
}

static void free_links(Links *links) {
    free(links->resource);
    StringList *lists[] = { &links->libs, &links->paths, &links->frameworks, &links->copy, &links->sources,
                            &links->includes, &links->defines };
    for (size_t i = 0; i < sizeof lists / sizeof *lists; i++) {
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

/* ` "value"`, quoted for the shell of `shell_run`: cmd and the C library of Windows, or sh. */
static void append_quoted(char **text, const char *value) {
    size_t length = strlen(value), size = 0;
    char *quoted = sosu_alloc(2 * length + 4);
    quoted[size++] = ' ';
    quoted[size++] = '"';
    for (size_t i = 0; i < length; i++) {
#ifdef _WIN32
        if (value[i] == '"') {
            for (size_t k = i; k > 0 && value[k - 1] == '\\'; k--) quoted[size++] = '\\'; /* doubled before a quote */
            quoted[size++] = '\\';
        }
#else
        if (strchr("\"\\$`", value[i])) quoted[size++] = '\\';
#endif
        quoted[size++] = value[i];
    }
#ifdef _WIN32
    for (size_t k = length; k > 0 && value[k - 1] == '\\'; k--) quoted[size++] = '\\'; /* before the closing quote */
#endif
    quoted[size++] = '"';
    quoted[size] = '\0';
    char *joined = sosu_format("%s%s", *text, quoted);
    free(*text);
    free(quoted);
    *text = joined;
}

/* The arguments that make the C compiler link the libraries. */
/* The C files of the packages ([link] c), after their folders and defines: ` -I"inc" -DX "a.c"`. */
static char *source_arguments(const Links *links, bool msvc) {
    char *args = sosu_strdup("");
    for (size_t i = 0; i < links->includes.count; i++) {
        char *native = shell_path(links->includes.items[i]);
        append(&args, msvc ? " /I\"%s\"" : " -I\"%s\"", native);
        free(native);
    }
    for (size_t i = 0; i < links->defines.count; i++) append(&args, msvc ? " /D%s" : " -D%s", links->defines.items[i]);
    for (size_t i = 0; i < links->sources.count; i++) {
        char *native = shell_path(links->sources.items[i]);
        append(&args, " \"%s\"", native);
        free(native);
    }
    return args;
}

/* Their objects in `dir`, for a static library: ` "dir/a.obj"`. */
static char *source_objects(const Links *links, const char *dir, const char *extension) {
    char *args = sosu_strdup("");
    for (size_t i = 0; i < links->sources.count; i++) {
        const char *path = links->sources.items[i];
        const char *slash = strrchr(path, '/');
        const char *back = strrchr(path, '\\');
        const char *base = back && (!slash || back > slash) ? back + 1 : slash ? slash + 1 : path;
        const char *dot = strrchr(base, '.');
        char *object = sosu_format("%s/%.*s%s", dir, (int)(dot ? (size_t)(dot - base) : strlen(base)), base, extension);
        char *native = shell_path(object);
        append(&args, " \"%s\"", native);
        free(native);
        free(object);
    }
    return args;
}

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

#ifdef _WIN32
/* A .bat script that runs `commands` with MSVC, found like build.bat does when cl.exe is not
   already on the PATH. */
static char *msvc_script_for(const char *commands, const char *arch) {
    return sosu_format(
        "@echo off\r\n"
        "%s"
        "set \"VSWHERE=%%ProgramFiles(x86)%%\\Microsoft Visual Studio\\Installer\\vswhere.exe\"\r\n"
        "if not exist \"%%VSWHERE%%\" goto :no_cl\r\n"
        "for /f \"usebackq tokens=*\" %%%%i in (`\"%%VSWHERE%%\" -latest -products * -requires "
        "Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set \"VSDIR=%%%%i\"\r\n"
        "if not defined VSDIR goto :no_cl\r\n"
        "set \"PATH=%%PATH%%;%%ProgramFiles(x86)%%\\Microsoft Visual Studio\\Installer\"\r\n"
        "call \"%%VSDIR%%\\VC\\Auxiliary\\Build\\%s\" %s >nul || goto :no_cl\r\n"
        ":have_cl\r\n"
        "%s"
        "exit /b 0\r\n"
        ":no_cl\r\n"
        "echo sosuc: no C compiler found: install Visual Studio with the C++ tools%s, or set SOSU_CC 1>&2\r\n"
        "exit /b 2\r\n",
        arch ? "" : "where cl >nul 2>nul && goto :have_cl\r\n", arch ? "vcvarsall.bat" : "vcvars64.bat", arch ? arch : "",
        commands, arch ? " of the architecture of the target (ARM64 or x64)" : "");
}

static char *msvc_script(const char *commands) { return msvc_script_for(commands, NULL); }
#endif

/* The C compiler of a desktop target, and the options that choose its target: false, after
   saying what is missing (spec §11.6). Not used for MSVC, which `compile_c` runs itself. */
static bool desktop_compiler(const Target *target, char **cc, char **flags) {
    const char *given = getenv("SOSU_CC");
    bool has_cc = given && given[0];
    *flags = sosu_strdup("");
    const char *system = target_system(target);
    const char *arch = target_arch(target);
    const char *host = target_system(&host_target);
    if (target->os == TARGET_HOST || (strcmp(system, host) == 0 && strcmp(arch, target_arch(&host_target)) == 0)) {
        *cc = sosu_strdup(has_cc ? given : "cc");
        return true;
    }
    if (strcmp(system, host) == 0 && strcmp(system, "macos") == 0) {
        *cc = sosu_strdup(has_cc ? given : "clang");
        free(*flags);
        *flags = sosu_strdup(strcmp(arch, "x64") == 0 ? " -arch x86_64" : strcmp(arch, "arm64") == 0 ? " -arch arm64"
                                                                        : " -arch arm64 -arch x86_64");
        return true;
    }
    if (strcmp(system, host) == 0 && strcmp(system, "linux") == 0) {
        *cc = sosu_strdup(has_cc ? given : strcmp(arch, "arm64") == 0 ? "aarch64-linux-gnu-gcc" : "x86_64-linux-gnu-gcc");
        return true;
    }
    if (strcmp(system, host) == 0 && has_cc) { /* Windows with MinGW */
        *cc = sosu_strdup(given);
        return true;
    }
    const char *sysroot = getenv("SOSU_SYSROOT");
    if (!target->triple[0] || !sysroot || !sysroot[0]) {
        fprintf(stderr,
                "sosuc: building for %s on %s needs clang and the files of %s (its headers and libraries): give their "
                "folder in SOSU_SYSROOT, and clang in SOSU_CC if it is not `clang` (spec 11.6)\n",
                target->name, host_system(), system);
        *cc = NULL;
        return false;
    }
    *cc = sosu_strdup(has_cc ? given : "clang");
    free(*flags);
    *flags = sosu_format(" --target=%s --sysroot=\"%s\" -fuse-ld=lld", target->triple, sysroot);
    return true;
}

/* Compiles the generated C file with the runtime. Returns false if the C compiler failed. */
static bool compile_c(const char *build_dir, const char *c_file, const char *exe, const char *lib, bool optimize,
                      const Links *links, const Target *target) {
    char *runtime = path_join(lib, "runtime");
    char *runtime_c = path_join(runtime, "sosu_rt.c");
    const char *cc = getenv("SOSU_CC");
    bool for_windows = strcmp(target_system(target), "windows") == 0;
    char *command;
#ifdef _WIN32
    if ((!cc || !cc[0]) && for_windows) {
        /* MSVC, found like build.bat does when cl.exe is not already on the PATH */
        char *bat = path_join(build_dir, "compile.bat");
        char *dir = shell_path(build_dir);
        char *rt = shell_path(runtime);
        char *rt_c = shell_path(runtime_c);
        char *c = shell_path(c_file);
        char *out = shell_path(exe);
        char *link_args = link_arguments(links, true);
        if (optimize && links->window) {  /* a program with windows: no console behind them */
            append(&link_args, "%s", link_args[0] ? " /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup"
                                                   : " /link /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup");
        }
        char *resource = links->resource ? sosu_format(" \"%s\"", links->resource) : sosu_strdup("");
        char *sources = source_arguments(links, true);
        char *commands = sosu_format(
            "cl /nologo /std:c11 /utf-8 /W0 /Zi %s /I\"%s\" /Fo\"%s\\\\\" /Fd\"%s\\\\\" /Fe\"%s\"%s \"%s\" \"%s\"%s%s "
            ">\"%s\\cl.log\" || (type \"%s\\cl.log\" & exit /b 1)\r\n",
            optimize ? "/O2" : "/Od", rt, dir, dir, out, sources, c, rt_c, resource, link_args, dir, dir);
        free(sources);
        free(resource);
        /* another architecture: the tools of MSVC that build for it, from this one */
        bool arm64 = strcmp(target_arch(target), "arm64") == 0;
        bool other = strcmp(target_arch(target), target_arch(&host_target)) != 0;
        char *script = msvc_script_for(commands, !other ? NULL : arm64 ? "x64_arm64" : "arm64_x64");
        free(commands);
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
        (void)cc; /* SOSU_CC: read by desktop_compiler */
        char *compiler, *flags;
        if (!desktop_compiler(target, &compiler, &flags)) {
            free(flags);
            free(runtime);
            free(runtime_c);
            return false;
        }
        char *link_args = link_arguments(links, false);
        if (optimize && links->window && for_windows) append(&link_args, "%s", " -mwindows");  /* MinGW: the same */
        if (!for_windows) append(&link_args, "%s", " -pthread -lm"); /* std.thread (spec §5.6), std.math */
        char *resource = links->resource ? sosu_format(" \"%s\"", links->resource) : sosu_strdup("");
        char *sources = source_arguments(links, false);
        command = sosu_format("\"%s\"%s -std=c99 -g %s -w -I\"%s\" \"%s\" \"%s\"%s%s -o \"%s\"%s", compiler, flags,
                              optimize ? "-O2" : "-O0", runtime, c_file, runtime_c, sources, resource, exe, link_args);
        free(sources);
        free(resource);
        free(link_args);
        free(flags);
        free(compiler);
    }
    int status = shell_run(command);
    free(command);
    free(runtime);
    free(runtime_c);
    return status == 0;
}

/* The system sosuc runs on, as a target name (spec §11.1). */
static const char *host_system(void) {
#if defined(_WIN32) && (defined(_M_ARM64) || defined(__aarch64__))
    return "windows-arm64";
#elif defined(_WIN32)
    return "windows-x64";
#elif defined(__APPLE__) && defined(__aarch64__)
    return "macos-arm64";
#elif defined(__APPLE__)
    return "macos-x64";
#elif defined(__aarch64__)
    return "linux-arm64";
#else
    return "linux-x64";
#endif
}

#ifdef _WIN32
#define LONG_IS_32 true
#else
#define LONG_IS_32 false
#endif

/* ---- sosuc bindgen (spec §11.6) ---- */

/* An absolute path with `/`. New string. */
static char *absolute_path(const char *path) {
#ifdef _WIN32
    char *full = _fullpath(NULL, path, 0);
#else
    char *full = realpath(path, NULL);
#endif
    char *result = sosu_strdup(full ? full : path);
    free(full);
    for (char *c = result; *c; c++) {
        if (*c == '\\') *c = '/';
    }
    return result;
}

/* A folder of its own for the files of one run, in the temporary folder of the system. */
static char *temporary_dir(const char *name) {
    const char *base = getenv("TMPDIR");
    if (!base || !base[0]) base = getenv("TEMP");
    if (!base || !base[0]) base = getenv("TMP");
    if (!base || !base[0]) base = "/tmp";
    char *dir = sosu_format("%s/sosu-%s-%lu", base, name, (unsigned long)time(NULL));
    for (char *c = dir; *c; c++) {
        if (*c == '\\') *c = '/';
    }
    shell_make_dir(dir);
    return dir;
}

typedef struct {
    const char *header;
    StringList includes; /* -I, absolute */
    StringList include_args; /* -I, as given: for the comment of the module */
    StringList defines;  /* -D */
    StringList prefixes;
    const char *output;
    bool check;
} BindgenOptions;

/* The C compiler of the machine on `c_file`: `mode` 'E' preprocesses into `out`, 'S' only checks.
   Its messages go to `log`. */
static bool run_c_compiler(const BindgenOptions *o, const char *dir, const char *c_file, char mode, const char *out,
                           const char *log) {
    const char *cc = getenv("SOSU_CC");
    char *args = sosu_strdup("");
    bool msvc = false;
#ifdef _WIN32
    msvc = !cc || !cc[0];
#endif
    for (size_t i = 0; i < o->includes.count; i++) {
        char *native = shell_path(o->includes.items[i]);
        append(&args, msvc ? " /I\"%s\"" : " -I\"%s\"", native);
        free(native);
    }
    for (size_t i = 0; i < o->defines.count; i++) append(&args, msvc ? " /D%s" : " -D%s", o->defines.items[i]);
    char *c = shell_path(c_file);
    char *output = out ? shell_path(out) : NULL;
    char *errors = shell_path(log);
    int status;
#ifdef _WIN32
    if (msvc) {
        char *commands = mode == 'E' ? sosu_format("cl /nologo /E /std:c11 /utf-8%s \"%s\" >\"%s\" 2>\"%s\" || exit /b 1\r\n",
                                                   args, c, output, errors)
                                     : sosu_format("cl /nologo /Zs /std:c11 /utf-8%s \"%s\" >\"%s\" 2>&1 || exit /b 1\r\n",
                                                   args, c, errors);
        char *script = msvc_script(commands);
        char *bat = path_join(dir, mode == 'E' ? "preprocess.bat" : "check.bat");
        char *native_bat = shell_path(bat);
        char *command = sosu_format("\"%s\"", native_bat);
        status = write_file(bat, script) ? shell_run(command) : 2;
        free(command);
        free(native_bat);
        free(bat);
        free(script);
        free(commands);
    } else
#endif
    {
        (void)dir;
        char *command = mode == 'E' ? sosu_format("\"%s\" -E -std=c11%s \"%s\" -o \"%s\" 2>\"%s\"", cc && cc[0] ? cc : "cc", args,
                                                  c, output, errors)
                                    : sosu_format("\"%s\" -fsyntax-only -std=c11%s \"%s\" >\"%s\" 2>&1", cc && cc[0] ? cc : "cc",
                                                  args, c, errors);
        status = shell_run(command);
        free(command);
    }
    free(args);
    free(c);
    free(output);
    free(errors);
    return status == 0;
}

static void print_file(const char *path, FILE *out) {
    SourceFile file;
    if (!source_load(&file, path)) return;
    fputs(file.text, out);
    source_free(&file);
}

/* `sosuc bindgen <header.h> [-I dir]... [-D NAME[=value]]... [--prefix P]... [-o file.sosu] [--check]` */
static int command_bindgen(int argc, char **argv) {
    BindgenOptions o;
    memset(&o, 0, sizeof o);
    for (int i = 0; i < argc; i++) {
        const char *arg = argv[i];
        const char *value = NULL;
        if ((strcmp(arg, "-I") == 0 || strcmp(arg, "-D") == 0 || strcmp(arg, "-o") == 0 || strcmp(arg, "--prefix") == 0)) {
            if (i + 1 >= argc) {
                fprintf(stderr, "sosuc: `%s` takes a value\n", arg);
                return 2;
            }
            value = argv[++i];
        } else if ((arg[0] == '-' && (arg[1] == 'I' || arg[1] == 'D' || arg[1] == 'o') && arg[2])) {
            value = arg + 2;
        } else if (strncmp(arg, "--prefix=", 9) == 0) {
            value = arg + 9;
        }
        if (arg[1] == 'I' && arg[0] == '-') {
            SOSU_VEC_PUSH(&o.includes, absolute_path(value));
            SOSU_VEC_PUSH(&o.include_args, sosu_strdup(value));
        } else if (arg[1] == 'D' && arg[0] == '-') {
            SOSU_VEC_PUSH(&o.defines, sosu_strdup(value));
        } else if (arg[1] == 'o' && arg[0] == '-') {
            o.output = value;
        } else if (strncmp(arg, "--prefix", 8) == 0) {
            SOSU_VEC_PUSH(&o.prefixes, sosu_strdup(value));
        } else if (strcmp(arg, "--check") == 0) {
            o.check = true;
        } else if (arg[0] == '-') {
            fprintf(stderr, "sosuc: unknown option `%s` of `sosuc bindgen`\n", arg);
            return 2;
        } else if (o.header) {
            fprintf(stderr, "sosuc: `sosuc bindgen` takes one header; it may include others\n");
            return 2;
        } else {
            o.header = arg;
        }
    }
    if (!o.header) {
        fprintf(stderr, "usage: sosuc bindgen <header.h> [-I dir]... [-D NAME[=value]]... [--prefix P]... "
                        "[-o file.sosu] [--check]\n");
        return 2;
    }
    if (!shell_file_exists(o.header)) {
        fprintf(stderr, "sosuc: cannot read `%s`\n", o.header);
        return 2;
    }
    char *header = absolute_path(o.header);
    char *header_dir = path_dirname(header);
    char *dir = temporary_dir("bindgen");
    int status = 2;

    /* the folders whose headers are bound: the header's, and the -I ones */
    const char **own = sosu_alloc((o.includes.count + 1) * sizeof *own);
    own[0] = header_dir;
    for (size_t i = 0; i < o.includes.count; i++) own[i + 1] = o.includes.items[i];
    size_t own_count = o.includes.count + 1;

    /* 1. the header preprocessed: the files it includes, and the macros they define */
    char *first = path_join(dir, "first.c");
    char *first_i = path_join(dir, "first.i");
    char *log = path_join(dir, "cc.log");
    char *wrapper = path_join(dir, "probe.c");
    char *wrapper_i = path_join(dir, "probe.i");
    char *include = sosu_format("#include \"%s\"\n", header);
    BindgenNames files = {0};
    BindgenNames macros = {0};
    SourceFile text;
    bool ok = write_file(first, include) && run_c_compiler(&o, dir, first, 'E', first_i, log);
    if (ok && source_load(&text, first_i)) {
        bindgen_files(text.text, text.length, &files);
        source_free(&text);
        for (size_t i = 0; i < files.count; i++) {
            if (!bindgen_is_own(files.items[i], own, own_count)) continue;
            SourceFile source;
            if (!source_load(&source, files.items[i])) continue;
            bindgen_macro_names(source.text, source.length, (const char *const *)o.prefixes.items, o.prefixes.count,
                                &macros);
            source_free(&source);
        }
    } else if (ok) {
        ok = false;
    }
    /* 2. again, with the macros asked of the preprocessor: it knows which `#if` defined them */
    if (ok) {
        char *probe = sosu_strdup(include);
        for (size_t i = 0; i < macros.count; i++) {
            append(&probe, BINDGEN_PROBE " \"%s\" ", macros.items[i]);
            append(&probe, "%s ;\n", macros.items[i]);
        }
        ok = write_file(wrapper, probe) && run_c_compiler(&o, dir, wrapper, 'E', wrapper_i, log);
        free(probe);
    }
    if (!ok) {
        print_file(log, stderr);
        fprintf(stderr, "sosuc: the C compiler cannot preprocess `%s`\n", o.header);
    } else if (source_load(&text, wrapper_i)) {
        char *command = sosu_format("sosuc bindgen %s", o.header);
        for (size_t i = 0; i < o.include_args.count; i++) append(&command, " -I %s", o.include_args.items[i]);
        for (size_t i = 0; i < o.defines.count; i++) append(&command, " -D %s", o.defines.items[i]);
        for (size_t i = 0; i < o.prefixes.count; i++) append(&command, " --prefix %s", o.prefixes.items[i]);
        if (o.output) append(&command, " -o %s", o.output);
        char *shown = sosu_strdup(o.header);
        for (char *c = shown; *c; c++) {
            if (*c == '\\') *c = '/';
        }
        char *comment = sosu_format("// Generated by sosuc bindgen %s from %s, for %s: do not edit (spec §11.6).\n"
                                    "// %s",
                                    SOSU_VERSION, shown, host_system(), command);
        BindgenInput input = { text.text, text.length, own, own_count, (const char *const *)o.prefixes.items,
                               o.prefixes.count, LONG_IS_32, comment, header };
        BindgenOutput result;
        bindgen_generate(&input, &result);
        fputs(result.warnings, stderr);
        /* the canonical form (spec §7.6) */
        SourceFile module;
        source_from_string(&module, o.output ? o.output : "bindgen.sosu", result.module);
        DiagList diags = {0};
        char *formatted = format_source(&module, &diags);
        if (!formatted) {
            diag_print_text(&diags, stderr);
            fprintf(stderr, "sosuc: the module generated does not parse: a bug of bindgen\n");
        }
        const char *final = formatted ? formatted : result.module;
        bool written = o.output ? write_file(o.output, final) : fputs(final, stdout) >= 0;
        if (!written) fprintf(stderr, "sosuc: cannot write `%s`\n", o.output);
        status = written && formatted ? 0 : 1;
        if (o.output && written) {
            printf("%s: %zu constants, %zu structs, %zu functions", o.output, result.constants, result.structs,
                   result.functions);
            if (result.skipped) printf(", %zu left out (listed at its end)", result.skipped);
            printf("\n");
        }
        if (status == 0 && o.check) {
            char *check = path_join(dir, "check.c");
            bool checked = write_file(check, result.check) && run_c_compiler(&o, dir, check, 'S', NULL, log);
            if (!checked) {
                print_file(log, stderr);
                fprintf(stderr, "sosuc: the binding does not match `%s` (sizes, alignments, offsets or values above)\n",
                        o.header);
                status = 1;
            } else if (o.output) {
                printf("checked against the header by the C compiler\n");
            }
            free(check);
        }
        free(formatted);
        diag_free(&diags);
        source_free(&module);
        bindgen_output_free(&result);
        free(comment);
        free(shown);
        free(command);
        source_free(&text);
    }
    if (!getenv("SOSU_BINDGEN_KEEP")) shell_remove_dir(dir); /* kept to debug bindgen */
    bindgen_names_free(&files);
    bindgen_names_free(&macros);
    free(include);
    free(wrapper);
    free(wrapper_i);
    free(first);
    free(first_i);
    free(log);
    free((void *)own);
    free(dir);
    free(header_dir);
    free(header);
    for (size_t i = 0; i < o.includes.count; i++) free(o.includes.items[i]);
    free(o.includes.items);
    for (size_t i = 0; i < o.include_args.count; i++) free(o.include_args.items[i]);
    free(o.include_args.items);
    for (size_t i = 0; i < o.defines.count; i++) free(o.defines.items[i]);
    free(o.defines.items);
    for (size_t i = 0; i < o.prefixes.count; i++) free(o.prefixes.items[i]);
    free(o.prefixes.items);
    return status;
}

/* ---- Libraries (spec §9.2) ---- */

/* The files of a library of `name` in build/, by kind of library and system. */
typedef struct {
    char *library; /* the static library, or the shared one */
    char *import;  /* MSVC: the import library of a DLL; NULL elsewhere */
    char *runtime; /* a static library: the runtime, linked once by the host */
} LibraryFiles;

static LibraryFiles library_files(const char *build_dir, const char *name, bool shared, bool msvc) {
    LibraryFiles files = {0};
    char *file;
    if (shared) {
#if defined(_WIN32)
        file = sosu_format("%s.dll", name);
        if (msvc) {
            char *import = sosu_format("%s.dll.lib", name);
            files.import = path_join(build_dir, import);
            free(import);
        }
#elif defined(__APPLE__)
        file = sosu_format("lib%s.dylib", name);
#else
        file = sosu_format("lib%s.so", name);
#endif
    } else {
        file = msvc ? sosu_format("%s.lib", name) : sosu_format("lib%s.a", name);
        files.runtime = path_join(build_dir, msvc ? "sosu_rt.lib" : "libsosu_rt.a");
    }
    files.library = path_join(build_dir, file);
    free(file);
    return files;
}

static void free_library_files(LibraryFiles *files) {
    free(files->library);
    free(files->import);
    free(files->runtime);
}

/* Compiles the generated C file of a library, and the runtime, into `files`. */
static bool compile_library(const char *build_dir, const char *c_file, const char *name, const char *lib, bool optimize,
                            bool shared, const Links *links, const LibraryFiles *files) {
    char *runtime = path_join(lib, "runtime");
    char *runtime_c = path_join(runtime, "sosu_rt.c");
    const char *cc = getenv("SOSU_CC");
    char *command = NULL;
#ifdef _WIN32
    if (!cc || !cc[0]) {
        char *bat = path_join(build_dir, "compile.bat");
        char *dir = shell_path(build_dir);
        char *rt = shell_path(runtime);
        char *rt_c = shell_path(runtime_c);
        char *c = shell_path(c_file);
        char *out = shell_path(files->library);
        char *flags = sosu_format("/nologo /std:c11 /utf-8 /W0 /Zi %s /I\"%s\" /Fo\"%s\\\\\" /Fd\"%s\\\\\"",
                                  optimize ? "/O2" : "/Od", rt, dir, dir);
        char *log = sosu_format(">\"%s\\cl.log\" || (type \"%s\\cl.log\" & exit /b 1)", dir, dir);
        char *sources = source_arguments(links, true);
        char *commands;
        if (shared) {
            char *import = shell_path(files->import);
            char *link_args = link_arguments(links, true);
            if (!link_args[0]) append(&link_args, "%s", " /link");
            append(&link_args, " /IMPLIB:\"%s\"", import);
            commands = sosu_format("cl %s /LD /DSOSU_SHARED /Fe\"%s\"%s \"%s\" \"%s\"%s %s\r\n", flags, out, sources, c,
                                   rt_c, link_args, log);
            free(import);
            free(link_args);
        } else {
            char *runtime_lib = shell_path(files->runtime);
            char *objects = source_objects(links, build_dir, ".obj"); /* the C files of the packages, in the library */
            commands = sosu_format("cl %s /c%s \"%s\" \"%s\" %s\r\n"
                                   "lib /nologo /OUT:\"%s\" \"%s\\%s.obj\"%s %s\r\n"
                                   "lib /nologo /OUT:\"%s\" \"%s\\sosu_rt.obj\" %s\r\n",
                                   flags, sources, c, rt_c, log, out, dir, name, objects, log, runtime_lib, dir, log);
            free(objects);
            free(runtime_lib);
        }
        free(sources);
        char *script = msvc_script(commands);
        if (write_file(bat, script)) {
            char *native_bat = shell_path(bat);
            command = sosu_format("\"%s\"", native_bat);
            free(native_bat);
        } else {
            fprintf(stderr, "sosuc: cannot write in `%s`\n", build_dir);
        }
        free(script);
        free(commands);
        free(log);
        free(flags);
        free(out);
        free(c);
        free(rt_c);
        free(rt);
        free(dir);
        free(bat);
    } else
#endif
    {
        const char *compiler = cc && cc[0] ? cc : "cc";
        const char *level = optimize ? "-O2" : "-O0";
        if (shared) {
            char *link_args = link_arguments(links, false);
#ifndef _WIN32
            append(&link_args, "%s", " -pthread -lm"); /* std.thread (spec §5.6), std.math */
#endif
#ifdef __APPLE__
            const char *kind = "-dynamiclib";
#else
            const char *kind = "-shared";
#endif
            char *sources = source_arguments(links, false);
            command = sosu_format("\"%s\" -std=c99 -g %s -w -fPIC %s -fvisibility=hidden -DSOSU_SHARED -I\"%s\" \"%s\" \"%s\"%s "
                                  "-o \"%s\"%s",
                                  compiler, level, kind, runtime, c_file, runtime_c, sources, files->library, link_args);
            free(sources);
            free(link_args);
        } else {
            char *object = sosu_format("%s/%s.o", build_dir, name);
            char *rt_object = sosu_format("%s/sosu_rt.o", build_dir);
            /* the C files of the packages: one object each, in the library */
            Links flags_only = *links;
            flags_only.sources.count = 0;
            char *flags = source_arguments(&flags_only, false);
            char *compile_sources = sosu_strdup("");
            for (size_t i = 0; i < links->sources.count; i++) {
                Links one = flags_only;
                one.sources.items = &links->sources.items[i];
                one.sources.count = 1;
                char *source = source_arguments(&one, false);
                char *output = source_objects(&one, build_dir, ".o");
                char *step = sosu_format("\"%s\" -std=c99 -g %s -w -fPIC%s -c%s -o%s && ", compiler, level, flags, source + strlen(flags),
                                         output);
                append(&compile_sources, "%s", step);
                free(step);
                free(output);
                free(source);
            }
            char *objects = source_objects(links, build_dir, ".o");
            command = sosu_format("%s\"%s\" -std=c99 -g %s -w -fPIC -I\"%s\" -c \"%s\" -o \"%s\" && "
                                  "\"%s\" -std=c99 -g %s -w -fPIC -I\"%s\" -c \"%s\" -o \"%s\" && "
                                  "ar rcs \"%s\" \"%s\"%s && ar rcs \"%s\" \"%s\"",
                                  compile_sources, compiler, level, runtime, c_file, object, compiler, level, runtime,
                                  runtime_c, rt_object, files->library, object, objects, files->runtime, rt_object);
            free(objects);
            free(compile_sources);
            free(flags);
            free(object);
            free(rt_object);
        }
    }
    int status = command ? shell_run(command) : 1;
    free(command);
    free(runtime);
    free(runtime_c);
    return status == 0;
}

/* The libraries a host must link with a static library: its [link] sections. */
static bool write_link_list(const char *build_dir, const char *name, const Links *links) {
    if (links->libs.count == 0 && links->paths.count == 0 && links->frameworks.count == 0) return true;
    char *file = sosu_format("%s.links", name);
    char *path = path_join(build_dir, file);
    char *text = sosu_strdup("# what a host links with this static library (spec 9.2)\n");
    for (size_t i = 0; i < links->paths.count; i++) append(&text, "path %s\n", links->paths.items[i]);
    for (size_t i = 0; i < links->libs.count; i++) append(&text, "lib %s\n", links->libs.items[i]);
    for (size_t i = 0; i < links->frameworks.count; i++) append(&text, "framework %s\n", links->frameworks.items[i]);
    bool ok = write_file(path, text);
    if (!ok) fprintf(stderr, "sosuc: cannot write `%s`\n", path);
    free(text);
    free(path);
    free(file);
    return ok;
}

/* `sosuc build --lib[=static|shared]`: the package (entry `src/lib.sosu`) as a C library and its
   header, in build/. */
static int command_build_library(const char *dir, const char *argv0, DiagFormat format, const BuildOptions *options) {
    char *lib = library_dir(argv0);
    Program program;
    Sema sema = {0};
    char *source = NULL;
    char *header = NULL;
    bool ok = program_load(&program, dir, lib);
    if (ok) {
        const char *module = program.units.items[0]->module;
        size_t length = strlen(module);
        if (length < 4 || strcmp(module + length - 4, ".lib") != 0) {
            fprintf(stderr, "sosuc: `--lib` builds a library, whose entry is `src/lib.sosu`; this package has `src/main.sosu`\n");
            ok = false;
        }
    }
    if (ok) ok = sema_check(&sema, &program);
    if (ok) {
        source = codegen_program(&sema, &program, CODEGEN_LIBRARY, options->checks);
        ok = source != NULL;
        if (ok) header = codegen_header(&sema, &program);
    }
    if (program.fatal) fprintf(stderr, "sosuc: %s\n", program.fatal);
    print_diagnostics(&program.diags, format);

    int status = program.fatal || !program.units.count ? 2 : 1;
    if (ok) {
        const Package *root = program.packages.items[0];
        char *build_dir = path_join(root->root, "build");
        shell_make_dir(build_dir);
        char *c_name = sosu_format("%s.c", root->name);
        char *h_name = sosu_format("%s.h", root->name);
        char *c_file = path_join(build_dir, c_name);
        char *h_file = path_join(build_dir, h_name);
        const char *cc = getenv("SOSU_CC");
#ifdef _WIN32
        bool msvc = !cc || !cc[0];
#else
        bool msvc = false;
        (void)cc;
#endif
        bool shared = options->lib == 2;
        LibraryFiles files = library_files(build_dir, root->name, shared, msvc);
        Links links;
        collect_links(&program, &links, options->target);
        if (!write_file(c_file, source) || !write_file(h_file, header)) {
            fprintf(stderr, "sosuc: cannot write in `%s`\n", build_dir);
            status = 2;
        } else if (!compile_library(build_dir, c_file, root->name, lib, options->release, shared, &links, &files)) {
            fprintf(stderr, "sosuc: the C compiler failed on `%s`\n", c_file);
            status = 2;
        } else if ((!shared && !write_link_list(build_dir, root->name, &links)) || !copy_files(&links, build_dir)) {
            status = 2;
        } else {
            printf("built %s", files.library);
            if (files.import) printf(", %s", files.import);
            if (files.runtime) printf(", %s", files.runtime);
            printf(" and %s\n", h_file);
            status = 0;
        }
        free_links(&links);
        free_library_files(&files);
        free(h_file);
        free(c_file);
        free(h_name);
        free(c_name);
        free(build_dir);
    }
    free(header);
    free(source);
    free(lib);
    sema_free(&sema);
    program_free(&program);
    return status;
}

/* ---- Phones (spec §11.5) ---- */

/* The folder of the build for a phone, in build/: `android/arm64-v8a`, `ios/simulator`... */
static char *mobile_dir(const char *build_dir, const Target *target) {
    if (target->os == TARGET_ANDROID) return sosu_format("%s/android/%s", build_dir, target->abi);
    return sosu_format("%s/ios/%s", build_dir, target->simulator ? "simulator" : "device");
}

/* Compiles a program for a phone: `libmain.so` that the activity of SDL loads (Android), or
   `libmain.a` that the Xcode project links (iOS); its entry is `SDL_main`. */
static bool compile_mobile(const char *out_dir, const char *c_file, const char *lib, bool optimize, const Links *links,
                           const Target *target, char **built) {
    char *runtime = path_join(lib, "runtime");
    char *runtime_c = path_join(runtime, "sosu_rt.c");
    const char *level = optimize ? "-O2" : "-O0";
    char *command = NULL;
    if (target->os == TARGET_ANDROID) {
        char *clang = android_clang(target);
        if (clang) {
            *built = sosu_format("%s/libmain.so", out_dir);
            char *link_args = link_arguments(links, false);
            char *sources = source_arguments(links, false);
            char *inner = sosu_format("\"%s\" -std=c99 -g %s -w -fPIC -shared -fvisibility=hidden -DSOSU_SDL_MAIN -I\"%s\" \"%s\" \"%s\"%s -o \"%s\"%s "
                                      "-llog",
                                      clang, level, runtime, c_file, runtime_c, sources, *built, link_args);
            free(sources);
#ifdef _WIN32
            command = sosu_format("\"%s\"", inner); /* cmd keeps the inner quotes when the whole is quoted */
            free(inner);
#else
            command = inner;
#endif
            free(link_args);
            free(clang);
        }
    } else {
#ifdef __APPLE__
        const char *sdk = target->simulator ? "iphonesimulator" : "iphoneos";
        const char *minimum = target->simulator ? "-mios-simulator-version-min=13.0" : "-miphoneos-version-min=13.0";
        *built = sosu_format("%s/libmain.a", out_dir);
        command = sosu_format("xcrun --sdk %s clang -arch %s %s -std=c99 -g %s -w -DSOSU_SDL_MAIN -I\"%s\" -c \"%s\" -o \"%s/main.o\" && "
                              "xcrun --sdk %s clang -arch %s %s -std=c99 -g %s -w -I\"%s\" -c \"%s\" -o \"%s/sosu_rt.o\" && "
                              "rm -f \"%s\" && ar rcs \"%s\" \"%s/main.o\" \"%s/sosu_rt.o\"",
                              sdk, "arm64", minimum, level, runtime, c_file, out_dir, sdk, "arm64", minimum, level,
                              runtime, runtime_c, out_dir, *built, *built, out_dir, out_dir);
#else
        (void)level;
        (void)links;
        fprintf(stderr, "sosuc: building for iOS needs a Mac with Xcode (spec 11.5)\n");
#endif
    }
    int status = command ? shell_run(command) : 1;
    free(command);
    free(runtime);
    free(runtime_c);
    return status == 0;
}

/* `sosuc build --target android-arm64|android-x64|ios-arm64|ios-sim`: the program, for a phone. */
static int command_build_mobile(const char *dir, const char *argv0, DiagFormat format, const BuildOptions *options) {
    const Target *target = options->target;
    char *lib = library_dir(argv0);
    Program program;
    Sema sema = {0};
    char *source = NULL;
    bool ok = program_load(&program, dir, lib);
    if (ok) ok = sema_check(&sema, &program);
    if (ok) {
        source = codegen_program(&sema, &program, CODEGEN_PROGRAM, options->checks);
        ok = source != NULL;
    }
    if (program.fatal) fprintf(stderr, "sosuc: %s\n", program.fatal);
    print_diagnostics(&program.diags, format);
    int status = program.fatal ? 2 : 1;
    if (ok) {
        const Package *root = program.packages.items[0];
        char *build_dir = path_join(root->root, "build");
        char *out_dir = mobile_dir(build_dir, target);
        shell_make_dir(out_dir);
        char *file = sosu_format("%s.c", root->name);
        char *c_file = path_join(out_dir, file);
        char *built = NULL;
        Links links;
        collect_links(&program, &links, target);
        if (!write_file(c_file, source)) {
            fprintf(stderr, "sosuc: cannot write `%s`\n", c_file);
            status = 2;
        } else if (!compile_mobile(out_dir, c_file, lib, options->release, &links, target, &built)) {
            if (built) fprintf(stderr, "sosuc: the C compiler failed on `%s`\n", c_file);
            status = 2;
        } else if (!copy_files(&links, out_dir)) {
            status = 2;
        } else {
            printf("built %s\n", built);
            status = 0;
        }
        free_links(&links);
        free(built);
        free(c_file);
        free(file);
        free(out_dir);
        free(build_dir);
    }
    free(source);
    free(lib);
    sema_free(&sema);
    program_free(&program);
    return status;
}

/* ---- sosuc package (spec §11.5) ---- */

/* The Android SDK: ANDROID_HOME, ANDROID_SDK_ROOT, or where Android Studio puts it. */
static char *find_android_sdk(void) {
    const char *names[] = { "ANDROID_HOME", "ANDROID_SDK_ROOT" };
    for (size_t i = 0; i < 2; i++) {
        const char *value = getenv(names[i]);
        if (value && value[0]) return sosu_strdup(value);
    }
#if defined(_WIN32)
    const char *local = getenv("LOCALAPPDATA");
    char *sdk = local ? sosu_format("%s/Android/Sdk", local) : NULL;
#elif defined(__APPLE__)
    const char *home = getenv("HOME");
    char *sdk = home ? sosu_format("%s/Library/Android/sdk", home) : NULL;
#else
    const char *home = getenv("HOME");
    char *sdk = home ? sosu_format("%s/Android/Sdk", home) : NULL;
#endif
    char *platforms = sdk ? path_join(sdk, "platforms") : NULL;
    char *native = platforms ? shell_path(platforms) : NULL;
    char *last = native ? shell_last_entry(native) : NULL;
    if (!last) {
        free(sdk);
        sdk = NULL;
    }
    free(last);
    free(native);
    free(platforms);
    return sdk;
}

/* The newest API level of the platforms of the SDK (`android-37.0` gives 37), or 0. */
static int newest_android_api(const char *sdk) {
    char *platforms = path_join(sdk, "platforms");
    char *native = shell_path(platforms);
    char *last = shell_last_entry(native);
    int api = last && strncmp(last, "android-", 8) == 0 ? atoi(last + 8) : 0;
    free(last);
    free(native);
    free(platforms);
    return api;
}

/* `text` with every `from` replaced by `to`. New string. */
static char *replace_all(const char *text, const char *from, const char *to) {
    char *result = sosu_strdup("");
    const char *at;
    while ((at = strstr(text, from))) {
        char *joined = sosu_format("%s%.*s%s", result, (int)(at - text), text, to);
        free(result);
        result = joined;
        text = at + strlen(from);
    }
    char *joined = sosu_format("%s%s", result, text);
    free(result);
    return joined;
}

/* A path for a .properties file: `\` and `:` escaped. */
static char *properties_path(const char *path) {
    char *slashes = sosu_strdup(path);
    for (char *c = slashes; *c; c++) {
        if (*c == '\\') *c = '/';
    }
    char *colons = replace_all(slashes, ":", "\\:");
    free(slashes);
    return colons;
}

typedef struct {
    const char *placeholder;
    char *value;
} Fill;

/* Copies the template `name` of lib/templates/android into the project, its placeholders filled. */
static bool write_template(const char *templates, const char *project, const char *name, const Fill *fills, size_t count) {
    char *from = path_join(templates, name);
    char *to = path_join(project, name);
    char *dir = sosu_strdup(to);
    char *slash = strrchr(dir, '/');
    if (slash) *slash = '\0';
    shell_make_dir(dir);
    free(dir);
    bool ok;
    SourceFile file;
    if (has_suffix(name, ".jar")) {
        ok = copy_file(from, to);
    } else if ((ok = source_load(&file, from))) {
        char *text = sosu_strdup(file.text);
        for (size_t i = 0; i < count; i++) {
            char *filled = replace_all(text, fills[i].placeholder, fills[i].value);
            free(text);
            text = filled;
        }
        ok = write_file(to, text);
        free(text);
        source_free(&file);
    }
    if (!ok) fprintf(stderr, "sosuc: cannot write `%s` from the template `%s`\n", to, from);
    free(from);
    free(to);
    return ok;
}

/* Copies a file or a folder of the package (an asset) to `to`. */
static bool copy_asset(const char *from, const char *to) {
    char *native_from = shell_path(from);
    char *native_to = shell_path(to);
    char *command;
#ifdef _WIN32
    command = sosu_format("xcopy /E /I /Y /Q \"%s\" \"%s\" >nul", native_from, native_to);
    SourceFile probe;
    if (source_load(&probe, from)) { /* a file: xcopy asks whether the target is a file */
        source_free(&probe);
        free(command);
        command = NULL;
    }
#else
    command = sosu_format("cp -R \"%s\" \"%s\"", native_from, native_to);
#endif
    bool ok = command ? shell_run(command) == 0 : copy_file(from, to);
    free(command);
    free(native_from);
    free(native_to);
    return ok;
}

/* The JDK that runs Gradle: JAVA_HOME, or the one of Android Studio. */
static char *find_jdk(void) {
    const char *home = getenv("JAVA_HOME");
    if (home && home[0]) return sosu_strdup(home);
#if defined(_WIN32)
    const char *studio = "C:/Program Files/Android/Android Studio/jbr";
#elif defined(__APPLE__)
    const char *studio = "/Applications/Android Studio.app/Contents/jbr/Contents/Home";
#else
    const char *studio = "/opt/android-studio/jbr";
#endif
    char *java = path_join(studio, "bin/java");
#ifdef _WIN32
    char *exe = sosu_format("%s.exe", java);
    free(java);
    java = exe;
#endif
    bool found = shell_file_exists(java);
    free(java);
    return found ? sosu_strdup(studio) : NULL;
}

static const char *const android_templates[] = {
    "settings.gradle", "build.gradle", "gradle.properties", "gradlew", "gradlew.bat", "gradle/wrapper/gradle-wrapper.jar",
    "gradle/wrapper/gradle-wrapper.properties", "app/build.gradle", "app/src/main/AndroidManifest.xml",
};

/* `sosuc package --target android`: `libmain.so` for arm64-v8a and x86_64 in a Gradle project made
   from lib/templates/android, then its APK (spec §11.5). */
static bool copy_app_assets(const Package *root, const char *dest);

static int command_package_android(const char *dir, const char *argv0, DiagFormat format, const BuildOptions *options) {
    char *lib = library_dir(argv0);
    Program program;
    Sema sema = {0};
    char *source = NULL;
    bool ok = program_load(&program, dir, lib);
    if (ok) ok = sema_check(&sema, &program);
    if (ok) {
        source = codegen_program(&sema, &program, CODEGEN_PROGRAM, options->checks);
        ok = source != NULL;
    }
    if (program.fatal) fprintf(stderr, "sosuc: %s\n", program.fatal);
    print_diagnostics(&program.diags, format);
    int status = program.fatal ? 2 : 1;
    const Package *platform = NULL;
    for (size_t i = 0; ok && i < program.packages.count; i++) {
        if (strcmp(program.packages.items[i]->name, "platform") == 0) platform = program.packages.items[i];
    }
    if (ok && !platform) {
        fprintf(stderr, "sosuc: an Android application opens its window with the package `platform` (SDL): this "
                        "program does not use it\n");
        ok = false;
    }
    char *sdk = ok ? find_android_sdk() : NULL;
    if (ok && !sdk) {
        fprintf(stderr, "sosuc: the Android SDK was not found: install Android Studio, or set ANDROID_HOME\n");
        ok = false;
    }
    if (ok) {
        status = 2;
        const Package *root = program.packages.items[0];
        const AppSection *app = &root->manifest.app;
        char *build_dir = path_join(root->root, "build");
        char *project = path_join(build_dir, "android/project");
        char *file = sosu_format("%s.c", root->name);
        bool built = true;
        for (size_t t = 0; built && t < 2; t++) {
            const Target *target = &mobile_targets[t];
            char *out_dir = mobile_dir(build_dir, target);
            shell_make_dir(out_dir);
            char *c_file = path_join(out_dir, file);
            char *library = NULL;
            Links links;
            collect_links(&program, &links, target);
            built = write_file(c_file, source) && compile_mobile(out_dir, c_file, lib, options->release, &links, target, &library);
            if (built) {
                char *jni = sosu_format("%s/app/jniLibs/%s", project, target->abi);
                shell_make_dir(jni);
                char *to = path_join(jni, "libmain.so");
                built = copy_file(library, to);
                free(to);
                /* libSDL3.so too: the archive of SDL gives its libraries for building (prefab), not for the APK */
                built = built && copy_files(&links, jni);
                free(jni);
            } else {
                fprintf(stderr, "sosuc: building for %s failed\n", target->name);
            }
            free_links(&links);
            free(library);
            free(c_file);
            free(out_dir);
        }

        /* the project: the template, the archive of SDL, the assets of [app] */
        char *templates = path_join(lib, "templates/android");
        char *vendor = path_join(platform->root, "vendor/SDL3-android");
        char *version_file = path_join(vendor, "VERSION.txt");
        SourceFile version;
        char *aar = NULL;
        if (built && source_load(&version, version_file)) {
            char *v = sosu_strdup(version.text);
            for (char *c = v; *c; c++) {
                if (*c == '\r' || *c == '\n') *c = '\0';
            }
            aar = sosu_format("SDL3-%s.aar", v);
            free(v);
            source_free(&version);
        } else if (built) {
            fprintf(stderr, "sosuc: SDL3 for Android is missing: run packages/platform/fetch-sdl3-android.ps1 once\n");
            built = false;
        }
        int api = newest_android_api(sdk);
        Fill fills[] = {
            { "@ID@", app->id ? sosu_strdup(app->id) : sosu_format("org.sosu.%s", root->name) },
            { "@NAME@", sosu_strdup(app->name ? app->name : root->name) },
            { "@VERSION@", sosu_strdup(app->version ? app->version : root->manifest.version ? root->manifest.version : "1.0") },
            { "@COMPILE_SDK@", sosu_format("%d", api > ANDROID_API ? api : 35) },
            { "@MIN_SDK@", sosu_format("%d", ANDROID_API) },
            { "@SDL_AAR@", sosu_strdup(aar ? aar : "") },
        };
        size_t fill_count = sizeof fills / sizeof *fills;
        for (size_t i = 0; built && i < sizeof android_templates / sizeof *android_templates; i++) {
            built = write_template(templates, project, android_templates[i], fills, fill_count);
        }
        if (built) {
            char *libs = path_join(project, "app/libs");
            shell_make_dir(libs);
            char *from = path_join(vendor, aar);
            char *to = path_join(libs, aar);
            built = copy_file(from, to);
            free(to);
            free(from);
            free(libs);
            char *sdk_path = properties_path(sdk);
            char *properties = sosu_format("# where the Android SDK is (written by sosuc)\nsdk.dir=%s\n", sdk_path);
            char *local = path_join(project, "local.properties");
            built = built && write_file(local, properties);
            free(local);
            free(properties);
            free(sdk_path);
        }
        if (built && app->assets.count) {
            char *assets = path_join(project, "app/assets");
            shell_make_dir(assets);
            built = copy_app_assets(root, assets);
            free(assets);
        }

        /* the APK, by Gradle */
        if (built) {
            char *jdk = find_jdk();
            char *native_project = shell_path(project);
            char *command;
#ifdef _WIN32
            char *native_jdk = jdk ? shell_path(jdk) : NULL;
            char *java = jdk ? sosu_format("set \"JAVA_HOME=%s\" && ", native_jdk) : sosu_strdup("");
            command = sosu_format("%s\"%s\\gradlew.bat\" --quiet -p \"%s\" assembleDebug", java, native_project, native_project);
            free(java);
            free(native_jdk);
#else
            command = jdk ? sosu_format("cd \"%s\" && chmod +x gradlew && JAVA_HOME=\"%s\" ./gradlew --quiet assembleDebug", native_project, jdk)
                          : sosu_format("cd \"%s\" && chmod +x gradlew && ./gradlew --quiet assembleDebug", native_project);
#endif
            if (shell_run(command) == 0) {
                printf("built %s/app/build/outputs/apk/debug/app-debug.apk\n", project);
                status = 0;
            } else {
                fprintf(stderr, "sosuc: Gradle failed on `%s`\n", project);
            }
            free(command);
            free(native_project);
            free(jdk);
        }
        for (size_t i = 0; i < fill_count; i++) free(fills[i].value);
        free(aar);
        free(version_file);
        free(vendor);
        free(templates);
        free(file);
        free(project);
        free(build_dir);
    }
    free(sdk);
    free(source);
    free(lib);
    sema_free(&sema);
    program_free(&program);
    return status;
}

/* `sosuc package --target ios-sim`: the application for the simulator, `build/ios/<Name>.app`,
   linked by clang with SDL3.framework, its Info.plist made from [app], signed for the simulator
   (spec §11.5). A device needs the signing team of the developer: `--target ios-arm64` gives
   `libmain.a` for their Xcode project. */
static int command_package_ios(const char *dir, const char *argv0, DiagFormat format, const BuildOptions *options) {
#ifndef __APPLE__
    (void)dir;
    (void)argv0;
    (void)format;
    (void)options;
    fprintf(stderr, "sosuc: building for iOS needs a Mac with Xcode (spec 11.5)\n");
    return 2;
#else
    const Target *target = options->target;
    if (!target->simulator) {
        fprintf(stderr, "sosuc: `sosuc package` builds for the simulator (--target ios-sim); for a device, build "
                        "libmain.a with `sosuc build --target ios-arm64` and link it in an Xcode project with your "
                        "signing team\n");
        return 2;
    }
    char *lib = library_dir(argv0);
    Program program;
    Sema sema = {0};
    char *source = NULL;
    bool ok = program_load(&program, dir, lib);
    if (ok) ok = sema_check(&sema, &program);
    if (ok) {
        source = codegen_program(&sema, &program, CODEGEN_PROGRAM, options->checks);
        ok = source != NULL;
    }
    if (program.fatal) fprintf(stderr, "sosuc: %s\n", program.fatal);
    print_diagnostics(&program.diags, format);
    int status = program.fatal ? 2 : 1;
    if (ok) {
        status = 2;
        const Package *root = program.packages.items[0];
        const AppSection *app = &root->manifest.app;
        char *build_dir = path_join(root->root, "build");
        char *out_dir = mobile_dir(build_dir, target);
        shell_make_dir(out_dir);
        char *file = sosu_format("%s.c", root->name);
        char *c_file = path_join(out_dir, file);
        char *library = NULL;
        Links links;
        collect_links(&program, &links, target);
        bool built = write_file(c_file, source) && compile_mobile(out_dir, c_file, lib, options->release, &links, target, &library);

        const char *name = app->name ? app->name : root->name;
        char *id = app->id ? sosu_strdup(app->id) : sosu_format("org.sosu.%s", root->name);
        const char *version = app->version ? app->version : root->manifest.version ? root->manifest.version : "1.0";
        char *bundle = sosu_format("%s/ios/%s.app", build_dir, root->name);
        if (built) {
            shell_remove_dir(bundle);
            char *frameworks = path_join(bundle, "Frameworks");
            shell_make_dir(frameworks);
            /* the libraries of [link.ios]: a framework where a path holds one, else a library */
            char *link_args = sosu_strdup("");
            for (size_t i = 0; i < links.paths.count; i++) append(&link_args, " -F\"%s\"", links.paths.items[i]);
            for (size_t i = 0; i < links.paths.count; i++) append(&link_args, " -L\"%s\"", links.paths.items[i]);
            for (size_t i = 0; i < links.libs.count; i++) {
                const char *lib_name = links.libs.items[i];
                bool framework = false;
                for (size_t j = 0; j < links.paths.count && !framework; j++) {
                    char *candidate = sosu_format("%s/%s.framework", links.paths.items[j], lib_name);
                    char *probe = sosu_format("%s/Info.plist", candidate);
                    framework = shell_file_exists(probe);
                    if (framework) {
                        char *copy = sosu_format("cp -R \"%s\" \"%s/\"", candidate, frameworks);
                        built = built && shell_run(copy) == 0;
                        free(copy);
                    }
                    free(probe);
                    free(candidate);
                }
                append(&link_args, framework ? " -framework %s" : " -l%s", lib_name);
            }
            for (size_t i = 0; i < links.frameworks.count; i++) append(&link_args, " -framework %s", links.frameworks.items[i]);
            char *command = sosu_format("xcrun --sdk iphonesimulator clang -arch arm64 -mios-simulator-version-min=13.0 "
                                        "-Wl,-force_load,\"%s\"%s -Wl,-rpath,@executable_path/Frameworks -o \"%s/%s\"",
                                        library, link_args, bundle, root->name);
            built = built && shell_run(command) == 0;
            free(command);
            free(link_args);
            free(frameworks);
        }
        if (built) {
            char *plist = sosu_format(
                "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
                "<plist version=\"1.0\">\n<dict>\n"
                "    <key>CFBundleIdentifier</key><string>%s</string>\n"
                "    <key>CFBundleName</key><string>%s</string>\n"
                "    <key>CFBundleDisplayName</key><string>%s</string>\n"
                "    <key>CFBundleExecutable</key><string>%s</string>\n"
                "    <key>CFBundleVersion</key><string>%s</string>\n"
                "    <key>CFBundleShortVersionString</key><string>%s</string>\n"
                "    <key>CFBundlePackageType</key><string>APPL</string>\n"
                "    <key>CFBundleSupportedPlatforms</key><array><string>iPhoneSimulator</string></array>\n"
                "    <key>MinimumOSVersion</key><string>13.0</string>\n"
                "    <key>LSRequiresIPhoneOS</key><true/>\n"
                "    <key>UIDeviceFamily</key><array><integer>1</integer><integer>2</integer></array>\n"
                "    <key>UILaunchScreen</key><dict/>\n"
                "    <key>UIRequiresFullScreen</key><true/>\n"
                "</dict>\n</plist>\n",
                id, name, name, root->name, version, version);
            char *plist_file = path_join(bundle, "Info.plist");
            built = write_file(plist_file, plist);
            free(plist_file);
            free(plist);
        }
        /* the assets of [app], under the path the program reads (SDL looks in the bundle) */
        built = built && copy_app_assets(root, bundle);
        if (built) {
            char *sign = sosu_format("codesign --force --sign - --timestamp=none \"%s\" >/dev/null 2>&1 || "
                                     "codesign --force --sign - \"%s\"",
                                     bundle, bundle);
            built = shell_run(sign) == 0;
            free(sign);
        }
        if (built) {
            printf("built %s\n", bundle);
            printf("on a booted simulator: xcrun simctl install booted %s && xcrun simctl launch booted %s\n", bundle, id);
            status = 0;
        } else {
            fprintf(stderr, "sosuc: building `%s` failed\n", bundle);
        }
        free(bundle);
        free(id);
        free_links(&links);
        free(library);
        free(c_file);
        free(file);
        free(out_dir);
        free(build_dir);
    }
    free(source);
    free(lib);
    sema_free(&sema);
    program_free(&program);
    return status;
#endif
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
        char *base_dir = path_join(root->root, "build");
        /* another desktop: build/<target>/, beside the program of this one (spec §11.6) */
        char *build_dir = options->target->os == TARGET_DESKTOP ? path_join(base_dir, options->target->name)
                                                                : sosu_strdup(base_dir);
        free(base_dir);
        shell_make_dir(build_dir);

        char *name = sosu_format(mode == MODE_TEST ? "%s_tests" : "%s", root->name);
        char *file = sosu_format("%s.c", name);
        char *c_file = path_join(build_dir, file);
        char *exe_name = strcmp(target_system(options->target), "windows") == 0 ? sosu_format("%s.exe", name)
                                                                                : sosu_strdup(name);
        char *exe = path_join(build_dir, exe_name);
        Links links;
        collect_links(&program, &links, options->target);
        if (!write_file(c_file, source)) {
            fprintf(stderr, "sosuc: cannot write `%s`\n", c_file);
            status = 2;
        } else if (!compile_c(build_dir, c_file, exe, lib, options->release, &links, options->target)) {
            fprintf(stderr, "sosuc: the C compiler failed on `%s`\n", c_file);
            status = 2;
        } else if (!copy_files(&links, build_dir)) {
            status = 2;
        } else if (mode == MODE_BUILD) {
            printf("built %s\n", exe);
            status = 0;
        } else {
            char *native_exe = shell_path(exe);
#ifdef _WIN32
            /* cmd takes away the first and the last quote of a line that has more than two */
            char *command = sosu_format("\"\"%s\"%s\"", native_exe, options->run_args ? options->run_args : "");
#else
            char *command = sosu_format("\"%s\"%s", native_exe, options->run_args ? options->run_args : "");
#endif
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

/* ---- sosuc package for the desktop (spec §11.6) ---- */

/* Copies the assets of [app] into `dest`, each under the path the program reads: its path
   relative to the current folder (spec §11.5). False, reported, if one cannot be copied. */
static bool copy_app_assets(const Package *root, const char *dest) {
    const AppSection *app = &root->manifest.app;
    for (size_t i = 0; i < app->assets.count; i++) {
        char *from = package_path(root, app->assets.items[i]);
        if (strncmp(from, "../", 3) == 0 || from[0] == '/' || (from[0] && from[1] == ':')) {
            fprintf(stderr, "sosuc: the asset `%s` is outside the current folder: run sosuc from a folder that "
                            "holds it, as the program does on the desktop\n", from);
            free(from);
            return false;
        }
        char *to = path_join(dest, from);
        char *parent = sosu_strdup(to);
        char *slash = strrchr(parent, '/');
        if (slash) *slash = '\0';
        shell_make_dir(parent);
        bool ok = copy_asset(from, to);
        if (!ok) fprintf(stderr, "sosuc: cannot copy the asset `%s`\n", from);
        free(parent);
        free(to);
        free(from);
        if (!ok) return false;
    }
    return true;
}

#ifdef _WIN32
/* An icon file (.ico) holding the PNG `png`: Windows reads PNG icons since Vista. */
static bool write_ico(const char *png, const char *ico) {
    SourceFile image;
    if (!source_load(&image, png)) return false;
    const unsigned char *bytes = (const unsigned char *)image.text;
    bool ok = image.length > 24 && memcmp(bytes, "\x89PNG", 4) == 0;
    FILE *out = ok ? fopen(ico, "wb") : NULL;
    if (out) {
        unsigned width = (unsigned)bytes[16] << 24 | (unsigned)bytes[17] << 16 | (unsigned)bytes[18] << 8 | bytes[19];
        unsigned height = (unsigned)bytes[20] << 24 | (unsigned)bytes[21] << 16 | (unsigned)bytes[22] << 8 | bytes[23];
        uint32_t size = (uint32_t)image.length;
        unsigned char header[22] = {
            0, 0, 1, 0, 1, 0,                                         /* ICONDIR: an icon, one image */
            (unsigned char)(width >= 256 ? 0 : width), (unsigned char)(height >= 256 ? 0 : height), 0, 0,
            1, 0, 32, 0,                                              /* planes, bits per pixel */
            (unsigned char)size, (unsigned char)(size >> 8), (unsigned char)(size >> 16), (unsigned char)(size >> 24),
            22, 0, 0, 0,                                              /* where the PNG starts */
        };
        ok = fwrite(header, 1, sizeof header, out) == sizeof header && fwrite(bytes, 1, image.length, out) == image.length;
        ok = fclose(out) == 0 && ok;
    } else {
        ok = false;
    }
    source_free(&image);
    return ok;
}
#endif

/* The icon of [app] as a resource of the program, on Windows: a .res (MSVC) or a .o (MinGW),
   or NULL if there is none or the tools are missing (reported). */
static char *windows_icon(const Package *root, const char *relative_dir) {
#ifdef _WIN32
    const char *icon = root->manifest.app.icon;
    if (!icon) return NULL;
    char *build_dir = absolute_path(relative_dir); /* rc runs from it */
    char *png = package_path(root, icon);
    char *ico = path_join(build_dir, "icon.ico");
    char *rc = path_join(build_dir, "icon.rc");
    const char *cc = getenv("SOSU_CC");
    bool msvc = !cc || !cc[0];
    char *res = path_join(build_dir, msvc ? "icon.res" : "icon_res.o");
    char *text = sosu_strdup("1 ICON \"icon.ico\"\n");
    bool ok = write_ico(png, ico) && write_file(rc, text);
    if (ok) {
        char *native_rc = shell_path(rc);
        char *native_res = shell_path(res);
        char *native_dir = shell_path(build_dir);
        if (msvc) {
            char *commands = sosu_format("cd /d \"%s\" && rc /nologo /fo \"%s\" \"%s\" >nul || exit /b 1\r\n", native_dir,
                                         native_res, native_rc);
            char *script = msvc_script(commands);
            char *bat = path_join(build_dir, "icon.bat");
            char *native_bat = shell_path(bat);
            char *command = sosu_format("\"%s\"", native_bat);
            ok = write_file(bat, script) && shell_run(command) == 0;
            free(command);
            free(native_bat);
            free(bat);
            free(script);
            free(commands);
        } else {
            char *command = sosu_format("cd /d \"%s\" && windres \"%s\" -O coff -o \"%s\"", native_dir, native_rc, native_res);
            ok = shell_run(command) == 0;
            free(command);
        }
        free(native_dir);
        free(native_res);
        free(native_rc);
    }
    if (!ok) fprintf(stderr, "sosuc: the icon `%s` was not added to the program (a PNG, and rc or windres, are needed)\n", icon);
    free(text);
    free(rc);
    free(ico);
    free(png);
    free(build_dir);
    if (ok) return res;
    free(res);
    return NULL;
#else
    (void)root;
    (void)relative_dir;
    return NULL;
#endif
}

/* `sosuc package <package>`: a release build of the program in build/package/<name>/, with the
   files of `copy` and the assets of [app], then its archive (spec §11.6). */
static int command_package_desktop(const char *dir, const char *argv0, DiagFormat format, const BuildOptions *given) {
    BuildOptions options = *given;
    options.release = true;
    options.checks.overflow_checks = false;
    options.checks.contracts = false;
    char *lib = library_dir(argv0);
    Program program;
    Sema sema = {0};
    char *source = NULL;
    bool ok = program_load(&program, dir, lib);
    if (ok) ok = sema_check(&sema, &program);
    if (ok) {
        source = codegen_program(&sema, &program, CODEGEN_PROGRAM, options.checks);
        ok = source != NULL;
    }
    if (program.fatal) fprintf(stderr, "sosuc: %s\n", program.fatal);
    print_diagnostics(&program.diags, format);
    int status = program.fatal ? 2 : 1;
    if (ok) {
        status = 2;
        const Package *root = program.packages.items[0];
        const AppSection *app = &root->manifest.app;
        /* the layout, the icon and the archive are those of the system of the target */
        const char *target_name = options.target->os == TARGET_HOST ? host_system() : options.target->name;
        const char *system = target_system(options.target);
        bool windows = strcmp(system, "windows") == 0;
        bool macos = strcmp(system, "macos") == 0;
        char *build_dir = path_join(root->root, "build");
        if (options.target->os == TARGET_DESKTOP) {
            char *own = path_join(build_dir, options.target->name);
            free(build_dir);
            build_dir = own;
        }
        char *packages = path_join(build_dir, "package");
        shell_make_dir(packages);
        char *file = sosu_format("%s.c", root->name);
        char *c_file = path_join(build_dir, file);
        const char *title = app->name ? app->name : root->name;
        /* macOS: <Name>.app, the program in Contents/MacOS, its libraries in Contents/Frameworks,
           the assets in Contents/Resources, which SDL_GetBasePath gives */
        char *folder_name = macos ? sosu_format("%s.app", title) : sosu_strdup(root->name);
        char *folder = path_join(packages, folder_name);
        char *bin_dir = macos ? path_join(folder, "Contents/MacOS") : sosu_strdup(folder);
        char *files_dir = macos ? path_join(folder, "Contents/Frameworks") : sosu_strdup(folder);
        char *assets_dir = macos ? path_join(folder, "Contents/Resources") : sosu_strdup(folder);
        shell_remove_dir(folder);
        shell_make_dir(bin_dir);
        shell_make_dir(files_dir);
        shell_make_dir(assets_dir);
        char *exe_name = windows ? sosu_format("%s.exe", root->name) : sosu_strdup(root->name);
        char *exe = path_join(bin_dir, exe_name);
        Links links;
        collect_links(&program, &links, options.target);
        if (windows && options.target->os == TARGET_HOST) links.resource = windows_icon(root, build_dir);
        bool built = write_file(c_file, source);
        if (!built) fprintf(stderr, "sosuc: cannot write `%s`\n", c_file);
        if (built && !compile_c(build_dir, c_file, exe, lib, true, &links, options.target)) {
            fprintf(stderr, "sosuc: the C compiler failed on `%s`\n", c_file);
            built = false;
        }
        if (built && windows) { /* what MSVC writes for the debugger stays out of the package */
            const char *extras[] = { ".ilk", ".pdb" };
            for (size_t i = 0; i < 2; i++) {
                char *extra = sosu_format("%s/%s%s", bin_dir, root->name, extras[i]);
                remove(extra);
                free(extra);
            }
        }
        built = built && copy_files(&links, files_dir) && copy_app_assets(root, assets_dir);
        if (built && app->icon && !windows) { /* Windows: in the program */
            char *png = package_path(root, app->icon);
            const char *slash = strrchr(png, '/');
            char *icon_copy = path_join(assets_dir, slash ? slash + 1 : png);
            built = copy_file(png, icon_copy);
            free(icon_copy);
            free(png);
        }
        if (built && !windows && !macos) { /* a .desktop file, for the menus of the system */
            char *icon_line = sosu_strdup("");
            if (app->icon) {
                const char *slash = strrchr(app->icon, '/');
                append(&icon_line, "Icon=%s\n", slash ? slash + 1 : app->icon);
            }
            char *entry = sosu_format("[Desktop Entry]\nType=Application\nName=%s\nExec=%s\n%sTerminal=false\n", title,
                                      root->name, icon_line);
            char *entry_name = sosu_format("%s.desktop", root->name);
            char *entry_path = path_join(folder, entry_name);
            built = write_file(entry_path, entry);
            free(entry_path);
            free(entry_name);
            free(entry);
            free(icon_line);
        }
        if (built && macos) {
            char *id = app->id ? sosu_strdup(app->id) : sosu_format("org.sosu.%s", root->name);
            const char *version = app->version ? app->version : root->manifest.version ? root->manifest.version : "1.0";
            char *plist = sosu_format(
                "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
                "<plist version=\"1.0\">\n<dict>\n"
                "    <key>CFBundleIdentifier</key><string>%s</string>\n"
                "    <key>CFBundleName</key><string>%s</string>\n"
                "    <key>CFBundleExecutable</key><string>%s</string>\n"
                "    <key>CFBundleVersion</key><string>%s</string>\n"
                "    <key>CFBundleShortVersionString</key><string>%s</string>\n"
                "    <key>CFBundlePackageType</key><string>APPL</string>\n"
                "    <key>NSHighResolutionCapable</key><true/>\n"
                "</dict>\n</plist>\n",
                id, title, root->name, version, version);
            char *plist_file = path_join(folder, "Contents/Info.plist");
            built = write_file(plist_file, plist);
#ifdef __APPLE__
            /* the program finds the libraries of Frameworks */
            char *rpath = sosu_format("install_name_tool -add_rpath @executable_path/../Frameworks \"%s\" 2>/dev/null; true", exe);
            shell_run(rpath);
            free(rpath);
#endif
            free(plist_file);
            free(plist);
            free(id);
        }
        /* the archive: .zip for Windows and macOS, .tar.gz for Linux, made with the tools of this machine */
        char *archive = NULL;
        if (built) {
            bool zip = windows || macos;
            archive = sosu_format("%s-%s.%s", root->name, target_name, zip ? "zip" : "tar.gz");
            char *native = shell_path(packages);
#if defined(_WIN32)
            char *command = sosu_format("cd /d \"%s\" && (del /q \"%s\" 2>nul & tar %s \"%s\" \"%s\")", native, archive,
                                        zip ? "-a -c -f" : "-czf", archive, folder_name);
#elif defined(__APPLE__)
            char *command = zip ? sosu_format("cd \"%s\" && rm -f \"%s\" && ditto -c -k --keepParent \"%s\" \"%s\"", native,
                                              archive, folder_name, archive)
                                : sosu_format("cd \"%s\" && rm -f \"%s\" && tar -czf \"%s\" \"%s\"", native, archive, archive,
                                              folder_name);
#else
            char *command = zip ? sosu_format("cd \"%s\" && rm -f \"%s\" && zip -qr \"%s\" \"%s\"", native, archive, archive,
                                              folder_name)
                                : sosu_format("cd \"%s\" && rm -f \"%s\" && tar -czf \"%s\" \"%s\"", native, archive, archive,
                                              folder_name);
#endif
            built = shell_run(command) == 0;
            if (!built) fprintf(stderr, "sosuc: cannot make the archive `%s`\n", archive);
            free(command);
            free(native);
        }
        if (built) {
            printf("packaged %s/%s\n", packages, folder_name);
            printf("archive %s/%s\n", packages, archive);
            status = 0;
        }
        free(archive);
        free_links(&links);
        free(exe);
        free(exe_name);
        free(assets_dir);
        free(files_dir);
        free(bin_dir);
        free(folder);
        free(folder_name);
        free(c_file);
        free(file);
        free(packages);
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
                if (unit->package == root) {  /* .sosu and .ui.sosu files */
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
    if (strcmp(command, "bindgen") == 0) return command_bindgen(argc - 2, argv + 2);

    DiagFormat format = FORMAT_TEXT;
    const char *path = NULL;
    BuildOptions options = { false, 0, &host_target, { true, true, true } };
    int checks = -1;    /* --checks: -1 when not given */
    int contracts = -1; /* --contracts */
    bool is_fmt = strcmp(command, "fmt") == 0;
    bool fmt_check = false;
    const char **paths = sosu_alloc((size_t)argc * sizeof *paths); /* fmt takes several */
    int path_count = 0;
    for (int i = 2; i < argc; i++) {
        if (strcmp(command, "run") == 0 && strcmp(argv[i], "--") == 0) {
            options.run_args = sosu_strdup("");
            for (int j = i + 1; j < argc; j++) append_quoted(&options.run_args, argv[j]);
            break;
        }
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
        } else if (strcmp(argv[i], "--target") == 0 || strncmp(argv[i], "--target=", 9) == 0) {
            const char *name = argv[i][8] == '=' ? argv[i] + 9 : i + 1 < argc ? argv[++i] : "";
            options.target = find_target(name);
            if (!options.target) return 2;
        } else if (strcmp(argv[i], "--lib") == 0 || strcmp(argv[i], "--lib=static") == 0) {
            options.lib = 1;
        } else if (strcmp(argv[i], "--lib=shared") == 0) {
            options.lib = 2;
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
    if (strcmp(command, "package") == 0) {
        if (options.target->os == TARGET_ANDROID) return command_package_android(path ? path : ".", argv[0], format, &options);
        if (options.target->os == TARGET_IOS) return command_package_ios(path ? path : ".", argv[0], format, &options);
        return command_package_desktop(path ? path : ".", argv[0], format, &options);
    }
    if (options.target->os == TARGET_DESKTOP && options.lib) {
        fprintf(stderr, "sosuc: a library is built for this machine only, for now (spec 11.6)\n");
        return 2;
    }
    if (options.target->os == TARGET_DESKTOP && strcmp(command, "build") != 0) {
        fprintf(stderr, "sosuc: a program for %s is built here, not run: `sosuc build --target %s` or `sosuc package "
                        "--target %s` (spec 11.6)\n",
                options.target->name, options.target->name, options.target->name);
        return 2;
    }
    if (options.target->os == TARGET_ANDROID || options.target->os == TARGET_IOS) {
        if (strcmp(command, "build") != 0 || options.lib) {
            fprintf(stderr, "sosuc: `--target %s` goes with `sosuc build` (a program for a phone, spec 11.5)\n",
                    options.target->name);
            return 2;
        }
        if (!options.target->abi[0]) {
            fprintf(stderr, "sosuc: `sosuc build` takes one architecture: --target android-arm64 or android-x64\n");
            return 2;
        }
        return command_build_mobile(path ? path : ".", argv[0], format, &options);
    }
    if (options.lib && strcmp(command, "build") != 0) {
        fprintf(stderr, "sosuc: `--lib` goes with `sosuc build`\n");
        return 2;
    }
    if (strcmp(command, "build") == 0 && options.lib) return command_build_library(path ? path : ".", argv[0], format, &options);
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
