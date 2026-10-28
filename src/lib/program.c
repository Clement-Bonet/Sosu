#include "program.h"

#include <stdlib.h>
#include <string.h>

#include "path.h"
#include "shell.h"

static Diagnostic *report(Program *program, const char *code, const SourceFile *file, size_t offset, size_t length,
                          const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    Diagnostic *diag = diag_verror(&program->diags, code, file, offset, length, fmt, args);
    va_end(args);
    return diag;
}

static bool is_builtin_name(const char *name) {
    return strcmp(name, "core") == 0 || strcmp(name, "std") == 0;
}

static char *normalized_join(const char *base, const char *relative) {
    char *joined = path_join(base, relative);
    char *result = path_normalize(joined);
    free(joined);
    return result;
}

/* ---- Packages ---- */

static Package *find_package(const Program *program, const char *root) {
    for (size_t i = 0; i < program->packages.count; i++) {
        if (strcmp(program->packages.items[i]->root, root) == 0) return program->packages.items[i];
    }
    return NULL;
}

static Package *load_package(Program *program, const char *root, bool builtin, bool *missing);

/* The folder of a git dependency, cloned at the commit of sosu.lock if it has one; NULL
   after reporting a failure. */
static char *fetch_git(Program *program, Package *package, const Dependency *dep) {
    const SourceFile *file = &package->manifest_file;
    const char *kind = dep->tag ? "tag" : "rev";
    const char *ref = dep->tag ? dep->tag : dep->rev;
    if (!ref) return NULL; /* invalid, already reported */
    const char *problem = git_check_source(dep->git, ref);
    if (problem) {
        Diagnostic *d = report(program, "E0404", file, dep->offset, dep->length, "dependency `%s`: %s", dep->name, problem);
        diag_help(d, "URLs, tags and revs may use letters, digits and `-._~:/@+=,`");
        return NULL;
    }
    if (!program->deps_dir) {
        report(program, "E0405", file, dep->offset, dep->length, "git dependencies need a package folder");
        return NULL;
    }
    /* A local URL is relative to the package, like a `path`. */
    char *url = git_is_local(dep->git) ? normalized_join(package->root, dep->git) : sosu_strdup(dep->git);
    const LockEntry *locked = lock_find(&program->lock, dep->name, dep->git, kind, ref);
    char *commit;
    char *error;
    char *root = git_fetch(program->deps_dir, dep->name, url, kind, ref, locked ? locked->commit : NULL, &commit, &error);
    if (root) {
        lock_set(&program->used, dep->name, dep->git, kind, ref, commit);
    } else {
        Diagnostic *d = report(program, "E0405", file, dep->offset, dep->length, "cannot fetch the dependency `%s`",
                               dep->name);
        diag_help(d, "%s", error);
    }
    free(url);
    free(commit);
    free(error);
    return root;
}

/* Loads the package a dependency points to, and checks it. */
static Package *load_dependency(Program *program, Package *package, const Dependency *dep) {
    const SourceFile *file = &package->manifest_file;
    char *root;
    if (dep->git) {
        root = fetch_git(program, package, dep);
        if (!root) return NULL;
    } else if (dep->path) {
        root = normalized_join(package->root, dep->path);
    } else {
        return NULL; /* invalid, already reported */
    }
    const char *source = dep->git ? dep->git : dep->path;

    Package *child = find_package(program, root);
    if (child && child->state == PACKAGE_LOADING) {
        Diagnostic *d = report(program, "E0407", file, dep->offset, dep->length,
                               "circular dependency between `%s` and `%s`", package->name, child->name);
        diag_help(d, "packages cannot depend on each other: move the shared code into a third package");
        free(root);
        return NULL;
    }
    if (!child) {
        bool missing;
        child = load_package(program, root, false, &missing);
        if (missing) {
            Diagnostic *d = report(program, "E0415", file, dep->offset, dep->length, "no package found for `%s`", dep->name);
            diag_help(d, "expected the file `%s/sosu.toml`", root);
        }
    }
    free(root);
    if (!child || !child->manifest.name) return child;

    if (strcmp(child->name, dep->name) != 0) {
        Diagnostic *d = report(program, "E0406", file, dep->offset, dep->length,
                               "the package at `%s` is named `%s`, not `%s`", source, child->name, dep->name);
        diag_help(d, "the key is the name used by `import`: write `%s = { %s = \"%s\"%s }`", child->name,
                  dep->git ? "git" : "path", source, dep->git ? ", ..." : "");
        return NULL;
    }
    for (size_t i = 0; i < program->packages.count; i++) {
        const Package *other = program->packages.items[i];
        if (other != child && strcmp(other->name, child->name) == 0) {
            Diagnostic *d = report(program, "E0413", file, dep->offset, dep->length,
                                   "two different packages are named `%s`", child->name);
            diag_help(d, "one is in `%s`, the other in `%s`", other->root, child->root);
            return NULL;
        }
    }
    return child;
}

/* Loads `root/sosu.toml` and, recursively, the dependencies. Sets *missing and returns
   NULL when there is no manifest. */
static Package *load_package(Program *program, const char *root, bool builtin, bool *missing) {
    *missing = false;
    Package *package = sosu_alloc(sizeof *package);
    memset(package, 0, sizeof *package);
    char *manifest_path = path_join(root, "sosu.toml");
    bool loaded = source_load(&package->manifest_file, manifest_path);
    free(manifest_path);
    if (!loaded) {
        free(package);
        *missing = true;
        return NULL;
    }
    package->root = sosu_strdup(root);
    package->builtin = builtin;
    manifest_parse(&package->manifest, &package->manifest_file, &program->diags);
    package->name = sosu_strdup(package->manifest.name ? package->manifest.name : "");
    SOSU_VEC_PUSH(&program->packages, package);

    if (!builtin && is_builtin_name(package->name)) {
        Diagnostic *d = report(program, "E0409", &package->manifest_file, package->manifest.name_offset,
                               package->manifest.name_length, "the package name `%s` is reserved", package->name);
        diag_help(d, "`core` and `std` are the packages of the standard library");
    }

    size_t count = package->manifest.deps.count;
    package->deps = sosu_alloc(count * sizeof *package->deps);
    package->state = PACKAGE_LOADING;
    for (size_t i = 0; i < count; i++) {
        package->deps[i] = load_dependency(program, package, &package->manifest.deps.items[i]);
    }
    package->state = PACKAGE_LOADED;
    return package;
}

/* ---- Modules ---- */

static Unit *find_unit(const Program *program, const char *path) {
    for (size_t i = 0; i < program->units.count; i++) {
        if (strcmp(program->units.items[i]->file.path, path) == 0) return program->units.items[i];
    }
    return NULL;
}

bool program_is_ui_file(const char *path) {
    size_t length = strlen(path);
    return length >= 8 && strcmp(path + length - 8, ".ui.sosu") == 0;
}

bool program_is_locale_file(const char *path) {
    const char *name = path;
    for (const char *c = path; *c; c++) {
        if (*c == '/' || *c == '\\') name = c + 1;
    }
    size_t length = strlen(name);
    return length >= 5 && strcmp(name + length - 5, ".sosu") == 0 && strstr(name, ".locale.") != NULL;
}

/* Lexes and parses a unit whose file is loaded, and adds it to the program. */
static void finish_unit(Program *program, Unit *unit, Package *package, char *module) {
    unit->index = program->units.count;
    unit->module = module;
    unit->package = package;
    lex_file(&unit->file, &unit->tokens, &program->diags);
    if (program_is_ui_file(unit->file.path)) {
        unit->ast = parse_ui_module(&unit->file, &unit->tokens, &program->arena, &program->diags);
    } else {
        unit->ast = parse_module(&unit->file, &unit->tokens, &program->arena, &program->diags);
    }
    SOSU_VEC_PUSH(&program->units, unit);
}

/* Reads, lexes and parses a module. Returns NULL if the file cannot be read. */
static Unit *load_unit(Program *program, Package *package, char *module, const char *path) {
    Unit *unit = sosu_alloc(sizeof *unit);
    memset(unit, 0, sizeof *unit);
    if (!source_load(&unit->file, path)) {
        free(unit);
        free(module);
        return NULL;
    }
    finish_unit(program, unit, package, module);
    return unit;
}

/* Joins path.items[from..] as text, with `separator` between names. */
static char *join_names(Path path, size_t from, const char *prefix, char separator, const char *suffix) {
    size_t size = strlen(prefix) + strlen(suffix) + 1;
    for (size_t i = from; i < path.count; i++) size += path.items[i].length + 1;
    char *result = sosu_alloc(size);
    char *out = result;
    memcpy(out, prefix, strlen(prefix));
    out += strlen(prefix);
    for (size_t i = from; i < path.count; i++) {
        if (i > from) *out++ = separator;
        memcpy(out, path.items[i].text, path.items[i].length);
        out += path.items[i].length;
    }
    memcpy(out, suffix, strlen(suffix) + 1); /* with its NUL: counted in `size` */
    return result;
}

/* The package an import refers to: the current one, a dependency, or `core` / `std`.
   Reports unknown packages; returns NULL when it cannot be used. */
static Package *import_target(Program *program, Unit *unit, const char *name, size_t offset, size_t length) {
    Package *package = unit->package;
    if (strcmp(name, package->name) == 0) return package;
    for (size_t i = 0; i < package->manifest.deps.count; i++) {
        if (strcmp(package->manifest.deps.items[i].name, name) == 0) return package->deps[i];
    }
    if (is_builtin_name(name)) {
        char *root = normalized_join(program->lib_dir, name);
        Package *builtin = find_package(program, root);
        bool missing = false;
        if (!builtin) builtin = load_package(program, root, true, &missing);
        if (missing) {
            Diagnostic *d = report(program, "E0414", &unit->file, offset, length, "the standard library was not found");
            diag_help(d, "expected the file `%s/sosu.toml`; set SOSU_HOME to the folder of the Sosu installation", root);
        }
        free(root);
        return builtin;
    }
    Diagnostic *d = report(program, "E0410", &unit->file, offset, length, "unknown package `%s`", name);
    diag_help(d, "declare it in the [dependencies] of `%s/sosu.toml`", package->root);
    return NULL;
}

static void resolve_imports(Program *program, Unit *unit) {
    struct { ImportLink *items; size_t count; size_t cap; } links = {0};
    for (size_t i = 0; i < unit->ast->count; i++) {
        const Decl *decl = unit->ast->decls[i];
        if (decl->kind != DECL_IMPORT) continue;
        ImportLink link = { decl, NULL };
        Path path = decl->as.import;
        if (path.count == 0 || path.items[0].length == 0) { /* parse error, already reported */
            SOSU_VEC_PUSH(&links, link);
            continue;
        }
        Name first = path.items[0];
        Name last = path.items[path.count - 1];
        size_t offset = first.offset;
        size_t length = last.offset + last.length - first.offset;
        char *name = sosu_format("%.*s", (int)first.length, first.text);

        Package *target = import_target(program, unit, name, offset, length);
        if (target && path.count < 2) {
            Diagnostic *d = report(program, "E0412", &unit->file, offset, length,
                                   "`import %s;` does not name a module", name);
            diag_help(d, "import a module of the package: `import %s.<module>;`", name);
        } else if (target) {
            /* `src/a/b.sosu`, or else an interface file `src/a/b.ui.sosu` (spec §10.2) */
            char *module = join_names(path, 0, "", '.', "");
            char *relative = join_names(path, 1, "src/", '/', ".sosu");
            char *ui_relative = join_names(path, 1, "src/", '/', ".ui.sosu");
            char *file = normalized_join(target->root, relative);
            char *ui_file = normalized_join(target->root, ui_relative);
            Unit *found = find_unit(program, file);
            if (!found) found = find_unit(program, ui_file);
            if (!found) found = load_unit(program, target, sosu_strdup(module), file);
            if (!found) found = load_unit(program, target, sosu_strdup(module), ui_file);
            if (!found) {
                Diagnostic *d = report(program, "E0411", &unit->file, offset, length, "module `%s` not found", module);
                diag_help(d, "expected the file `%s` (or `%s`)", file, ui_file);
            }
            link.unit = found;
            free(module);
            free(relative);
            free(ui_relative);
            free(file);
            free(ui_file);
        }
        free(name);
        SOSU_VEC_PUSH(&links, link);
    }
    unit->imports = links.items;
    unit->import_count = links.count;
}

/* ---- Program ---- */

/* `core.prelude`, loaded with every program (spec §5.3); nothing if the library is absent
   (an import of `core` or `std` then reports it). */
static void load_prelude(Program *program) {
    char *root = normalized_join(program->lib_dir, "core");
    Package *core = find_package(program, root);
    bool missing = false;
    if (!core) core = load_package(program, root, true, &missing);
    if (core) {
        char *file = normalized_join(core->root, "src/prelude.sosu");
        if (!find_unit(program, file)) load_unit(program, core, sosu_strdup("core.prelude"), file);
        free(file);
    }
    free(root);
}

bool program_load(Program *program, const char *dir, const char *lib_dir) {
    memset(program, 0, sizeof *program);
    program->lib_dir = path_normalize(lib_dir);
    char *root = path_normalize(dir);
    char *lock_path = path_join(root, "sosu.lock");
    program->deps_dir = path_join(root, "build/deps");
    lock_read(&program->lock, lock_path);

    bool missing;
    Package *package = load_package(program, root, false, &missing);
    if (!package) program->fatal = sosu_format("no sosu.toml in `%s`", root);
    free(root);
    /* The lock follows the git dependencies, unless a dependency has an error. */
    if (package && program->diags.error_count == 0 && !lock_equal(&program->lock, &program->used) &&
        (program->used.count || shell_file_exists(lock_path)) && !lock_write(&program->used, lock_path)) {
        program->fatal = sosu_format("cannot write `%s`", lock_path);
    }
    free(lock_path);
    if (program->fatal) return false;

    const char *entries[] = { "main", "lib" };
    Unit *entry = NULL;
    for (size_t i = 0; i < 2 && !entry; i++) {
        char *relative = sosu_format("src/%s.sosu", entries[i]);
        char *file = normalized_join(package->root, relative);
        entry = load_unit(program, package, sosu_format("%s.%s", package->name, entries[i]), file);
        free(relative);
        free(file);
    }
    if (!entry) {
        program->fatal = sosu_format("`%s` has neither `src/main.sosu` (an application) nor `src/lib.sosu` (a library)",
                                     package->root);
        return false;
    }

    load_prelude(program);
    /* Resolving imports loads new units, which are resolved in turn. */
    for (size_t i = 0; i < program->units.count; i++) resolve_imports(program, program->units.items[i]);
    return program->diags.error_count == 0;
}

bool program_load_text(Program *program, const char *text, const char *lib_dir) {
    memset(program, 0, sizeof *program);
    program->lib_dir = path_normalize(lib_dir);

    Package *package = sosu_alloc(sizeof *package);
    memset(package, 0, sizeof *package);
    package->name = sosu_strdup("test");
    package->root = sosu_strdup("<memory>");
    package->deps = sosu_alloc(0);
    package->state = PACKAGE_LOADED;
    SOSU_VEC_PUSH(&program->packages, package);

    Unit *unit = sosu_alloc(sizeof *unit);
    memset(unit, 0, sizeof *unit);
    source_from_string(&unit->file, "test.sosu", text);
    finish_unit(program, unit, package, sosu_strdup("test.main"));
    load_prelude(program);
    for (size_t i = 0; i < program->units.count; i++) resolve_imports(program, program->units.items[i]);
    return program->diags.error_count == 0;
}

void program_free(Program *program) {
    diag_free(&program->diags);
    for (size_t i = 0; i < program->units.count; i++) {
        Unit *unit = program->units.items[i];
        free(unit->module);
        free(unit->imports);
        token_list_free(&unit->tokens);
        source_free(&unit->file);
        free(unit);
    }
    for (size_t i = 0; i < program->packages.count; i++) {
        Package *package = program->packages.items[i];
        free(package->name);
        free(package->root);
        free(package->deps);
        manifest_free(&package->manifest);
        source_free(&package->manifest_file);
        free(package);
    }
    free(program->units.items);
    free(program->packages.items);
    arena_free(&program->arena);
    free(program->lib_dir);
    free(program->fatal);
    free(program->deps_dir);
    lock_free(&program->lock);
    lock_free(&program->used);
    memset(program, 0, sizeof *program);
}
