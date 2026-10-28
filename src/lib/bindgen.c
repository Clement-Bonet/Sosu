#include "bindgen.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* `sosuc bindgen` (spec §11.6). The preprocessed header is read as tokens; declarations are
   parsed with their types (typedefs, structs, unions, enums, functions), the constants are
   evaluated, the layouts computed for the target; then the Sosu module is written, with the
   declarations of the bound headers whose names have the prefixes, and the types they need. */

/* ---- Text ---- */

typedef struct {
    char *text;
    size_t length;
    size_t cap;
} Buf;

static void buf_add(Buf *b, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    char *piece = sosu_vformat(fmt, args);
    va_end(args);
    size_t n = strlen(piece);
    if (b->length + n + 1 > b->cap) {
        b->cap = (b->length + n + 1) * 2;
        b->text = sosu_realloc(b->text, b->cap);
    }
    memcpy(b->text + b->length, piece, n + 1);
    b->length += n;
    free(piece);
}

static char *buf_take(Buf *b) {
    if (!b->text) return sosu_strdup("");
    char *text = b->text;
    b->text = NULL;
    b->length = b->cap = 0;
    return text;
}

static char *dup_n(const char *text, size_t length) {
    char *copy = sosu_alloc(length + 1);
    memcpy(copy, text, length);
    copy[length] = 0;
    return copy;
}

/* ---- Files ---- */

/* A path of a line marker: `\\` unescaped, `/` as separator. */
static char *marker_path(const char *text, size_t length) {
    char *path = sosu_alloc(length + 1);
    size_t n = 0;
    for (size_t i = 0; i < length; i++) {
        char c = text[i];
        if (c == '\\' && i + 1 < length && text[i + 1] == '\\') i++;
        path[n++] = c == '\\' ? '/' : c;
    }
    path[n] = 0;
    /* `a/./b` and `a//b` */
    char *out = path;
    size_t w = 0;
    for (size_t r = 0; path[r]; r++) {
        if (path[r] == '/' && path[r + 1] == '.' && path[r + 2] == '/') {
            r++;
            continue;
        }
        if (path[r] == '/' && w > 0 && out[w - 1] == '/') continue;
        out[w++] = path[r];
    }
    out[w] = 0;
    return path;
}

static bool same_char(char a, char b) {
#ifdef _WIN32
    return tolower((unsigned char)a) == tolower((unsigned char)b);
#else
    return a == b;
#endif
}

bool bindgen_is_own(const char *path, const char *const *dirs, size_t count) {
    for (size_t i = 0; i < count; i++) {
        const char *dir = dirs[i];
        size_t n = strlen(dir);
        while (n > 0 && dir[n - 1] == '/') n--;
        size_t j = 0;
        while (j < n && path[j] && same_char(path[j], dir[j])) j++;
        if (j == n && (path[j] == '/' || n == 0)) return true;
    }
    return false;
}

/* Calls `found` for each line marker (`# 12 "file"`, `#line 12 "file"`) of `text`. */
static size_t marker_at(const char *text, size_t length, size_t pos, const char **path, size_t *path_length) {
    /* `pos` is the `#` at the start of a line; returns the end of the line */
    size_t i = pos + 1;
    while (i < length && (text[i] == ' ' || text[i] == '\t')) i++;
    if (i + 4 <= length && strncmp(text + i, "line", 4) == 0) i += 4;
    while (i < length && (text[i] == ' ' || text[i] == '\t')) i++;
    *path = NULL;
    if (i < length && isdigit((unsigned char)text[i])) {
        while (i < length && isdigit((unsigned char)text[i])) i++;
        while (i < length && (text[i] == ' ' || text[i] == '\t')) i++;
        if (i < length && text[i] == '"') {
            size_t start = ++i;
            while (i < length && text[i] != '"' && text[i] != '\n') {
                if (text[i] == '\\' && i + 1 < length) i++;
                i++;
            }
            *path = text + start;
            *path_length = i - start;
        }
    }
    while (i < length && text[i] != '\n') i++;
    return i;
}

void bindgen_files(const char *text, size_t length, BindgenNames *files) {
    bool line_start = true;
    for (size_t i = 0; i < length; i++) {
        char c = text[i];
        if (c == '\n') {
            line_start = true;
            continue;
        }
        if (line_start && c == '#') {
            const char *path;
            size_t path_length = 0;
            i = marker_at(text, length, i, &path, &path_length) - 1;
            if (path) {
                char *normal = marker_path(path, path_length);
                bool known = false;
                for (size_t j = 0; j < files->count && !known; j++) known = strcmp(files->items[j], normal) == 0;
                if (known) free(normal);
                else SOSU_VEC_PUSH(files, normal);
            }
            continue;
        }
        if (c != ' ' && c != '\t' && c != '\r') line_start = false;
    }
}

static bool has_prefix(const char *name, size_t length, const char *const *prefixes, size_t count) {
    if (count == 0) return true;
    for (size_t i = 0; i < count; i++) {
        size_t n = strlen(prefixes[i]);
        if (length >= n && strncmp(name, prefixes[i], n) == 0) return true;
    }
    return false;
}

static bool ident_start(char c) { return isalpha((unsigned char)c) || c == '_'; }
static bool ident_char(char c) { return isalnum((unsigned char)c) || c == '_'; }

void bindgen_macro_names(const char *source, size_t length, const char *const *prefixes, size_t prefix_count,
                         BindgenNames *names) {
    size_t i = 0;
    while (i < length) {
        size_t line = i;
        while (i < length && (source[i] == ' ' || source[i] == '\t')) i++;
        if (i < length && source[i] == '#') {
            i++;
            while (i < length && (source[i] == ' ' || source[i] == '\t')) i++;
            if (i + 6 < length && strncmp(source + i, "define", 6) == 0 && (source[i + 6] == ' ' || source[i + 6] == '\t')) {
                i += 6;
                while (i < length && (source[i] == ' ' || source[i] == '\t')) i++;
                size_t start = i;
                while (i < length && ident_char(source[i])) i++;
                size_t n = i - start;
                bool function_like = i < length && source[i] == '(';
                /* a value that depends on where it is used (`__LINE__`...) is not a constant */
                size_t end = i;
                while (end < length && source[end] != '\n') end++;
                bool placed = false;
                static const char *const places[] = { "__LINE__", "__FILE__", "__COUNTER__", "__DATE__", "__TIME__",
                                                      "__func__", "__FUNCTION__" };
                for (size_t k = 0; k < sizeof places / sizeof *places && !placed; k++) {
                    size_t m = strlen(places[k]);
                    for (size_t j = i; j + m <= end && !placed; j++) placed = strncmp(source + j, places[k], m) == 0;
                }
                if (n > 0 && ident_start(source[start]) && !function_like && !placed &&
                    has_prefix(source + start, n, prefixes, prefix_count)) {
                    bool known = false;
                    for (size_t j = 0; j < names->count && !known; j++) {
                        known = strlen(names->items[j]) == n && strncmp(names->items[j], source + start, n) == 0;
                    }
                    if (!known) SOSU_VEC_PUSH(names, dup_n(source + start, n));
                }
            }
        }
        /* the end of the line, with its continuations */
        i = line;
        while (i < length && source[i] != '\n') {
            if (source[i] == '\\' && i + 1 < length && (source[i + 1] == '\n' || source[i + 1] == '\r')) {
                i++;
                if (source[i] == '\r' && i + 1 < length && source[i + 1] == '\n') i++;
            }
            i++;
        }
        i++;
    }
}

void bindgen_names_free(BindgenNames *names) {
    for (size_t i = 0; i < names->count; i++) free(names->items[i]);
    free(names->items);
    memset(names, 0, sizeof *names);
}

/* ---- Tokens ---- */

typedef enum {
    TK_IDENT,
    TK_NUMBER,
    TK_STRING,
    TK_CHAR,
    TK_PUNCT,
    TK_EOF,
} TokKind;

typedef struct {
    TokKind kind;
    const char *text;
    size_t length;
    int file; /* index in the files, -1 before the first marker */
} Tok;

typedef struct {
    Tok *items;
    size_t count;
    size_t cap;
} Toks;

typedef struct {
    char *path;
    bool own;
} File;

typedef struct {
    File *items;
    size_t count;
    size_t cap;
} Files;

static const char *const puncts[] = { "...", "<<=", ">>=", "->", "++", "--", "<<", ">>", "<=", ">=", "==", "!=", "&&",
                                      "||", "*=", "/=", "%=", "+=", "-=", "&=", "^=", "|=", "##", "::", "[[", "]]" };

static void lex(const BindgenInput *in, Toks *toks, Files *files) {
    const char *s = in->text;
    size_t n = in->length;
    int file = -1;
    bool line_start = true;
    size_t i = 0;
    while (i < n) {
        char c = s[i];
        if (c == '\n') {
            line_start = true;
            i++;
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v') {
            i++;
            continue;
        }
        if (line_start && c == '#') {
            const char *path;
            size_t path_length = 0;
            i = marker_at(s, n, i, &path, &path_length);
            if (path) {
                char *normal = marker_path(path, path_length);
                file = -1;
                for (size_t j = 0; j < files->count && file < 0; j++) {
                    if (strcmp(files->items[j].path, normal) == 0) file = (int)j;
                }
                if (file < 0) {
                    File f = { normal, bindgen_is_own(normal, in->own_dirs, in->own_dir_count) };
                    SOSU_VEC_PUSH(files, f);
                    file = (int)files->count - 1;
                } else {
                    free(normal);
                }
            }
            continue;
        }
        line_start = false;
        Tok t = { TK_PUNCT, s + i, 1, file };
        /* comments, in case the preprocessor kept some */
        if (c == '/' && i + 1 < n && s[i + 1] == '/') {
            while (i < n && s[i] != '\n') i++;
            continue;
        }
        if (c == '/' && i + 1 < n && s[i + 1] == '*') {
            i += 2;
            while (i + 1 < n && !(s[i] == '*' && s[i + 1] == '/')) i++;
            i += 2;
            continue;
        }
        size_t prefix = 0; /* L"", u8"", u'' */
        if (c == 'L' || c == 'U' || c == 'u') {
            prefix = (c == 'u' && i + 1 < n && s[i + 1] == '8') ? 2 : 1;
            if (!(i + prefix < n && (s[i + prefix] == '"' || s[i + prefix] == '\''))) prefix = 0;
        }
        if (s[i + prefix] == '"' || s[i + prefix] == '\'') {
            char quote = s[i + prefix];
            size_t j = i + prefix + 1;
            while (j < n && s[j] != quote && s[j] != '\n') {
                if (s[j] == '\\' && j + 1 < n) j++;
                j++;
            }
            t.kind = quote == '"' ? TK_STRING : TK_CHAR;
            t.length = j + 1 - i;
            i = j + 1;
        } else if (ident_start(c) || c == '$') {
            size_t j = i;
            while (j < n && (ident_char(s[j]) || s[j] == '$')) j++;
            t.kind = TK_IDENT;
            t.length = j - i;
            i = j;
        } else if (isdigit((unsigned char)c) || (c == '.' && i + 1 < n && isdigit((unsigned char)s[i + 1]))) {
            size_t j = i + 1;
            while (j < n) {
                if ((s[j] == '+' || s[j] == '-') && strchr("eEpP", s[j - 1])) j++;
                else if (ident_char(s[j]) || s[j] == '.' || s[j] == '\'') j++;
                else break;
            }
            t.kind = TK_NUMBER;
            t.length = j - i;
            i = j;
        } else {
            for (size_t k = 0; k < sizeof puncts / sizeof *puncts; k++) {
                size_t len = strlen(puncts[k]);
                if (i + len <= n && strncmp(s + i, puncts[k], len) == 0) {
                    t.length = len;
                    break;
                }
            }
            i += t.length;
        }
        SOSU_VEC_PUSH(toks, t);
    }
    Tok eof = { TK_EOF, s + n, 0, file };
    SOSU_VEC_PUSH(toks, eof);
}

/* ---- Types ---- */

typedef enum {
    CK_VOID,
    CK_INT,
    CK_FLOAT,
    CK_BOOL,
    CK_POINTER,
    CK_ARRAY,
    CK_FUNC,
    CK_RECORD,
    CK_ENUM,
    CK_TYPEDEF,
    CK_UNKNOWN, /* a type the parser could not read: anything using it is left out */
} CKind;

typedef struct Record Record;
typedef struct EnumDef EnumDef;
typedef struct CType CType;

struct CType {
    CKind kind;
    unsigned size;   /* INT, FLOAT: bytes */
    bool is_signed;  /* INT */
    bool is_char;    /* INT: plain `char`, bound as `ui8` */
    bool is_long;    /* INT: `long`, whose size depends on the system */
    CType *inner;    /* POINTER, ARRAY: element, FUNC: result, TYPEDEF: the type named */
    uint64_t count;  /* ARRAY: 0 when unknown (`int a[]`) */
    CType **params;  /* FUNC */
    char **param_names;
    size_t param_count;
    bool variadic;
    Record *record;  /* RECORD */
    EnumDef *enum_;  /* ENUM */
    char *name;      /* TYPEDEF: its name */
};

typedef struct {
    char *name; /* NULL: an anonymous struct or union member */
    CType *type;
    int bits;   /* a bit field: its width; -1 otherwise */
    uint64_t offset;
} RField;

struct Record {
    char *tag;
    char *name; /* the typedef that names it, or its tag */
    bool is_union;
    bool complete;
    bool own;   /* defined in a bound header */
    RField *fields;
    size_t field_count;
    bool has_bits;
    /* layout */
    int laid;   /* 0: not yet, 1: in progress, 2: done, 3: impossible */
    uint64_t size;
    uint64_t align;
    /* emission */
    bool needed;
    bool emitted;
    char *sosu_name;
};

typedef struct {
    char *name;
    int64_t value;
    bool known;
    bool own;
} Enumerator;

struct EnumDef {
    char *tag;
    char *name;
    Enumerator *items;
    size_t count;
    size_t cap;
    CType *fixed; /* `enum E : ui8` (C23) */
};

typedef struct {
    char *name;
    CType *type;
    bool own;
} Typedef;

typedef struct {
    char *name;
    CType *type;
    bool own;
    bool is_static;
} Function;

typedef struct {
    char *name;
    size_t first; /* tokens of the expansion */
    size_t end;
    int state;    /* 0: not evaluated, 1: in progress, 2: done */
    bool ok;
} Macro;

/* ---- Values of constant expressions ---- */

typedef enum { V_INT, V_FLOAT, V_STRING } VKind;

typedef struct {
    VKind kind;
    uint64_t bits;   /* V_INT, in `width` bits */
    unsigned width;  /* 8, 16, 32, 64 */
    bool is_unsigned;
    double f;
    bool is_f32;
    char *s;         /* V_STRING, its bytes */
    size_t s_length;
} Value;

/* ---- The state ---- */

typedef struct {
    const BindgenInput *in;
    Toks toks;
    Files files;
    size_t pos;
    bool failed; /* the current declaration could not be read */
    struct { Typedef *items; size_t count; size_t cap; } typedefs;
    struct { Record **items; size_t count; size_t cap; } records;
    struct { EnumDef **items; size_t count; size_t cap; } enums;
    struct { Function *items; size_t count; size_t cap; } functions;
    struct { Macro *items; size_t count; size_t cap; } macros;
    struct { CType **items; size_t count; size_t cap; } types; /* every CType, freed at the end */
    Buf warnings;
    size_t anonymous;
} P;

static CType *new_type(P *p, CKind kind) {
    CType *t = sosu_alloc(sizeof *t);
    memset(t, 0, sizeof *t);
    t->kind = kind;
    SOSU_VEC_PUSH(&p->types, t);
    return t;
}

static CType *int_type(P *p, unsigned size, bool is_signed) {
    CType *t = new_type(p, CK_INT);
    t->size = size;
    t->is_signed = is_signed;
    return t;
}

static CType *pointer_to(P *p, CType *inner) {
    CType *t = new_type(p, CK_POINTER);
    t->inner = inner;
    return t;
}

static CType *strip(CType *t) {
    while (t && t->kind == CK_TYPEDEF) t = t->inner;
    return t;
}

static const Tok *cur(P *p) { return &p->toks.items[p->pos]; }
static const Tok *at(P *p, size_t k) {
    size_t i = p->pos + k;
    return &p->toks.items[i < p->toks.count ? i : p->toks.count - 1];
}
static bool tok_is(const Tok *t, const char *text) {
    size_t n = strlen(text);
    return t->length == n && strncmp(t->text, text, n) == 0 && t->kind != TK_STRING && t->kind != TK_CHAR;
}
static bool is(P *p, const char *text) { return tok_is(cur(p), text); }
static bool accept(P *p, const char *text) {
    if (!is(p, text)) return false;
    p->pos++;
    return true;
}
static bool at_eof(P *p) { return cur(p)->kind == TK_EOF; }
static char *tok_dup(const Tok *t) { return dup_n(t->text, t->length); }
static bool tok_own(P *p, const Tok *t) { return t->file >= 0 && p->files.items[t->file].own; }

/* Skips a balanced `( ... )`, `[ ... ]` or `{ ... }` starting at the current token. */
static void skip_balanced(P *p) {
    int depth = 0;
    do {
        const Tok *t = cur(p);
        if (t->kind == TK_EOF) return;
        if (t->kind == TK_PUNCT && (tok_is(t, "(") || tok_is(t, "[") || tok_is(t, "{") || tok_is(t, "[["))) {
            depth += tok_is(t, "[[") ? 2 : 1;
        } else if (t->kind == TK_PUNCT && (tok_is(t, ")") || tok_is(t, "]") || tok_is(t, "}") || tok_is(t, "]]"))) {
            depth -= tok_is(t, "]]") ? 2 : 1;
        }
        p->pos++;
    } while (depth > 0);
}

/* Skips to the end of the current declaration: after its `;`, or after a body `{ ... }`. */
static void skip_declaration(P *p) {
    while (!at_eof(p)) {
        if (is(p, "}")) return; /* the end of a struct: its reader stops there */
        if (is(p, ";")) {
            p->pos++;
            return;
        }
        if (is(p, "{")) {
            skip_balanced(p);
            if (is(p, ";")) p->pos++;
            return;
        }
        if (is(p, "(") || is(p, "[")) {
            skip_balanced(p);
            continue;
        }
        p->pos++;
    }
}

static Typedef *find_typedef(P *p, const Tok *t) {
    for (size_t i = p->typedefs.count; i-- > 0;) {
        const char *name = p->typedefs.items[i].name;
        if (strlen(name) == t->length && strncmp(name, t->text, t->length) == 0) return &p->typedefs.items[i];
    }
    return NULL;
}

/* Words that the compilers add to declarations and that change nothing for a binding. */
static const char *const ignored_words[] = {
    "const", "volatile", "restrict", "__restrict", "__restrict__", "_Nullable", "_Nonnull", "_Null_unspecified",
    "__nullable", "__nonnull", "__cdecl", "_cdecl", "__stdcall", "_stdcall", "__fastcall", "__vectorcall",
    "__thiscall", "__clrcall", "__ptr32", "__ptr64", "__unaligned", "__w64", "__extension__", "__inline",
    "__inline__", "inline", "__forceinline", "_Noreturn", "noreturn", "register", "__thread", "_Thread_local",
    "thread_local", "__const", "__volatile", "__volatile__", "__sptr", "__uptr", "__signed", "__signed__",
    "_Nonnull_unspecified", "__kindof", "__unsafe_unretained", "__strong", "__weak", "__autoreleasing",
    "__single", "__indexable", "__counted_by", "__sized_by",
};

/* Words followed by `( ... )` that change nothing for a binding. */
static const char *const ignored_calls[] = {
    "__attribute__", "__attribute", "__declspec", "_Alignas", "alignas", "__asm__", "__asm", "asm", "__pragma",
    "_Pragma", "__typeof_unqual__", "_Static_assert_", "__counted_by", "__sized_by", "__enable_if",
    "__availability", "__deprecated_msg", "__API_AVAILABLE", "__swift_name",
};

static bool is_ignored_word(const Tok *t) {
    if (t->kind != TK_IDENT) return false;
    for (size_t i = 0; i < sizeof ignored_words / sizeof *ignored_words; i++) {
        if (tok_is(t, ignored_words[i])) return true;
    }
    return false;
}

static bool is_ignored_call(const Tok *t) {
    if (t->kind != TK_IDENT) return false;
    for (size_t i = 0; i < sizeof ignored_calls / sizeof *ignored_calls; i++) {
        if (tok_is(t, ignored_calls[i])) return true;
    }
    return false;
}

/* Skips the words and attributes that change nothing. */
static void skip_ignored(P *p) {
    for (;;) {
        if (is_ignored_word(cur(p))) {
            p->pos++;
        } else if (is_ignored_call(cur(p))) {
            p->pos++;
            if (is(p, "(")) skip_balanced(p);
        } else if (is(p, "[[")) {
            skip_balanced(p);
        } else {
            return;
        }
    }
}

static const char *const type_words[] = { "void", "char", "short", "int", "long", "float", "double", "signed",
                                          "unsigned", "_Bool", "bool", "__int8", "__int16", "__int32", "__int64",
                                          "__int128", "_Complex", "struct", "union", "enum", "_Atomic", "typeof",
                                          "__typeof__", "__typeof", "_Float16", "__fp16" };

static bool is_type_word(const Tok *t) {
    if (t->kind != TK_IDENT) return false;
    for (size_t i = 0; i < sizeof type_words / sizeof *type_words; i++) {
        if (tok_is(t, type_words[i])) return true;
    }
    return false;
}

/* True if the current token starts a type: a type word, a typedef, or a qualifier. */
static bool starts_type(P *p) {
    const Tok *t = cur(p);
    return is_type_word(t) || (t->kind == TK_IDENT && find_typedef(p, t)) || is_ignored_word(t) || is_ignored_call(t);
}

static bool starts_type_at(P *p, size_t k) {
    const Tok *t = at(p, k);
    return is_type_word(t) || (t->kind == TK_IDENT && find_typedef(p, t)) || is_ignored_word(t);
}

/* ---- Constant expressions ---- */

static bool eval_expr(P *p, Value *v);
static CType *parse_type_name(P *p);
static bool layout(P *p, CType *t, uint64_t *size, uint64_t *align);

static uint64_t mask(unsigned width) { return width >= 64 ? ~(uint64_t)0 : ((uint64_t)1 << width) - 1; }

static int64_t as_signed(const Value *v) {
    uint64_t b = v->bits & mask(v->width);
    if (v->width < 64 && (b >> (v->width - 1)) & 1) b |= ~mask(v->width);
    return (int64_t)b;
}

static Value int_value(uint64_t bits, unsigned width, bool is_unsigned) {
    Value v;
    memset(&v, 0, sizeof v);
    v.kind = V_INT;
    v.width = width;
    v.is_unsigned = is_unsigned;
    v.bits = bits & mask(width);
    return v;
}

/* A value converted to an integer of `width` bits (C's conversion). */
static Value convert_int(const Value *v, unsigned width, bool is_unsigned) {
    if (v->kind == V_FLOAT) {
        uint64_t bits = v->f < 0 ? (uint64_t)(int64_t)v->f : (uint64_t)v->f;
        return int_value(bits, width, is_unsigned);
    }
    uint64_t bits = v->is_unsigned ? v->bits : (uint64_t)as_signed(v);
    return int_value(bits, width, is_unsigned);
}

static double as_double(const Value *v) {
    if (v->kind == V_FLOAT) return v->f;
    return v->is_unsigned ? (double)v->bits : (double)as_signed(v);
}

/* Integer promotion: what is narrower than `int` becomes an `int`. */
static Value promote(const Value *v) {
    if (v->kind == V_INT && v->width < 32) return convert_int(v, 32, false);
    return *v;
}

/* The usual arithmetic conversions of C, for two integers. */
static void balance(Value *a, Value *b) {
    *a = promote(a);
    *b = promote(b);
    unsigned width = a->width > b->width ? a->width : b->width;
    bool is_unsigned = (a->width == width && a->is_unsigned) || (b->width == width && b->is_unsigned);
    *a = convert_int(a, width, is_unsigned);
    *b = convert_int(b, width, is_unsigned);
}

static bool parse_number(P *p, const Tok *t, Value *v) {
    char *text = tok_dup(t);
    size_t n = strlen(text);
    /* digit separators (C23) */
    size_t w = 0;
    for (size_t i = 0; i < n; i++) {
        if (text[i] != '\'') text[w++] = text[i];
    }
    text[w] = 0;
    n = w;
    bool is_float = false;
    bool hex = n > 1 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X');
    for (size_t i = 0; i < n; i++) {
        if (text[i] == '.' || (!hex && (text[i] == 'e' || text[i] == 'E')) || (hex && (text[i] == 'p' || text[i] == 'P'))) {
            is_float = true;
        }
    }
    memset(v, 0, sizeof *v);
    if (is_float) {
        char *end;
        v->kind = V_FLOAT;
        v->f = strtod(text, &end);
        v->is_f32 = *end == 'f' || *end == 'F';
        bool ok = *end == 0 || strspn(end, "fFlL") == strlen(end);
        free(text);
        return ok;
    }
    char *end;
    uint64_t value = strtoull(text, &end, hex ? 16 : (n > 1 && text[0] == '0') ? 8 : 0);
    if (n > 1 && text[0] == '0' && (text[1] == 'b' || text[1] == 'B')) value = strtoull(text + 2, &end, 2);
    bool is_unsigned = false;
    int longs = 0;
    for (const char *s = end; *s; s++) {
        if (*s == 'u' || *s == 'U') is_unsigned = true;
        else if (*s == 'l' || *s == 'L') longs++;
        else if (*s == 'i' || *s == 'I' || *s == '6' || *s == '4' || *s == '3' || *s == '2' || *s == '8') continue; /* i64 */
        else {
            free(text);
            return false;
        }
    }
    bool decimal = !hex && !(n > 1 && text[0] == '0');
    free(text);
    unsigned long_width = (longs == 1 && p->in->long_is_32) ? 32 : longs ? 64 : 32;
    unsigned width = long_width;
    /* the first type of int, long, long long (unsigned too for hexadecimal and octal) that fits */
    for (;;) {
        uint64_t limit = width == 64 ? (uint64_t)INT64_MAX : (uint64_t)INT32_MAX;
        if (is_unsigned) limit = mask(width);
        if (value <= limit) break;
        if (!is_unsigned && !decimal && value <= mask(width)) {
            is_unsigned = true;
            break;
        }
        if (width == 64) {
            is_unsigned = true;
            break;
        }
        width = 64;
    }
    *v = int_value(value, width, is_unsigned);
    return true;
}

static bool parse_char(const Tok *t, Value *v) {
    const char *s = t->text;
    size_t i = 0;
    while (s[i] != '\'') i++;
    i++;
    uint64_t value = 0;
    if (s[i] == '\\') {
        i++;
        char c = s[i++];
        switch (c) {
        case 'n': value = '\n'; break;
        case 't': value = '\t'; break;
        case 'r': value = '\r'; break;
        case '0': case '1': case '2': case '3': case '4': case '5': case '6': case '7':
            value = (uint64_t)(c - '0');
            while (s[i] >= '0' && s[i] <= '7') value = value * 8 + (uint64_t)(s[i++] - '0');
            break;
        case 'x':
            while (isxdigit((unsigned char)s[i])) {
                char d = s[i++];
                value = value * 16 + (uint64_t)(isdigit((unsigned char)d) ? d - '0' : tolower((unsigned char)d) - 'a' + 10);
            }
            break;
        case 'a': value = 7; break;
        case 'b': value = 8; break;
        case 'f': value = 12; break;
        case 'v': value = 11; break;
        default: value = (unsigned char)c; break;
        }
    } else {
        value = (unsigned char)s[i++];
    }
    if (s[i] != '\'') return false;
    *v = int_value(value, 32, false);
    return true;
}

/* The bytes of a string literal, its escapes decoded; false for wide strings. */
static bool parse_string(const Tok *t, Buf *out) {
    if (t->text[0] != '"' && !(t->text[0] == 'u' && t->text[1] == '8')) return false;
    const char *s = strchr(t->text, '"') + 1;
    const char *end = t->text + t->length - 1;
    while (s < end) {
        char c = *s++;
        if (c != '\\') {
            buf_add(out, "%c", c);
            continue;
        }
        c = *s++;
        unsigned value = 0;
        switch (c) {
        case 'n': value = '\n'; break;
        case 't': value = '\t'; break;
        case 'r': value = '\r'; break;
        case 'x':
            while (s < end && isxdigit((unsigned char)*s)) {
                char d = *s++;
                value = value * 16 + (unsigned)(isdigit((unsigned char)d) ? d - '0' : tolower((unsigned char)d) - 'a' + 10);
            }
            break;
        case '0': case '1': case '2': case '3': case '4': case '5': case '6': case '7':
            value = (unsigned)(c - '0');
            while (s < end && *s >= '0' && *s <= '7') value = value * 8 + (unsigned)(*s++ - '0');
            break;
        default: value = (unsigned char)c; break;
        }
        if (value == 0) return false; /* a NUL inside: not a text */
        buf_add(out, "%c", (char)value);
    }
    return true;
}

static Enumerator *find_enumerator(P *p, const Tok *t) {
    for (size_t i = p->enums.count; i-- > 0;) {
        EnumDef *e = p->enums.items[i];
        for (size_t j = 0; j < e->count; j++) {
            const char *name = e->items[j].name;
            if (strlen(name) == t->length && strncmp(name, t->text, t->length) == 0) return &e->items[j];
        }
    }
    return NULL;
}

static Macro *find_macro(P *p, const Tok *t) {
    for (size_t i = 0; i < p->macros.count; i++) {
        const char *name = p->macros.items[i].name;
        if (strlen(name) == t->length && strncmp(name, t->text, t->length) == 0) return &p->macros.items[i];
    }
    return NULL;
}

static bool eval_macro(P *p, Macro *m, Value *v);

static bool eval_primary(P *p, Value *v) {
    const Tok *t = cur(p);
    if (t->kind == TK_NUMBER) {
        p->pos++;
        return parse_number(p, t, v);
    }
    if (t->kind == TK_CHAR) {
        p->pos++;
        return parse_char(t, v);
    }
    if (t->kind == TK_STRING) {
        Buf bytes = {0};
        bool ok = true;
        while (cur(p)->kind == TK_STRING) {
            ok = ok && parse_string(cur(p), &bytes);
            p->pos++;
        }
        memset(v, 0, sizeof *v);
        v->kind = V_STRING;
        v->s_length = bytes.length;
        v->s = buf_take(&bytes);
        return ok;
    }
    if (tok_is(t, "sizeof")) {
        p->pos++;
        if (!accept(p, "(") || !starts_type(p)) return false;
        CType *type = parse_type_name(p);
        if (!type || !accept(p, ")")) return false;
        uint64_t size, align;
        if (!layout(p, type, &size, &align)) return false;
        *v = int_value(size, 64, true);
        return true;
    }
    if (t->kind == TK_IDENT) {
        p->pos++;
        Enumerator *e = find_enumerator(p, t);
        if (e && e->known) {
            *v = int_value((uint64_t)e->value, 32, false);
            if (e->value > INT32_MAX || e->value < INT32_MIN) *v = int_value((uint64_t)e->value, 64, false);
            return true;
        }
        Macro *m = find_macro(p, t);
        if (m) return eval_macro(p, m, v);
        return false;
    }
    if (accept(p, "(")) {
        bool ok = eval_expr(p, v);
        return ok && accept(p, ")");
    }
    return false;
}

static bool eval_unary(P *p, Value *v) {
    if (is(p, "(") && (starts_type_at(p, 1))) {
        /* a cast */
        p->pos++;
        CType *type = parse_type_name(p);
        if (!type || !accept(p, ")")) return false;
        Value inner;
        if (!eval_unary(p, &inner)) return false;
        CType *s = strip(type);
        if (s->kind == CK_ENUM) {
            *v = convert_int(&inner, 32, false);
            return true;
        }
        if (s->kind == CK_BOOL) {
            *v = int_value(as_double(&inner) != 0, 8, true);
            return true;
        }
        if (s->kind == CK_INT) {
            *v = convert_int(&inner, s->size * 8, !s->is_signed);
            return true;
        }
        if (s->kind == CK_FLOAT) {
            memset(v, 0, sizeof *v);
            v->kind = V_FLOAT;
            v->f = as_double(&inner);
            v->is_f32 = s->size == 4;
            return inner.kind != V_STRING;
        }
        return false;
    }
    if (accept(p, "-")) {
        if (!eval_unary(p, v)) return false;
        if (v->kind == V_FLOAT) {
            v->f = -v->f;
            return true;
        }
        if (v->kind != V_INT) return false;
        *v = promote(v);
        v->bits = (~v->bits + 1) & mask(v->width);
        return true;
    }
    if (accept(p, "+")) return eval_unary(p, v) && v->kind != V_STRING;
    if (accept(p, "~")) {
        if (!eval_unary(p, v) || v->kind != V_INT) return false;
        *v = promote(v);
        v->bits = ~v->bits & mask(v->width);
        return true;
    }
    if (accept(p, "!")) {
        if (!eval_unary(p, v) || v->kind == V_STRING) return false;
        *v = int_value(as_double(v) == 0, 32, false);
        return true;
    }
    return eval_primary(p, v);
}

static int precedence(const Tok *t) {
    static const char *const levels[][5] = {
        { "||" }, { "&&" }, { "|" }, { "^" }, { "&" }, { "==", "!=" }, { "<", ">", "<=", ">=" },
        { "<<", ">>" }, { "+", "-" }, { "*", "/", "%" },
    };
    if (t->kind != TK_PUNCT) return -1;
    for (int level = 0; level < 10; level++) {
        for (int k = 0; k < 5 && levels[level][k]; k++) {
            if (tok_is(t, levels[level][k])) return level;
        }
    }
    return -1;
}

static bool apply_binary(const Tok *op, Value *a, Value b) {
    if (a->kind == V_STRING || b.kind == V_STRING) return false;
    bool compare = tok_is(op, "==") || tok_is(op, "!=") || tok_is(op, "<") || tok_is(op, ">") || tok_is(op, "<=") ||
                   tok_is(op, ">=");
    if (tok_is(op, "&&") || tok_is(op, "||")) {
        bool x = as_double(a) != 0, y = as_double(&b) != 0;
        *a = int_value(tok_is(op, "&&") ? (x && y) : (x || y), 32, false);
        return true;
    }
    if (a->kind == V_FLOAT || b.kind == V_FLOAT) {
        double x = as_double(a), y = as_double(&b);
        bool f32 = (a->kind != V_FLOAT || a->is_f32) && (b.kind != V_FLOAT || b.is_f32);
        if (compare) {
            bool r = tok_is(op, "==") ? x == y : tok_is(op, "!=") ? x != y : tok_is(op, "<") ? x < y
                   : tok_is(op, ">") ? x > y : tok_is(op, "<=") ? x <= y : x >= y;
            *a = int_value(r, 32, false);
            return true;
        }
        double r;
        if (tok_is(op, "+")) r = x + y;
        else if (tok_is(op, "-")) r = x - y;
        else if (tok_is(op, "*")) r = x * y;
        else if (tok_is(op, "/") && y != 0) r = x / y;
        else return false;
        memset(a, 0, sizeof *a);
        a->kind = V_FLOAT;
        a->f = r;
        a->is_f32 = f32;
        return true;
    }
    if (tok_is(op, "<<") || tok_is(op, ">>")) {
        *a = promote(a);
        b = promote(&b);
        uint64_t count = b.is_unsigned ? b.bits : (uint64_t)as_signed(&b);
        if (count >= a->width) return false;
        if (tok_is(op, "<<")) a->bits = (a->bits << count) & mask(a->width);
        else if (a->is_unsigned) a->bits >>= count;
        else a->bits = (uint64_t)(as_signed(a) >> count) & mask(a->width);
        return true;
    }
    balance(a, &b);
    unsigned width = a->width;
    bool u = a->is_unsigned;
    uint64_t x = a->bits, y = b.bits;
    int64_t sx = as_signed(a), sy = as_signed(&b);
    if (compare) {
        bool r;
        if (tok_is(op, "==")) r = x == y;
        else if (tok_is(op, "!=")) r = x != y;
        else if (tok_is(op, "<")) r = u ? x < y : sx < sy;
        else if (tok_is(op, ">")) r = u ? x > y : sx > sy;
        else if (tok_is(op, "<=")) r = u ? x <= y : sx <= sy;
        else r = u ? x >= y : sx >= sy;
        *a = int_value(r, 32, false);
        return true;
    }
    uint64_t r;
    if (tok_is(op, "+")) r = x + y;
    else if (tok_is(op, "-")) r = x - y;
    else if (tok_is(op, "*")) r = x * y;
    else if (tok_is(op, "/") || tok_is(op, "%")) {
        if (y == 0) return false;
        if (u) r = tok_is(op, "/") ? x / y : x % y;
        else if (sx == INT64_MIN && sy == -1) return false;
        else r = (uint64_t)(tok_is(op, "/") ? sx / sy : sx % sy);
    } else if (tok_is(op, "&")) r = x & y;
    else if (tok_is(op, "|")) r = x | y;
    else if (tok_is(op, "^")) r = x ^ y;
    else return false;
    *a = int_value(r, width, u);
    return true;
}

static bool eval_binary(P *p, Value *v, int min) {
    if (!eval_unary(p, v)) return false;
    for (;;) {
        const Tok *op = cur(p);
        int level = precedence(op);
        if (level < min) return true;
        p->pos++;
        Value right;
        if (!eval_binary(p, &right, level + 1)) return false;
        if (!apply_binary(op, v, right)) return false;
    }
}

static bool eval_expr(P *p, Value *v) {
    if (!eval_binary(p, v, 0)) return false;
    if (accept(p, "?")) {
        Value a, b;
        if (!eval_expr(p, &a) || !accept(p, ":") || !eval_expr(p, &b)) return false;
        *v = as_double(v) != 0 ? a : b;
    }
    return true;
}

/* Evaluates the tokens [first, end) as a whole expression. */
static bool eval_range(P *p, size_t first, size_t end, Value *v) {
    size_t saved = p->pos;
    p->pos = first;
    bool ok = first < end && eval_expr(p, v) && p->pos == end;
    p->pos = saved;
    return ok;
}

static bool eval_macro(P *p, Macro *m, Value *v) {
    if (m->state == 1) return false;
    m->state = 1;
    bool ok = eval_range(p, m->first, m->end, v);
    m->state = 2;
    return ok;
}

/* ---- Declarations ---- */

static Record *find_record(P *p, const Tok *tag) {
    for (size_t i = 0; i < p->records.count; i++) {
        Record *r = p->records.items[i];
        if (r->tag && strlen(r->tag) == tag->length && strncmp(r->tag, tag->text, tag->length) == 0) return r;
    }
    return NULL;
}

static Record *new_record(P *p, char *tag, bool is_union) {
    Record *r = sosu_alloc(sizeof *r);
    memset(r, 0, sizeof *r);
    r->tag = tag;
    r->is_union = is_union;
    SOSU_VEC_PUSH(&p->records, r);
    return r;
}

static CType *parse_declarator(P *p, CType *base, const Tok **name);
static CType *parse_specifiers(P *p, bool *is_typedef, bool *is_static);

static void parse_record_body(P *p, Record *r) {
    RField *fields = NULL;
    size_t count = 0, cap = 0;
    while (!at_eof(p) && !is(p, "}")) {
        skip_ignored(p);
        if (is(p, "_Static_assert") || is(p, "static_assert")) {
            skip_declaration(p);
            continue;
        }
        if (accept(p, ";")) continue;
        bool is_typedef = false, is_static = false;
        CType *base = parse_specifiers(p, &is_typedef, &is_static);
        if (!base) {
            p->failed = true;
            skip_declaration(p);
            continue;
        }
        if (accept(p, ";")) { /* an anonymous struct or union */
            RField f = { NULL, base, -1, 0 };
            if (count == cap) {
                cap = cap ? cap * 2 : 8;
                fields = sosu_realloc(fields, cap * sizeof *fields);
            }
            fields[count++] = f;
            continue;
        }
        for (;;) {
            const Tok *name = NULL;
            CType *type = is(p, ":") ? base : parse_declarator(p, base, &name);
            int bits = -1;
            if (accept(p, ":")) {
                size_t first = p->pos;
                while (!at_eof(p) && !is(p, ",") && !is(p, ";")) p->pos++;
                Value v;
                if (eval_range(p, first, p->pos, &v) && v.kind == V_INT) bits = (int)v.bits;
                else p->failed = true;
                r->has_bits = true;
            }
            skip_ignored(p);
            RField f = { name ? tok_dup(name) : NULL, type, bits, 0 };
            if (count == cap) {
                cap = cap ? cap * 2 : 8;
                fields = sosu_realloc(fields, cap * sizeof *fields);
            }
            fields[count++] = f;
            if (!accept(p, ",")) break;
        }
        if (!accept(p, ";")) {
            p->failed = true;
            skip_declaration(p);
        }
    }
    accept(p, "}");
    r->fields = fields;
    r->field_count = count;
    r->complete = true;
}

static CType *parse_record(P *p, bool is_union) {
    skip_ignored(p);
    const Tok *tag = cur(p)->kind == TK_IDENT ? cur(p) : NULL;
    if (tag) p->pos++;
    skip_ignored(p);
    Record *r = tag ? find_record(p, tag) : NULL;
    if (accept(p, "{")) {
        if (!r || r->complete) r = new_record(p, tag ? tok_dup(tag) : NULL, is_union);
        r->is_union = is_union;
        r->own = tok_own(p, tag ? tag : &p->toks.items[p->pos - 1]);
        parse_record_body(p, r);
    } else if (!r) {
        if (!tag) return NULL;
        r = new_record(p, tok_dup(tag), is_union);
        r->own = tok_own(p, tag); /* `typedef struct SDL_Window SDL_Window;`: an opaque struct of the header */
    }
    skip_ignored(p);
    CType *t = new_type(p, CK_RECORD);
    t->record = r;
    return t;
}

static EnumDef *find_enum(P *p, const Tok *tag) {
    for (size_t i = 0; i < p->enums.count; i++) {
        EnumDef *e = p->enums.items[i];
        if (e->tag && strlen(e->tag) == tag->length && strncmp(e->tag, tag->text, tag->length) == 0) return e;
    }
    return NULL;
}

static CType *parse_enum(P *p) {
    skip_ignored(p);
    const Tok *tag = cur(p)->kind == TK_IDENT ? cur(p) : NULL;
    if (tag) p->pos++;
    skip_ignored(p);
    CType *fixed = NULL;
    if (accept(p, ":")) {
        bool t, s;
        fixed = parse_specifiers(p, &t, &s);
    }
    EnumDef *e = tag ? find_enum(p, tag) : NULL;
    if (is(p, "{")) {
        p->pos++;
        e = sosu_alloc(sizeof *e);
        memset(e, 0, sizeof *e);
        e->tag = tag ? tok_dup(tag) : NULL;
        e->fixed = fixed;
        SOSU_VEC_PUSH(&p->enums, e);
        int64_t next = 0;
        bool known = true;
        while (!at_eof(p) && !is(p, "}")) {
            const Tok *name = cur(p);
            if (name->kind != TK_IDENT) {
                p->failed = true;
                break;
            }
            p->pos++;
            skip_ignored(p);
            Enumerator item = { tok_dup(name), next, known, tok_own(p, name) };
            if (accept(p, "=")) {
                size_t first = p->pos;
                int depth = 0;
                while (!at_eof(p) && !(depth == 0 && (is(p, ",") || is(p, "}")))) {
                    if (is(p, "(")) depth++;
                    if (is(p, ")")) depth--;
                    p->pos++;
                }
                Value v;
                /* the enumerator is visible in its own value? no: evaluate before adding it */
                item.known = eval_range(p, first, p->pos, &v) && v.kind == V_INT;
                item.value = item.known ? (v.is_unsigned ? (int64_t)v.bits : as_signed(&v)) : 0;
            }
            SOSU_VEC_PUSH(e, item);
            known = item.known;
            next = item.value + 1;
            if (!accept(p, ",")) break;
        }
        accept(p, "}");
    } else if (!e) {
        e = sosu_alloc(sizeof *e);
        memset(e, 0, sizeof *e);
        e->tag = tag ? tok_dup(tag) : NULL;
        e->fixed = fixed;
        SOSU_VEC_PUSH(&p->enums, e);
    }
    skip_ignored(p);
    CType *t = new_type(p, CK_ENUM);
    t->enum_ = e;
    return t;
}

/* The specifiers of a declaration: storage, qualifiers and the base type. NULL if there is none. */
static CType *parse_specifiers(P *p, bool *is_typedef, bool *is_static) {
    *is_typedef = false;
    *is_static = false;
    int longs = 0, shorts = 0, chars = 0, ints = 0, signs = 0, unsigns = 0, floats = 0, doubles = 0, voids = 0;
    int bools = 0, fixed = 0;
    CType *named = NULL;
    bool any = false;
    for (;;) {
        const Tok *t = cur(p);
        if (is_ignored_word(t) || is_ignored_call(t) || tok_is(t, "[[")) {
            skip_ignored(p);
            continue;
        }
        if (t->kind != TK_IDENT) break;
        if (tok_is(t, "typedef")) *is_typedef = true;
        else if (tok_is(t, "extern")) { /* nothing */ }
        else if (tok_is(t, "static")) *is_static = true;
        else if (tok_is(t, "auto") && !any) { /* a storage class in C */ }
        else if (tok_is(t, "long")) longs++;
        else if (tok_is(t, "short")) shorts++;
        else if (tok_is(t, "char")) chars++;
        else if (tok_is(t, "int")) ints++;
        else if (tok_is(t, "signed")) signs++;
        else if (tok_is(t, "unsigned")) unsigns++;
        else if (tok_is(t, "float")) floats++;
        else if (tok_is(t, "double")) doubles++;
        else if (tok_is(t, "void")) voids++;
        else if (tok_is(t, "_Bool") || tok_is(t, "bool")) bools++;
        else if (tok_is(t, "__int8")) fixed = 1;
        else if (tok_is(t, "__int16")) fixed = 2;
        else if (tok_is(t, "__int32")) fixed = 4;
        else if (tok_is(t, "__int64")) fixed = 8;
        else if (tok_is(t, "__int128") || tok_is(t, "_Complex") || tok_is(t, "_Float16") || tok_is(t, "__fp16")) {
            named = new_type(p, CK_UNKNOWN);
        } else if (tok_is(t, "_Atomic") || tok_is(t, "typeof") || tok_is(t, "__typeof__") || tok_is(t, "__typeof")) {
            p->pos++;
            if (is(p, "(")) skip_balanced(p);
            named = new_type(p, CK_UNKNOWN);
            any = true;
            continue;
        } else if (tok_is(t, "struct") || tok_is(t, "union")) {
            p->pos++;
            named = parse_record(p, tok_is(t, "union"));
            if (!named) return NULL;
            any = true;
            continue;
        } else if (tok_is(t, "enum")) {
            p->pos++;
            named = parse_enum(p);
            any = true;
            continue;
        } else if (!named && !(longs | shorts | chars | ints | signs | unsigns | floats | doubles | voids | bools | fixed)) {
            Typedef *def = find_typedef(p, t);
            if (def) {
                named = new_type(p, CK_TYPEDEF);
                named->name = def->name;
                named->inner = def->type;
            } else {
                /* a type this reader does not know (from a header it could not read): only if a
                   declarator follows */
                const Tok *next = at(p, 1);
                if (!(next->kind == TK_IDENT || tok_is(next, "*") || tok_is(next, "("))) break;
                if (next->kind == TK_IDENT && (tok_is(next, "__attribute__") || is_ignored_word(next))) break;
                named = new_type(p, CK_UNKNOWN);
                named->name = tok_dup(t);
            }
        } else {
            break;
        }
        any = true;
        p->pos++;
    }
    if (named) return named;
    if (!any) return NULL;
    if (voids) return new_type(p, CK_VOID);
    if (bools) {
        CType *t = new_type(p, CK_BOOL);
        t->size = 1;
        return t;
    }
    if (floats || doubles) {
        if (longs) return new_type(p, CK_UNKNOWN); /* long double */
        CType *t = new_type(p, CK_FLOAT);
        t->size = floats ? 4 : 8;
        return t;
    }
    if (fixed) return int_type(p, (unsigned)fixed, !unsigns);
    if (chars) {
        CType *t = int_type(p, 1, signs > 0);
        t->is_char = !signs && !unsigns;
        return t;
    }
    if (shorts) return int_type(p, 2, !unsigns);
    if (longs >= 2) return int_type(p, 8, !unsigns);
    if (longs == 1) {
        CType *t = int_type(p, p->in->long_is_32 ? 4 : 8, !unsigns);
        t->is_long = true;
        return t;
    }
    return int_type(p, 4, !unsigns);
}

typedef struct {
    bool is_array;
    uint64_t count;
    bool is_func;
    CType **params;
    char **names;
    size_t param_count;
    bool variadic;
} Suffix;

static void parse_params(P *p, Suffix *s) {
    /* after `(` */
    CType **params = NULL;
    char **names = NULL;
    size_t count = 0, cap = 0;
    if (is(p, "void") && tok_is(at(p, 1), ")")) p->pos++;
    while (!at_eof(p) && !is(p, ")")) {
        if (accept(p, "...")) {
            s->variadic = true;
            break;
        }
        bool is_typedef, is_static;
        CType *base = parse_specifiers(p, &is_typedef, &is_static);
        if (!base) {
            p->failed = true;
            break;
        }
        const Tok *name = NULL;
        CType *type = parse_declarator(p, base, &name);
        CType *stripped = strip(type);
        if (stripped && stripped->kind == CK_ARRAY) type = pointer_to(p, stripped->inner); /* decays */
        else if (stripped && stripped->kind == CK_FUNC) type = pointer_to(p, type);
        if (count == cap) {
            cap = cap ? cap * 2 : 8;
            params = sosu_realloc(params, cap * sizeof *params);
            names = sosu_realloc(names, cap * sizeof *names);
        }
        params[count] = type;
        names[count] = name ? tok_dup(name) : NULL;
        count++;
        if (!accept(p, ",")) break;
    }
    if (!accept(p, ")")) {
        p->failed = true;
        skip_balanced(p);
    }
    s->is_func = true;
    s->params = params;
    s->names = names;
    s->param_count = count;
}

static CType *apply_suffixes(P *p, CType *base) {
    Suffix *suffixes = NULL;
    size_t count = 0, cap = 0;
    for (;;) {
        skip_ignored(p);
        Suffix s;
        memset(&s, 0, sizeof s);
        if (accept(p, "[")) {
            size_t first = p->pos;
            while (!at_eof(p) && !is(p, "]")) {
                if (is(p, "[") || is(p, "(")) skip_balanced(p);
                else p->pos++;
            }
            Value v;
            s.is_array = true;
            if (p->pos > first) {
                /* `static` and qualifiers of array parameters */
                size_t start = first;
                while (start < p->pos && (tok_is(&p->toks.items[start], "static") || is_ignored_word(&p->toks.items[start]))) start++;
                if (start < p->pos) {
                    if (eval_range(p, start, p->pos, &v) && v.kind == V_INT) s.count = v.bits;
                    else p->failed = true;
                }
            }
            accept(p, "]");
        } else if (accept(p, "(")) {
            parse_params(p, &s);
        } else {
            break;
        }
        if (count == cap) {
            cap = cap ? cap * 2 : 4;
            suffixes = sosu_realloc(suffixes, cap * sizeof *suffixes);
        }
        suffixes[count++] = s;
    }
    for (size_t i = count; i-- > 0;) {
        CType *t;
        if (suffixes[i].is_array) {
            t = new_type(p, CK_ARRAY);
            t->count = suffixes[i].count;
        } else {
            t = new_type(p, CK_FUNC);
            t->params = suffixes[i].params;
            t->param_names = suffixes[i].names;
            t->param_count = suffixes[i].param_count;
            t->variadic = suffixes[i].variadic;
        }
        t->inner = base;
        base = t;
    }
    free(suffixes);
    return base;
}

/* After `(`: a nested declarator `(*f)`, or the parameters of a function `(int x)`. */
static bool nested_declarator(P *p) {
    const Tok *t = at(p, 1);
    if (tok_is(t, "*") || tok_is(t, "(") || tok_is(t, "^") || tok_is(t, "[[")) return true;
    if (t->kind != TK_IDENT) return false;
    if (is_ignored_call(t)) return true;
    if (is_ignored_word(t)) {
        /* `(__cdecl *f)` is nested, `(const char *s)` is a parameter */
        return tok_is(t, "__cdecl") || tok_is(t, "_cdecl") || tok_is(t, "__stdcall") || tok_is(t, "_stdcall") ||
               tok_is(t, "__fastcall") || tok_is(t, "__vectorcall") || tok_is(t, "__thiscall");
    }
    return !is_type_word(t) && !find_typedef(p, t);
}

static CType *parse_declarator(P *p, CType *base, const Tok **name) {
    *name = NULL;
    skip_ignored(p);
    while (accept(p, "*") || accept(p, "^")) {
        base = pointer_to(p, base);
        skip_ignored(p);
    }
    if (is(p, "(") && nested_declarator(p)) {
        size_t inner = p->pos + 1;
        skip_balanced(p);
        base = apply_suffixes(p, base);
        size_t end = p->pos;
        p->pos = inner;
        CType *type = parse_declarator(p, base, name);
        skip_ignored(p);
        if (!accept(p, ")")) p->failed = true;
        p->pos = end;
        return type;
    }
    if (cur(p)->kind == TK_IDENT && !is_type_word(cur(p))) {
        *name = cur(p);
        p->pos++;
    }
    return apply_suffixes(p, base);
}

/* `type-name` of a cast or of `sizeof`: specifiers and an abstract declarator. */
static CType *parse_type_name(P *p) {
    bool is_typedef, is_static;
    CType *base = parse_specifiers(p, &is_typedef, &is_static);
    if (!base) return NULL;
    const Tok *name;
    return parse_declarator(p, base, &name);
}

static void add_typedef(P *p, const Tok *name, CType *type) {
    Typedef def = { tok_dup(name), type, tok_own(p, name) };
    SOSU_VEC_PUSH(&p->typedefs, def);
    CType *s = type;
    if (s->kind == CK_RECORD && !s->record->name) s->record->name = tok_dup(name);
    if (s->kind == CK_ENUM && !s->enum_->name) s->enum_->name = tok_dup(name);
}

static void add_function(P *p, const Tok *name, CType *type, bool is_static) {
    for (size_t i = 0; i < p->functions.count; i++) {
        Function *f = &p->functions.items[i];
        if (strlen(f->name) == name->length && strncmp(f->name, name->text, name->length) == 0) return;
    }
    Function f = { tok_dup(name), type, tok_own(p, name), is_static };
    SOSU_VEC_PUSH(&p->functions, f);
}

/* `__sosu_macro__ "NAME" expansion ;` */
static void parse_probe(P *p) {
    p->pos++;
    const Tok *name = cur(p);
    if (name->kind != TK_STRING) {
        skip_declaration(p);
        return;
    }
    p->pos++;
    size_t first = p->pos;
    while (!at_eof(p) && !is(p, ";")) {
        if (cur(p)->kind == TK_IDENT && tok_is(cur(p), BINDGEN_PROBE)) break;
        p->pos++;
    }
    size_t end = p->pos;
    accept(p, ";");
    Macro m;
    memset(&m, 0, sizeof m);
    m.name = dup_n(name->text + 1, name->length - 2);
    m.first = first;
    m.end = end;
    /* a name that is not a macro comes back as itself */
    if (end == first + 1 && strlen(m.name) == p->toks.items[first].length &&
        strncmp(m.name, p->toks.items[first].text, p->toks.items[first].length) == 0) {
        free(m.name);
        return;
    }
    if (end == first) {
        free(m.name);
        return;
    }
    SOSU_VEC_PUSH(&p->macros, m);
}

static void parse_declaration(P *p) {
    p->failed = false;
    skip_ignored(p);
    if (accept(p, ";")) return;
    const Tok *t = cur(p);
    if (tok_is(t, BINDGEN_PROBE)) {
        parse_probe(p);
        return;
    }
    if (tok_is(t, "_Static_assert") || tok_is(t, "static_assert") || tok_is(t, "__pragma") || tok_is(t, "_Pragma")) {
        p->pos++;
        if (is(p, "(")) skip_balanced(p);
        accept(p, ";");
        return;
    }
    if (tok_is(t, "extern") && at(p, 1)->kind == TK_STRING) { /* C++ */
        p->pos += 2;
        if (is(p, "{")) p->pos++;
        return;
    }
    if (is(p, "}")) { /* the end of `extern "C" {` */
        p->pos++;
        return;
    }
    bool is_typedef, is_static;
    size_t start = p->pos;
    CType *base = parse_specifiers(p, &is_typedef, &is_static);
    if (!base) {
        p->pos = start + 1;
        skip_declaration(p);
        return;
    }
    if (accept(p, ";")) return; /* `struct S { ... };` */
    for (;;) {
        const Tok *name = NULL;
        CType *type = parse_declarator(p, base, &name);
        skip_ignored(p);
        if (name && !p->failed) {
            if (is_typedef) add_typedef(p, name, type);
            else if (strip(type) && strip(type)->kind == CK_FUNC) add_function(p, name, type, is_static);
        }
        if (is(p, "=")) { /* a variable with a value */
            skip_declaration(p);
            return;
        }
        if (is(p, "{")) { /* the body of a function */
            skip_balanced(p);
            return;
        }
        if (!accept(p, ",")) break;
    }
    if (!accept(p, ";")) skip_declaration(p);
}

/* ---- Layout ---- */

static bool layout_record(P *p, Record *r) {
    if (r->laid == 2) return true;
    if (r->laid == 1 || r->laid == 3 || !r->complete) return false;
    r->laid = 1;
    uint64_t offset = 0, align = 1, size = 0;
    uint64_t unit_offset = 0, unit_size = 0, bit = 0; /* the current unit of bit fields */
    for (size_t i = 0; i < r->field_count; i++) {
        RField *f = &r->fields[i];
        uint64_t fs, fa;
        if (!layout(p, f->type, &fs, &fa)) {
            r->laid = 3;
            return false;
        }
        if (fa > align) align = fa;
        if (r->is_union) {
            f->offset = 0;
            if (fs > size) size = fs;
            continue;
        }
        if (f->bits >= 0) {
            /* a bit field: packed in units of its type, a new unit when it does not fit */
            if (f->bits == 0 || unit_size != fs || bit + (uint64_t)f->bits > fs * 8) {
                offset = unit_size ? unit_offset + unit_size : offset;
                offset = (offset + fa - 1) / fa * fa;
                unit_offset = offset;
                unit_size = f->bits == 0 ? 0 : fs;
                bit = 0;
            }
            f->offset = unit_offset;
            bit += (uint64_t)f->bits;
            continue;
        }
        if (unit_size) {
            offset = unit_offset + unit_size;
            unit_size = 0;
            bit = 0;
        }
        offset = (offset + fa - 1) / fa * fa;
        f->offset = offset;
        offset += fs;
    }
    if (!r->is_union) size = unit_size ? unit_offset + unit_size : offset;
    size = (size + align - 1) / align * align;
    r->size = size;
    r->align = align;
    r->laid = 2;
    return true;
}

static bool layout(P *p, CType *t, uint64_t *size, uint64_t *align) {
    t = strip(t);
    if (!t) return false;
    switch (t->kind) {
    case CK_INT:
    case CK_FLOAT:
    case CK_BOOL:
        *size = *align = t->size ? t->size : 1;
        return true;
    case CK_POINTER:
        *size = *align = 8;
        return true;
    case CK_ENUM:
        if (t->enum_->fixed) return layout(p, t->enum_->fixed, size, align);
        *size = *align = 4;
        return true;
    case CK_ARRAY: {
        uint64_t es, ea;
        if (!t->count || !layout(p, t->inner, &es, &ea)) return false;
        *size = es * t->count;
        *align = ea;
        return true;
    }
    case CK_RECORD:
        if (!layout_record(p, t->record)) return false;
        *size = t->record->size;
        *align = t->record->align;
        return true;
    default:
        return false;
    }
}

/* ---- Sosu ---- */

static const char *const sosu_words[] = {
    "struct", "class", "interface", "enum", "import", "extern", "pub", "return", "if", "else", "while", "for", "in",
    "break", "continue", "defer", "mut", "auto", "true", "false", "as", "match", "try", "requires", "ensures", "test",
    "uses", "unsafe", "virtual", "override", "self", "base", "export", "fn", "i8", "i16", "i32", "i64", "ui8", "ui16",
    "ui32", "ui64", "isize", "usize", "f32", "f64", "uf32", "uf64", "bool", "char", "str", "void", "Arena", "some",
    "none", "ok", "err", "result", "KB", "MB", "GB", "io", "time", "random", "threads",
};

static bool is_sosu_word(const char *name) {
    for (size_t i = 0; i < sizeof sosu_words / sizeof *sosu_words; i++) {
        if (strcmp(name, sosu_words[i]) == 0) return true;
    }
    return false;
}

static const char *int_name(unsigned size, bool is_signed) {
    switch (size) {
    case 1: return is_signed ? "i8" : "ui8";
    case 2: return is_signed ? "i16" : "ui16";
    case 4: return is_signed ? "i32" : "ui32";
    default: return is_signed ? "i64" : "ui64";
    }
}

/* The Sosu type of an enum: `i32`, unless a value needs more. */
static const char *enum_type(P *p, EnumDef *e) {
    if (e->fixed) {
        CType *f = strip(e->fixed);
        if (f && f->kind == CK_INT) return int_name(f->size, f->is_signed);
    }
    bool negative = false, wide = false, high = false;
    for (size_t i = 0; i < e->count; i++) {
        int64_t v = e->items[i].value;
        if (v < 0) negative = true;
        if (v > INT32_MAX) high = true;
        if (v > (int64_t)UINT32_MAX || v < INT32_MIN) wide = true;
    }
    (void)p;
    if (wide) return negative ? "i64" : "ui64";
    if (high && !negative) return "ui32";
    return "i32";
}

typedef struct {
    bool unsupported; /* the type cannot be written: the declaration is left out */
    bool uses_long;
    const char *why;
} TypeNote;

static void mark_needed(P *p, Record *r);

/* The Sosu type of `t`, or NULL; `position`: 0 a value, 1 a result (void allowed), 2 behind a pointer. */
static char *sosu_type(P *p, CType *t, int position, TypeNote *note) {
    /* typedefs whose name says more than their type */
    for (CType *d = t; d && d->kind == CK_TYPEDEF; d = d->inner) {
        if (strcmp(d->name, "size_t") == 0 || strcmp(d->name, "uintptr_t") == 0) return sosu_strdup("usize");
        if (strcmp(d->name, "ssize_t") == 0 || strcmp(d->name, "intptr_t") == 0 || strcmp(d->name, "ptrdiff_t") == 0) {
            return sosu_strdup("isize");
        }
        if (strcmp(d->name, "bool") == 0 || strcmp(d->name, "_Bool") == 0) return sosu_strdup("bool");
    }
    CType *s = strip(t);
    if (!s) {
        note->unsupported = true;
        return NULL;
    }
    switch (s->kind) {
    case CK_VOID:
        if (position == 0) {
            note->unsupported = true;
            note->why = "`void` as a value";
            return NULL;
        }
        return sosu_strdup(position == 2 ? "ui8" : "void");
    case CK_BOOL: return sosu_strdup("bool");
    case CK_INT:
        if (s->is_long) note->uses_long = true;
        return sosu_strdup(s->is_char ? "ui8" : int_name(s->size, s->is_signed));
    case CK_FLOAT: return sosu_strdup(s->size == 4 ? "f32" : "f64");
    case CK_ENUM: return sosu_strdup(enum_type(p, s->enum_));
    case CK_POINTER: {
        CType *inner = strip(s->inner);
        if (inner && inner->kind == CK_FUNC) return sosu_type(p, s->inner, 0, note); /* a function pointer */
        if (inner && inner->kind == CK_RECORD && !inner->record->own) return sosu_strdup("ui8*"); /* FILE*... */
        if (inner && inner->kind == CK_POINTER && strip(inner->inner) && strip(inner->inner)->kind == CK_FUNC) {
            return sosu_strdup("ui8*"); /* a pointer to a function pointer */
        }
        if (inner && (inner->kind == CK_UNKNOWN || inner->kind == CK_FUNC)) return sosu_strdup("ui8*");
        TypeNote inner_note = { false, false, NULL };
        char *text = sosu_type(p, s->inner, 2, &inner_note);
        if (!text || inner_note.unsupported) {
            free(text);
            return sosu_strdup("ui8*");
        }
        note->uses_long |= inner_note.uses_long;
        char *result = sosu_format("%s*", text);
        free(text);
        return result;
    }
    case CK_ARRAY: {
        if (!s->count) {
            note->unsupported = true;
            note->why = "an array without a size";
            return NULL;
        }
        char *element = sosu_type(p, s->inner, 0, note);
        if (!element || strncmp(element, "fn(", 3) == 0) {
            free(element);
            note->unsupported = true;
            return NULL;
        }
        char *result = sosu_format("%s[%llu]", element, (unsigned long long)s->count);
        free(element);
        return result;
    }
    case CK_RECORD: {
        Record *r = s->record;
        if (!r->own || !(r->name || r->tag || r->sosu_name)) {
            if (position == 2 && (r->name || r->tag)) return sosu_strdup("ui8"); /* a pointer to a foreign struct */
            note->unsupported = true;
            note->why = "a struct of another header";
            return NULL;
        }
        if (position != 2) {
            uint64_t size, align;
            if (!layout(p, s, &size, &align)) {
                note->unsupported = true;
                note->why = "a struct whose size is unknown";
                return NULL;
            }
        }
        mark_needed(p, r);
        return sosu_strdup(r->sosu_name);
    }
    case CK_FUNC: {
        if (s->variadic) {
            note->unsupported = true;
            note->why = "a variadic function";
            return NULL;
        }
        Buf b = {0};
        buf_add(&b, "fn(");
        for (size_t i = 0; i < s->param_count; i++) {
            char *param = sosu_type(p, s->params[i], 0, note);
            if (!param || note->unsupported) {
                free(param);
                free(buf_take(&b));
                note->unsupported = true;
                return NULL;
            }
            buf_add(&b, "%s%s", i ? ", " : "", param);
            free(param);
        }
        char *ret = sosu_type(p, s->inner, 1, note);
        if (!ret || note->unsupported) {
            free(ret);
            free(buf_take(&b));
            note->unsupported = true;
            return NULL;
        }
        buf_add(&b, ") -> %s uses io", ret);
        free(ret);
        return buf_take(&b);
    }
    default:
        note->unsupported = true;
        note->why = "a type this reader does not know";
        return NULL;
    }
}

static void name_record(P *p, Record *r, const char *parent, size_t index) {
    if (r->sosu_name) return;
    if (r->name) r->sosu_name = sosu_strdup(r->name);
    else if (r->tag) r->sosu_name = sosu_strdup(r->tag);
    else if (parent) r->sosu_name = sosu_format("%s_%zu", parent, index);
    else r->sosu_name = sosu_format("anonymous_%zu", ++p->anonymous);
}

static void mark_needed(P *p, Record *r) {
    name_record(p, r, NULL, 0);
    r->needed = true;
}

/* A field or parameter name that Sosu accepts. */
static char *sosu_name(const char *name, size_t index, const char *fallback) {
    if (!name) return sosu_format("%s%zu", fallback, index);
    if (is_sosu_word(name)) return sosu_format("%s_", name);
    return sosu_strdup(name);
}

static const char *integer_literal(Buf *b, const Value *v, const char *type) {
    (void)type;
    if (v->is_unsigned) {
        if (v->bits > 9) buf_add(b, "0x%llX", (unsigned long long)v->bits);
        else buf_add(b, "%llu", (unsigned long long)v->bits);
    } else {
        int64_t s = as_signed(v);
        if (s == INT64_MIN) buf_add(b, "-9223372036854775807 - 1");
        else buf_add(b, "%lld", (long long)s);
    }
    return b->text;
}

static bool float_literal(Buf *b, double f) {
    if (f != f || f > 1.7976931348623157e308 || f < -1.7976931348623157e308) return false;
    char text[64];
    snprintf(text, sizeof text, "%.17g", f);
    /* the shortest that reads back the same */
    for (int digits = 1; digits <= 17; digits++) {
        char shorter[64];
        snprintf(shorter, sizeof shorter, "%.*g", digits, f);
        if (strtod(shorter, NULL) == f) {
            snprintf(text, sizeof text, "%s", shorter);
            break;
        }
    }
    const char *e = strpbrk(text, "eE");
    if (strchr(text, '.')) {
        buf_add(b, "%s", text);
    } else if (e) { /* 1e+20 becomes 1.0e+20: a float literal of Sosu has a dot */
        buf_add(b, "%.*s.0%s", (int)(e - text), text, e);
    } else {
        buf_add(b, "%s.0", text);
    }
    return true;
}

static bool string_literal(Buf *b, const Value *v) {
    buf_add(b, "\"");
    for (size_t i = 0; i < v->s_length; i++) {
        unsigned char c = (unsigned char)v->s[i];
        if (c == '"') buf_add(b, "\\\"");
        else if (c == '\\') buf_add(b, "\\\\");
        else if (c == '\n') buf_add(b, "\\n");
        else if (c == '\t') buf_add(b, "\\t");
        else if (c == '\r') buf_add(b, "\\r");
        else if (c < 0x20 || c == 0x7F) buf_add(b, "\\x%02X", c);
        else if (c >= 0x80) return false; /* UTF-8 is kept only as ASCII here: simpler, and rare */
        else buf_add(b, "%c", c);
    }
    buf_add(b, "\"");
    return true;
}

typedef struct {
    Buf constants;
    Buf structs;
    Buf functions;
    Buf skipped;
    Buf check;
    struct { char **items; size_t count; size_t cap; } names; /* every name of the module */
} Out;

static bool take_name(Out *o, const char *name) {
    for (size_t i = 0; i < o->names.count; i++) {
        if (strcmp(o->names.items[i], name) == 0) return false;
    }
    SOSU_VEC_PUSH(&o->names, sosu_strdup(name));
    return true;
}

/* A constant: `pub <type> NAME = value;`, and its check. */
static bool write_constant(P *p, Out *o, const char *name, const Value *v, const char *forced_type,
                           BindgenOutput *out) {
    (void)p;
    Buf value = {0};
    const char *type;
    if (v->kind == V_INT) {
        type = forced_type ? forced_type : int_name(v->width / 8, !v->is_unsigned);
        Value shown = *v;
        if (forced_type) {
            bool is_signed = forced_type[0] == 'i';
            unsigned width = (unsigned)atoi(forced_type + (is_signed ? 1 : 2));
            shown = convert_int(v, width, !is_signed);
        }
        integer_literal(&value, &shown, type);
        if (shown.is_unsigned) {
            buf_add(&o->check, "_Static_assert((unsigned long long)(%s) == %lluULL, \"%s\");\n", name,
                    (unsigned long long)shown.bits, name);
        } else {
            int64_t s = as_signed(&shown);
            if (s == INT64_MIN) buf_add(&o->check, "_Static_assert((long long)(%s) == -9223372036854775807LL - 1, \"%s\");\n", name, name);
            else buf_add(&o->check, "_Static_assert((long long)(%s) == %lldLL, \"%s\");\n", name, (long long)s, name);
        }
    } else if (v->kind == V_FLOAT) {
        type = v->is_f32 ? "f32" : "f64";
        if (!float_literal(&value, v->f)) {
            free(buf_take(&value));
            return false;
        }
    } else {
        type = "str";
        if (!string_literal(&value, v)) {
            free(buf_take(&value));
            return false;
        }
    }
    if (is_sosu_word(name) || !take_name(o, name)) {
        free(buf_take(&value));
        return false;
    }
    buf_add(&o->constants, "pub %s %s = %s;\n", type, name, value.text);
    free(buf_take(&value));
    out->constants++;
    return true;
}

static void skip_note(Out *o, BindgenOutput *out, const char *what, const char *name, const char *why) {
    buf_add(&o->skipped, "// %s `%s`: %s\n", what, name, why ? why : "a type that Sosu cannot write");
    out->skipped++;
}

/* The C name of a record for the check file. */
static char *c_name(const Record *r) {
    if (r->name) return sosu_strdup(r->name);
    return sosu_format("%s %s", r->is_union ? "union" : "struct", r->tag);
}

static void write_record(P *p, Out *o, Record *r, BindgenOutput *out) {
    r->emitted = true;
    uint64_t size, align;
    bool laid = layout_record(p, r);
    if (!take_name(o, r->sosu_name)) return;
    bool opaque = !r->complete || !laid;
    bool as_bytes = !opaque && (r->is_union || r->has_bits);
    /* the fields first: a field whose type cannot be written makes the struct bytes */
    Buf fields = {0};
    if (!opaque && !as_bytes) {
        for (size_t i = 0; i < r->field_count && !as_bytes; i++) {
            RField *f = &r->fields[i];
            CType *s = strip(f->type);
            if (s && s->kind == CK_RECORD && !s->record->name && !s->record->tag) name_record(p, s->record, r->sosu_name, i);
            TypeNote note = { false, false, NULL };
            char *type = sosu_type(p, f->type, 0, &note);
            if (!type || note.unsupported) {
                as_bytes = true;
                free(type);
                break;
            }
            if (note.uses_long) {
                buf_add(&p->warnings, "warning: `%s` uses `long`: written for the target (32 bits on Windows, 64 elsewhere)\n",
                        r->sosu_name);
            }
            char *name = sosu_name(f->name, i, "anonymous");
            buf_add(&fields, "    pub mut %s %s;\n", type, name);
            free(name);
            free(type);
        }
    }
    if (opaque) {
        buf_add(&o->structs, "pub struct %s { }\n\n", r->sosu_name);
        out->structs++;
        free(buf_take(&fields));
        return;
    }
    size = r->size;
    align = r->align;
    if (as_bytes) {
        free(buf_take(&fields));
        const char *element = align >= 8 ? "ui64" : align == 4 ? "ui32" : align == 2 ? "ui16" : "ui8";
        uint64_t unit = align >= 8 ? 8 : align;
        buf_add(&o->structs, "// %s %s: its bytes, of the same size and alignment\n",
                r->is_union ? "a union" : "a struct with bit fields", r->tag ? r->tag : r->sosu_name);
        if (size == 0) buf_add(&o->structs, "pub struct %s { }\n\n", r->sosu_name);
        else buf_add(&o->structs, "pub struct %s {\n    pub mut %s[%llu] bytes;\n}\n\n", r->sosu_name, element,
                     (unsigned long long)(size / unit));
    } else {
        buf_add(&o->structs, "pub struct %s {\n%s}\n\n", r->sosu_name, fields.text ? fields.text : "");
        free(buf_take(&fields));
    }
    out->structs++;
    /* the check: what the header says, and what Sosu will lay out */
    if (r->name || r->tag) {
        char *cn = c_name(r);
        buf_add(&o->check, "_Static_assert(sizeof(%s) == %llu, \"%s: size\");\n", cn, (unsigned long long)size, cn);
        buf_add(&o->check, "_Static_assert(_Alignof(%s) == %llu, \"%s: alignment\");\n", cn, (unsigned long long)align, cn);
        if (!as_bytes) {
            for (size_t i = 0; i < r->field_count; i++) {
                if (!r->fields[i].name) continue;
                buf_add(&o->check, "_Static_assert(offsetof(%s, %s) == %llu, \"%s.%s: offset\");\n", cn,
                        r->fields[i].name, (unsigned long long)r->fields[i].offset, cn, r->fields[i].name);
            }
        }
        free(cn);
    }
}

static void write_function(P *p, Out *o, Function *f, BindgenOutput *out) {
    CType *s = strip(f->type);
    if (is_sosu_word(f->name)) {
        skip_note(o, out, "function", f->name, "its name is a word of Sosu");
        return;
    }
    if (s->variadic) {
        skip_note(o, out, "function", f->name, "variadic (`...`)");
        return;
    }
    TypeNote note = { false, false, NULL };
    Buf b = {0};
    char *ret = sosu_type(p, s->inner, 1, &note);
    if (ret && !note.unsupported) {
        buf_add(&b, "    %s %s(", ret, f->name);
        for (size_t i = 0; i < s->param_count && !note.unsupported; i++) {
            char *type = sosu_type(p, s->params[i], 0, &note);
            if (!type || note.unsupported) {
                free(type);
                note.unsupported = true;
                break;
            }
            char *name = sosu_name(s->param_names[i], i, "a");
            /* two parameters of the same name, or one named like the function */
            for (size_t j = 0; j < i; j++) {
                if (s->param_names[j] && name && strcmp(s->param_names[j], name) == 0) {
                    free(name);
                    name = sosu_format("a%zu", i);
                }
            }
            buf_add(&b, "%s%s %s", i ? ", " : "", type, name);
            free(name);
            free(type);
        }
        buf_add(&b, ") uses io;\n");
    }
    free(ret);
    if (note.unsupported) {
        free(buf_take(&b));
        skip_note(o, out, "function", f->name, note.why);
        return;
    }
    if (!take_name(o, f->name)) {
        free(buf_take(&b));
        skip_note(o, out, "function", f->name, "its name is already taken");
        return;
    }
    if (note.uses_long) {
        buf_add(&p->warnings, "warning: `%s` uses `long`: written for the target (32 bits on Windows, 64 elsewhere)\n",
                f->name);
    }
    buf_add(&o->functions, "%s", b.text);
    free(buf_take(&b));
    out->functions++;
}

static bool wanted(const BindgenInput *in, const char *name) {
    return has_prefix(name, strlen(name), in->prefixes, in->prefix_count);
}

bool bindgen_generate(const BindgenInput *in, BindgenOutput *out) {
    memset(out, 0, sizeof *out);
    P p;
    memset(&p, 0, sizeof p);
    p.in = in;
    lex(in, &p.toks, &p.files);
    while (!at_eof(&p)) {
        size_t before = p.pos;
        parse_declaration(&p);
        if (p.pos == before) p.pos++;
    }

    Out o;
    memset(&o, 0, sizeof o);
    /* constants: the macros, then the values of the enums */
    for (size_t i = 0; i < p.macros.count; i++) {
        Macro *m = &p.macros.items[i];
        Value v;
        if (!eval_macro(&p, m, &v)) continue;
        write_constant(&p, &o, m->name, &v, NULL, out);
        if (v.kind == V_STRING) free(v.s);
    }
    for (size_t i = 0; i < p.enums.count; i++) {
        EnumDef *e = p.enums.items[i];
        const char *type = enum_type(&p, e);
        for (size_t j = 0; j < e->count; j++) {
            Enumerator *item = &e->items[j];
            if (!item->own || !item->known || !wanted(in, item->name)) continue;
            Value v = int_value((uint64_t)item->value, 64, false);
            write_constant(&p, &o, item->name, &v, type, out);
        }
    }
    /* functions, then the structs they need, and those that have the prefix */
    for (size_t i = 0; i < p.functions.count; i++) {
        Function *f = &p.functions.items[i];
        if (!f->own || f->is_static || !wanted(in, f->name)) continue;
        write_function(&p, &o, f, out);
    }
    for (size_t i = 0; i < p.records.count; i++) {
        Record *r = p.records.items[i];
        if (r->own && (r->name || r->tag) && wanted(in, r->name ? r->name : r->tag)) mark_needed(&p, r);
    }
    for (bool more = true; more;) {
        more = false;
        for (size_t i = 0; i < p.records.count; i++) {
            Record *r = p.records.items[i];
            if (r->needed && !r->emitted) {
                write_record(&p, &o, r, out);
                more = true;
            }
        }
    }

    Buf module = {0};
    buf_add(&module, "%s\n", in->comment);
    if (o.constants.text) buf_add(&module, "%s\n", o.constants.text);
    if (o.structs.text) buf_add(&module, "%s", o.structs.text);
    if (o.functions.text) buf_add(&module, "pub extern \"C\" {\n%s}\n", o.functions.text);
    if (o.skipped.text) buf_add(&module, "\n// Left out:\n%s", o.skipped.text);
    out->module = buf_take(&module);
    Buf check = {0};
    buf_add(&check, "/* Generated by sosuc bindgen --check: the layouts and values of the binding, checked\n"
                    "   against the header by the C compiler. */\n#include <stddef.h>\n#include \"%s\"\n\n%s",
            in->include, o.check.text ? o.check.text : "");
    out->check = buf_take(&check);
    out->warnings = buf_take(&p.warnings);

    free(buf_take(&o.constants));
    free(buf_take(&o.structs));
    free(buf_take(&o.functions));
    free(buf_take(&o.skipped));
    free(buf_take(&o.check));
    for (size_t i = 0; i < o.names.count; i++) free(o.names.items[i]);
    free(o.names.items);

    /* the state */
    for (size_t i = 0; i < p.types.count; i++) {
        CType *t = p.types.items[i];
        if (t->kind == CK_FUNC) {
            for (size_t j = 0; j < t->param_count; j++) free(t->param_names[j]);
            free(t->param_names);
            free(t->params);
        }
        if (t->kind == CK_UNKNOWN) free(t->name);
        free(t);
    }
    free(p.types.items);
    for (size_t i = 0; i < p.records.count; i++) {
        Record *r = p.records.items[i];
        for (size_t j = 0; j < r->field_count; j++) free(r->fields[j].name);
        free(r->fields);
        free(r->tag);
        free(r->name);
        free(r->sosu_name);
        free(r);
    }
    free(p.records.items);
    for (size_t i = 0; i < p.enums.count; i++) {
        EnumDef *e = p.enums.items[i];
        for (size_t j = 0; j < e->count; j++) free(e->items[j].name);
        free(e->items);
        free(e->tag);
        free(e->name);
        free(e);
    }
    free(p.enums.items);
    for (size_t i = 0; i < p.typedefs.count; i++) free(p.typedefs.items[i].name);
    free(p.typedefs.items);
    for (size_t i = 0; i < p.functions.count; i++) free(p.functions.items[i].name);
    free(p.functions.items);
    for (size_t i = 0; i < p.macros.count; i++) free(p.macros.items[i].name);
    free(p.macros.items);
    for (size_t i = 0; i < p.files.count; i++) free(p.files.items[i].path);
    free(p.files.items);
    free(p.toks.items);
    return true;
}

void bindgen_output_free(BindgenOutput *output) {
    free(output->module);
    free(output->check);
    free(output->warnings);
    memset(output, 0, sizeof *output);
}
