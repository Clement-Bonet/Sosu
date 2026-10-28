#include "manifest.h"

#include <stdlib.h>
#include <string.h>

typedef enum {
    SECTION_NONE,
    SECTION_PACKAGE,
    SECTION_DEPENDENCIES,
    SECTION_UNKNOWN,
    SECTION_LINK,
    SECTION_APP, /* [app]: what a phone shows of the program (spec §11.5) */
} Section;

const char *const link_section_names[LINK_SECTION_COUNT] = { "", "windows", "linux", "macos", "android", "ios",
                                                             "windows-x64", "windows-arm64", "linux-x64", "linux-arm64",
                                                             "macos-x64", "macos-arm64" };

typedef struct {
    const SourceFile *file;
    const char *s;
    size_t len;
    size_t pos;
    DiagList *diags;
    Manifest *manifest;
    Section section;
    int link;                /* SECTION_LINK: LINK_ALL, LINK_WINDOWS... */
    unsigned seen_sections;  /* bits, to report a section written twice */
    bool has_package;
    size_t package_offset; /* `[package]` header */
    size_t errors_before;
} Toml;

typedef struct {
    char *items;
    size_t count;
    size_t cap;
} CharVec;

bool is_identifier(const char *text) {
    if (!((text[0] >= 'a' && text[0] <= 'z') || (text[0] >= 'A' && text[0] <= 'Z') || text[0] == '_')) return false;
    for (const char *c = text + 1; *c; c++) {
        bool ok = (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == '_';
        if (!ok) return false;
    }
    return true;
}

static Diagnostic *error_at(Toml *t, const char *code, size_t offset, size_t length, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    Diagnostic *diag = diag_verror(t->diags, code, t->file, offset, length, fmt, args);
    va_end(args);
    return diag;
}

static char cur(const Toml *t) { return t->pos < t->len ? t->s[t->pos] : '\0'; }

static void skip_spaces(Toml *t) {
    while (cur(t) == ' ' || cur(t) == '\t') t->pos++;
}

static bool at_line_end(const Toml *t) {
    char c = cur(t);
    return t->pos >= t->len || c == '\n' || c == '\r' || c == '#';
}

static void skip_line(Toml *t) {
    while (t->pos < t->len && t->s[t->pos] != '\n') t->pos++;
    if (t->pos < t->len) t->pos++;
}

static bool is_key_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
}

static bool parse_key(Toml *t, size_t *start, size_t *length) {
    *start = t->pos;
    while (is_key_char(cur(t))) t->pos++;
    *length = t->pos - *start;
    if (*length > 0) return true;
    error_at(t, "E0401", t->pos, 1, "expected a key");
    return false;
}

static bool key_is(const Toml *t, size_t start, size_t length, const char *key) {
    return strlen(key) == length && memcmp(t->s + start, key, length) == 0;
}

/* Parses `"..."` at the current position. Returns a new string, or NULL after an error. */
static char *parse_string(Toml *t) {
    if (cur(t) != '"') {
        error_at(t, "E0401", t->pos, 1, "expected a string");
        return NULL;
    }
    size_t start = t->pos++;
    CharVec text = {0};
    for (;;) {
        if (t->pos >= t->len || cur(t) == '\n') {
            error_at(t, "E0401", start, 1, "unterminated string");
            free(text.items);
            return NULL;
        }
        char c = t->s[t->pos++];
        if (c == '"') break;
        if (c == '\\') {
            char e = cur(t);
            t->pos++;
            if (e == '"' || e == '\\') c = e;
            else if (e == 'n') c = '\n';
            else if (e == 't') c = '\t';
            else {
                Diagnostic *d = error_at(t, "E0401", t->pos - 2, 2, "unsupported escape sequence in sosu.toml");
                diag_help(d, "valid escapes are \\\" \\\\ \\n \\t");
                free(text.items);
                return NULL;
            }
        }
        SOSU_VEC_PUSH(&text, c);
    }
    SOSU_VEC_PUSH(&text, '\0');
    return text.items;
}

/* Stores a string value in `*slot`, reporting a duplicate key. */
static bool set_string(Toml *t, char **slot, size_t key_start, size_t key_length) {
    if (*slot) {
        error_at(t, "E0408", key_start, key_length, "duplicate key `%.*s`", (int)key_length, t->s + key_start);
        return false;
    }
    *slot = parse_string(t);
    return *slot != NULL;
}

static void parse_package_entry(Toml *t, size_t key_start, size_t key_length) {
    if (key_is(t, key_start, key_length, "name")) {
        t->manifest->name_offset = t->pos;
        if (set_string(t, &t->manifest->name, key_start, key_length)) {
            t->manifest->name_length = t->pos - t->manifest->name_offset;
        }
    } else if (key_is(t, key_start, key_length, "version")) {
        set_string(t, &t->manifest->version, key_start, key_length);
    } else {
        Diagnostic *d = error_at(t, "E0402", key_start, key_length, "unknown key `%.*s` in [package]",
                                 (int)key_length, t->s + key_start);
        diag_help(d, "[package] contains `name` and `version`");
    }
}

/* `name = { path = "..." }` or `name = { git = "...", tag = "..." }` */
static void parse_dependency(Toml *t, size_t key_start, size_t key_length) {
    for (size_t i = 0; i < t->manifest->deps.count; i++) {
        const Dependency *other = &t->manifest->deps.items[i];
        if (other->length == key_length && memcmp(t->s + other->offset, t->s + key_start, key_length) == 0) {
            error_at(t, "E0408", key_start, key_length, "duplicate dependency `%.*s`", (int)key_length, t->s + key_start);
            return;
        }
    }
    if (cur(t) != '{') {
        Diagnostic *d = error_at(t, "E0404", key_start, key_length, "dependency `%.*s` must be an inline table",
                                 (int)key_length, t->s + key_start);
        diag_help(d, "write `%.*s = { path = \"../%.*s\" }`", (int)key_length, t->s + key_start,
                  (int)key_length, t->s + key_start);
        return;
    }
    t->pos++;

    Dependency dep = {0};
    dep.name = sosu_format("%.*s", (int)key_length, t->s + key_start);
    dep.offset = key_start;
    dep.length = key_length;
    skip_spaces(t);
    bool ok = true;
    if (cur(t) == '}') {
        t->pos++;
    } else {
        for (;;) {
            size_t start, length;
            if (!parse_key(t, &start, &length)) { ok = false; break; }
            skip_spaces(t);
            if (cur(t) != '=') {
                error_at(t, "E0401", t->pos, 1, "expected `=` after `%.*s`", (int)length, t->s + start);
                ok = false;
                break;
            }
            t->pos++;
            skip_spaces(t);
            char **slot = key_is(t, start, length, "path") ? &dep.path
                        : key_is(t, start, length, "git")  ? &dep.git
                        : key_is(t, start, length, "tag")  ? &dep.tag
                        : key_is(t, start, length, "rev")  ? &dep.rev
                        : NULL;
            if (!slot) {
                Diagnostic *d = error_at(t, "E0402", start, length, "unknown key `%.*s` in dependency `%s`",
                                         (int)length, t->s + start, dep.name);
                diag_help(d, "a dependency has a `path`, or a `git` URL with a `tag` or a `rev`");
                ok = false;
                break;
            }
            if (!set_string(t, slot, start, length)) { ok = false; break; }
            skip_spaces(t);
            if (cur(t) == ',') {
                t->pos++;
                skip_spaces(t);
            } else if (cur(t) == '}') {
                t->pos++;
                break;
            } else {
                error_at(t, "E0401", t->pos, 1, "expected `,` or `}` in the inline table");
                ok = false;
                break;
            }
        }
    }
    if (ok) {
        SOSU_VEC_PUSH(&t->manifest->deps, dep);
    } else {
        free(dep.name);
        free(dep.path);
        free(dep.git);
        free(dep.tag);
        free(dep.rev);
    }
}

static void parse_header(Toml *t) {
    size_t start = t->pos++;
    skip_spaces(t);
    size_t key_start, key_length;
    if (!parse_key(t, &key_start, &key_length)) return;
    while (cur(t) == '.') { /* [link.windows] */
        t->pos++;
        size_t part_start, part_length;
        if (!parse_key(t, &part_start, &part_length)) return;
        key_length = t->pos - key_start;
    }
    skip_spaces(t);
    if (cur(t) != ']') {
        error_at(t, "E0401", t->pos, 1, "expected `]`");
        return;
    }
    t->pos++;
    Section section = SECTION_UNKNOWN;
    int link = -1;
    if (key_is(t, key_start, key_length, "package")) {
        section = SECTION_PACKAGE;
        t->has_package = true;
        t->package_offset = start;
    } else if (key_is(t, key_start, key_length, "dependencies")) {
        section = SECTION_DEPENDENCIES;
    } else if (key_is(t, key_start, key_length, "app")) {
        section = SECTION_APP;
    } else {
        for (int i = 0; i < LINK_SECTION_COUNT; i++) {
            char *name = i == LINK_ALL ? sosu_strdup("link") : sosu_format("link.%s", link_section_names[i]);
            if (key_is(t, key_start, key_length, name)) link = i;
            free(name);
        }
        if (link >= 0) section = SECTION_LINK;
    }
    t->section = section;
    t->link = link;
    if (section == SECTION_UNKNOWN) {
        Diagnostic *d = error_at(t, "E0402", key_start, key_length, "unknown section [%.*s]",
                                 (int)key_length, t->s + key_start);
        diag_help(d, "sosu.toml contains [package], [dependencies], [app], [link] and [link.<target>] "
                     "(windows, linux, macos, android, ios; windows-arm64, linux-x64...)");
        return;
    }
    int index = section == SECTION_LINK ? 3 + link : section == SECTION_APP ? 15 : (int)section; /* link sections after the others */
    unsigned bit = 1u << index;
    if (t->seen_sections & bit) {
        error_at(t, "E0408", key_start, key_length, "the section [%.*s] appears twice", (int)key_length,
                 t->s + key_start);
    }
    t->seen_sections |= bit;
}

/* `["a", "b"]` on one line, into `list`. */
static bool parse_string_array(Toml *t, StringList *list) {
    if (cur(t) != '[') {
        error_at(t, "E0401", t->pos, 1, "expected a list of strings, e.g. `[\"SDL3\"]`");
        return false;
    }
    t->pos++;
    skip_spaces(t);
    while (cur(t) != ']') {
        char *item = parse_string(t);
        if (!item) return false;
        SOSU_VEC_PUSH(list, item);
        skip_spaces(t);
        if (cur(t) == ',') {
            t->pos++;
            skip_spaces(t);
        } else if (cur(t) != ']') {
            error_at(t, "E0401", t->pos, 1, "expected `,` or `]` in the list");
            return false;
        }
    }
    t->pos++;
    return true;
}

static void parse_link_entry(Toml *t, size_t key_start, size_t key_length) {
    LinkSection *section = &t->manifest->link[t->link];
    if (key_is(t, key_start, key_length, "subsystem")) {
        if (section->subsystem) {
            error_at(t, "E0408", key_start, key_length, "duplicate key `subsystem`");
            return;
        }
        size_t value_start = t->pos;
        char *value = parse_string(t);
        if (!value) return;
        if (strcmp(value, "window") != 0 && strcmp(value, "console") != 0) {
            Diagnostic *d = error_at(t, "E0402", value_start, t->pos - value_start, "unknown subsystem `%s`", value);
            diag_help(d, "a program is a `\"window\"` program or a `\"console\"` one");
            free(value);
            return;
        }
        section->subsystem = value;
        return;
    }
    StringList *list = key_is(t, key_start, key_length, "libs")         ? &section->libs
                     : key_is(t, key_start, key_length, "paths")        ? &section->paths
                     : key_is(t, key_start, key_length, "frameworks")   ? &section->frameworks
                     : key_is(t, key_start, key_length, "copy")         ? &section->copy
                     : key_is(t, key_start, key_length, "c")            ? &section->c
                     : key_is(t, key_start, key_length, "include")      ? &section->include
                     : key_is(t, key_start, key_length, "defines")      ? &section->defines
                     : NULL;
    if (!list) {
        Diagnostic *d = error_at(t, "E0402", key_start, key_length, "unknown key `%.*s` in [%s]", (int)key_length,
                                 t->s + key_start, t->link == LINK_ALL ? "link" : "link.<target>");
        diag_help(d, "a link section contains `libs`, `paths`, `frameworks`, `copy`, `c`, `include`, `defines` and `subsystem`");
        return;
    }
    if (list->count > 0) {
        error_at(t, "E0408", key_start, key_length, "duplicate key `%.*s`", (int)key_length, t->s + key_start);
        return;
    }
    parse_string_array(t, list);
}

/* `id = "org.sosu.window"`, `name`, `version`, `icon`, `assets = [...]` in [app]. */
static void parse_app_entry(Toml *t, size_t key_start, size_t key_length) {
    AppSection *app = &t->manifest->app;
    if (key_is(t, key_start, key_length, "assets")) {
        if (app->assets.count > 0) {
            error_at(t, "E0408", key_start, key_length, "duplicate key `assets`");
            return;
        }
        parse_string_array(t, &app->assets);
        return;
    }
    char **field = key_is(t, key_start, key_length, "id")        ? &app->id
                 : key_is(t, key_start, key_length, "name")    ? &app->name
                 : key_is(t, key_start, key_length, "version") ? &app->version
                 : key_is(t, key_start, key_length, "icon")    ? &app->icon
                 : NULL;
    if (!field) {
        Diagnostic *d = error_at(t, "E0402", key_start, key_length, "unknown key `%.*s` in [app]", (int)key_length,
                                 t->s + key_start);
        diag_help(d, "[app] contains `id`, `name`, `version`, `icon` and `assets` (spec §11.5)");
        return;
    }
    if (*field) {
        error_at(t, "E0408", key_start, key_length, "duplicate key `%.*s`", (int)key_length, t->s + key_start);
        return;
    }
    *field = parse_string(t);
}

static void parse_line(Toml *t) {
    skip_spaces(t);
    if (at_line_end(t)) return;
    if (cur(t) == '[') {
        parse_header(t);
        return;
    }
    size_t key_start, key_length;
    if (!parse_key(t, &key_start, &key_length)) return;
    skip_spaces(t);
    if (cur(t) != '=') {
        error_at(t, "E0401", t->pos, 1, "expected `=` after `%.*s`", (int)key_length, t->s + key_start);
        return;
    }
    t->pos++;
    skip_spaces(t);

    switch (t->section) {
    case SECTION_NONE: {
        Diagnostic *d = error_at(t, "E0401", key_start, key_length, "key outside of a section");
        diag_help(d, "start the file with `[package]`");
        return;
    }
    case SECTION_PACKAGE: parse_package_entry(t, key_start, key_length); break;
    case SECTION_LINK: parse_link_entry(t, key_start, key_length); break;
    case SECTION_APP: parse_app_entry(t, key_start, key_length); break;
    case SECTION_DEPENDENCIES: parse_dependency(t, key_start, key_length); break;
    case SECTION_UNKNOWN: /* already reported: ignore its content */
        while (t->pos < t->len && t->s[t->pos] != '\n') t->pos++;
        break;
    }
}

static void validate(Toml *t) {
    Manifest *m = t->manifest;
    if (!t->has_package) {
        Diagnostic *d = error_at(t, "E0403", 0, 0, "missing [package] section");
        diag_help(d, "start the file with `[package]`, `name = \"...\"` and `version = \"...\"`");
        return;
    }
    if (!m->name) {
        error_at(t, "E0403", t->package_offset, 9, "missing `name` in [package]");
    } else if (!is_identifier(m->name)) {
        Diagnostic *d = error_at(t, "E0409", m->name_offset, m->name_length, "`%s` is not a valid package name", m->name);
        diag_help(d, "package names are used by `import`: letters, digits and `_`, not starting with a digit");
    }
    if (!m->version) error_at(t, "E0403", t->package_offset, 9, "missing `version` in [package]");

    for (size_t i = 0; i < m->deps.count; i++) {
        const Dependency *dep = &m->deps.items[i];
        const char *problem = NULL;
        const char *help = NULL;
        if (!is_identifier(dep->name)) {
            error_at(t, "E0409", dep->offset, dep->length, "`%s` is not a valid package name", dep->name);
            continue;
        }
        if (!dep->path && !dep->git) {
            problem = "needs a `path` or a `git` source";
            help = "e.g. `{ path = \"../shared\" }`";
        } else if (dep->path && dep->git) {
            problem = "cannot have both a `path` and a `git` source";
        } else if (dep->path && (dep->tag || dep->rev)) {
            problem = "uses `tag` or `rev` without `git`";
            help = "`tag` and `rev` select the version of a git dependency";
        } else if (dep->git && !dep->tag && !dep->rev) {
            problem = "needs a `tag` or a `rev`";
            help = "the version of a git dependency is always pinned";
        } else if (dep->tag && dep->rev) {
            problem = "cannot have both a `tag` and a `rev`";
        }
        if (problem) {
            Diagnostic *d = error_at(t, "E0404", dep->offset, dep->length, "dependency `%s` %s", dep->name, problem);
            if (help) diag_help(d, "%s", help);
        }
    }
}

bool manifest_parse(Manifest *manifest, const SourceFile *file, DiagList *diags) {
    memset(manifest, 0, sizeof *manifest);
    Toml t = {0};
    t.file = file;
    t.s = file->text;
    t.len = file->length;
    t.diags = diags;
    t.manifest = manifest;
    t.errors_before = diags->error_count;
    if (t.len >= 3 && memcmp(t.s, "\xEF\xBB\xBF", 3) == 0) t.pos = 3;

    while (t.pos < t.len) {
        size_t errors = diags->error_count;
        parse_line(&t);
        if (diags->error_count == errors) {
            skip_spaces(&t);
            if (!at_line_end(&t)) error_at(&t, "E0401", t.pos, 1, "expected the end of the line");
        }
        skip_line(&t);
    }
    validate(&t);
    return diags->error_count == t.errors_before;
}

void manifest_free(Manifest *manifest) {
    free(manifest->name);
    free(manifest->version);
    for (size_t i = 0; i < manifest->deps.count; i++) {
        Dependency *dep = &manifest->deps.items[i];
        free(dep->name);
        free(dep->path);
        free(dep->git);
        free(dep->tag);
        free(dep->rev);
    }
    free(manifest->deps.items);
    for (int i = 0; i < LINK_SECTION_COUNT; i++) {
        StringList *lists[] = { &manifest->link[i].libs, &manifest->link[i].paths, &manifest->link[i].frameworks,
                                &manifest->link[i].copy, &manifest->link[i].c, &manifest->link[i].include,
                                &manifest->link[i].defines };
        for (size_t j = 0; j < sizeof lists / sizeof *lists; j++) {
            for (size_t k = 0; k < lists[j]->count; k++) free(lists[j]->items[k]);
            free(lists[j]->items);
        }
        free(manifest->link[i].subsystem);
    }
    free(manifest->app.id);
    free(manifest->app.name);
    free(manifest->app.version);
    free(manifest->app.icon);
    for (size_t i = 0; i < manifest->app.assets.count; i++) free(manifest->app.assets.items[i]);
    free(manifest->app.assets.items);
    memset(manifest, 0, sizeof *manifest);
}
