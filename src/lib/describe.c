/* `sosuc describe` (spec §11.4): the components of a package, described from their comments.

   A `pub struct` whose comment above has a line `// component` (or `// component "name"`) is a
   component: each of its `pub mut` fields is described (shown by the editor, saved with the scene)
   from its type and the words of its comments (`// label "Speed"`, `// range 0..10`...). What is
   written is a module of Sosu, `src/described.sosu`: for each component the class that describes and
   saves it (`describe.Describe<T>` and `save.Codec<T>`, as `engine.components` writes them by hand),
   the class that makes it a kind of the editor (`kinds.Kind` and `host.Saved`), and `add_all`, which
   adds every one to a level.

   The tool reads the syntax only, never the types: a field of a type it does not know is an error
   at its line, it never guesses. The words of a comment are read when its first word is one of
   them (or a word followed by a text or a number: a word misspelled); any other comment is prose. */
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "arena.h"
#include "ast.h"
#include "describe.h"
#include "fmt.h"
#include "lexer.h"
#include "parser.h"

/* ---- Text written ---- */

static Diagnostic *warning(DiagList *list, const char *code, const SourceFile *file, size_t offset, size_t length,
                           const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    Diagnostic *d = diag_vwarning(list, code, file, offset, length, fmt, args);
    va_end(args);
    return d;
}

typedef struct {
    char *items;
    size_t count;
    size_t cap;
} Out;

static void put(Out *out, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    char *text = sosu_vformat(fmt, args);
    va_end(args);
    for (const char *p = text; *p; p++) SOSU_VEC_PUSH(out, *p);
    free(text);
}

/* `text` (of `length` bytes) as a string literal of Sosu. */
static void put_string(Out *out, const char *text, size_t length) {
    SOSU_VEC_PUSH(out, '"');
    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char)text[i];
        if (c == '"' || c == '\\') {
            SOSU_VEC_PUSH(out, '\\');
            SOSU_VEC_PUSH(out, (char)c);
        } else if (c < 0x20) {
            put(out, "\\x%02X", c);
        } else {
            SOSU_VEC_PUSH(out, (char)c);
        }
    }
    SOSU_VEC_PUSH(out, '"');
}

/* ---- What is read ---- */

typedef enum {
    TYPE_F32,
    TYPE_I32,
    TYPE_I64,
    TYPE_USIZE,
    TYPE_BOOL,
    TYPE_STR,
    TYPE_VEC3,
    TYPE_ENUM,
} FieldType;

typedef struct {
    const Decl *decl;   /* an enum without payloads */
    size_t source;      /* its module */
} EnumRef;

typedef struct {
    const char *text; /* a value read from a comment: unescaped text, or a number as written */
    size_t length;
    bool set;
} Word;

typedef struct {
    const Field *field;
    FieldType type;
    size_t enum_index; /* TYPE_ENUM: in `Reader.enums` */
    Word label, unit, group, tooltip, low, high;
    bool asset, color, hidden;
} Described;

typedef struct {
    Described *items;
    size_t count;
    size_t cap;
} DescribedList;

typedef struct {
    const Decl *decl;
    size_t source;
    char *table; /* its name in a scene file */
    DescribedList fields;
} Component;

typedef struct {
    const SourceFile *file;
    Module *module;
    CommentList comments;
    TokenList tokens;
} Read;

typedef struct {
    const DescribeSource *sources;
    size_t count;
    Read *read;
    DiagList *diags;
    struct {
        EnumRef *items;
        size_t count;
        size_t cap;
    } enums;
    struct {
        Component *items;
        size_t count;
        size_t cap;
    } components;
    Arena words; /* the texts of the comments, unescaped */
} Reader;

/* The last part of a module path: what names it after `import` (`ai.brain` -> `brain`). */
static const char *short_module(const char *module) {
    const char *dot = strrchr(module, '.');
    return dot ? dot + 1 : module;
}

static bool name_is(Name name, const char *text) {
    return strlen(text) == name.length && memcmp(name.text, text, name.length) == 0;
}

/* `DoorLock` -> `door_lock`, `HTTPServer` -> `http_server`. */
static char *snake(Name name) {
    Out out = {0};
    for (size_t i = 0; i < name.length; i++) {
        char c = name.text[i];
        bool upper = c >= 'A' && c <= 'Z';
        if (upper && i > 0) {
            char before = name.text[i - 1];
            bool after_lower = (before >= 'a' && before <= 'z') || (before >= '0' && before <= '9');
            bool before_lower = i + 1 < name.length && name.text[i + 1] >= 'a' && name.text[i + 1] <= 'z' &&
                                before >= 'A' && before <= 'Z';
            if (after_lower || before_lower) SOSU_VEC_PUSH(&out, '_');
        }
        SOSU_VEC_PUSH(&out, (char)(upper ? c - 'A' + 'a' : c));
    }
    SOSU_VEC_PUSH(&out, '\0');
    return out.items;
}

/* `gravity_scale` -> `Gravity scale`. */
static char *in_words(Name name) {
    char *text = sosu_alloc(name.length + 1);
    for (size_t i = 0; i < name.length; i++) {
        char c = name.text[i];
        text[i] = c == '_' ? ' ' : c;
    }
    text[name.length] = '\0';
    if (text[0] >= 'a' && text[0] <= 'z') text[0] = (char)(text[0] - 'a' + 'A');
    return text;
}

static bool is_table_name(const char *text, size_t length) {
    if (length == 0 || !(text[0] >= 'a' && text[0] <= 'z')) return false;
    for (size_t i = 0; i < length; i++) {
        char c = text[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
    }
    return true;
}

/* ---- Comments ---- */

/* The index of the first comment that starts at `offset` or after it. */
static size_t comment_at(const CommentList *comments, size_t offset) {
    size_t low = 0, high = comments->count;
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        if (comments->items[middle].offset < offset) low = middle + 1;
        else high = middle;
    }
    return low;
}

static bool is_line_comment(const Read *r, const Comment *c) {
    return c->length >= 2 && r->file->text[c->offset] == '/' && r->file->text[c->offset + 1] == '/';
}

/* The comments of what starts at `offset`: those on the lines just above it, each alone on its
   line, the nearest last, in [*from, *to); and the one at the end of `end_line`, or none (-1). */
static void comments_of(const Read *r, size_t offset, size_t end_line, size_t *from, size_t *to, size_t *trailing) {
    size_t line = source_line_of(r->file, offset);
    size_t i = comment_at(&r->comments, offset);
    *to = i;
    size_t expected = line - 1;
    while (i > 0) {
        const Comment *c = &r->comments.items[i - 1];
        if (!c->own_line || !is_line_comment(r, c) || source_line_of(r->file, c->offset) != expected) break;
        i--;
        expected--;
    }
    *from = i;
    *trailing = (size_t)-1;
    for (size_t k = *to; k < r->comments.count; k++) {
        const Comment *c = &r->comments.items[k];
        size_t at = source_line_of(r->file, c->offset);
        if (at > end_line) break;
        if (at == end_line && !c->own_line && is_line_comment(r, c)) {
            *trailing = k;
            break;
        }
    }
}

typedef enum {
    WORD_COMPONENT,
    WORD_LABEL,
    WORD_RANGE,
    WORD_UNIT,
    WORD_GROUP,
    WORD_TOOLTIP,
    WORD_HIDDEN,
    WORD_TRANSIENT,
    WORD_ASSET,
    WORD_COLOR,
    WORD_COUNT,
} WordKind;

static const char *const word_names[WORD_COUNT] = {
    "component", "label", "range", "unit", "group", "tooltip", "hidden", "transient", "asset", "color",
};

/* What a comment says, word by word. */
typedef struct {
    WordKind kind;
    Word value;   /* label, unit, group, tooltip, component: the text; range: `low` */
    Word high;    /* range */
    size_t offset; /* of the word, in the file */
    size_t length;
} Said;

typedef struct {
    Said *items;
    size_t count;
    size_t cap;
} SaidList;

typedef struct {
    Reader *reader;
    const Read *r;
    const char *p;   /* in the text of the file */
    const char *end; /* of the comment */
} Cursor;

static size_t at_offset(const Cursor *c, const char *p) {
    return (size_t)(p - c->r->file->text);
}

static void skip_spaces(Cursor *c) {
    while (c->p < c->end && (*c->p == ' ' || *c->p == '\t')) c->p++;
}

static bool error_at(Cursor *c, const char *code, const char *p, size_t length, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    diag_verror(c->reader->diags, code, c->r->file, at_offset(c, p), length ? length : 1, fmt, args);
    va_end(args);
    return false;
}

static bool is_word_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

static int word_kind(const char *text, size_t length) {
    for (int k = 0; k < WORD_COUNT; k++) {
        if (strlen(word_names[k]) == length && memcmp(word_names[k], text, length) == 0) return k;
    }
    return -1;
}

/* A text in quotes, `\"` and `\\` read as the character they stand for. */
static bool read_text(Cursor *c, Word *out) {
    const char *start = c->p;
    if (c->p >= c->end || *c->p != '"') {
        return error_at(c, "E0604", c->p, 0, "a text in quotes is expected here");
    }
    c->p++;
    char *text = arena_alloc(&c->reader->words, (size_t)(c->end - c->p) + 1);
    size_t n = 0;
    while (c->p < c->end && *c->p != '"') {
        if (*c->p == '\\' && c->p + 1 < c->end && (c->p[1] == '"' || c->p[1] == '\\')) c->p++;
        text[n++] = *c->p++;
    }
    if (c->p >= c->end) {
        return error_at(c, "E0604", start, (size_t)(c->end - start), "this text has no closing quote");
    }
    c->p++;
    out->text = text;
    out->length = n;
    out->set = true;
    return true;
}

/* A number as Sosu writes it: `-2`, `0.5`. */
static bool read_number(Cursor *c, Word *out) {
    const char *start = c->p;
    if (c->p < c->end && *c->p == '-') c->p++;
    const char *digits = c->p;
    while (c->p < c->end && isdigit((unsigned char)*c->p)) c->p++;
    if (c->p == digits) {
        c->p = start;
        return error_at(c, "E0604", start, 0, "a number is expected here");
    }
    if (c->p + 1 < c->end && *c->p == '.' && isdigit((unsigned char)c->p[1])) {
        c->p++;
        while (c->p < c->end && isdigit((unsigned char)*c->p)) c->p++;
    }
    out->text = start;
    out->length = (size_t)(c->p - start);
    out->set = true;
    return true;
}

/* The end of a word and its value: the end of the comment, or `;` before the next. */
static bool read_end(Cursor *c, const char *word) {
    skip_spaces(c);
    if (c->p >= c->end) return true;
    if (*c->p == ';') {
        c->p++;
        return true;
    }
    const char *start = c->p;
    while (c->p < c->end && *c->p != ';') c->p++;
    return error_at(c, "E0604", start, (size_t)(c->p - start), "unexpected text after `%s`: words are separated by `;`",
                    word);
}

/* Whether the comment `[p, end)` is read as words: its first word is one, or it is a word
   followed by a text or a number (a word misspelled). */
static bool says_words(const char *p, const char *end) {
    while (p < end && (*p == ' ' || *p == '\t')) p++;
    const char *word = p;
    while (p < end && is_word_char(*p)) p++;
    if (p == word || !(word[0] >= 'a' && word[0] <= 'z')) return false;
    const char *after = p;
    while (after < end && (*after == ' ' || *after == '\t')) after++;
    bool value = after < end && (*after == '"' || isdigit((unsigned char)*after) || *after == '-');
    bool ends = after >= end || *after == ';';
    if (word_kind(word, (size_t)(p - word)) >= 0) return ends || value;
    return value && after > p;
}

/* The words of the comment `index` of `r`, added to `said`. False after an error. */
static bool read_comment(Reader *reader, const Read *r, size_t index, SaidList *said) {
    const Comment *comment = &r->comments.items[index];
    Cursor c = { reader, r, r->file->text + comment->offset + 2, r->file->text + comment->offset + comment->length };
    while (c.end > c.p && (c.end[-1] == ' ' || c.end[-1] == '\t' || c.end[-1] == '\r')) c.end--;
    if (!says_words(c.p, c.end)) return true;
    bool good = true;
    while (c.p < c.end) {
        skip_spaces(&c);
        if (c.p >= c.end) break;
        const char *word = c.p;
        while (c.p < c.end && is_word_char(*c.p)) c.p++;
        size_t length = (size_t)(c.p - word);
        int kind = word_kind(word, length);
        if (kind < 0) {
            const char *rest = c.p;
            while (c.p < c.end && *c.p != ';') c.p++;
            if (c.p < c.end) c.p++;
            Diagnostic *d = diag_error(reader->diags, "E0603", r->file, at_offset(&c, word), length ? length : (size_t)(rest - word) + 1,
                                       "unknown word `%.*s` in a comment of a component", (int)length, word);
            diag_help(d, "the words are component, label, range, unit, group, tooltip, hidden, transient, asset and color");
            good = false;
            continue;
        }
        Said s = { (WordKind)kind, {0}, {0}, at_offset(&c, word), length };
        skip_spaces(&c);
        bool ok = true;
        switch ((WordKind)kind) {
        case WORD_COMPONENT:
            if (c.p < c.end && *c.p == '"') ok = read_text(&c, &s.value);
            break;
        case WORD_LABEL:
        case WORD_UNIT:
        case WORD_GROUP:
        case WORD_TOOLTIP:
            ok = read_text(&c, &s.value);
            break;
        case WORD_RANGE:
            ok = read_number(&c, &s.value);
            if (ok) {
                if (c.p + 1 < c.end && c.p[0] == '.' && c.p[1] == '.') {
                    c.p += 2;
                    ok = read_number(&c, &s.high);
                } else {
                    ok = error_at(&c, "E0604", c.p, 0, "a range is written `low..high`: `range 0..10`");
                }
            }
            break;
        default:
            break;
        }
        if (ok) ok = read_end(&c, word_names[kind]);
        if (!ok) {
            while (c.p < c.end && *c.p != ';') c.p++;
            if (c.p < c.end) c.p++;
            good = false;
            continue;
        }
        SOSU_VEC_PUSH(said, s);
    }
    return good;
}

/* The words of the comments of what starts at `offset` and ends on `end_line`. */
static bool words_of(Reader *reader, const Read *r, size_t offset, size_t end_line, SaidList *said) {
    size_t from, to, trailing;
    comments_of(r, offset, end_line, &from, &to, &trailing);
    bool good = true;
    for (size_t i = from; i < to; i++) good = read_comment(reader, r, i, said) && good;
    if (trailing != (size_t)-1) good = read_comment(reader, r, trailing, said) && good;
    return good;
}

/* ---- Types ---- */

static int find_enum(Reader *reader, size_t source, const TypeExpr *type) {
    if (type->kind != TYPE_NAME || type->arg_count > 0) return -1;
    const Path *path = &type->as.name;
    for (size_t i = 0; i < reader->count; i++) {
        bool here;
        Name name;
        if (path->count == 1) {
            here = i == source;
            name = path->items[0];
        } else if (path->count == 2) {
            here = name_is(path->items[0], short_module(reader->sources[i].module));
            name = path->items[1];
        } else {
            return -1;
        }
        if (!here || !reader->read[i].module) continue;
        const Module *m = reader->read[i].module;
        for (size_t d = 0; d < m->count; d++) {
            const Decl *decl = m->decls[d];
            if (decl->kind != DECL_ENUM || decl->as.enum_.name.length != name.length ||
                memcmp(decl->as.enum_.name.text, name.text, name.length) != 0) {
                continue;
            }
            if (!decl->is_pub || decl->type_params.count > 0) return -2;
            for (size_t v = 0; v < decl->as.enum_.variant_count; v++) {
                if (decl->as.enum_.variants[v].field_count > 0 || decl->as.enum_.variants[v].has_parens) return -2;
            }
            for (size_t k = 0; k < reader->enums.count; k++) {
                if (reader->enums.items[k].decl == decl) return (int)k;
            }
            EnumRef ref = { decl, i };
            SOSU_VEC_PUSH(&reader->enums, ref);
            return (int)reader->enums.count - 1;
        }
    }
    return -1;
}

static bool type_of(Reader *reader, size_t source, const Field *field, Described *out) {
    const TypeExpr *type = field->type;
    const SourceFile *file = reader->read[source].file;
    if (type->kind == TYPE_NAME && type->arg_count == 0 && type->as.name.count == 1) {
        static const struct { const char *name; FieldType type; } plain[] = {
            { "f32", TYPE_F32 }, { "i32", TYPE_I32 }, { "i64", TYPE_I64 },
            { "usize", TYPE_USIZE }, { "bool", TYPE_BOOL }, { "str", TYPE_STR },
        };
        for (size_t i = 0; i < sizeof plain / sizeof plain[0]; i++) {
            if (name_is(type->as.name.items[0], plain[i].name)) {
                out->type = plain[i].type;
                return true;
            }
        }
    }
    if (type->kind == TYPE_NAME && type->arg_count == 0 && type->as.name.count == 2 &&
        name_is(type->as.name.items[0], "linear") && name_is(type->as.name.items[1], "Vec3")) {
        out->type = TYPE_VEC3;
        return true;
    }
    int e = find_enum(reader, source, type);
    if (e >= 0) {
        out->type = TYPE_ENUM;
        out->enum_index = (size_t)e;
        return true;
    }
    Diagnostic *d;
    if (e == -2) {
        d = diag_error(reader->diags, "E0602", file, type->offset, type->length,
                       "a field of a component can be an enum only if it is `pub`, not generic, and its variants carry nothing");
    } else if (type->kind == TYPE_SLICE) {
        d = diag_error(reader->diags, "E0602", file, type->offset, type->length,
                       "a list cannot be a field of a component yet");
    } else {
        d = diag_error(reader->diags, "E0602", file, type->offset, type->length,
                       "`%.*s` cannot be a field of a component", (int)type->length, file->text + type->offset);
    }
    diag_help(d, "a field is f32, i32, i64, usize, bool, str, linear.Vec3 or an enum of this package; "
                 "`// transient` leaves it out");
    return false;
}

/* Whether `w` is a whole number: no `.`. */
static bool is_whole(Word w) {
    return memchr(w.text, '.', w.length) == NULL;
}

static double number_of(Word w) {
    char buffer[64];
    size_t n = w.length < sizeof buffer - 1 ? w.length : sizeof buffer - 1;
    memcpy(buffer, w.text, n);
    buffer[n] = '\0';
    return strtod(buffer, NULL);
}

/* ---- Reading a component ---- */

static bool is_number(FieldType t) {
    return t == TYPE_F32;
}

static bool is_whole_type(FieldType t) {
    return t == TYPE_I32 || t == TYPE_I64 || t == TYPE_USIZE;
}

static void said_wrong(Reader *reader, const Read *r, const Said *s, const char *fmt) {
    diag_error(reader->diags, "E0605", r->file, s->offset, s->length, fmt, word_names[s->kind]);
}

static bool read_field(Reader *reader, size_t source, const Field *field, Word *group, DescribedList *out) {
    const Read *r = &reader->read[source];
    SaidList said = {0};
    size_t end_line = source_line_of(r->file, field->name.offset);
    bool good = words_of(reader, r, field->type->offset, end_line, &said);
    bool transient = false;
    for (size_t i = 0; i < said.count; i++) {
        if (said.items[i].kind == WORD_TRANSIENT) transient = true;
    }
    if (transient || !field->is_pub) {
        free(said.items);
        return good;
    }
    if (!field->is_mut) {
        Diagnostic *d = diag_error(reader->diags, "E0606", r->file, field->name.offset, field->name.length,
                                   "the field `%.*s` of a component is not `mut`: it could not be changed by the editor nor read from a scene",
                                   (int)field->name.length, field->name.text);
        diag_help(d, "write it `pub mut`, or leave it out with `// transient`");
        free(said.items);
        return false;
    }
    Described f;
    memset(&f, 0, sizeof f);
    f.field = field;
    if (!type_of(reader, source, field, &f)) {
        free(said.items);
        return false;
    }
    for (size_t i = 0; i < said.count; i++) {
        const Said *s = &said.items[i];
        switch (s->kind) {
        case WORD_COMPONENT:
            said_wrong(reader, r, s, "`%s` is written above a struct, not a field");
            good = false;
            break;
        case WORD_LABEL: f.label = s->value; break;
        case WORD_TOOLTIP: f.tooltip = s->value; break;
        case WORD_GROUP: *group = s->value; break;
        case WORD_HIDDEN: f.hidden = true; break;
        case WORD_TRANSIENT: break;
        case WORD_UNIT:
            if (is_number(f.type) || is_whole_type(f.type) || f.type == TYPE_VEC3) {
                f.unit = s->value;
            } else {
                said_wrong(reader, r, s, "`%s` is for a number or a vector");
                good = false;
            }
            break;
        case WORD_ASSET:
            if (f.type == TYPE_STR) {
                f.asset = true;
            } else {
                said_wrong(reader, r, s, "`%s` is for a field of type str");
                good = false;
            }
            break;
        case WORD_COLOR:
            if (f.type == TYPE_VEC3) {
                f.color = true;
            } else {
                said_wrong(reader, r, s, "`%s` is for a field of type linear.Vec3");
                good = false;
            }
            break;
        case WORD_RANGE:
            if (!is_number(f.type) && !is_whole_type(f.type)) {
                said_wrong(reader, r, s, "`%s` is for a number (f32, i32, i64 or usize)");
                good = false;
            } else if (is_whole_type(f.type) && (!is_whole(s->value) || !is_whole(s->high))) {
                diag_error(reader->diags, "E0604", r->file, s->offset, s->length,
                           "the range of a whole number is of whole numbers");
                good = false;
            } else if (!(number_of(s->value) < number_of(s->high))) {
                diag_error(reader->diags, "E0604", r->file, s->offset, s->length,
                           "a range goes from the lower bound to the higher: `range %.*s..%.*s`", (int)s->high.length,
                           s->high.text, (int)s->value.length, s->value.text);
                good = false;
            } else if (f.type == TYPE_USIZE && number_of(s->value) < 0) {
                diag_error(reader->diags, "E0604", r->file, s->offset, s->length,
                           "a usize is never below 0");
                good = false;
            } else {
                f.low = s->value;
                f.high = s->high;
            }
            break;
        case WORD_COUNT: break;
        }
    }
    free(said.items);
    if (f.asset && f.color) good = false; /* cannot happen: two types */
    f.group = *group;
    SOSU_VEC_PUSH(out, f);
    return good;
}

static bool reserved_table(const char *name) {
    static const char *const common[] = { "mesh", "light", "view", "body", "sound", "emitter", "animator", "text" };
    for (size_t i = 0; i < sizeof common / sizeof common[0]; i++) {
        if (strcmp(common[i], name) == 0) return true;
    }
    return false;
}

static const Said *component_mark(const SaidList *said) {
    for (size_t i = 0; i < said->count; i++) {
        if (said->items[i].kind == WORD_COMPONENT) return &said->items[i];
    }
    return NULL;
}

static Name decl_name(const Decl *decl) {
    Name none = { "", 0, decl->offset };
    switch (decl->kind) {
    case DECL_STRUCT:
    case DECL_CLASS:
    case DECL_INTERFACE: return decl->as.type.name;
    case DECL_ENUM: return decl->as.enum_.name;
    case DECL_FN: return decl->as.fn.name;
    default: return none;
    }
}

static bool read_decl(Reader *reader, size_t source, const Decl *decl) {
    const Read *r = &reader->read[source];
    SaidList said = {0};
    Name name = decl_name(decl);
    size_t end_line = source_line_of(r->file, name.length ? name.offset : decl->offset);
    bool good = words_of(reader, r, decl->offset, end_line, &said);
    const Said *mark = component_mark(&said);
    for (size_t i = 0; i < said.count; i++) {
        if (said.items[i].kind != WORD_COMPONENT) {
            said_wrong(reader, r, &said.items[i], "`%s` is written above a field of a component");
            good = false;
        }
    }
    if (!mark) {
        free(said.items);
        return good;
    }
    if (decl->kind != DECL_STRUCT || !decl->is_pub || decl->type_params.count > 0) {
        Diagnostic *d = diag_error(reader->diags, "E0601", r->file, mark->offset, mark->length,
                                   "a component is a `pub struct` that is not generic");
        diag_help(d, "a class keeps its place in memory and an enum is a choice: a component is plain values");
        free(said.items);
        return false;
    }
    Component c;
    memset(&c, 0, sizeof c);
    c.decl = decl;
    c.source = source;
    if (mark->value.set) {
        if (!is_table_name(mark->value.text, mark->value.length)) {
            diag_error(reader->diags, "E0601", r->file, mark->offset, mark->length,
                       "the name of a component is lowercase letters, digits and `_`, a letter first: `door_lock`");
            free(said.items);
            return false;
        }
        c.table = sosu_format("%.*s", (int)mark->value.length, mark->value.text);
    } else {
        c.table = snake(decl->as.type.name);
    }
    if (reserved_table(c.table)) {
        diag_error(reader->diags, "E0601", r->file, mark->offset, mark->length,
                   "`%s` is the name of a component of engine", c.table);
        free(c.table);
        free(said.items);
        return false;
    }
    for (size_t i = 0; i < reader->components.count; i++) {
        const Component *other = &reader->components.items[i];
        bool same_table = strcmp(other->table, c.table) == 0;
        Name a = other->decl->as.type.name, b = decl->as.type.name;
        bool same_struct = a.length == b.length && memcmp(a.text, b.text, a.length) == 0;
        if (same_table || same_struct) {
            if (same_table) {
                diag_error(reader->diags, "E0601", r->file, mark->offset, mark->length, "two components named `%s`",
                           c.table);
            } else {
                diag_error(reader->diags, "E0601", r->file, mark->offset, mark->length,
                           "two components of the same struct name `%.*s`", (int)b.length, b.text);
            }
            free(c.table);
            free(said.items);
            return false;
        }
    }
    free(said.items);
    /* `fresh` makes one with `T{ }`, which gives every field its default */
    for (size_t i = 0; i < decl->as.type.field_count; i++) {
        const Field *f = &decl->as.type.fields[i];
        if (f->init) continue;
        Diagnostic *d = diag_error(reader->diags, "E0607", r->file, f->name.offset, f->name.length,
                                   "the field `%.*s` of a component has no default value", (int)f->name.length, f->name.text);
        diag_help(d, "a component is made new with `%.*s{ }`: give it one, `= 0.0`", (int)decl->as.type.name.length,
                  decl->as.type.name.text);
        good = false;
    }
    Word group = {0};
    for (size_t i = 0; i < decl->as.type.field_count; i++) {
        good = read_field(reader, source, &decl->as.type.fields[i], &group, &c.fields) && good;
    }
    SOSU_VEC_PUSH(&reader->components, c);
    return good;
}

/* ---- Writing ---- */

static void put_name(Out *out, Name name) {
    put(out, "%.*s", (int)name.length, name.text);
}

/* A number of a comment as a literal of f32: `2` -> `2.0`. */
static void put_f32(Out *out, Word w) {
    put(out, "%.*s%s", (int)w.length, w.text, is_whole(w) ? ".0" : "");
}

/* `parts_mode`: what names the functions of an enum. */
static char *enum_prefix(const Reader *reader, const EnumRef *e) {
    char *name = snake(e->decl->as.enum_.name);
    char *prefix = sosu_format("%s_%s", short_module(reader->sources[e->source].module), name);
    free(name);
    return prefix;
}

static void put_field(Out *out, const Described *f, size_t index) {
    static const char *const kinds[] = { "Number", "Whole", "Whole", "Whole", "Flag", "Text", "Vector", "Choice" };
    const char *kind = f->asset ? "Asset" : f->color ? "Color" : kinds[f->type];
    put(out, "describe.Field{ name: ");
    put_string(out, f->field->name.text, f->field->name.length);
    put(out, ", label: ");
    if (f->label.set) {
        put_string(out, f->label.text, f->label.length);
    } else {
        char *words = in_words(f->field->name);
        put_string(out, words, strlen(words));
        free(words);
    }
    put(out, ", kind: describe.Kind.%s", kind);
    if (f->low.set) {
        put(out, ", low: ");
        put_f32(out, f->low);
        put(out, ", high: ");
        put_f32(out, f->high);
    }
    if (f->type == TYPE_ENUM) put(out, ", choices: some(choices_%zu)", index);
    const Word *texts[3] = { &f->unit, &f->group, &f->tooltip };
    const char *names[3] = { "unit", "group", "tooltip" };
    for (size_t i = 0; i < 3; i++) {
        if (!texts[i]->set) continue;
        put(out, ", %s: ", names[i]);
        put_string(out, texts[i]->text, texts[i]->length);
    }
    if (f->hidden) put(out, ", hidden: true");
    put(out, " }");
}

static void put_get(Out *out, const Reader *reader, const Described *f) {
    Name n = f->field->name;
    int length = (int)n.length;
    switch (f->type) {
    case TYPE_F32: put(out, "describe.Value.Number(c.%.*s)", length, n.text); break;
    case TYPE_I64: put(out, "describe.Value.Whole(c.%.*s)", length, n.text); break;
    case TYPE_I32:
    case TYPE_USIZE: put(out, "describe.Value.Whole(c.%.*s as i64)", length, n.text); break;
    case TYPE_BOOL: put(out, "describe.Value.Flag(c.%.*s)", length, n.text); break;
    case TYPE_STR: put(out, "describe.Value.Text(c.%.*s)", length, n.text); break;
    case TYPE_VEC3: put(out, "describe.Value.Vector(c.%.*s)", length, n.text); break;
    case TYPE_ENUM: {
        char *prefix = enum_prefix(reader, &reader->enums.items[f->enum_index]);
        put(out, "describe.Value.Choice(%s_index(c.%.*s))", prefix, length, n.text);
        free(prefix);
        break;
    }
    }
}

static void put_set(Out *out, const Reader *reader, const Described *f) {
    Name n = f->field->name;
    int length = (int)n.length;
    static const char *const of[] = { "number_of", "i32_of", "whole_of", "usize_of", "flag_of", "text_of", "vector_of" };
    if (f->type == TYPE_ENUM) {
        char *prefix = enum_prefix(reader, &reader->enums.items[f->enum_index]);
        put(out, "c.%.*s = %s_at(describe.choice_of(v, %s_index(c.%.*s)));", length, n.text, prefix, prefix, length,
            n.text);
        free(prefix);
        return;
    }
    put(out, "c.%.*s = describe.%s(v, c.%.*s);", length, n.text, of[f->type], length, n.text);
}

static void put_component(Out *out, const Reader *reader, const Component *c) {
    Name s = c->decl->as.type.name;
    int sl = (int)s.length;
    char *t = sosu_format("%s.%.*s", short_module(reader->sources[c->source].module), sl, s.text);
    size_t n = c->fields.count;
    bool choices = false;
    for (size_t i = 0; i < n; i++) choices = choices || c->fields.items[i].type == TYPE_ENUM;

    put(out, "\n// ---- %s: %s ----\n\n", c->table, t);
    put(out, "// What describes `%s` and saves it.\n", t);
    put(out, "pub class %.*sFields : describe.Describe<%s>, save.Codec<%s> {\n", sl, s.text, t, t);
    for (size_t i = 0; i < n; i++) {
        if (c->fields.items[i].type == TYPE_ENUM) put(out, "    str[] choices_%zu;\n", i);
    }
    if (choices) {
        put(out, "\n    void init(keeps Arena* arena) {\n");
        for (size_t i = 0; i < n; i++) {
            const Described *f = &c->fields.items[i];
            if (f->type != TYPE_ENUM) continue;
            const Decl *e = reader->enums.items[f->enum_index].decl;
            size_t count = e->as.enum_.variant_count;
            put(out, "        mut str[] made_%zu = arena.alloc_slice(str, %zu);\n", i, count);
            for (size_t v = 0; v < count; v++) {
                char *name = snake(e->as.enum_.variants[v].name);
                put(out, "        made_%zu[%zu] = \"%s\";\n", i, v, name);
                free(name);
            }
            put(out, "        choices_%zu = made_%zu;\n", i, i);
        }
        put(out, "    }\n");
    } else {
        put(out, "    void init() { }\n");
    }
    put(out, "\n    override str name() {\n        return \"%s\";\n    }\n", c->table);
    put(out, "\n    override %s fresh() {\n        return %s{ };\n    }\n", t, t);
    put(out, "\n    override usize count() {\n        return %zu;\n    }\n", n);

    put(out, "\n    override describe.Field field(usize i) {\n");
    for (size_t i = 0; i + 1 < n; i++) {
        put(out, "        if (i == %zu) {\n            return ", i);
        put_field(out, &c->fields.items[i], i);
        put(out, ";\n        }\n");
    }
    if (n == 0) {
        put(out, "        return describe.Field{ name: \"\", label: \"\", kind: describe.Kind.Flag };\n");
    } else {
        put(out, "        return ");
        put_field(out, &c->fields.items[n - 1], n - 1);
        put(out, ";\n");
    }
    put(out, "    }\n");

    put(out, "\n    override describe.Value get(%s* c, usize i) {\n", t);
    for (size_t i = 0; i + 1 < n; i++) {
        put(out, "        if (i == %zu) {\n            return ", i);
        put_get(out, reader, &c->fields.items[i]);
        put(out, ";\n        }\n");
    }
    put(out, "        return ");
    if (n == 0) put(out, "describe.Value.Flag(false)");
    else put_get(out, reader, &c->fields.items[n - 1]);
    put(out, ";\n    }\n");

    put(out, "\n    override void set(%s* c, usize i, describe.Value v) {\n", t);
    for (size_t i = 0; i < n; i++) {
        if (n == 1) {
            put(out, "        ");
            put_set(out, reader, &c->fields.items[i]);
            put(out, "\n");
            break;
        }
        if (i == 0) put(out, "        if (i == 0) {\n");
        else if (i + 1 < n) put(out, " else if (i == %zu) {\n", i);
        else put(out, " else {\n");
        put(out, "            ");
        put_set(out, reader, &c->fields.items[i]);
        put(out, "\n        }");
        if (i + 1 == n) put(out, "\n");
    }
    put(out, "    }\n");

    put(out, "\n    override void write(text.TextBuilder* out, %s* value) {\n", t);
    put(out, "        describe.write_object(out, self as describe.Describe<%s>*, value);\n    }\n", t);
    put(out, "\n    override %s? read(Arena* arena, json.Value v) {\n", t);
    put(out, "        return describe.read_object(arena, self as describe.Describe<%s>*, v);\n    }\n}\n", t);

    put(out, "\n// `%s` a kind of component of the editor, saved with the scene.\n", t);
    put(out, "pub class %.*sKind : kinds.Kind, host.Saved {\n", sl, s.text);
    put(out, "    Arena* arena;\n    scene.Table<%s>* table;\n    %.*sFields* fields;\n\n", t, sl, s.text);
    put(out, "    void init(keeps Arena* arena, keeps scene.Table<%s>* table) {\n", t);
    put(out, "        self.arena = arena;\n        self.table = table;\n");
    if (choices) put(out, "        fields = arena.new(%.*sFields, arena);\n    }\n", sl, s.text);
    else put(out, "        fields = arena.new(%.*sFields);\n    }\n", sl, s.text);
    put(out, "\n    describe.Describe<%s>* d() {\n        return fields as describe.Describe<%s>*;\n    }\n", t, t);
    put(out, "\n    override str name() {\n        return d().name();\n    }\n");
    put(out, "\n    override usize count() {\n        return d().count();\n    }\n");
    put(out, "\n    override describe.Field field(usize i) {\n        return d().field(i);\n    }\n");
    put(out, "\n    override bool has(scene.Entity e) {\n        return table.has(e);\n    }\n");
    put(out, "\n    override void add(scene.Entity e) {\n        kinds.table_add(table, d(), e);\n    }\n");
    put(out, "\n    override void remove(scene.Entity e) {\n        table.remove(e);\n    }\n");
    put(out, "\n    override describe.Value? get(scene.Entity e, usize i) {\n"
             "        return kinds.table_get(table, d(), e, i);\n    }\n");
    put(out, "\n    override describe.Value? fresh(usize i) {\n        return kinds.table_fresh(d(), i);\n    }\n");
    put(out, "\n    override bool set(scene.Entity e, usize i, describe.Value v) {\n"
             "        return kinds.table_set(arena, table, d(), e, i, v);\n    }\n");
    put(out, "\n    override void write(save.Writer* out) {\n        out.add(table, fields as save.Codec<%s>*);\n    }\n", t);
    put(out, "\n    override bool read(save.Reader* r) {\n        return r.add(table, fields as save.Codec<%s>*);\n    }\n}\n",
        t);

    put(out, "\n// The kind of `%s`, on its table in `world`.\n", t);
    put(out, "pub %.*sKind* %s_kind(keeps Arena* arena, keeps scene.World* world) {\n", sl, s.text, c->table);
    put(out, "    return arena.new(%.*sKind, arena, world.table<%s>());\n}\n", sl, s.text, t);
    free(t);
}

static void put_enum(Out *out, const Reader *reader, const EnumRef *e) {
    char *prefix = enum_prefix(reader, e);
    const char *module = short_module(reader->sources[e->source].module);
    Name name = e->decl->as.enum_.name;
    int nl = (int)name.length;
    size_t count = e->decl->as.enum_.variant_count;
    put(out, "\n// The index of a variant of `%s.%.*s`, and the variant of an index.\n", module, nl, name.text);
    put(out, "usize %s_index(%s.%.*s x) {\n    return match (x) {\n", prefix, module, nl, name.text);
    for (size_t v = 0; v < count; v++) {
        put(out, "        ");
        put_name(out, e->decl->as.enum_.variants[v].name);
        put(out, " => %zu,\n", v);
    }
    put(out, "    };\n}\n");
    put(out, "\n%s.%.*s %s_at(usize i) {\n", module, nl, name.text, prefix);
    for (size_t v = 1; v < count; v++) {
        put(out, "    if (i == %zu) {\n        return %s.%.*s.", v, module, nl, name.text);
        put_name(out, e->decl->as.enum_.variants[v].name);
        put(out, ";\n    }\n");
    }
    put(out, "    return %s.%.*s.", module, nl, name.text);
    put_name(out, e->decl->as.enum_.variants[0].name);
    put(out, ";\n}\n");
    free(prefix);
}

static int compare_text(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static char *write_module(const char *package, const Reader *reader) {
    Out out = {0};
    put(&out,
        "// Generated by `sosuc describe` from the comments `// component` of the package %s: do not edit it,\n"
        "// it is written again at each change (spec 11.4).\n"
        "//\n"
        "// For each component, the class that describes and saves it, the class that makes it a kind of\n"
        "// the editor, and a function that makes that kind; `add_all` adds them all to a level.\n",
        package);
    /* the imports, sorted */
    const char **imports = sosu_alloc((reader->count + 8) * sizeof *imports);
    size_t count = 0;
    if (reader->components.count > 0) {
        imports[count++] = "std.json";
        imports[count++] = "std.text";
        imports[count++] = "scene.scene";
        imports[count++] = "scene.save";
        imports[count++] = "engine.describe";
        imports[count++] = "engine.kinds";
    }
    imports[count++] = "engine.host";
    bool vec3 = false;
    for (size_t i = 0; i < reader->components.count; i++) {
        const Component *c = &reader->components.items[i];
        for (size_t f = 0; f < c->fields.count; f++) vec3 = vec3 || c->fields.items[f].type == TYPE_VEC3;
    }
    if (vec3) imports[count++] = "math.linear";
    char **owned = sosu_calloc(reader->count + 1, sizeof *owned);
    for (size_t i = 0; i < reader->count; i++) {
        bool used = false;
        for (size_t c = 0; c < reader->components.count; c++) used = used || reader->components.items[c].source == i;
        for (size_t e = 0; e < reader->enums.count; e++) used = used || reader->enums.items[e].source == i;
        if (!used) continue;
        owned[i] = sosu_format("%s.%s", package, reader->sources[i].module);
        imports[count++] = owned[i];
    }
    qsort(imports, count, sizeof *imports, compare_text);
    for (size_t i = 0; i < count; i++) {
        if (i > 0 && strcmp(imports[i], imports[i - 1]) == 0) continue;
        put(&out, "import %s;\n", imports[i]);
    }
    for (size_t i = 0; i < reader->count; i++) free(owned[i]);
    free(owned);
    free(imports);

    for (size_t i = 0; i < reader->components.count; i++) put_component(&out, reader, &reader->components.items[i]);
    for (size_t i = 0; i < reader->enums.count; i++) put_enum(&out, reader, &reader->enums.items[i]);

    put(&out, "\n// Every component of the package, a kind of the editor saved with the scene: what `sozo_setup`\n"
              "// gives `host.on_world`.\n"
              "pub void add_all(host.Level* l) {\n");
    for (size_t i = 0; i < reader->components.count; i++) {
        const Component *c = &reader->components.items[i];
        put(&out, "    %.*sKind* k%zu = %s_kind(l.arena, l.world);\n", (int)c->decl->as.type.name.length,
            c->decl->as.type.name.text, i, c->table);
        put(&out, "    l.add(k%zu as kinds.Kind*);\n    l.add_saved(k%zu as host.Saved*);\n", i, i);
        /* where its struct is written, for the editor to open: `src/a/b.sosu` of module `a.b` */
        char *path = sosu_format("src/%s.sosu", reader->sources[c->source].module);
        for (char *p = path; *p; p++) {
            if (*p == '.' && strcmp(p, ".sosu") != 0) *p = '/';
        }
        put(&out, "    l.add_source(\"%s\", \"%s\", %zu);\n", c->table, path,
            source_line_of(reader->sources[c->source].file, c->decl->offset));
        free(path);
    }
    put(&out, "}\n");
    SOSU_VEC_PUSH(&out, '\0');
    return out.items;
}

/* ---- Entry point ---- */

static bool is_name(Name n, const char *text) {
    return n.length == strlen(text) && memcmp(n.text, text, n.length) == 0;
}

/* Components described, but no module imports the module written: `add_all` is never called, and
   the editor would not see them (a project made before `sosuc describe`). A warning, on the first. */
static void warn_unused(const char *package, const Reader *reader) {
    for (size_t i = 0; i < reader->count; i++) {
        const Module *m = reader->read[i].module;
        for (size_t d = 0; m && d < m->count; d++) {
            const Decl *decl = m->decls[d];
            if (decl->kind == DECL_IMPORT && decl->as.import.count == 2 && is_name(decl->as.import.items[0], package) &&
                is_name(decl->as.import.items[1], DESCRIBE_MODULE)) {
                return;
            }
        }
    }
    const Component *c = &reader->components.items[0];
    Diagnostic *d = warning(reader->diags, "E0608", reader->sources[c->source].file, c->decl->offset, c->decl->length,
                            "components are described, but no module imports `%s.%s`: the editor will not see them",
                            package, DESCRIBE_MODULE);
    diag_help(d, "import `%s.%s` and call `host.on_world(g, described.add_all);` in `sozo_setup`", package,
              DESCRIBE_MODULE);
}

char *describe_package(const char *package, const DescribeSource *sources, size_t count, DiagList *diags,
                       size_t *components) {
    Reader reader;
    memset(&reader, 0, sizeof reader);
    reader.sources = sources;
    reader.count = count;
    reader.diags = diags;
    reader.read = sosu_calloc(count + 1, sizeof *reader.read);
    Arena trees = {0};
    size_t errors = diags->error_count;
    for (size_t i = 0; i < count; i++) {
        Read *r = &reader.read[i];
        r->file = sources[i].file;
        if (!lex_file_comments(r->file, &r->tokens, &r->comments, diags)) continue;
        r->module = parse_module(r->file, &r->tokens, &trees, diags);
    }
    if (diags->error_count == errors) {
        for (size_t i = 0; i < count; i++) {
            const Module *m = reader.read[i].module;
            for (size_t d = 0; m && d < m->count; d++) read_decl(&reader, i, m->decls[d]);
        }
    }
    if (diags->error_count == errors && reader.components.count > 0) warn_unused(package, &reader);
    char *text = NULL;
    if (diags->error_count == errors) {
        /* in the canonical form of `sosuc fmt`, so that the package stays formatted */
        char *written = write_module(package, &reader);
        SourceFile file;
        source_from_string(&file, DESCRIBE_MODULE ".sosu", written);
        text = format_source(&file, diags);
        source_free(&file);
        free(written);
    }
    if (components) *components = reader.components.count;
    for (size_t i = 0; i < reader.components.count; i++) {
        free(reader.components.items[i].table);
        free(reader.components.items[i].fields.items);
    }
    free(reader.components.items);
    free(reader.enums.items);
    for (size_t i = 0; i < count; i++) {
        token_list_free(&reader.read[i].tokens);
        free(reader.read[i].comments.items);
    }
    free(reader.read);
    arena_free(&trees);
    arena_free(&reader.words);
    return text;
}
