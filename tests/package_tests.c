#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "path.h"
#include "program.h"
#include "shell.h"
#include "test.h"

/* Run from the repository root: the test packages are in tests/packages. */

static void expect_normalized(const char *path, const char *expected) {
    char *result = path_normalize(path);
    CHECK(strcmp(result, expected) == 0, "normalize(`%s`) = `%s`, expected `%s`", path, result, expected);
    free(result);
}

static void test_paths(void) {
    expect_normalized("a/b/../c", "a/c");
    expect_normalized("./a//b/", "a/b");
    expect_normalized("tests/packages/app/../geometry", "tests/packages/geometry");
    expect_normalized("../x/../../y", "../../y");
    expect_normalized("a/..", ".");
    expect_normalized("/a/../../b", "/b");
    expect_normalized("C:\\dev\\sosu\\..\\lib", "C:/dev/lib");

    char *joined = path_join("a/b", "c.sosu");
    CHECK(strcmp(joined, "a/b/c.sosu") == 0, "join: `%s`", joined);
    free(joined);
    char *dir = path_dirname("build/sosuc.exe");
    CHECK(strcmp(dir, "build") == 0, "dirname: `%s`", dir);
    free(dir);
    dir = path_dirname("sosuc");
    CHECK(strcmp(dir, ".") == 0, "dirname: `%s`", dir);
    free(dir);

    /* sosuc run as `build/sosuc` is there; run as `sosuc`, it is found in PATH */
    char *program = path_of_program("build/sosuc");
    CHECK(strcmp(program, "build/sosuc") == 0, "program with a folder: `%s`", program);
    free(program);
    program = path_of_program("no-such-program-in-path");
    CHECK(strcmp(program, "no-such-program-in-path") == 0, "program not in PATH: `%s`", program);
    free(program);
}

typedef struct {
    SourceFile file;
    Manifest manifest;
    DiagList diags;
} ParsedManifest;

static void parse_manifest(ParsedManifest *out, const char *text) {
    memset(out, 0, sizeof *out);
    source_from_string(&out->file, "sosu.toml", text);
    manifest_parse(&out->manifest, &out->file, &out->diags);
}

static void parsed_manifest_free(ParsedManifest *parsed) {
    manifest_free(&parsed->manifest);
    diag_free(&parsed->diags);
    source_free(&parsed->file);
}

/* Checks the codes of all the diagnostics, separated by spaces ("" for none). */
static void check_codes(const char *what, const DiagList *diags, const char *expected) {
    char codes[256] = "";
    for (size_t i = 0; i < diags->count && strlen(codes) + 8 < sizeof codes; i++) {
        if (i) strcat(codes, " ");
        strcat(codes, diags->items[i].code);
    }
    const Diagnostic *first = diags->count ? &diags->items[0] : NULL;
    CHECK(strcmp(codes, expected) == 0, "%s: got [%s], expected [%s]%s%s%s%s", what, codes, expected,
          first ? "; first: " : "", first ? first->message : "", first && first->help ? " / " : "",
          first && first->help ? first->help : "");
}

static void expect_manifest_errors(const char *text, const char *expected) {
    ParsedManifest parsed;
    parse_manifest(&parsed, text);
    check_codes(text, &parsed.diags, expected);
    parsed_manifest_free(&parsed);
}

static void test_manifest(void) {
    ParsedManifest parsed;
    parse_manifest(&parsed,
                   "# comment\n"
                   "[package]\n"
                   "name = \"myapp\"   # trailing comment\n"
                   "version = \"0.1.0\"\r\n"
                   "\n"
                   "[dependencies]\n"
                   "ui = { git = \"https://git.example/sosu-ui\", tag = \"v0.1.0\" }\n"
                   "shared = {path=\"../shared\"}\n");
    check_codes("valid manifest", &parsed.diags, "");
    CHECK(parsed.manifest.name && strcmp(parsed.manifest.name, "myapp") == 0, "name");
    CHECK(parsed.manifest.version && strcmp(parsed.manifest.version, "0.1.0") == 0, "version");
    CHECK(parsed.manifest.deps.count == 2, "%zu dependencies", parsed.manifest.deps.count);
    if (parsed.manifest.deps.count == 2) {
        const Dependency *ui = &parsed.manifest.deps.items[0];
        const Dependency *shared = &parsed.manifest.deps.items[1];
        CHECK(strcmp(ui->name, "ui") == 0 && ui->git && strcmp(ui->tag, "v0.1.0") == 0 && !ui->path, "ui");
        CHECK(strcmp(shared->name, "shared") == 0 && strcmp(shared->path, "../shared") == 0, "shared");
    }
    parsed_manifest_free(&parsed);

    const char *package = "[package]\nname = \"a\"\nversion = \"1\"\n";
    char text[512];

    expect_manifest_errors("name = \"a\"\n", "E0401 E0403");
    expect_manifest_errors("[package]\nname \"a\"\nversion = \"1\"\n", "E0401 E0403");
    expect_manifest_errors("[package]\nname = \"a\nversion = \"1\"\n", "E0401 E0403");
    expect_manifest_errors("[package]\nname = 3\nversion = \"1\"\n", "E0401 E0403");
    expect_manifest_errors("[package]\nname = \"a\" x\nversion = \"1\"\n", "E0401");
    expect_manifest_errors("[package]\nname = \"a\"\nversion = \"1\"\nauthor = \"me\"\n", "E0402");
    expect_manifest_errors("[package]\nname = \"a\"\nversion = \"1\"\n[tools]\nx = 1\n", "E0402");
    expect_manifest_errors("", "E0403");
    expect_manifest_errors("[package]\nname = \"a\"\n", "E0403");
    expect_manifest_errors("[package]\nname = \"a\"\nname = \"b\"\nversion = \"1\"\n", "E0408");
    expect_manifest_errors("[package]\nname = \"my-app\"\nversion = \"1\"\n", "E0409");

    parse_manifest(&parsed, "[package]\nname = \"a\"\nversion = \"1\"\n"
                            "[link]\nlibs = [\"SDL3\", \"m\",]\nsubsystem = \"window\"\n"
                            "[link.windows]\npaths = [\"vendor/lib\"]\ncopy = [ \"vendor/SDL3.dll\" ]  # next to the exe\n"
                            "[link.macos]\nframeworks = []\n");
    check_codes("link sections", &parsed.diags, "");
    CHECK(parsed.manifest.link[LINK_ALL].libs.count == 2 &&
          strcmp(parsed.manifest.link[LINK_ALL].libs.items[1], "m") == 0, "libs");
    CHECK(parsed.manifest.link[LINK_WINDOWS].paths.count == 1 && parsed.manifest.link[LINK_WINDOWS].copy.count == 1,
          "windows section");
    CHECK(parsed.manifest.link[LINK_MACOS].frameworks.count == 0, "empty list");
    CHECK(parsed.manifest.link[LINK_ALL].subsystem && strcmp(parsed.manifest.link[LINK_ALL].subsystem, "window") == 0,
          "subsystem");
    parsed_manifest_free(&parsed);

    /* [app]: what a phone shows of the program (spec §11.5) */
    parse_manifest(&parsed, "[package]\nname = \"a\"\nversion = \"1\"\n"
                            "[app]\nid = \"org.sosu.a\"\nname = \"A\"\nassets = [\"assets\", \"fonts/x.ttf\"]\n"
                            "[link.android]\npaths = [\"vendor/{abi}\"]\n");
    check_codes("app section", &parsed.diags, "");
    CHECK(parsed.manifest.app.id && strcmp(parsed.manifest.app.id, "org.sosu.a") == 0 && parsed.manifest.app.name &&
          strcmp(parsed.manifest.app.name, "A") == 0 && !parsed.manifest.app.version && parsed.manifest.app.assets.count == 2,
          "app section");
    CHECK(parsed.manifest.link[LINK_ANDROID].paths.count == 1, "android section");
    parsed_manifest_free(&parsed);
    expect_manifest_errors("[package]\nname = \"a\"\nversion = \"1\"\n[app]\nlabel = \"A\"\n", "E0402");
    expect_manifest_errors("[package]\nname = \"a\"\nversion = \"1\"\n[app]\nid = \"x\"\nid = \"y\"\n", "E0408");

    expect_manifest_errors("[package]\nname = \"a\"\nversion = \"1\"\n[link.beos]\nlibs = [\"x\"]\n", "E0402");
    expect_manifest_errors("[package]\nname = \"a\"\nversion = \"1\"\n[link]\nlibraries = [\"x\"]\n", "E0402");
    expect_manifest_errors("[package]\nname = \"a\"\nversion = \"1\"\n[link]\nsubsystem = \"gui\"\n", "E0402");
    expect_manifest_errors("[package]\nname = \"a\"\nversion = \"1\"\n[link]\nlibs = \"x\"\n", "E0401");
    expect_manifest_errors("[package]\nname = \"a\"\nversion = \"1\"\n[link]\nlibs = [\"x\" \"y\"]\n", "E0401");
    expect_manifest_errors("[package]\nname = \"a\"\nversion = \"1\"\n[link]\nlibs = [\"x\"]\n[link]\n", "E0408");

    snprintf(text, sizeof text, "%s[dependencies]\nx = \"../x\"\n", package);
    expect_manifest_errors(text, "E0404");
    snprintf(text, sizeof text, "%s[dependencies]\nx = { }\n", package);
    expect_manifest_errors(text, "E0404");
    snprintf(text, sizeof text, "%s[dependencies]\nx = { path = \"../x\", git = \"g\", tag = \"t\" }\n", package);
    expect_manifest_errors(text, "E0404");
    snprintf(text, sizeof text, "%s[dependencies]\nx = { git = \"g\" }\n", package);
    expect_manifest_errors(text, "E0404");
    snprintf(text, sizeof text, "%s[dependencies]\nx = { path = \"../x\", tag = \"t\" }\n", package);
    expect_manifest_errors(text, "E0404");
    snprintf(text, sizeof text, "%s[dependencies]\nx = { path = \"../x\", branch = \"main\" }\n", package);
    expect_manifest_errors(text, "E0402");
    snprintf(text, sizeof text, "%s[dependencies]\nx = { path = \"../x\" }\nx = { path = \"../y\" }\n", package);
    expect_manifest_errors(text, "E0408");
    snprintf(text, sizeof text, "%s[dependencies]\nmy-lib = { path = \"../x\" }\n", package);
    expect_manifest_errors(text, "E0409");
}

static void expect_program(const char *dir, const char *codes, size_t units, size_t packages) {
    Program program;
    program_load(&program, dir, "lib");
    CHECK(!program.fatal, "%s: fatal error: %s", dir, program.fatal ? program.fatal : "");
    check_codes(dir, &program.diags, codes);
    CHECK(program.units.count == units, "%s: %zu modules, expected %zu", dir, program.units.count, units);
    CHECK(program.packages.count == packages, "%s: %zu packages, expected %zu", dir, program.packages.count, packages);
    program_free(&program);
}

static void test_program(void) {
    expect_program("tests/packages/app", "", 5, 4); /* + core.prelude, loaded with every program */
    expect_program("tests/packages/geometry", "", 3, 2);
    expect_program("tests/packages/bad_imports", "E0410 E0411 E0412 E0410", 2, 3);
    expect_program("tests/packages/bad_deps", "E0406 E0405 E0415 E0407", 2, 4);

    Program program;
    program_load(&program, "tests/packages/app", "nowhere");
    check_codes("missing standard library", &program.diags, "E0414");
    program_free(&program);

    program_load(&program, "tests/packages/no_entry", "lib");
    CHECK(program.fatal && strstr(program.fatal, "src/main.sosu"), "no entry file: %s", program.fatal ? program.fatal : "");
    program_free(&program);

    program_load(&program, "tests/packages/does_not_exist", "lib");
    CHECK(program.fatal && strstr(program.fatal, "no sosu.toml"), "no manifest: %s", program.fatal ? program.fatal : "");
    program_free(&program);

    /* Imports are linked to the loaded modules. */
    program_load(&program, "tests/packages/app", "lib");
    const Unit *main_unit = program.units.count ? program.units.items[0] : NULL;
    CHECK(main_unit && main_unit->import_count == 3, "app.main imports");
    if (main_unit && main_unit->import_count == 3) {
        CHECK(strcmp(main_unit->module, "app.main") == 0, "entry module: %s", main_unit->module);
        CHECK(main_unit->imports[0].unit && strcmp(main_unit->imports[0].unit->module, "std.io") == 0, "std.io");
        CHECK(main_unit->imports[2].unit && main_unit->imports[2].unit->package->manifest.version &&
              strcmp(main_unit->imports[2].unit->package->manifest.version, "0.2.0") == 0, "geometry.vector");
    }
    program_free(&program);
}

/* ---- Git dependencies: a local repository, built in build/git_test ---- */

static void write_text(const char *path, const char *text) {
    FILE *file = fopen(path, "wb");
    CHECK(file != NULL, "cannot write %s", path);
    if (!file) return;
    fputs(text, file);
    fclose(file);
}

static bool git(const char *args) {
    char *command = sosu_format("git -C build/git_test/shapes -c user.name=test -c user.email=test@example.com %s"
#ifdef _WIN32
                                " >nul 2>nul",
#else
                                " >/dev/null 2>&1",
#endif
                                args);
    int status = shell_run(command);
    free(command);
    return status == 0;
}

static void write_app(const char *source) {
    char *manifest = sosu_format("[package]\nname = \"app\"\nversion = \"0.1.0\"\n\n[dependencies]\nshapes = { %s }\n",
                                 source);
    write_text("build/git_test/app/sosu.toml", manifest);
    free(manifest);
}

/* The folder of `shapes` in the program: build/deps/shapes-<commit>. */
static char *shapes_root(const Program *program) {
    for (size_t i = 0; i < program->packages.count; i++) {
        if (strcmp(program->packages.items[i]->name, "shapes") == 0) return sosu_strdup(program->packages.items[i]->root);
    }
    return sosu_strdup("");
}

static char *load_shapes(const char *what, const char *codes) {
    Program program;
    program_load(&program, "build/git_test/app", "lib");
    CHECK(!program.fatal, "%s: fatal error: %s", what, program.fatal ? program.fatal : "");
    check_codes(what, &program.diags, codes);
    char *root = shapes_root(&program);
    program_free(&program);
    return root;
}

static char *read_lock(void) {
    SourceFile file;
    if (!source_load(&file, "build/git_test/app/sosu.lock")) return sosu_strdup("");
    char *text = sosu_strdup(file.text);
    source_free(&file);
    return text;
}

static void test_git(void) {
    shell_remove_dir("build/git_test");
    shell_make_dir("build/git_test/shapes/src");
    shell_make_dir("build/git_test/app/src");
    write_text("build/git_test/shapes/sosu.toml", "[package]\nname = \"shapes\"\nversion = \"1.0.0\"\n");
    write_text("build/git_test/shapes/src/lib.sosu", "pub i32 sides() {\n    return 3;\n}\n");
    write_text("build/git_test/app/src/main.sosu", "import shapes.lib;\n\ni32 main() {\n    return lib.sides();\n}\n");
    bool ready = git("init --quiet") && git("add .") && git("commit --quiet -m one") && git("tag v1");
    CHECK(ready, "cannot create the test repository (is git installed?)");
    if (!ready) return;

    write_app("git = \"../shapes\", tag = \"v1\"");
    char *first = load_shapes("git tag", "");
    CHECK(strstr(first, "build/git_test/app/build/deps/shapes-") != NULL, "cloned into build/deps: %s", first);
    char *lock = read_lock();
    CHECK(strncmp(lock, "# ", 2) == 0 && strstr(lock, "\nshapes ../shapes tag v1 "), "sosu.lock written:\n%s", lock);
    free(lock);

    /* The tag moves: the lock keeps the first commit. */
    write_text("build/git_test/shapes/src/lib.sosu", "pub i32 sides() {\n    return 4;\n}\n");
    CHECK(git("commit --quiet -am two") && git("tag -f v1") && git("tag v2"), "second commit");
    char *again = load_shapes("git tag, locked", "");
    CHECK(strcmp(first, again) == 0, "the lock is used: %s, then %s", first, again);

    /* Another tag: fetched, and the lock follows. */
    write_app("git = \"../shapes\", tag = \"v2\"");
    char *second = load_shapes("git tag changed", "");
    CHECK(second[0] && strcmp(first, second) != 0, "a new tag gives a new commit: %s", second);
    lock = read_lock();
    CHECK(strstr(lock, "\nshapes ../shapes tag v2 ") != NULL, "sosu.lock updated:\n%s", lock);
    free(lock);

    /* A rev: a commit, short or full. */
    const char *commit = strrchr(first, '-') + 1;
    char *source = sosu_format("git = \"../shapes\", rev = \"%.8s\"", commit);
    write_app(source);
    char *by_rev = load_shapes("git rev", "");
    CHECK(strcmp(by_rev, first) == 0, "rev %.8s: %s", commit, by_rev);

    write_app("git = \"../shapes\", tag = \"v9\"");
    free(load_shapes("missing tag", "E0405"));
    write_app("git = \"../shapes\", tag = \"v1 && echo\"");
    free(load_shapes("unsafe tag", "E0404"));
    write_app("git = \"../sh\\\"apes\", tag = \"v1\"");
    free(load_shapes("unsafe url", "E0404"));
    write_app("git = \"$(evil)\", tag = \"v1\"");
    free(load_shapes("unsafe url", "E0404"));

    free(source);
    free(by_rev);
    free(second);
    free(again);
    free(first);
}

int main(void) {
    test_paths();
    test_manifest();
    test_program();
    test_git();
    return test_summary("package tests");
}
