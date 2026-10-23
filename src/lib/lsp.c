#if !defined(_WIN32) && !defined(_XOPEN_SOURCE)
#define _XOPEN_SOURCE 700 /* realpath */
#endif

#include "lsp.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "json.h"
#include "lexer.h"
#include "path.h"
#include "sema.h"
#include "shell.h"
#include "utf8.h"

/* The text of open documents replaces their file on disk (source_set_overlay). After each
   change, the package of the document is loaded and checked again, like `sosuc check`, and
   its diagnostics are published. The last analysis is kept: hover and go to definition
   read the types and resolutions that semantic analysis recorded in the syntax trees. */

#define NAME_ARGS(name) (int)(name).length, (name).text

typedef struct {
    char *uri;
    char *path;
    char *root; /* the package of the document (its folder with a sosu.toml), or NULL */
    char *text;
    size_t length;
} Document;

/* A file whose diagnostics were published, and the package (or lone file) they came from. */
typedef struct {
    char *path;
    char *uri;
    char *owner;
} Published;

typedef struct {
    Published *items;
    size_t count;
    size_t cap;
} PublishedList;

typedef struct {
    char *root;   /* the package analysed */
    bool loaded;  /* `program` is set */
    bool checked; /* `sema` too: the program loaded without errors */
    Program program;
    Sema sema;
} Analysis;

struct LspServer {
    char *lib_dir;
    struct { Document *items; size_t count; size_t cap; } docs;
    PublishedList published;
    Analysis analysis;
    bool shutdown;
    int exit_code; /* -1 while running */
    LspMessages *out;
};

/* ---- Paths and URIs ---- */

static bool same_path(const char *a, const char *b) {
#ifdef _WIN32
    for (; *a && *b; a++, b++) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
    }
    return *a == *b;
#else
    return strcmp(a, b) == 0;
#endif
}

static int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

char *lsp_uri_to_path(const char *uri) {
    if (strncmp(uri, "file://", 7) != 0) return NULL;
    const char *p = uri + 7;
    while (*p && *p != '/') p++; /* the host, usually empty */
    size_t n = strlen(p);
    char *decoded = sosu_alloc(n + 1);
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        if (p[i] == '%' && i + 2 < n && hex_value(p[i + 1]) >= 0 && hex_value(p[i + 2]) >= 0) {
            decoded[k++] = (char)(hex_value(p[i + 1]) * 16 + hex_value(p[i + 2]));
            i += 2;
        } else {
            decoded[k++] = p[i];
        }
    }
    decoded[k] = '\0';
    /* file:///d:/x is the Windows path d:/x */
    const char *start = decoded;
    if (decoded[0] == '/' && isalpha((unsigned char)decoded[1]) && decoded[2] == ':') start++;
    char *path = path_normalize(start);
    free(decoded);
    return path;
}

char *lsp_path_to_uri(const char *path) {
    size_t n = strlen(path);
    char *uri = sosu_alloc(n * 3 + 9);
    char *out = uri;
    memcpy(out, "file://", 7);
    out += 7;
    if (path[0] != '/') *out++ = '/';
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)path[i];
        if (isalnum(c) || strchr("-._~/", c)) {
            *out++ = (char)c;
        } else if (c == '\\') {
            *out++ = '/';
        } else {
            static const char digits[] = "0123456789ABCDEF";
            *out++ = '%';
            *out++ = digits[c >> 4];
            *out++ = digits[c & 15];
        }
    }
    *out = '\0';
    return uri;
}

static char *absolute_path(const char *path) {
#ifdef _WIN32
    char *full = _fullpath(NULL, path, 0);
#else
    char *full = realpath(path, NULL);
#endif
    char *result = path_normalize(full ? full : path);
    free(full);
    return result;
}

/* The nearest folder up from the file with a sosu.toml, or NULL. */
static char *package_root(const char *path) {
    char *dir = path_dirname(path);
    for (;;) {
        char *manifest = path_join(dir, "sosu.toml");
        bool found = shell_file_exists(manifest);
        free(manifest);
        if (found) return dir;
        char *parent = path_dirname(dir);
        if (strcmp(parent, ".") == 0 || strlen(parent) >= strlen(dir)) {
            free(parent);
            free(dir);
            return NULL;
        }
        free(dir);
        dir = parent;
    }
}

/* ---- Documents ---- */

static Document *find_document(LspServer *s, const char *path) {
    for (size_t i = 0; i < s->docs.count; i++) {
        if (same_path(s->docs.items[i].path, path)) return &s->docs.items[i];
    }
    return NULL;
}

static const char *overlay(const char *path, size_t *length, void *data) {
    Document *doc = find_document(data, path);
    if (!doc) return NULL;
    *length = doc->length;
    return doc->text;
}

static void document_free(Document *doc) {
    free(doc->uri);
    free(doc->path);
    free(doc->root);
    free(doc->text);
}

/* The URI the client knows a file by: the one of its open document, if there is one. */
static char *uri_for(LspServer *s, const char *path) {
    Document *doc = find_document(s, path);
    return doc ? sosu_strdup(doc->uri) : lsp_path_to_uri(path);
}

/* ---- Positions: lines from 0, and columns in UTF-16 code units ---- */

static size_t utf16_units(int32_t c) { return c >= 0x10000 ? 2 : 1; }

static size_t offset_in(const char *text, size_t length, size_t line, size_t character) {
    size_t i = 0;
    if (length >= 3 && memcmp(text, "\xEF\xBB\xBF", 3) == 0) i = 3;
    for (size_t l = 0; l < line; l++) {
        while (i < length && text[i] != '\n') i++;
        if (i == length) return length;
        i++;
    }
    size_t units = 0;
    while (i < length && text[i] != '\n' && text[i] != '\r' && units < character) {
        int32_t c;
        size_t n = utf8_decode(text + i, length - i, &c);
        units += utf16_units(c);
        i += n;
    }
    return i;
}

static void position_of(const SourceFile *file, size_t offset, size_t *line, size_t *character) {
    if (offset > file->length) offset = file->length;
    size_t lo = 0;
    size_t hi = file->line_count;
    while (hi - lo > 1) {
        size_t mid = lo + (hi - lo) / 2;
        if (file->line_starts[mid] <= offset) lo = mid;
        else hi = mid;
    }
    size_t i = file->line_starts[lo];
    if (lo == 0 && file->length >= 3 && memcmp(file->text, "\xEF\xBB\xBF", 3) == 0 && offset >= 3) i = 3;
    size_t units = 0;
    while (i < offset) {
        int32_t c;
        size_t n = utf8_decode(file->text + i, file->length - i, &c);
        units += utf16_units(c);
        i += n;
    }
    *line = lo;
    *character = units;
}

static void write_range(JsonOut *o, const SourceFile *file, size_t offset, size_t length) {
    size_t line, character, end_line, end_character;
    position_of(file, offset, &line, &character);
    position_of(file, offset + length, &end_line, &end_character);
    json_raw(o, "{\"start\":{\"line\":%zu,\"character\":%zu},\"end\":{\"line\":%zu,\"character\":%zu}}", line,
             character, end_line, end_character);
}

/* ---- Messages ---- */

static void send(LspServer *s, JsonOut *o) {
    SOSU_VEC_PUSH(s->out, o->text);
    memset(o, 0, sizeof *o);
}

static void write_id(JsonOut *o, const Json *id) {
    if (id && id->kind == JSON_STRING) json_strn(o, id->string, id->length);
    else if (id && id->kind == JSON_NUMBER) json_raw(o, "%.0f", id->number);
    else json_raw(o, "null");
}

/* Starts a response: the result is written next, then `}` by send_response. */
static void begin_response(JsonOut *o, const Json *id) {
    json_raw(o, "{\"jsonrpc\":\"2.0\",\"id\":");
    write_id(o, id);
    json_raw(o, ",\"result\":");
}

static void send_response(LspServer *s, JsonOut *o) {
    json_raw(o, "}");
    send(s, o);
}

static void send_error(LspServer *s, const Json *id, int code, const char *message) {
    JsonOut o = {0};
    json_raw(&o, "{\"jsonrpc\":\"2.0\",\"id\":");
    write_id(&o, id);
    json_raw(&o, ",\"error\":{\"code\":%d,\"message\":", code);
    json_str(&o, message);
    json_raw(&o, "}}");
    send(s, &o);
}

static void log_message(LspServer *s, const char *message) {
    JsonOut o = {0};
    json_raw(&o, "{\"jsonrpc\":\"2.0\",\"method\":\"window/logMessage\",\"params\":{\"type\":1,\"message\":");
    json_str(&o, message);
    json_raw(&o, "}}");
    send(s, &o);
}

/* ---- Diagnostics ---- */

/* Publishes the diagnostics of `diags` that are in `file` (none: they are cleared). */
static void publish(LspServer *s, const char *uri, const DiagList *diags, const SourceFile *file) {
    JsonOut o = {0};
    json_raw(&o, "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/publishDiagnostics\",\"params\":{\"uri\":");
    json_str(&o, uri);
    json_raw(&o, ",\"diagnostics\":[");
    bool first = true;
    for (size_t i = 0; diags && i < diags->count; i++) {
        const Diagnostic *d = &diags->items[i];
        if (d->file != file) continue;
        json_raw(&o, first ? "{\"range\":" : ",{\"range\":");
        first = false;
        write_range(&o, file, d->offset, d->length);
        json_raw(&o, ",\"severity\":%d,\"source\":\"sosuc\",\"code\":", d->severity == SEVERITY_ERROR ? 1 : 2);
        json_str(&o, d->code ? d->code : "");
        json_raw(&o, ",\"message\":");
        char *message = d->help ? sosu_format("%s\nhelp: %s", d->message, d->help) : sosu_strdup(d->message);
        json_str(&o, message);
        free(message);
        json_raw(&o, "}");
    }
    json_raw(&o, "]}}");
    send(s, &o);
}

static bool published_has(const PublishedList *list, const char *path) {
    for (size_t i = 0; i < list->count; i++) {
        if (same_path(list->items[i].path, path)) return true;
    }
    return false;
}

static void published_add(PublishedList *list, const char *path, char *uri, const char *owner) {
    Published p = { sosu_strdup(path), uri, sosu_strdup(owner) };
    SOSU_VEC_PUSH(list, p);
}

/* `fresh` replaces what `owner` published before: the files it no longer names are cleared. */
static void finish_round(LspServer *s, const char *owner, PublishedList *fresh) {
    size_t kept = 0;
    for (size_t i = 0; i < s->published.count; i++) {
        Published *p = &s->published.items[i];
        if (!same_path(p->owner, owner)) {
            s->published.items[kept++] = *p;
            continue;
        }
        if (!published_has(fresh, p->path)) publish(s, p->uri, NULL, NULL);
        free(p->path);
        free(p->uri);
        free(p->owner);
    }
    s->published.count = kept;
    for (size_t i = 0; i < fresh->count; i++) SOSU_VEC_PUSH(&s->published, fresh->items[i]);
    free(fresh->items);
}

/* A document outside any program: only lexed and parsed. */
static void check_alone(LspServer *s, Document *doc, const char *owner, PublishedList *fresh) {
    SourceFile file;
    source_from_string(&file, doc->path, doc->text);
    TokenList tokens = {0};
    DiagList diags = {0};
    Arena arena = {0};
    lex_file(&file, &tokens, &diags);
    if (program_is_ui_file(doc->path)) parse_ui_module(&file, &tokens, &arena, &diags);
    else parse_module(&file, &tokens, &arena, &diags);
    publish(s, doc->uri, &diags, &file);
    published_add(fresh, doc->path, sosu_strdup(doc->uri), owner);
    arena_free(&arena);
    diag_free(&diags);
    token_list_free(&tokens);
    source_free(&file);
}

/* ---- Analysis ---- */

static void analysis_free(Analysis *a) {
    if (a->checked) sema_free(&a->sema);
    if (a->loaded) program_free(&a->program);
    free(a->root);
    memset(a, 0, sizeof *a);
}

/* Loads and checks the package in `root`, like `sosuc check`: semantic analysis only runs
   on a program that loaded without errors. */
static void analysis_run(LspServer *s, Analysis *a, const char *root) {
    memset(a, 0, sizeof *a);
    a->root = sosu_strdup(root);
    bool ok = program_load(&a->program, root, s->lib_dir);
    a->loaded = true;
    if (ok) {
        a->checked = true;
        sema_check(&a->sema, &a->program);
    }
}

static Unit *find_unit(const Analysis *a, const char *path) {
    if (!a->loaded) return NULL;
    for (size_t i = 0; i < a->program.units.count; i++) {
        Unit *unit = a->program.units.items[i];
        if (same_path(unit->file.path, path)) return unit;
    }
    return NULL;
}

/* Analyses the package again, and publishes its diagnostics and those of its open documents. */
static void analyse_package(LspServer *s, const char *root) {
    analysis_free(&s->analysis);
    analysis_run(s, &s->analysis, root);
    Analysis *a = &s->analysis;
    if (a->program.fatal) log_message(s, a->program.fatal);

    PublishedList fresh = {0};
    const DiagList *diags = &a->program.diags;
    for (size_t i = 0; i < diags->count; i++) {
        const SourceFile *file = diags->items[i].file;
        if (!file || published_has(&fresh, file->path)) continue;
        char *uri = uri_for(s, file->path);
        publish(s, uri, diags, file);
        published_add(&fresh, file->path, uri, root);
    }
    for (size_t i = 0; i < s->docs.count; i++) {
        Document *doc = &s->docs.items[i];
        if (!doc->root || !same_path(doc->root, root) || published_has(&fresh, doc->path)) continue;
        if (find_unit(a, doc->path)) {
            publish(s, doc->uri, NULL, NULL);
            published_add(&fresh, doc->path, sosu_strdup(doc->uri), root);
        } else {
            check_alone(s, doc, root, &fresh);
        }
    }
    finish_round(s, root, &fresh);
}

static void analyse_document(LspServer *s, Document *doc) {
    if (doc->root) {
        analyse_package(s, doc->root);
        return;
    }
    PublishedList fresh = {0};
    check_alone(s, doc, doc->path, &fresh);
    finish_round(s, doc->path, &fresh);
}

/* The analysis of the package of `doc`, run again if it is another package. */
static Analysis *analysis_for(LspServer *s, Document *doc) {
    if (!doc->root) return NULL;
    if (!s->analysis.root || !same_path(s->analysis.root, doc->root)) analyse_package(s, doc->root);
    return &s->analysis;
}

/* ---- Finding what is at a position ---- */

typedef enum {
    FOUND_NONE,
    FOUND_EXPR,     /* expr */
    FOUND_TYPE,     /* type: a TYPE_NAME */
    FOUND_PATH,     /* path: a parent class or an interface of a type */
    FOUND_IMPORT,   /* decl: an import */
    FOUND_INIT,     /* literal and init: a field given in a struct literal */
    FOUND_PATTERN,  /* match and pattern: a variant in a pattern */
    FOUND_DECLARED, /* declared: a name being declared (declared_fn or declared_decl when it is theirs) */
} FoundKind;

typedef struct {
    size_t offset;
    FoundKind kind;
    size_t best; /* length of what was found: the smallest wins */
    const Expr *expr;
    const TypeExpr *type;
    const Path *path;
    const Decl *import;
    const Expr *literal;
    const FieldInit *init;
    const Expr *match;
    const Pattern *pattern;
    Name declared;
    const FnDecl *declared_fn;
    const Decl *declared_decl;
    const Decl *decl; /* the declaration around the position */
    const FnDecl *fn; /* the function or method around it */
} Find;

static bool covers(size_t start, size_t length, size_t offset) {
    return offset >= start && offset <= start + length;
}

/* True when the node is at the offset and not larger than what was found. */
static bool better(Find *f, size_t start, size_t length, FoundKind kind) {
    if (!covers(start, length, f->offset) || (f->kind != FOUND_NONE && length > f->best)) return false;
    f->kind = kind;
    f->best = length;
    return true;
}

static void find_name(Find *f, Name name, const FnDecl *fn, const Decl *decl) {
    if (!name.text || !better(f, name.offset, name.length, FOUND_DECLARED)) return;
    f->declared = name;
    f->declared_fn = fn;
    f->declared_decl = decl;
}

static void find_path(Find *f, const Path *path) {
    if (path->count == 0) return;
    const Name *last = &path->items[path->count - 1];
    size_t start = path->items[0].offset;
    if (better(f, start, last->offset + last->length - start, FOUND_PATH)) f->path = path;
}

static void find_type(Find *f, const TypeExpr *t) {
    if (!t) return;
    if (t->kind == TYPE_NAME && better(f, t->offset, t->length, FOUND_TYPE)) f->type = t;
    switch (t->kind) {
    case TYPE_POINTER: case TYPE_OPTION: case TYPE_SLICE: find_type(f, t->as.inner); break;
    case TYPE_ARRAY: find_type(f, t->as.array.element); break;
    case TYPE_RESULT:
        find_type(f, t->as.result.value);
        find_type(f, t->as.result.error);
        break;
    case TYPE_FN:
        for (size_t i = 0; i < t->as.fn.param_count; i++) find_type(f, t->as.fn.params[i]);
        find_type(f, t->as.fn.ret);
        break;
    default: break;
    }
}

static void find_stmt(Find *f, const Stmt *s);

static void find_expr(Find *f, const Expr *e) {
    if (!e) return;
    if (better(f, e->offset, e->length, FOUND_EXPR)) f->expr = e;
    switch (e->kind) {
    case EXPR_UNARY: find_expr(f, e->as.unary.operand); break;
    case EXPR_BINARY:
        find_expr(f, e->as.binary.left);
        find_expr(f, e->as.binary.right);
        break;
    case EXPR_CAST: case EXPR_TRY_CAST:
        find_expr(f, e->as.cast.value);
        find_type(f, e->as.cast.type);
        break;
    case EXPR_TERNARY:
        find_expr(f, e->as.ternary.cond);
        find_expr(f, e->as.ternary.then_expr);
        find_expr(f, e->as.ternary.else_expr);
        break;
    case EXPR_RANGE:
        find_expr(f, e->as.range.start);
        find_expr(f, e->as.range.end);
        break;
    case EXPR_CALL:
        find_expr(f, e->as.call.callee);
        for (size_t i = 0; i < e->as.call.arg_count; i++) find_expr(f, e->as.call.args[i]);
        break;
    case EXPR_INDEX:
        find_expr(f, e->as.index.object);
        find_expr(f, e->as.index.index);
        break;
    case EXPR_FIELD: find_expr(f, e->as.field.object); break;
    case EXPR_STRUCT_LIT:
        find_type(f, e->as.struct_lit.type);
        for (size_t i = 0; i < e->as.struct_lit.field_count; i++) {
            const FieldInit *init = &e->as.struct_lit.fields[i];
            if (better(f, init->name.offset, init->name.length, FOUND_INIT)) {
                f->literal = e;
                f->init = init;
            }
            find_expr(f, init->value);
        }
        break;
    case EXPR_MATCH:
        find_expr(f, e->as.match.subject);
        for (size_t i = 0; i < e->as.match.arm_count; i++) {
            const MatchArm *arm = &e->as.match.arms[i];
            const Path *path = &arm->pattern.path;
            if (arm->pattern.kind == PATTERN_VARIANT && path->count > 0) {
                const Name *last = &path->items[path->count - 1];
                size_t start = path->items[0].offset;
                if (better(f, start, last->offset + last->length - start, FOUND_PATTERN)) {
                    f->match = e;
                    f->pattern = &arm->pattern;
                }
            }
            for (size_t j = 0; j < arm->pattern.binding_count; j++) find_name(f, arm->pattern.bindings[j], NULL, NULL);
            find_expr(f, arm->expr);
            find_stmt(f, arm->block);
        }
        break;
    case EXPR_TRY: find_expr(f, e->as.inner); break;
    case EXPR_ARRAY:
        for (size_t i = 0; i < e->as.array.count; i++) find_expr(f, e->as.array.items[i]);
        find_expr(f, e->as.array.repeat);
        break;
    default: break;
    }
}

static void find_stmt(Find *f, const Stmt *s) {
    if (!s) return;
    switch (s->kind) {
    case STMT_BLOCK:
        for (size_t i = 0; i < s->as.block.count; i++) find_stmt(f, s->as.block.items[i]);
        break;
    case STMT_VAR:
        find_type(f, s->as.var.type);
        find_name(f, s->as.var.name, NULL, NULL);
        find_expr(f, s->as.var.init);
        break;
    case STMT_ASSIGN:
        find_expr(f, s->as.assign.target);
        find_expr(f, s->as.assign.value);
        break;
    case STMT_EXPR: case STMT_RETURN: find_expr(f, s->as.expr); break;
    case STMT_IF:
        find_expr(f, s->as.if_.cond);
        find_stmt(f, s->as.if_.then_block);
        find_stmt(f, s->as.if_.else_stmt);
        break;
    case STMT_WHILE:
        find_expr(f, s->as.while_.cond);
        find_stmt(f, s->as.while_.body);
        break;
    case STMT_FOR:
        find_stmt(f, s->as.for_.init);
        find_expr(f, s->as.for_.cond);
        find_stmt(f, s->as.for_.step);
        find_stmt(f, s->as.for_.body);
        break;
    case STMT_FOR_IN:
        find_type(f, s->as.for_in.type);
        find_name(f, s->as.for_in.name, NULL, NULL);
        find_expr(f, s->as.for_in.iterable);
        find_stmt(f, s->as.for_in.body);
        break;
    case STMT_DEFER: case STMT_UNSAFE: find_stmt(f, s->as.inner); break;
    default: break;
    }
}

static void find_fn(Find *f, const FnDecl *fn, const Decl *decl) {
    if (covers(fn->offset, fn->length, f->offset)) f->fn = fn;
    find_type(f, fn->ret);
    find_name(f, fn->name, fn, decl);
    for (size_t i = 0; i < fn->param_count; i++) {
        find_type(f, fn->params[i].type);
        find_name(f, fn->params[i].name, NULL, NULL);
    }
    for (size_t i = 0; i < fn->contract_count; i++) find_expr(f, fn->contracts[i].expr);
    find_stmt(f, fn->body);
}

static void find_decl(Find *f, const Decl *d) {
    if (covers(d->offset, d->length, f->offset)) f->decl = d;
    switch (d->kind) {
    case DECL_IMPORT:
        if (d->as.import.count > 0) {
            const Path *path = &d->as.import;
            const Name *last = &path->items[path->count - 1];
            size_t start = path->items[0].offset;
            if (better(f, start, last->offset + last->length - start, FOUND_IMPORT)) f->import = d;
        }
        break;
    case DECL_STRUCT: case DECL_CLASS: case DECL_INTERFACE:
        find_name(f, d->as.type.name, NULL, d);
        for (size_t i = 0; i < d->as.type.base_count; i++) find_path(f, &d->as.type.bases[i]);
        for (size_t i = 0; i < d->as.type.field_count; i++) {
            const Field *field = &d->as.type.fields[i];
            find_type(f, field->type);
            find_name(f, field->name, NULL, NULL);
            find_expr(f, field->init);
        }
        for (size_t i = 0; i < d->as.type.method_count; i++) find_fn(f, &d->as.type.methods[i], NULL);
        break;
    case DECL_ENUM:
        find_name(f, d->as.enum_.name, NULL, d);
        for (size_t i = 0; i < d->as.enum_.variant_count; i++) {
            const Variant *v = &d->as.enum_.variants[i];
            find_name(f, v->name, NULL, NULL);
            for (size_t j = 0; j < v->field_count; j++) {
                find_type(f, v->fields[j].type);
                find_name(f, v->fields[j].name, NULL, NULL);
            }
        }
        break;
    case DECL_FN: find_fn(f, &d->as.fn, d); break;
    case DECL_EXTERN:
        for (size_t i = 0; i < d->as.extern_.fn_count; i++) find_fn(f, &d->as.extern_.fns[i], NULL);
        break;
    case DECL_TEST: find_stmt(f, d->as.test.body); break;
    default: break;
    }
}

static Find find_at(const Unit *unit, size_t offset) {
    Find f;
    memset(&f, 0, sizeof f);
    f.offset = offset;
    for (size_t i = 0; unit->ast && i < unit->ast->count; i++) find_decl(&f, unit->ast->decls[i]);
    return f;
}

/* ---- What a node designates ---- */

/* Where something is declared, and how it is shown on hover. */
typedef struct {
    const SourceFile *file;
    size_t offset;
    size_t length;
    JsonOut text; /* Sosu code, shown in a code block */
    JsonOut note; /* a sentence under it, or empty */
} Target;

static bool name_eq(Name a, Name b) {
    return a.length == b.length && memcmp(a.text, b.text, a.length) == 0;
}

static Symbol *scope_find(const Scope *scope, Name name) {
    for (size_t i = 0; i < scope->count; i++) {
        if (name_eq(scope->items[i].name, name)) return &scope->items[i];
    }
    return NULL;
}

/* A name of a module: declared or imported in it, or public in core.prelude. */
static Symbol *lookup(const Analysis *a, const Unit *unit, Name name) {
    Symbol *sym = scope_find(&a->sema.scopes[unit->index], name);
    if (!sym && a->sema.prelude) {
        sym = scope_find(&a->sema.scopes[a->sema.prelude->index], name);
        if (sym && !sym->is_pub) sym = NULL;
    }
    return sym;
}

static Name decl_name(const Decl *d) {
    if (d->kind == DECL_ENUM) return d->as.enum_.name;
    return d->as.type.name;
}

static void set_target(Target *t, const SourceFile *file, Name name) {
    t->file = file;
    t->offset = name.offset;
    t->length = name.length;
}

/* Source text from `start` to `end`, without the spaces around it. */
static void write_source(JsonOut *out, const SourceFile *file, size_t start, size_t end) {
    if (end > file->length) end = file->length;
    while (start < end && isspace((unsigned char)file->text[start])) start++;
    while (end > start && (isspace((unsigned char)file->text[end - 1]) || file->text[end - 1] == ';')) end--;
    json_raw(out, "%.*s", (int)(end - start), file->text + start);
}

/* The signature of a function or method, as written. */
static void write_signature(JsonOut *out, const SourceFile *file, const FnDecl *fn) {
    size_t end = fn->body ? fn->body->offset : fn->offset + fn->length;
    write_source(out, file, fn->offset, end);
}

/* `struct Point`, `class Button : Widget`...: a declaration up to its `{`. */
static void write_header(JsonOut *out, const SourceFile *file, const Decl *d) {
    size_t end = d->offset;
    while (end < d->offset + d->length && end < file->length && file->text[end] != '{') end++;
    write_source(out, file, d->offset, end);
}

static void describe_fn(Target *t, const FnInfo *fn) {
    if (!fn->unit) return;
    write_signature(&t->text, &fn->unit->file, fn->decl);
    set_target(t, &fn->unit->file, fn->decl->name);
}

static void describe_method(Target *t, const MethodInfo *m) {
    write_signature(&t->text, &m->owner->unit->file, m->decl);
    json_raw(&t->note, "method of `%.*s`", NAME_ARGS(m->owner->name));
    set_target(t, &m->owner->unit->file, m->decl->name);
}

static void describe_field(Target *t, const TypeInfo *owner, const FieldInfo *field) {
    char *type = type_to_string(field->type);
    json_raw(&t->text, "%s %.*s", type, NAME_ARGS(field->name));
    free(type);
    json_raw(&t->note, "field of `%.*s`", NAME_ARGS(owner->name));
    if (field->decl) set_target(t, &owner->unit->file, field->decl->name);
}

static void describe_variant(Target *t, const TypeInfo *info, size_t index) {
    const VariantInfo *v = &info->variants[index];
    json_raw(&t->text, "%.*s.%.*s", NAME_ARGS(info->name), NAME_ARGS(v->name));
    if (v->field_count) {
        json_raw(&t->text, "(");
        for (size_t i = 0; i < v->field_count; i++) {
            char *type = type_to_string(v->fields[i]);
            json_raw(&t->text, i ? ", %s" : "%s", type);
            free(type);
        }
        json_raw(&t->text, ")");
    }
    if (v->decl) set_target(t, &info->unit->file, v->decl->name);
}

static void describe_type(Target *t, const TypeInfo *info) {
    write_header(&t->text, &info->unit->file, info->decl);
    set_target(t, &info->unit->file, decl_name(info->decl));
}

static void describe_module(Target *t, const Unit *module) {
    json_raw(&t->text, "module %s", module->module);
    t->file = &module->file;
    t->offset = 0;
    t->length = 0;
}

static void describe_symbol(Target *t, const Symbol *sym) {
    switch (sym->kind) {
    case SYM_TYPE: describe_type(t, sym->type_info); break;
    case SYM_FN: describe_fn(t, sym->fn); break;
    case SYM_MODULE: if (sym->module) describe_module(t, sym->module); break;
    }
}

/* A field or method named `name` of a struct, class or interface, or of its parents. */
static bool describe_member(Target *t, const TypeInfo *info, Name name) {
    for (const TypeInfo *owner = info; owner; owner = owner->parent) {
        for (size_t i = 0; i < owner->field_count; i++) {
            if (name_eq(owner->fields[i].name, name)) {
                describe_field(t, owner, &owner->fields[i]);
                return true;
            }
        }
        for (size_t i = 0; i < owner->method_count; i++) {
            if (name_eq(owner->methods[i].decl->name, name)) {
                describe_method(t, &owner->methods[i]);
                return true;
            }
        }
    }
    return false;
}

/* The struct, class or interface of a value, through a pointer. */
static const TypeInfo *info_of(const Type *type) {
    if (type && type->tag == T_POINTER) type = type->as.inner;
    if (!type || (type->tag != T_STRUCT && type->tag != T_CLASS && type->tag != T_INTERFACE && type->tag != T_ENUM)) {
        return NULL;
    }
    return type->as.info;
}

/* The module, type or function that a path expression names (`io`, `shapes.Circle`). */
static Symbol *symbol_of(const Analysis *a, const Unit *unit, const Expr *e) {
    if (e->resolution != RESOLVED_NONE) return NULL;
    if (e->kind == EXPR_NAME) return lookup(a, unit, e->as.name);
    if (e->kind != EXPR_FIELD) return NULL;
    Symbol *outer = symbol_of(a, unit, e->as.field.object);
    if (!outer || outer->kind != SYM_MODULE || !outer->module) return NULL;
    Symbol *sym = scope_find(&a->sema.scopes[outer->module->index], e->as.field.name);
    return sym && sym->kind != SYM_MODULE ? sym : NULL;
}

static const TypeInfo *owner_of_field(const Analysis *a, const FieldInfo *field) {
    for (size_t i = 0; i < a->sema.type_infos.count; i++) {
        const TypeInfo *info = a->sema.type_infos.items[i];
        if (info->field_count && field >= info->fields && field < info->fields + info->field_count) return info;
    }
    return NULL;
}

/* A type name, a parent class...: the symbol of `path` named at the offset. */
static void describe_path(Target *t, const Analysis *a, const Unit *unit, const Path *path, size_t offset) {
    Symbol *sym = NULL;
    for (size_t i = 0; i < path->count; i++) {
        if (i == 0) sym = lookup(a, unit, path->items[0]);
        else if (sym && sym->kind == SYM_MODULE && sym->module) sym = scope_find(&a->sema.scopes[sym->module->index], path->items[i]);
        else sym = NULL;
        if (!sym || covers(path->items[i].offset, path->items[i].length, offset)) break;
    }
    if (sym) describe_symbol(t, sym);
}

static void describe_expr(Target *t, const Analysis *a, const Unit *unit, const Expr *e) {
    switch (e->resolution) {
    case RESOLVED_LOCAL: {
        char *type = e->type ? type_to_string(e->type) : sosu_strdup("auto");
        json_raw(&t->text, "%s %.*s", type, NAME_ARGS(e->as.name));
        free(type);
        t->file = &unit->file;
        t->offset = e->target_index;
        t->length = e->as.name.length;
        return;
    }
    case RESOLVED_SELF_FIELD: {
        const TypeInfo *owner = owner_of_field(a, e->target);
        if (owner) describe_field(t, owner, e->target);
        return;
    }
    case RESOLVED_SELF_METHOD: case RESOLVED_METHOD: describe_method(t, e->target); return;
    case RESOLVED_FN: describe_fn(t, e->target); return;
    case RESOLVED_VARIANT: describe_variant(t, e->target, e->target_index); return;
    default: break;
    }
    Symbol *sym = symbol_of(a, unit, e);
    if (sym) {
        describe_symbol(t, sym);
        return;
    }
    if (e->kind == EXPR_FIELD) {
        const TypeInfo *info = info_of(e->as.field.object->type);
        if (info && describe_member(t, info, e->as.field.name)) return;
    }
    if (e->type && e->type->tag != T_ERROR) {
        char *type = type_to_string(e->type);
        json_raw(&t->text, "%s", type);
        free(type);
    }
}

/* What is at `offset` in `unit`. */
static Target describe(const Analysis *a, const Unit *unit, size_t offset) {
    Target t;
    memset(&t, 0, sizeof t);
    Find f = find_at(unit, offset);
    switch (f.kind) {
    case FOUND_EXPR:
        if (a->checked) describe_expr(&t, a, unit, f.expr);
        break;
    case FOUND_TYPE:
        if (a->checked) describe_path(&t, a, unit, &f.type->as.name, offset);
        break;
    case FOUND_PATH:
        if (a->checked) describe_path(&t, a, unit, f.path, offset);
        break;
    case FOUND_IMPORT:
        for (size_t i = 0; i < unit->import_count; i++) {
            if (unit->imports[i].decl == f.import && unit->imports[i].unit) describe_module(&t, unit->imports[i].unit);
        }
        break;
    case FOUND_INIT: {
        const TypeInfo *info = a->checked ? info_of(f.literal->type) : NULL;
        if (info) describe_member(&t, info, f.init->name);
        break;
    }
    case FOUND_PATTERN: {
        const TypeInfo *info = a->checked ? info_of(f.match->as.match.subject->type) : NULL;
        Name last = f.pattern->path.items[f.pattern->path.count - 1];
        for (size_t i = 0; info && i < info->variant_count; i++) {
            if (name_eq(info->variants[i].name, last)) describe_variant(&t, info, i);
        }
        break;
    }
    case FOUND_DECLARED:
        set_target(&t, &unit->file, f.declared);
        if (f.declared_fn) write_signature(&t.text, &unit->file, f.declared_fn);
        else if (f.declared_decl) write_header(&t.text, &unit->file, f.declared_decl);
        break;
    default: break;
    }
    return t;
}

static void target_free(Target *t) {
    json_out_free(&t->text);
    json_out_free(&t->note);
}

/* ---- Requests on a position ---- */

typedef struct {
    Document *doc;
    size_t line;
    size_t character;
} Position;

static bool read_position(LspServer *s, const Json *params, Position *p) {
    const Json *document = json_get(params, "textDocument");
    const Json *position = json_get(params, "position");
    char *path = lsp_uri_to_path(json_string(json_get(document, "uri"), ""));
    p->doc = path ? find_document(s, path) : NULL;
    free(path);
    p->line = (size_t)json_number(json_get(position, "line"), 0);
    p->character = (size_t)json_number(json_get(position, "character"), 0);
    return p->doc != NULL;
}

static void hover(LspServer *s, const Json *id, const Json *params) {
    JsonOut o = {0};
    begin_response(&o, id);
    Position p;
    Analysis *a = read_position(s, params, &p) ? analysis_for(s, p.doc) : NULL;
    Unit *unit = a ? find_unit(a, p.doc->path) : NULL;
    Target t;
    memset(&t, 0, sizeof t);
    if (unit) t = describe(a, unit, offset_in(unit->file.text, unit->file.length, p.line, p.character));
    if (t.text.length) {
        JsonOut value = {0};
        json_raw(&value, "```sosu\n%s\n```", t.text.text);
        if (t.note.length) json_raw(&value, "\n\n%s", t.note.text);
        json_raw(&o, "{\"contents\":{\"kind\":\"markdown\",\"value\":");
        json_str(&o, value.text);
        json_raw(&o, "}}");
        json_out_free(&value);
    } else {
        json_raw(&o, "null");
    }
    target_free(&t);
    send_response(s, &o);
}

static void definition(LspServer *s, const Json *id, const Json *params) {
    JsonOut o = {0};
    begin_response(&o, id);
    Position p;
    Analysis *a = read_position(s, params, &p) ? analysis_for(s, p.doc) : NULL;
    Unit *unit = a ? find_unit(a, p.doc->path) : NULL;
    Target t;
    memset(&t, 0, sizeof t);
    if (unit) t = describe(a, unit, offset_in(unit->file.text, unit->file.length, p.line, p.character));
    if (t.file) {
        char *uri = uri_for(s, t.file->path);
        json_raw(&o, "{\"uri\":");
        json_str(&o, uri);
        json_raw(&o, ",\"range\":");
        write_range(&o, t.file, t.offset, t.length);
        json_raw(&o, "}");
        free(uri);
    } else {
        json_raw(&o, "null");
    }
    target_free(&t);
    send_response(s, &o);
}

/* ---- Completion ---- */

enum {
    ITEM_METHOD = 2,
    ITEM_FUNCTION = 3,
    ITEM_FIELD = 5,
    ITEM_VARIABLE = 6,
    ITEM_CLASS = 7,
    ITEM_INTERFACE = 8,
    ITEM_MODULE = 9,
    ITEM_KEYWORD = 14,
    ITEM_ENUM = 13,
    ITEM_STRUCT = 22,
    ITEM_ENUM_MEMBER = 20,
};

typedef struct {
    JsonOut *out;
    struct { char **items; size_t count; size_t cap; } labels; /* given already */
} Items;

static void add_item(Items *items, const char *label, size_t length, int kind, const char *detail) {
    for (size_t i = 0; i < items->labels.count; i++) {
        if (strlen(items->labels.items[i]) == length && memcmp(items->labels.items[i], label, length) == 0) return;
    }
    char *copy = sosu_alloc(length + 1);
    memcpy(copy, label, length);
    copy[length] = '\0';
    SOSU_VEC_PUSH(&items->labels, copy);
    json_raw(items->out, items->labels.count == 1 ? "{\"label\":" : ",{\"label\":");
    json_strn(items->out, label, length);
    json_raw(items->out, ",\"kind\":%d", kind);
    if (detail && detail[0]) {
        json_raw(items->out, ",\"detail\":");
        json_str(items->out, detail);
    }
    json_raw(items->out, "}");
}

static void add_name(Items *items, Name name, int kind, const char *detail) {
    add_item(items, name.text, name.length, kind, detail);
}

static int type_kind(const TypeInfo *info) {
    switch (info->type->tag) {
    case T_CLASS: return ITEM_CLASS;
    case T_INTERFACE: return ITEM_INTERFACE;
    case T_ENUM: return ITEM_ENUM;
    default: return ITEM_STRUCT;
    }
}

static void add_symbol(Items *items, const Symbol *sym) {
    JsonOut detail = {0};
    int kind = ITEM_MODULE;
    if (sym->kind == SYM_TYPE) {
        kind = type_kind(sym->type_info);
        write_header(&detail, &sym->type_info->unit->file, sym->type_info->decl);
    } else if (sym->kind == SYM_FN) {
        kind = ITEM_FUNCTION;
        if (sym->fn->unit) write_signature(&detail, &sym->fn->unit->file, sym->fn->decl);
    } else if (sym->module) {
        json_raw(&detail, "module %s", sym->module->module);
    }
    add_name(items, sym->name, kind, detail.text);
    json_out_free(&detail);
}

/* The fields and methods of a type and of its parents; the variants of an enum. */
static void add_members(Items *items, const TypeInfo *info) {
    for (const TypeInfo *owner = info; owner; owner = owner->parent) {
        for (size_t i = 0; i < owner->field_count; i++) {
            char *type = type_to_string(owner->fields[i].type);
            add_name(items, owner->fields[i].name, ITEM_FIELD, type);
            free(type);
        }
        for (size_t i = 0; i < owner->method_count; i++) {
            JsonOut detail = {0};
            write_signature(&detail, &owner->unit->file, owner->methods[i].decl);
            add_name(items, owner->methods[i].decl->name, ITEM_METHOD, detail.text);
            json_out_free(&detail);
        }
    }
}

static void add_variants(Items *items, const TypeInfo *info) {
    for (size_t i = 0; i < info->variant_count; i++) add_name(items, info->variants[i].name, ITEM_ENUM_MEMBER, NULL);
}

/* After `object.`: what the object has, from an analysis of the text where the member is
   completed by a placeholder. True if the object was found. */
static bool add_object_members(Items *items, const Analysis *a, const Unit *unit, size_t name_offset) {
    Find f = find_at(unit, name_offset);
    if (f.kind != FOUND_EXPR || f.expr->kind != EXPR_FIELD || f.expr->as.field.name.offset != name_offset) return false;
    const Expr *object = f.expr->as.field.object;
    Symbol *sym = symbol_of(a, unit, object);
    if (sym && sym->kind == SYM_MODULE && sym->module) {
        const Scope *scope = &a->sema.scopes[sym->module->index];
        bool same_package = sym->module->package == unit->package;
        for (size_t i = 0; i < scope->count; i++) {
            if (scope->items[i].kind != SYM_MODULE && (scope->items[i].is_pub || same_package)) add_symbol(items, &scope->items[i]);
        }
        return true;
    }
    if (sym && sym->kind == SYM_TYPE) {
        if (sym->type_info->type->tag == T_ENUM) add_variants(items, sym->type_info);
        return true;
    }
    const TypeInfo *info = info_of(object->type);
    if (!info) return object->type != NULL;
    if (info->type->tag != T_ENUM) add_members(items, info);
    return true;
}

static bool is_ident(char c) {
    return isalnum((unsigned char)c) || c == '_' || (unsigned char)c >= 0x80;
}

static void complete_member(LspServer *s, Items *items, Document *doc, size_t offset, size_t start) {
    if (!doc->root) return;
    static const char *const fillers[] = { "__sosu_lsp", "__sosu_lsp;" };
    char *original = doc->text;
    size_t original_length = doc->length;
    for (size_t i = 0; i < 2; i++) {
        const char *filler = fillers[i];
        if (start < offset) filler += strlen("__sosu_lsp"); /* a name is started already */
        size_t n = strlen(filler);
        char *text = sosu_alloc(original_length + n + 1);
        memcpy(text, original, offset);
        memcpy(text + offset, filler, n);
        memcpy(text + offset + n, original + offset, original_length - offset);
        text[original_length + n] = '\0';
        doc->text = text;
        doc->length = original_length + n;
        Analysis a;
        analysis_run(s, &a, doc->root);
        doc->text = original;
        doc->length = original_length;
        Unit *unit = a.checked ? find_unit(&a, doc->path) : NULL;
        bool found = unit && add_object_members(items, &a, unit, start);
        analysis_free(&a);
        free(text);
        if (found) break;
    }
}

/* The variables and parameters declared before `offset` in the blocks around it. */
static void add_locals_of(Items *items, const SourceFile *file, const Stmt *s, size_t offset) {
    if (!s || !covers(s->offset, s->length, offset)) return;
    switch (s->kind) {
    case STMT_BLOCK:
        for (size_t i = 0; i < s->as.block.count; i++) {
            const Stmt *item = s->as.block.items[i];
            if (item->kind == STMT_VAR && item->offset + item->length <= offset) {
                JsonOut type = {0};
                if (item->as.var.type) write_source(&type, file, item->as.var.type->offset, item->as.var.type->offset + item->as.var.type->length);
                else json_raw(&type, "auto");
                add_name(items, item->as.var.name, ITEM_VARIABLE, type.text);
                json_out_free(&type);
            }
            add_locals_of(items, file, item, offset);
        }
        break;
    case STMT_IF:
        add_locals_of(items, file, s->as.if_.then_block, offset);
        add_locals_of(items, file, s->as.if_.else_stmt, offset);
        break;
    case STMT_WHILE: add_locals_of(items, file, s->as.while_.body, offset); break;
    case STMT_FOR:
        if (s->as.for_.init && s->as.for_.init->kind == STMT_VAR) add_name(items, s->as.for_.init->as.var.name, ITEM_VARIABLE, NULL);
        add_locals_of(items, file, s->as.for_.body, offset);
        break;
    case STMT_FOR_IN:
        add_name(items, s->as.for_in.name, ITEM_VARIABLE, NULL);
        add_locals_of(items, file, s->as.for_in.body, offset);
        break;
    case STMT_DEFER: case STMT_UNSAFE: add_locals_of(items, file, s->as.inner, offset); break;
    default: break;
    }
}

static const char *const builtin_types[] = {
    "i8", "i16", "i32", "i64", "isize", "u8", "u16", "u32", "u64", "usize",
    "f32", "f64", "bool", "char", "str", "void", "Arena",
};

static const char *const keywords[] = {
#define X(kind, text) text,
    SOSU_KEYWORDS(X)
#undef X
};

static void complete_names(LspServer *s, Items *items, Document *doc, size_t offset) {
    Analysis *a = analysis_for(s, doc);
    Unit *unit = a ? find_unit(a, doc->path) : NULL;
    if (unit) {
        Find f = find_at(unit, offset);
        if (f.fn) {
            for (size_t i = 0; i < f.fn->param_count; i++) {
                JsonOut type = {0};
                const TypeExpr *te = f.fn->params[i].type;
                if (te) write_source(&type, &unit->file, te->offset, te->offset + te->length);
                add_name(items, f.fn->params[i].name, ITEM_VARIABLE, type.text);
                json_out_free(&type);
            }
            add_locals_of(items, &unit->file, f.fn->body, offset);
        }
        if (a->checked && f.decl && (f.decl->kind == DECL_STRUCT || f.decl->kind == DECL_CLASS)) {
            for (size_t i = 0; i < a->sema.type_infos.count; i++) {
                if (a->sema.type_infos.items[i]->decl == f.decl) add_members(items, a->sema.type_infos.items[i]);
            }
        }
        if (a->checked) {
            const Scope *scope = &a->sema.scopes[unit->index];
            for (size_t i = 0; i < scope->count; i++) add_symbol(items, &scope->items[i]);
            if (a->sema.prelude) {
                const Scope *prelude = &a->sema.scopes[a->sema.prelude->index];
                for (size_t i = 0; i < prelude->count; i++) {
                    if (prelude->items[i].is_pub && prelude->items[i].kind != SYM_MODULE) add_symbol(items, &prelude->items[i]);
                }
            }
        }
    }
    for (size_t i = 0; i < sizeof builtin_types / sizeof *builtin_types; i++) {
        add_item(items, builtin_types[i], strlen(builtin_types[i]), ITEM_STRUCT, "built-in type");
    }
    for (size_t i = 0; i < sizeof keywords / sizeof *keywords; i++) {
        if (strcmp(keywords[i], "comptime") != 0) add_item(items, keywords[i], strlen(keywords[i]), ITEM_KEYWORD, NULL);
    }
}

static void completion(LspServer *s, const Json *id, const Json *params) {
    JsonOut o = {0};
    begin_response(&o, id);
    json_raw(&o, "[");
    Position p;
    if (read_position(s, params, &p)) {
        Items items = { &o, { NULL, 0, 0 } };
        Document *doc = p.doc;
        size_t offset = offset_in(doc->text, doc->length, p.line, p.character);
        size_t start = offset;
        while (start > 0 && is_ident(doc->text[start - 1])) start--;
        if (start > 0 && doc->text[start - 1] == '.') complete_member(s, &items, doc, offset, start);
        else complete_names(s, &items, doc, offset);
        for (size_t i = 0; i < items.labels.count; i++) free(items.labels.items[i]);
        free(items.labels.items);
    }
    json_raw(&o, "]");
    send_response(s, &o);
}

/* ---- Documents of the client ---- */

static Document *document_of(LspServer *s, const Json *params) {
    char *path = lsp_uri_to_path(json_string(json_get(json_get(params, "textDocument"), "uri"), ""));
    Document *doc = path ? find_document(s, path) : NULL;
    free(path);
    return doc;
}

static void set_text(Document *doc, const Json *text) {
    free(doc->text);
    doc->length = text && text->kind == JSON_STRING ? text->length : 0;
    doc->text = sosu_alloc(doc->length + 1);
    if (doc->length) memcpy(doc->text, text->string, doc->length);
    doc->text[doc->length] = '\0';
}

static void did_open(LspServer *s, const Json *params) {
    const Json *item = json_get(params, "textDocument");
    const char *uri = json_string(json_get(item, "uri"), "");
    char *path = lsp_uri_to_path(uri);
    if (!path) return;
    Document *doc = find_document(s, path);
    if (doc) {
        free(path);
    } else {
        Document fresh = { sosu_strdup(uri), path, package_root(path), NULL, 0 };
        SOSU_VEC_PUSH(&s->docs, fresh);
        doc = &s->docs.items[s->docs.count - 1];
    }
    set_text(doc, json_get(item, "text"));
    analyse_document(s, doc);
}

static void did_change(LspServer *s, const Json *params) {
    Document *doc = document_of(s, params);
    const Json *changes = json_get(params, "contentChanges");
    if (!doc || !changes || changes->kind != JSON_ARRAY || changes->count == 0) return;
    /* the whole text (textDocumentSync 1): the last change has it */
    set_text(doc, json_get(&changes->items[changes->count - 1], "text"));
    analyse_document(s, doc);
}

static void did_close(LspServer *s, const Json *params) {
    Document *doc = document_of(s, params);
    if (!doc) return;
    char *root = doc->root ? sosu_strdup(doc->root) : NULL;
    char *path = sosu_strdup(doc->path);
    document_free(doc);
    *doc = s->docs.items[--s->docs.count];
    if (root) {
        analyse_package(s, root); /* the file on disk counts again */
    } else {
        PublishedList none = {0};
        finish_round(s, path, &none);
    }
    free(root);
    free(path);
}

/* ---- Server ---- */

LspServer *lsp_new(const char *lib_dir) {
    LspServer *s = sosu_alloc(sizeof *s);
    memset(s, 0, sizeof *s);
    s->lib_dir = absolute_path(lib_dir);
    s->exit_code = -1;
    return s;
}

void lsp_free(LspServer *s) {
    source_set_overlay(NULL, NULL);
    analysis_free(&s->analysis);
    for (size_t i = 0; i < s->docs.count; i++) document_free(&s->docs.items[i]);
    free(s->docs.items);
    for (size_t i = 0; i < s->published.count; i++) {
        free(s->published.items[i].path);
        free(s->published.items[i].uri);
        free(s->published.items[i].owner);
    }
    free(s->published.items);
    free(s->lib_dir);
    free(s);
}

int lsp_exit_code(const LspServer *s) { return s->exit_code < 0 ? 0 : s->exit_code; }

void lsp_messages_free(LspMessages *messages) {
    for (size_t i = 0; i < messages->count; i++) free(messages->items[i]);
    free(messages->items);
    memset(messages, 0, sizeof *messages);
}

static void initialize(LspServer *s, const Json *id) {
    JsonOut o = {0};
    begin_response(&o, id);
    json_raw(&o, "{\"capabilities\":{\"textDocumentSync\":{\"openClose\":true,\"change\":1},"
                 "\"hoverProvider\":true,\"definitionProvider\":true,"
                 "\"completionProvider\":{\"triggerCharacters\":[\".\"]}},"
                 "\"serverInfo\":{\"name\":\"sosuc\",\"version\":\"%s\"}}", SOSU_VERSION);
    send_response(s, &o);
}

bool lsp_handle(LspServer *s, const char *text, size_t length, LspMessages *out) {
    s->out = out;
    source_set_overlay(overlay, s);
    Arena arena = {0};
    Json *message = json_parse(text, length, &arena);
    const char *method = json_string(json_get(message, "method"), NULL);
    const Json *id = json_get(message, "id");
    const Json *params = json_get(message, "params");
    if (!message) {
        send_error(s, NULL, -32700, "the message is not valid JSON");
    } else if (!method) {
        /* a response: the server sends no requests */
    } else if (strcmp(method, "initialize") == 0) {
        initialize(s, id);
    } else if (strcmp(method, "shutdown") == 0) {
        s->shutdown = true;
        JsonOut o = {0};
        begin_response(&o, id);
        json_raw(&o, "null");
        send_response(s, &o);
    } else if (strcmp(method, "exit") == 0) {
        s->exit_code = s->shutdown ? 0 : 1;
    } else if (strcmp(method, "textDocument/didOpen") == 0) {
        did_open(s, params);
    } else if (strcmp(method, "textDocument/didChange") == 0) {
        did_change(s, params);
    } else if (strcmp(method, "textDocument/didClose") == 0) {
        did_close(s, params);
    } else if (strcmp(method, "textDocument/hover") == 0) {
        hover(s, id, params);
    } else if (strcmp(method, "textDocument/definition") == 0) {
        definition(s, id, params);
    } else if (strcmp(method, "textDocument/completion") == 0) {
        completion(s, id, params);
    } else if (id) {
        char *error = sosu_format("`%s` is not supported", method);
        send_error(s, id, -32601, error);
        free(error);
    }
    /* other notifications (initialized, $/cancelRequest, didSave...) need nothing */
    arena_free(&arena);
    s->out = NULL;
    return s->exit_code < 0;
}
