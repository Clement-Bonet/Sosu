/* docgen: the reference of the public API of `core` and `std` (docs/REFERENCE.md), made from
   the sources: what is `pub`, with the `//` comments written above it. Run by `make doc`.
   The sources are in the canonical form of `sosuc fmt` (spec 7.6): a declaration starts at
   column 0, a member at column 4, and a body ends with `}` at the column of its start.
   Usage: docgen OUT.md FILE.sosu... (each FILE in lib/<package>/src/) */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char *text; /* the file, its line ends replaced by NUL */
    char **items;
    size_t count;
} Lines;

static void *checked_alloc(size_t size) {
    void *p = malloc(size ? size : 1);
    if (!p) {
        fprintf(stderr, "docgen: out of memory\n");
        exit(1);
    }
    return p;
}

static Lines read_lines(const char *path) {
    Lines lines = {0};
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "docgen: cannot read `%s`\n", path);
        exit(1);
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    rewind(f);
    lines.text = checked_alloc((size_t)size + 1);
    size_t read = fread(lines.text, 1, (size_t)size, f);
    fclose(f);
    lines.text[read] = '\0';
    size_t cap = 1;
    for (size_t i = 0; i < read; i++) cap += lines.text[i] == '\n';
    lines.items = checked_alloc(cap * sizeof *lines.items);
    char *start = lines.text;
    for (char *c = lines.text;; c++) {
        if (*c == '\n' || *c == '\0') {
            bool last = *c == '\0';
            if (c > start && c[-1] == '\r') c[-1] = '\0';
            *c = '\0';
            if (!last || c > start) lines.items[lines.count++] = start;
            if (last) break;
            start = c + 1;
        }
    }
    return lines;
}

static size_t indent_of(const char *s) {
    size_t n = 0;
    while (s[n] == ' ') n++;
    return n;
}

static bool is_comment(const char *s) { return strncmp(s + indent_of(s), "//", 2) == 0; }

static const char *comment_text(const char *s) {
    s += indent_of(s) + 2;
    return *s == ' ' ? s + 1 : s;
}

/* The code of a line, without its indentation, its trailing comment and its trailing spaces
   (a `//` in a string stays); `*comment` is the text of the trailing comment, or NULL. */
static char *code_of(const char *s, const char **comment) {
    s += indent_of(s);
    size_t end = strlen(s);
    bool in_string = false;
    if (comment) *comment = NULL;
    for (size_t i = 0; s[i]; i++) {
        if (s[i] == '"' && (i == 0 || s[i - 1] != '\\')) {
            in_string = !in_string;
        } else if (!in_string && s[i] == '/' && s[i + 1] == '/') {
            end = i;
            if (comment) *comment = comment_text(s + i);
            break;
        }
    }
    while (end > 0 && s[end - 1] == ' ') end--;
    char *code = checked_alloc(end + 1);
    memcpy(code, s, end);
    code[end] = '\0';
    return code;
}

static bool ends_with(const char *s, char c) {
    size_t n = strlen(s);
    return n > 0 && s[n - 1] == c;
}

static bool starts_with(const char *s, const char *prefix) { return strncmp(s, prefix, strlen(prefix)) == 0; }

/* The comment lines above a declaration, as one paragraph. */
typedef struct {
    char text[4096];
} Doc;

static void doc_add(Doc *doc, const char *line) {
    size_t used = strlen(doc->text);
    if (used + strlen(line) + 2 >= sizeof doc->text) return;
    if (used) strcat(doc->text, " ");
    strcat(doc->text, line);
}

/* A declaration that may go on over several lines (`requires`, a `{` alone): its lines joined,
   up to the one ending in `{`, `;` or `}`. `*i` ends on that line. `*body` tells whether a body
   follows. */
static char *signature(const Lines *lines, size_t *i, bool *body) {
    char *sig = code_of(lines->items[*i], NULL);
    while (!ends_with(sig, '{') && !ends_with(sig, ';') && !ends_with(sig, '}') && *i + 1 < lines->count) {
        char *next = code_of(lines->items[++*i], NULL);
        char *joined = checked_alloc(strlen(sig) + strlen(next) + 2);
        sprintf(joined, "%s %s", sig, next);
        free(sig);
        free(next);
        sig = joined;
    }
    *body = ends_with(sig, '{');
    size_t n = strlen(sig);
    if (*body || ends_with(sig, ';')) n--;
    while (n > 0 && sig[n - 1] == ' ') n--;
    sig[n] = '\0';
    return sig;
}

/* Moves `*i` to the `}` that closes the body opened at column `column`. */
static void skip_body(const Lines *lines, size_t *i, size_t column) {
    while (*i + 1 < lines->count) {
        const char *line = lines->items[++*i];
        if (indent_of(line) == column && line[column] == '}') return;
    }
}

static const char *type_keyword(const char *sig) {
    static const char *const keywords[] = { "struct ", "enum ", "class ", "interface " };
    const char *rest = sig + strlen("pub ");
    for (size_t k = 0; k < sizeof keywords / sizeof *keywords; k++) {
        if (starts_with(rest, keywords[k])) return keywords[k];
    }
    return NULL;
}

/* The name to show in a heading: the type after its keyword, the function before its
   parameters, or the whole declaration (a constant). */
static void name_of(const char *sig, const char *keyword, char *name, size_t size) {
    const char *start, *end;
    if (keyword) {
        start = sig + strlen("pub ") + strlen(keyword);
        end = start;
        int depth = 0;
        while (*end && (depth > 0 || (*end != ' ' && *end != '{' && *end != ':'))) {
            depth += *end == '<' ? 1 : *end == '>' ? -1 : 0;
            end++;
        }
    } else {
        end = strchr(sig, '(');
        if (!end) end = strstr(sig, " =");
        if (!end) {
            snprintf(name, size, "%s", sig + strlen("pub "));
            return;
        }
        if (end > sig && end[-1] == '>') {
            int depth = 0;
            do {
                end--;
                depth += *end == '>' ? 1 : *end == '<' ? -1 : 0;
            } while (end > sig && depth > 0);
        }
        start = end;
        while (start > sig && start[-1] != ' ' && start[-1] != '*' && start[-1] != '?' && start[-1] != '>') start--;
    }
    snprintf(name, size, "%.*s", (int)(end - start), start);
}

/* `std.io` from `lib/std/src/io.sosu`. */
static void module_of(const char *path, char *module, size_t size) {
    char copy[1024];
    snprintf(copy, sizeof copy, "%s", path);
    for (char *c = copy; *c; c++) {
        if (*c == '\\') *c = '/';
    }
    char *file = strrchr(copy, '/');
    file = file ? file + 1 : copy;
    char *dot = strrchr(file, '.');
    if (dot) *dot = '\0';
    char *src = strstr(copy, "/src/");
    const char *package = "?";
    if (src) {
        *src = '\0';
        char *slash = strrchr(copy, '/');
        package = slash ? slash + 1 : copy;
    }
    snprintf(module, size, "%s.%s", package, file);
}

/* The comment at the top of the file, without its `module: ` prefix. */
static void header_of(const Lines *lines, const char *module, Doc *doc) {
    doc->text[0] = '\0';
    for (size_t i = 0; i < lines->count && is_comment(lines->items[i]); i++) doc_add(doc, comment_text(lines->items[i]));
    size_t prefix = strlen(module);
    if (strncmp(doc->text, module, prefix) == 0 && doc->text[prefix] == ':') {
        memmove(doc->text, doc->text + prefix + 2, strlen(doc->text + prefix + 2) + 1);
    }
    if (doc->text[0] >= 'a' && doc->text[0] <= 'z') doc->text[0] = (char)(doc->text[0] - 'a' + 'A');
}

/* The members of a public type: enum variants in a block of code, `pub` members (and the
   methods of an interface) in a list. `*i` is on the line that opens the body. */
static void write_members(FILE *out, const Lines *lines, size_t *i, const char *keyword, const char *sig) {
    bool is_enum = strcmp(keyword, "enum ") == 0;
    bool is_interface = strcmp(keyword, "interface ") == 0;
    bool open_block = false;
    Doc doc = {{0}};
    if (is_enum) {
        fprintf(out, "```sosu\n%s {\n", sig);
        open_block = true;
    }
    while (*i + 1 < lines->count) {
        const char *line = lines->items[++*i];
        if (indent_of(line) == 0 && line[0] == '}') break;
        if (line[0] == '\0') {
            doc.text[0] = '\0';
            continue;
        }
        if (indent_of(line) != 4) continue;
        if (is_comment(line)) {
            doc_add(&doc, comment_text(line));
            continue;
        }
        const char *comment;
        char *code = code_of(line, &comment);
        bool is_pub = starts_with(code, "pub ");
        if (is_enum && !is_pub && !ends_with(code, '{')) {
            fprintf(out, "    %s%s%s\n", code, comment ? "  // " : "", comment ? comment : "");
        } else if (is_pub || (is_interface && ends_with(code, ';'))) {
            bool body;
            char *member = signature(lines, i, &body);
            if (open_block) {
                fprintf(out, "}\n```\n\n");
                open_block = false;
            }
            if (comment) doc_add(&doc, comment);
            fprintf(out, "- `%s`%s%s\n", member, doc.text[0] ? "  \n  " : "", doc.text);
            if (body) skip_body(lines, i, 4);
            free(member);
        } else if (ends_with(code, '{')) {
            skip_body(lines, i, 4);
        }
        free(code);
        doc.text[0] = '\0';
    }
    if (open_block) fprintf(out, "}\n```\n");
    fprintf(out, "\n");
}

static void write_module(FILE *out, const char *path) {
    Lines lines = read_lines(path);
    char module[256];
    Doc doc;
    module_of(path, module, sizeof module);
    header_of(&lines, module, &doc);
    fprintf(out, "## %s\n\n", module);
    if (doc.text[0]) fprintf(out, "%s\n\n", doc.text);
    doc.text[0] = '\0';
    size_t i = 0;
    while (i < lines.count && is_comment(lines.items[i])) i++;
    for (; i < lines.count; i++) {
        const char *line = lines.items[i];
        if (line[0] == '\0' || line[0] == ' ') {
            if (line[0] == '\0') doc.text[0] = '\0';
            continue;
        }
        if (is_comment(line)) {
            doc_add(&doc, comment_text(line));
            continue;
        }
        if (!starts_with(line, "pub ")) {
            const char *comment;
            char *code = code_of(line, &comment);
            if (ends_with(code, '{')) skip_body(&lines, &i, 0);
            free(code);
            doc.text[0] = '\0';
            continue;
        }
        bool body;
        char *sig = signature(&lines, &i, &body);
        const char *keyword = type_keyword(sig);
        char name[256];
        name_of(sig, keyword, name, sizeof name);
        if (keyword) {
            fprintf(out, "### %.*s `%s`\n\n", (int)strlen(keyword) - 1, keyword, name);
        } else {
            fprintf(out, "### `%s`\n\n", name);
        }
        if (doc.text[0]) fprintf(out, "%s\n\n", doc.text);
        if (keyword && body) {
            if (strcmp(keyword, "enum ") != 0) fprintf(out, "```sosu\n%s\n```\n\n", sig);
            write_members(out, &lines, &i, keyword, sig);
        } else {
            fprintf(out, "```sosu\n%s\n```\n\n", sig);
            if (body) skip_body(&lines, &i, 0);
        }
        free(sig);
        doc.text[0] = '\0';
    }
    free(lines.items);
    free(lines.text);
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: docgen OUT.md FILE.sosu...\n");
        return 2;
    }
    FILE *out = fopen(argv[1], "wb"); /* "\n" line ends on every system */
    if (!out) {
        fprintf(stderr, "docgen: cannot write `%s`\n", argv[1]);
        return 1;
    }
    fprintf(out, "# Sosu library reference\n\n"
                 "The public API of the `core` and `std` packages, and of the packages of spec 8.3: what is `pub`,\n"
                 "with the comments written above it. Generated by `make doc` (src/docgen) from lib/ and packages/;\n"
                 "do not edit. The language itself is described in [SPEC.md](SPEC.md).\n\n"
                 "| Module | Content |\n|---|---|\n");
    for (int f = 2; f < argc; f++) {
        Lines lines = read_lines(argv[f]);
        char module[256], anchor[256];
        Doc doc;
        module_of(argv[f], module, sizeof module);
        header_of(&lines, module, &doc);
        char *stop = strstr(doc.text, ". ");
        if (stop) *stop = '\0';
        size_t a = 0;
        for (const char *c = module; *c && a + 1 < sizeof anchor; c++) {
            if (*c != '.') anchor[a++] = *c;
        }
        anchor[a] = '\0';
        fprintf(out, "| [%s](#%s) | %s |\n", module, anchor, doc.text);
        free(lines.items);
        free(lines.text);
    }
    fprintf(out, "\n");
    for (int f = 2; f < argc; f++) write_module(out, argv[f]);
    fclose(out);
    return 0;
}
