#include "fmt.h"

#include <stdlib.h>
#include <string.h>

#include "arena.h"
#include "ast.h"
#include "lexer.h"
#include "parser.h"

/* The style (spec §7.6):
   - 4 spaces of indentation; `{` at the end of the line, `}` on its own line;
   - one statement per line; non-empty blocks always span several lines; empty ones are `{ }`;
   - spaces around binary operators and after commas; none inside parentheses;
   - parentheses only where the precedence needs them;
   - at most one blank line, where the source has one;
   - comments stay where they are: on their own line, or at the end of a line after two spaces;
   - a line longer than LINE_WIDTH is broken at its outermost list or chain of operators (see
     "Long lines"). */

#define NAME_ARGS(name) (int)(name).length, (name).text

/* The longest line, in characters, before the formatter breaks it where it can. */
#define LINE_WIDTH 120

/* The constructs a line can be broken at: a list (arguments, parameters, the fields of a struct
   literal, the items of an array), or a chain of the same operator. */
enum { BREAK_LIST, BREAK_CHAIN };
#define MAX_BREAKS 64

typedef struct {
    char *items;
    size_t count;
    size_t cap;
} Buf;

static void put_n(Buf *buf, const char *text, size_t length) {
    if (buf->count + length + 1 > buf->cap) {
        buf->cap = (buf->count + length + 1) * 2;
        buf->items = sosu_realloc(buf->items, buf->cap);
    }
    memcpy(buf->items + buf->count, text, length);
    buf->count += length;
    buf->items[buf->count] = '\0';
}

typedef struct {
    const SourceFile *file;
    Buf out;
    int indent;
    const Comment *comments;
    size_t comment_count;
    size_t next_comment;
    bool block_start; /* just after `{` or at the start of the file: no blank line */
    /* The line being written (see "Long lines"): */
    int wrap;                         /* the construct to break, by its order on the line; -1: none */
    int breaks;                       /* the constructs met so far on the line, outside lists */
    int depth;                        /* the lists open on the line */
    unsigned char kinds[MAX_BREAKS];  /* BREAK_LIST or BREAK_CHAIN, by order */
    size_t widest;                    /* the widest line the broken construct went on to */
    size_t close_at;                  /* the end of the broken list, or 0 */
    size_t limit;                     /* the width a broken chain or array keeps to */
    size_t end_at;                    /* the end of the broken chain, or 0 */
} Fmt;

static void fmt_stmt(Fmt *f, const Stmt *stmt);
static void fmt_expr(Fmt *f, const Expr *e);
static void fmt_block(Fmt *f, const Stmt *block);

/* ---- Output ---- */

static void put(Fmt *f, const char *text) { put_n(&f->out, text, strlen(text)); }
static void put_name(Fmt *f, Name name) { put_n(&f->out, name.text, name.length); }

static void put_source(Fmt *f, size_t offset, size_t length) { put_n(&f->out, f->file->text + offset, length); }

static void start_line(Fmt *f) {
    for (int i = 0; i < f->indent; i++) put(f, "    ");
}

static void end_line(Fmt *f) {
    while (f->out.count > 0 && f->out.items[f->out.count - 1] == ' ') f->out.items[--f->out.count] = '\0';
    put(f, "\n");
}

static void blank_line(Fmt *f) {
    size_t n = f->out.count;
    if (f->block_start || n == 0 || (n >= 2 && f->out.items[n - 1] == '\n' && f->out.items[n - 2] == '\n')) return;
    put(f, "\n");
}

/* ---- Source layout ---- */

static size_t line_start(const Fmt *f, size_t offset) {
    while (offset > 0 && f->file->text[offset - 1] != '\n') offset--;
    return offset;
}

static size_t line_number(const Fmt *f, size_t offset) {
    return source_line_of(f->file, offset);
}

/* True if the line before the one of `offset` is empty in the source. */
static bool blank_before(const Fmt *f, size_t offset) {
    size_t start = line_start(f, offset);
    if (start == 0) return false;
    size_t previous = line_start(f, start - 1);
    for (size_t i = previous; i < start - 1; i++) {
        char c = f->file->text[i];
        if (c != ' ' && c != '\t' && c != '\r') return false;
    }
    return true;
}

/* ---- Comments ---- */

static void emit_comment(Fmt *f, const Comment *comment) {
    if (!comment->own_line && f->out.count > 0 && f->out.items[f->out.count - 1] == '\n') {
        f->out.items[--f->out.count] = '\0'; /* at the end of the previous line */
        put(f, "  ");
        put_source(f, comment->offset, comment->length);
        end_line(f);
        return;
    }
    if (blank_before(f, comment->offset)) blank_line(f);
    start_line(f);
    put_source(f, comment->offset, comment->length);
    end_line(f);
    f->block_start = false;
}

/* Prints the comments that come before `offset` in the source. */
static void flush_comments(Fmt *f, size_t offset) {
    while (f->next_comment < f->comment_count && f->comments[f->next_comment].offset < offset) {
        emit_comment(f, &f->comments[f->next_comment++]);
    }
}

static bool comment_before(const Fmt *f, size_t offset) {
    return f->next_comment < f->comment_count && f->comments[f->next_comment].offset < offset;
}

/* Before an item (declaration, member, statement, arm, variant): its comments, and a
   blank line if the source has one. */
static void begin_item(Fmt *f, size_t offset) {
    flush_comments(f, offset);
    if (blank_before(f, offset)) blank_line(f);
    f->block_start = false;
}

/* ---- Long lines ----

   A line is written whole first. If it is longer than LINE_WIDTH, it is written again with one
   of its constructs broken: the constructs that are not inside a list of the line, a chain of
   operators or a conditional (`c ? a : b`) first, then the lists from the last one to the first,
   until the lines it makes fit (or
   else the one whose widest line is the shortest, if it is shorter than the line whole). A broken list puts each item on its own line, one level
   deeper, and its closing symbol on a line of its own; each item is a line of its own, broken in
   turn if it is too long. A broken chain goes on over the next lines, two levels deeper, each
   starting with its operator; a broken conditional puts each `:` at the start of a line. Comments and string literals are never broken. The result depends
   on the syntax tree only, so formatting twice changes nothing. */

typedef struct {
    size_t count;
    size_t next_comment;
    bool block_start;
    int indent;
} Mark;

static Mark mark_of(const Fmt *f) {
    Mark m = { f->out.count, f->next_comment, f->block_start, f->indent };
    return m;
}

static void rewind_to(Fmt *f, Mark m) {
    f->out.count = m.count;
    f->out.items[m.count] = '\0';
    f->next_comment = m.next_comment;
    f->block_start = m.block_start;
    f->indent = m.indent;
}

/* The width in characters of the line that holds the output at `at`, up to `at`. */
static size_t width_to(const Fmt *f, size_t at) {
    size_t start = at;
    while (start > 0 && f->out.items[start - 1] != '\n') start--;
    size_t width = 0;
    for (size_t i = start; i < at; i++) {
        if (((unsigned char)f->out.items[i] & 0xC0) != 0x80) width++; /* not a continuation byte of UTF-8 */
    }
    return width;
}

/* The width of the line that holds the output at `from`, up to the end of the output or the
   first line end after `from`. */
static size_t line_width(const Fmt *f, size_t from) {
    size_t end = from;
    while (end < f->out.count && f->out.items[end] != '\n') end++;
    return width_to(f, end);
}

/* Starts a construct that can be broken; true if it is the one to break on this line. */
static bool enter_break(Fmt *f, int kind) {
    if (f->depth > 0) return false;
    int index = f->breaks++;
    if (index < MAX_BREAKS) f->kinds[index] = (unsigned char)kind;
    return index == f->wrap;
}

typedef void (*LineFn)(Fmt *f, const void *what);

/* Writes a line, breaking construct `wrap`; returns the width of its widest line among the first
   one and those the broken construct made. */
static size_t write_line(Fmt *f, LineFn print, const void *what, const char *suffix, int wrap, size_t from,
                         size_t limit) {
    f->wrap = wrap;
    f->limit = limit;
    f->breaks = 0;
    f->depth = 0;
    f->widest = 0;
    f->close_at = 0;
    f->end_at = 0;
    print(f, what);
    if (suffix) put(f, suffix);
    size_t width = line_width(f, from);
    if (f->widest > width) width = f->widest;
    if (f->close_at) {
        size_t closing = line_width(f, f->close_at);
        if (closing > width) width = closing;
    }
    return width;
}

/* Writes a line with `print` (from its indentation, without its line end), then `suffix`;
   broken if it is too long. */
static void fit_line(Fmt *f, LineFn print, const void *what, const char *suffix) {
    int saved_wrap = f->wrap, saved_breaks = f->breaks, saved_depth = f->depth;
    size_t saved_widest = f->widest, saved_close = f->close_at, saved_limit = f->limit, saved_end = f->end_at;
    unsigned char saved_kinds[MAX_BREAKS];
    memcpy(saved_kinds, f->kinds, sizeof saved_kinds);
    Mark mark = mark_of(f);
    size_t best_width = write_line(f, print, what, suffix, -1, mark.count, LINE_WIDTH);
    int count = f->breaks < MAX_BREAKS ? f->breaks : MAX_BREAKS;
    if (best_width > LINE_WIDTH && count > 0) {
        unsigned char kinds[MAX_BREAKS];
        memcpy(kinds, f->kinds, (size_t)count);
        int order[MAX_BREAKS], n = 0;
        for (int i = 0; i < count; i++) {
            if (kinds[i] == BREAK_CHAIN) order[n++] = i;
        }
        for (int i = count - 1; i >= 0; i--) {
            if (kinds[i] == BREAK_LIST) order[n++] = i;
        }
        int best = -1;
        size_t best_limit = LINE_WIDTH;
        bool fits = false;
        for (int k = 0; k < n && !fits; k++) {
            /* what follows the construct on its last line may not fit: then it keeps narrower */
            size_t limit = LINE_WIDTH;
            for (int attempt = 0; attempt < 2 && !fits; attempt++) {
                rewind_to(f, mark);
                size_t width = write_line(f, print, what, suffix, order[k], mark.count, limit);
                fits = width <= LINE_WIDTH;
                if (width < best_width) {
                    best_width = width;
                    best = order[k];
                    best_limit = limit;
                }
                if (width <= LINE_WIDTH || !f->end_at) break;
                size_t tail = line_width(f, f->end_at) - width_to(f, f->end_at);
                if (tail == 0 || tail >= LINE_WIDTH / 2) break;
                limit = LINE_WIDTH - tail;
            }
        }
        if (!fits) {
            rewind_to(f, mark);
            write_line(f, print, what, suffix, best, mark.count, best_limit);
        }
    }
    f->wrap = saved_wrap;
    f->breaks = saved_breaks;
    f->depth = saved_depth;
    f->widest = saved_widest;
    f->close_at = saved_close;
    f->limit = saved_limit;
    f->end_at = saved_end;
    memcpy(f->kinds, saved_kinds, sizeof saved_kinds);
}

/* Prints item `i` of a list. */
typedef void (*ItemFn)(Fmt *f, const void *items, size_t i);

typedef struct {
    const void *items;
    size_t i;
    ItemFn print;
} ItemRef;

static void item_line(Fmt *f, const void *what) {
    const ItemRef *ref = what;
    start_line(f);
    ref->print(f, ref->items, ref->i);
}

/* `open`, the items separated by `, `, `close` (with a space inside when `spaced`); or, when it
   is the construct broken, each item on its own line, followed by `,` (the last one too when
   `trailing`). */
static void fmt_list(Fmt *f, const char *open, const char *close, bool spaced, const void *items, size_t count,
                     ItemFn print, bool trailing) {
    if (count == 1) { /* one item: what breaks is inside it, `f(g(` ... `))` */
        put(f, open);
        if (spaced) put(f, " ");
        print(f, items, 0);
        if (spaced) put(f, " ");
        put(f, close);
        return;
    }
    bool broken = count > 0 && enter_break(f, BREAK_LIST);
    f->depth++;
    put(f, open);
    if (!broken) {
        if (spaced) put(f, " ");
        for (size_t i = 0; i < count; i++) {
            if (i) put(f, ", ");
            print(f, items, i);
        }
        if (spaced) put(f, " ");
    } else {
        end_line(f);
        f->indent++;
        for (size_t i = 0; i < count; i++) {
            ItemRef ref = { items, i, print };
            fit_line(f, item_line, &ref, i + 1 < count || trailing ? "," : NULL);
            end_line(f);
        }
        f->indent--;
        start_line(f);
    }
    put(f, close);
    if (broken) f->close_at = f->out.count;
    f->depth--;
}

/* ---- Types ---- */

static void fmt_path(Fmt *f, Path path) {
    for (size_t i = 0; i < path.count; i++) {
        if (i) put(f, ".");
        put_name(f, path.items[i]);
    }
}

static void fmt_effects(Fmt *f, Path effects) {
    if (effects.count == 0) return;
    put(f, " uses ");
    for (size_t i = 0; i < effects.count; i++) {
        if (i) put(f, ", ");
        put_name(f, effects.items[i]);
    }
}

static void fmt_type(Fmt *f, const TypeExpr *type);

/* `<A, B>` (spec §5.4). */
static void fmt_type_args(Fmt *f, TypeExpr **args, size_t count) {
    if (!count) return;
    put(f, "<");
    for (size_t i = 0; i < count; i++) {
        if (i) put(f, ", ");
        fmt_type(f, args[i]);
    }
    put(f, ">");
}

/* `<T: Ordered + Hash, uses E>`. */
static void fmt_type_params(Fmt *f, TypeParams params) {
    if (!params.count) return;
    put(f, "<");
    for (size_t i = 0; i < params.count; i++) {
        const TypeParam *param = &params.items[i];
        if (i) put(f, ", ");
        if (param->is_effect) put(f, "uses ");
        put_name(f, param->name);
        for (size_t j = 0; j < param->constraint_count; j++) {
            put(f, j ? " + " : ": ");
            fmt_path(f, param->constraints[j]);
        }
    }
    put(f, ">");
}

/* The type under a suffix: a function type in parentheses, `(fn(i32) -> i32)[2]`. */
static void fmt_inner_type(Fmt *f, const TypeExpr *type) {
    if (type->kind != TYPE_FN) {
        fmt_type(f, type);
        return;
    }
    put(f, "(");
    fmt_type(f, type);
    put(f, ")");
}

static void fmt_type(Fmt *f, const TypeExpr *type) {
    char size[32];
    switch (type->kind) {
    case TYPE_NAME:
        fmt_path(f, type->as.name);
        fmt_type_args(f, type->args, type->arg_count);
        break;
    case TYPE_POINTER: fmt_inner_type(f, type->as.inner); put(f, "*"); break;
    case TYPE_OPTION: fmt_inner_type(f, type->as.inner); put(f, "?"); break;
    case TYPE_SLICE: fmt_inner_type(f, type->as.inner); put(f, "[]"); break;
    case TYPE_ARRAY:
        fmt_inner_type(f, type->as.array.element);
        if (type->as.array.size_name.count) {
            put(f, "[");
            fmt_path(f, type->as.array.size_name);
            put(f, "]");
        } else {
            snprintf(size, sizeof size, "[%llu]", (unsigned long long)type->as.array.size);
            put(f, size);
        }
        break;
    case TYPE_RESULT:
        fmt_inner_type(f, type->as.result.value);
        put(f, "!");
        fmt_type(f, type->as.result.error);
        break;
    case TYPE_FN:
        put(f, "fn(");
        for (size_t i = 0; i < type->as.fn.param_count; i++) {
            if (i) put(f, ", ");
            if (type->as.fn.keeps[i]) put(f, "keeps ");
            fmt_type(f, type->as.fn.params[i]);
        }
        put(f, ") -> ");
        fmt_type(f, type->as.fn.ret);
        fmt_effects(f, type->as.fn.effects);
        break;
    case TYPE_EFFECTS:
        put(f, "uses ");
        for (size_t i = 0; i < type->as.effects.count; i++) {
            if (i) put(f, ", ");
            put_name(f, type->as.effects.items[i]);
        }
        break;
    }
}

/* True if the type ends with `*` or `?`, which an operator after `as` would extend. */
static bool ends_with_suffix(const TypeExpr *type) {
    if (type->kind == TYPE_POINTER || type->kind == TYPE_OPTION) return true;
    if (type->kind == TYPE_FN) return type->as.fn.effects.count == 0 && ends_with_suffix(type->as.fn.ret);
    return false;
}

/* ---- Expressions ---- */

enum {
    P_TERNARY = 1,
    P_RANGE,
    P_OR,
    P_AND,
    P_COMPARE,
    P_BIT_OR,
    P_BIT_XOR,
    P_BIT_AND,
    P_SHIFT,
    P_ADD,
    P_MUL,
    P_PREFIX,  /* unary operators, try, as */
    P_POSTFIX, /* calls, indexes, members, and everything that needs no parentheses */
};

static int binary_precedence(TokenKind op) {
    switch (op) {
    case TOK_PIPE_PIPE: return P_OR;
    case TOK_AMP_AMP: return P_AND;
    case TOK_EQ_EQ: case TOK_BANG_EQ: case TOK_LT: case TOK_LT_EQ: case TOK_GT: case TOK_GT_EQ: return P_COMPARE;
    case TOK_PIPE: return P_BIT_OR;
    case TOK_CARET: return P_BIT_XOR;
    case TOK_AMP: return P_BIT_AND;
    case TOK_SHL: case TOK_SHR: return P_SHIFT;
    case TOK_PLUS: case TOK_MINUS: return P_ADD;
    default: return P_MUL;
    }
}

static int precedence(const Expr *e) {
    switch (e->kind) {
    case EXPR_LAMBDA: return 0; /* below everything: `((x) => x)(3)` */
    case EXPR_TERNARY: return P_TERNARY;
    case EXPR_RANGE: return P_RANGE;
    case EXPR_BINARY: return binary_precedence(e->as.binary.op);
    case EXPR_UNARY: case EXPR_TRY: case EXPR_CAST: case EXPR_TRY_CAST: return P_PREFIX;
    default: return P_POSTFIX;
    }
}

static bool is_cast(const Expr *e) { return e->kind == EXPR_CAST || e->kind == EXPR_TRY_CAST; }

/* `x as T*` followed by an operator: the operator could be read as part of the type. */
static bool open_cast(const Expr *e) { return is_cast(e) && ends_with_suffix(e->as.cast.type); }

static void fmt_operand(Fmt *f, const Expr *e, bool parens) {
    if (parens) put(f, "(");
    fmt_expr(f, e);
    if (parens) put(f, ")");
}

static void expr_item(Fmt *f, const void *items, size_t i) { fmt_expr(f, ((Expr *const *)items)[i]); }

static void field_item(Fmt *f, const void *items, size_t i) {
    const FieldInit *field = &((const FieldInit *)items)[i];
    put_name(f, field->name);
    put(f, ": ");
    fmt_expr(f, field->value);
}

/* An array broken over several lines: its items go on a line while they fit. */
static void fmt_array_fill(Fmt *f, const Expr *e) {
    f->depth++;
    put(f, "[");
    end_line(f);
    f->indent++;
    start_line(f);
    for (size_t i = 0; i < e->as.array.count; i++) {
        Mark mark = mark_of(f);
        if (i) put(f, " ");
        fmt_expr(f, e->as.array.items[i]);
        put(f, ",");
        if (i && line_width(f, mark.count) > f->limit) {
            rewind_to(f, mark);
            end_line(f);
            start_line(f);
            fmt_expr(f, e->as.array.items[i]);
            put(f, ",");
        }
    }
    end_line(f);
    f->indent--;
    start_line(f);
    put(f, "]");
    f->depth--;
}

/* True if `op` makes chains broken over several lines when they are too long. */
static bool chains(TokenKind op) {
    int p = binary_precedence(op);
    return p != P_COMPARE; /* `a < b < c` does not parse */
}

/* `op` and an operand at the start of a new line, two levels deeper than `indent`. */
static void chain_line(Fmt *f, int indent, const char *op, const Expr *operand, bool parens) {
    end_line(f);
    f->indent = indent + 2;
    start_line(f);
    size_t start = f->out.count;
    put(f, op);
    put(f, " ");
    fmt_operand(f, operand, parens);
    size_t width = line_width(f, start);
    if (width > f->widest) f->widest = width;
}

/* ` op ` and an operand; in a broken chain, on the next line, two levels deeper than `indent`,
   when it does not fit on this one. */
static void chain_part(Fmt *f, bool broken, int indent, const char *op, const Expr *operand, bool parens) {
    Mark mark = mark_of(f);
    put(f, " ");
    put(f, op);
    put(f, " ");
    fmt_operand(f, operand, parens);
    if (broken && line_width(f, mark.count) > f->limit) {
        rewind_to(f, mark);
        chain_line(f, indent, op, operand, parens);
    }
}

/* `a && b && c`: the operands of the binary operators of the same precedence on the left of
   `e`, from the first; at most `max`. Returns how many there are, or 0 if there are more. */
static size_t chain_of(const Expr *e, const Expr **nodes, size_t max) {
    int p = binary_precedence(e->as.binary.op);
    size_t n = 0;
    for (const Expr *node = e;; node = node->as.binary.left) {
        if (n == max) return 0;
        nodes[n++] = node;
        const Expr *left = node->as.binary.left;
        if (left->kind != EXPR_BINARY || binary_precedence(left->as.binary.op) != p || open_cast(left)) break;
    }
    for (size_t i = 0; i < n / 2; i++) { /* from the first operator */
        const Expr *t = nodes[i];
        nodes[i] = nodes[n - 1 - i];
        nodes[n - 1 - i] = t;
    }
    return n;
}

/* A chain of binary operators of the same precedence: on one line, or, when it is the construct
   broken, going on over the next lines while its operands do not fit. False if `e` is not one. */
static bool fmt_chain(Fmt *f, const Expr *e) {
    const Expr *nodes[256];
    size_t n = chains(e->as.binary.op) ? chain_of(e, nodes, sizeof nodes / sizeof *nodes) : 0;
    if (n == 0) return false;
    int p = binary_precedence(e->as.binary.op);
    bool broken = enter_break(f, BREAK_CHAIN);
    const Expr *first = nodes[0]->as.binary.left;
    fmt_operand(f, first, precedence(first) < p || open_cast(first));
    int indent = f->indent;
    for (size_t i = 0; i < n; i++) {
        const Expr *right = nodes[i]->as.binary.right;
        chain_part(f, broken, indent, token_kind_spelling(nodes[i]->as.binary.op), right,
                   precedence(right) <= p || open_cast(right));
    }
    f->indent = indent;
    if (broken) f->end_at = f->out.count;
    return true;
}

static void fmt_pattern(Fmt *f, const Pattern *pattern) {
    if (pattern->kind == PATTERN_WILDCARD) {
        put(f, "_");
        return;
    }
    fmt_path(f, pattern->path);
    if (!pattern->has_parens) return;
    put(f, "(");
    for (size_t i = 0; i < pattern->binding_count; i++) {
        if (i) put(f, ", ");
        put_name(f, pattern->bindings[i]);
    }
    put(f, ")");
}

static void arm_line(Fmt *f, const void *what) {
    const MatchArm *arm = what;
    start_line(f);
    fmt_pattern(f, &arm->pattern);
    put(f, " => ");
    if (arm->block) fmt_block(f, arm->block);
    else fmt_expr(f, arm->expr);
}

static void fmt_match(Fmt *f, const Expr *e) {
    put(f, "match (");
    fmt_expr(f, e->as.match.subject);
    put(f, ") {");
    end_line(f);
    f->indent++;
    f->block_start = true;
    for (size_t i = 0; i < e->as.match.arm_count; i++) {
        const MatchArm *arm = &e->as.match.arms[i];
        begin_item(f, arm->pattern.offset);
        fit_line(f, arm_line, arm, ",");
        end_line(f);
    }
    flush_comments(f, e->offset + e->length - 1);
    f->indent--;
    start_line(f);
    put(f, "}");
}

/* An array literal written over several lines (tables of data): its lines stay, each with the
   items that start on it. */
static void fmt_array_lines(Fmt *f, const Expr *e) {
    f->depth++;
    put(f, "[");
    end_line(f);
    f->indent++;
    f->block_start = true;
    for (size_t i = 0; i < e->as.array.count; i++) {
        const Expr *item = e->as.array.items[i];
        if (i > 0 && line_number(f, item->offset) == line_number(f, e->as.array.items[i - 1]->offset) &&
            !comment_before(f, item->offset)) {
            put(f, " ");
        } else {
            if (i > 0) end_line(f);
            begin_item(f, item->offset);
            start_line(f);
        }
        fmt_expr(f, item);
        put(f, ",");
    }
    end_line(f);
    flush_comments(f, e->offset + e->length - 1);
    f->indent--;
    start_line(f);
    put(f, "]");
    f->depth--;
}

static void fmt_expr(Fmt *f, const Expr *e) {
    switch (e->kind) {
    case EXPR_ERROR:
        break;
    case EXPR_INT:
    case EXPR_FLOAT:
    case EXPR_STRING:
    case EXPR_CHAR:
        put_source(f, e->offset, e->length);
        break;
    case EXPR_BOOL: put(f, e->as.bool_value ? "true" : "false"); break;
    case EXPR_NAME:
        put_name(f, e->as.name);
        fmt_type_args(f, e->type_args, e->type_arg_count);
        break;
    case EXPR_SELF: put(f, "self"); break;
    case EXPR_BASE: put(f, "base"); break;
    case EXPR_RESULT: put(f, "result"); break;
    case EXPR_UNARY: {
        const Expr *operand = e->as.unary.operand;
        put(f, token_kind_spelling(e->as.unary.op));
        /* `-(-x)`, `-(x as f32)`: without parentheses they would read differently */
        fmt_operand(f, operand, precedence(operand) < P_PREFIX || is_cast(operand) || operand->kind == EXPR_UNARY);
        break;
    }
    case EXPR_TYPE:
        fmt_type(f, e->as.type_expr);
        break;
    case EXPR_TRY:
        put(f, "try ");
        fmt_operand(f, e->as.inner, precedence(e->as.inner) < P_PREFIX || is_cast(e->as.inner));
        break;
    case EXPR_BINARY: {
        if (fmt_chain(f, e)) break;
        int p = binary_precedence(e->as.binary.op);
        const Expr *left = e->as.binary.left;
        const Expr *right = e->as.binary.right;
        bool left_parens = precedence(left) < p || (p == P_COMPARE && precedence(left) == P_COMPARE) || open_cast(left);
        bool right_parens = precedence(right) <= p || open_cast(right);
        fmt_operand(f, left, left_parens);
        put(f, " ");
        put(f, token_kind_spelling(e->as.binary.op));
        put(f, " ");
        fmt_operand(f, right, right_parens);
        break;
    }
    case EXPR_CAST:
    case EXPR_TRY_CAST:
        fmt_operand(f, e->as.cast.value, precedence(e->as.cast.value) < P_PREFIX);
        put(f, e->kind == EXPR_CAST ? " as " : " as? ");
        fmt_type(f, e->as.cast.type);
        break;
    case EXPR_TERNARY: {
        /* broken: `: c2 ? b` at the start of each line, for `c1 ? a : c2 ? b : d` */
        bool broken = enter_break(f, BREAK_CHAIN);
        int indent = f->indent;
        const Expr *node = e;
        const Expr *cond = node->as.ternary.cond;
        fmt_operand(f, cond, precedence(cond) <= P_TERNARY || open_cast(cond));
        for (;;) {
            chain_part(f, broken, indent, "?", node->as.ternary.then_expr, false);
            const Expr *other = node->as.ternary.else_expr;
            if (!broken) {
                chain_part(f, false, indent, ":", other, false);
                break;
            }
            if (other->kind != EXPR_TERNARY) {
                chain_line(f, indent, ":", other, false);
                break;
            }
            cond = other->as.ternary.cond;
            chain_line(f, indent, ":", cond, precedence(cond) <= P_TERNARY || open_cast(cond));
            node = other;
        }
        f->indent = indent;
        if (broken) f->end_at = f->out.count;
        break;
    }
    case EXPR_RANGE:
        fmt_operand(f, e->as.range.start, precedence(e->as.range.start) < P_OR);
        put(f, e->as.range.inclusive ? "..=" : "..");
        fmt_operand(f, e->as.range.end, precedence(e->as.range.end) < P_OR);
        break;
    case EXPR_CALL:
        fmt_operand(f, e->as.call.callee, precedence(e->as.call.callee) < P_POSTFIX);
        fmt_list(f, "(", ")", false, e->as.call.args, e->as.call.arg_count, expr_item, false);
        break;
    case EXPR_INDEX:
        fmt_operand(f, e->as.index.object, precedence(e->as.index.object) < P_POSTFIX);
        put(f, "[");
        fmt_expr(f, e->as.index.index);
        put(f, "]");
        break;
    case EXPR_FIELD:
        fmt_operand(f, e->as.field.object, precedence(e->as.field.object) < P_POSTFIX);
        put(f, ".");
        put_name(f, e->as.field.name);
        fmt_type_args(f, e->type_args, e->type_arg_count);
        break;
    case EXPR_STRUCT_LIT:
        fmt_type(f, e->as.struct_lit.type);
        if (e->as.struct_lit.field_count == 0) {
            put(f, "{ }");
            break;
        }
        fmt_list(f, "{", "}", true, e->as.struct_lit.fields, e->as.struct_lit.field_count, field_item, false);
        break;
    case EXPR_MATCH:
        fmt_match(f, e);
        break;
    case EXPR_ARRAY:
        if (!e->as.array.repeat && e->as.array.count &&
            line_number(f, e->offset) != line_number(f, e->offset + e->length - 1)) {
            fmt_array_lines(f, e);
            break;
        }
        if (e->as.array.repeat) {
            char count[32];
            put(f, "[");
            fmt_expr(f, e->as.array.repeat);
            snprintf(count, sizeof count, "; %llu", (unsigned long long)e->as.array.repeat_count);
            put(f, count);
            put(f, "]");
        } else if (e->as.array.count > 0 && enter_break(f, BREAK_LIST)) {
            fmt_array_fill(f, e);
        } else {
            f->depth++;
            put(f, "[");
            for (size_t i = 0; i < e->as.array.count; i++) {
                if (i) put(f, ", ");
                fmt_expr(f, e->as.array.items[i]);
            }
            put(f, "]");
            f->depth--;
        }
        break;
    case EXPR_LAMBDA:
        put(f, "(");
        for (size_t i = 0; i < e->as.lambda.param_count; i++) {
            const Param *param = &e->as.lambda.params[i];
            if (i) put(f, ", ");
            if (param->is_keeps) put(f, "keeps ");
            if (param->type) {
                fmt_type(f, param->type);
                put(f, " ");
            }
            put_name(f, param->name);
        }
        put(f, ") => ");
        if (e->as.lambda.block) fmt_block(f, e->as.lambda.block);
        else fmt_expr(f, e->as.lambda.body);
        break;
    }
}

/* ---- Statements ---- */

/* `{ ... }` after what the line already holds; the caller ends the line. */
static void fmt_block(Fmt *f, const Stmt *block) {
    size_t end = block->offset + block->length - 1; /* the `}` */
    if (block->as.block.count == 0 && !comment_before(f, end)) {
        put(f, "{ }");
        return;
    }
    put(f, "{");
    end_line(f);
    f->indent++;
    f->block_start = true;
    for (size_t i = 0; i < block->as.block.count; i++) fmt_stmt(f, block->as.block.items[i]);
    flush_comments(f, end);
    f->indent--;
    start_line(f);
    put(f, "}");
    f->block_start = false;
}

/* A variable, an assignment or an expression, without `;` (also used in `for` and `defer`). */
static void fmt_simple(Fmt *f, const Stmt *stmt) {
    switch (stmt->kind) {
    case STMT_VAR:
        if (stmt->as.var.is_mut) put(f, "mut ");
        if (stmt->as.var.type) fmt_type(f, stmt->as.var.type);
        else put(f, "auto");
        put(f, " ");
        put_name(f, stmt->as.var.name);
        put(f, " = ");
        fmt_expr(f, stmt->as.var.init);
        break;
    case STMT_ASSIGN:
        fmt_expr(f, stmt->as.assign.target);
        put(f, " ");
        put(f, token_kind_spelling(stmt->as.assign.op));
        put(f, " ");
        fmt_expr(f, stmt->as.assign.value);
        break;
    case STMT_EXPR:
        fmt_expr(f, stmt->as.expr);
        break;
    default:
        break;
    }
}

static void fmt_if(Fmt *f, const Stmt *stmt);

/* `if (...) {` after `} else `: measured as a line of its own. */
static void else_if_line(Fmt *f, const void *what) { fmt_if(f, what); }

static void fmt_if(Fmt *f, const Stmt *stmt) {
    put(f, "if (");
    fmt_expr(f, stmt->as.if_.cond);
    put(f, ") ");
    fmt_block(f, stmt->as.if_.then_block);
    const Stmt *other = stmt->as.if_.else_stmt;
    if (!other) return;
    put(f, " else ");
    if (other->kind == STMT_IF) fit_line(f, else_if_line, other, NULL);
    else fmt_block(f, other);
}

static void stmt_line(Fmt *f, const void *what) {
    const Stmt *stmt = what;
    start_line(f);
    switch (stmt->kind) {
    case STMT_BLOCK:
        fmt_block(f, stmt);
        break;
    case STMT_VAR:
    case STMT_ASSIGN:
        fmt_simple(f, stmt);
        put(f, ";");
        break;
    case STMT_EXPR:
        fmt_simple(f, stmt);
        if (stmt->as.expr->kind != EXPR_MATCH) put(f, ";");
        break;
    case STMT_IF:
        fmt_if(f, stmt);
        break;
    case STMT_WHILE:
        put(f, "while (");
        fmt_expr(f, stmt->as.while_.cond);
        put(f, ") ");
        fmt_block(f, stmt->as.while_.body);
        break;
    case STMT_FOR:
        put(f, "for (");
        if (stmt->as.for_.init) fmt_simple(f, stmt->as.for_.init);
        put(f, ";");
        if (stmt->as.for_.cond) {
            put(f, " ");
            fmt_expr(f, stmt->as.for_.cond);
        }
        put(f, ";");
        if (stmt->as.for_.step) {
            put(f, " ");
            fmt_simple(f, stmt->as.for_.step);
        }
        put(f, ") ");
        fmt_block(f, stmt->as.for_.body);
        break;
    case STMT_FOR_IN:
        put(f, "for (");
        if (stmt->as.for_in.is_mut) put(f, "mut ");
        if (stmt->as.for_in.type) fmt_type(f, stmt->as.for_in.type);
        else put(f, "auto");
        put(f, " ");
        put_name(f, stmt->as.for_in.name);
        put(f, " in ");
        fmt_expr(f, stmt->as.for_in.iterable);
        put(f, ") ");
        fmt_block(f, stmt->as.for_in.body);
        break;
    case STMT_RETURN:
        put(f, "return");
        if (stmt->as.expr) {
            put(f, " ");
            fmt_expr(f, stmt->as.expr);
        }
        put(f, ";");
        break;
    case STMT_BREAK: put(f, "break;"); break;
    case STMT_CONTINUE: put(f, "continue;"); break;
    case STMT_DEFER:
        put(f, "defer ");
        if (stmt->as.inner->kind == STMT_BLOCK) {
            fmt_block(f, stmt->as.inner);
        } else {
            fmt_simple(f, stmt->as.inner);
            put(f, ";");
        }
        break;
    case STMT_UNSAFE:
        put(f, "unsafe ");
        fmt_block(f, stmt->as.inner);
        break;
    }
}

static void fmt_stmt(Fmt *f, const Stmt *stmt) {
    begin_item(f, stmt->offset);
    fit_line(f, stmt_line, stmt, NULL);
    end_line(f);
}

/* ---- Declarations ---- */

static void param_item(Fmt *f, const void *items, size_t i) {
    const Param *param = &((const Param *)items)[i];
    if (param->is_mut) put(f, "mut ");
    if (param->is_keeps) put(f, "keeps ");
    fmt_type(f, param->type);
    put(f, " ");
    put_name(f, param->name);
}

static void fmt_params(Fmt *f, const Param *params, size_t count) {
    fmt_list(f, "(", ")", false, params, count, param_item, false);
}

static void contract_line(Fmt *f, const void *what) {
    const Contract *contract = what;
    start_line(f);
    put(f, contract->kind == CONTRACT_REQUIRES ? "requires " : "ensures ");
    fmt_expr(f, contract->expr);
}

static void function_line(Fmt *f, const void *what) {
    const FnDecl *fn = what;
    start_line(f);
    if (fn->is_export) put(f, "export \"C\" ");
    else if (fn->is_pub) put(f, "pub ");
    if (fn->method_kind == METHOD_VIRTUAL) put(f, "virtual ");
    if (fn->method_kind == METHOD_OVERRIDE) put(f, "override ");
    if (fn->is_unsafe) put(f, "unsafe ");
    fmt_type(f, fn->ret);
    put(f, " ");
    put_name(f, fn->name);
    fmt_type_params(f, fn->type_params);
    fmt_params(f, fn->params, fn->param_count);
    if (fn->keeps_self) put(f, " keeps self");
    fmt_effects(f, fn->effects);
    if (!fn->body) {
        put(f, ";");
        return;
    }
    if (fn->contract_count == 0) {
        put(f, " ");
    } else {
        end_line(f);
        f->indent++;
        for (size_t i = 0; i < fn->contract_count; i++) {
            fit_line(f, contract_line, &fn->contracts[i], NULL);
            end_line(f);
        }
        f->indent--;
        start_line(f);
    }
    fmt_block(f, fn->body);
}

static void fmt_function(Fmt *f, const FnDecl *fn) {
    fit_line(f, function_line, fn, NULL);
    end_line(f);
}

/* A member of a struct or class: a field or a method, in the order of the source. */
typedef struct {
    size_t offset;
    const Field *field;
    const FnDecl *method;
} Member;

static void field_line(Fmt *f, const void *what) {
    const Field *field = what;
    start_line(f);
    if (field->is_pub) put(f, "pub ");
    if (field->is_mut) put(f, "mut ");
    fmt_type(f, field->type);
    put(f, " ");
    put_name(f, field->name);
    if (field->init) {
        put(f, " = ");
        fmt_expr(f, field->init);
    }
}

static void fmt_members(Fmt *f, const Decl *decl) {
    size_t count = decl->as.type.field_count + decl->as.type.method_count;
    Member *members = sosu_alloc((count + 1) * sizeof *members);
    size_t n = 0;
    for (size_t i = 0; i < decl->as.type.field_count; i++) {
        Member m = { decl->as.type.fields[i].type->offset, &decl->as.type.fields[i], NULL };
        members[n++] = m;
    }
    for (size_t i = 0; i < decl->as.type.method_count; i++) {
        Member m = { decl->as.type.methods[i].offset, NULL, &decl->as.type.methods[i] };
        members[n++] = m;
    }
    for (size_t i = 1; i < n; i++) { /* insertion sort by position */
        Member m = members[i];
        size_t j = i;
        while (j > 0 && members[j - 1].offset > m.offset) {
            members[j] = members[j - 1];
            j--;
        }
        members[j] = m;
    }

    size_t end = decl->offset + decl->length - 1;
    if (n == 0 && !comment_before(f, end)) {
        put(f, " { }");
        end_line(f);
        free(members);
        return;
    }
    put(f, " {");
    end_line(f);
    f->indent++;
    f->block_start = true;
    for (size_t i = 0; i < n; i++) {
        begin_item(f, members[i].offset);
        if (members[i].method) {
            fmt_function(f, members[i].method);
            continue;
        }
        fit_line(f, field_line, members[i].field, ";");
        end_line(f);
    }
    flush_comments(f, end);
    f->indent--;
    start_line(f);
    put(f, "}");
    end_line(f);
    free(members);
}

static void fmt_variant(Fmt *f, const Variant *variant) {
    put_name(f, variant->name);
    if (variant->has_parens) fmt_params(f, variant->fields, variant->field_count);
}

static void fmt_enum(Fmt *f, const Decl *decl) {
    size_t end = decl->offset + decl->length - 1;
    size_t count = decl->as.enum_.variant_count;
    /* written on one line in the source, without comments: it stays on one line */
    bool one_line = line_number(f, decl->offset) == line_number(f, end) && !comment_before(f, end);
    if (count == 0 && !comment_before(f, end)) {
        put(f, " { }");
        end_line(f);
        return;
    }
    if (one_line) {
        put(f, " { ");
        for (size_t i = 0; i < count; i++) {
            if (i) put(f, ", ");
            fmt_variant(f, &decl->as.enum_.variants[i]);
        }
        put(f, " }");
        end_line(f);
        return;
    }
    put(f, " {");
    end_line(f);
    f->indent++;
    f->block_start = true;
    for (size_t i = 0; i < count; i++) {
        begin_item(f, decl->as.enum_.variants[i].name.offset);
        start_line(f);
        fmt_variant(f, &decl->as.enum_.variants[i]);
        put(f, ",");
        end_line(f);
    }
    flush_comments(f, end);
    f->indent--;
    start_line(f);
    put(f, "}");
    end_line(f);
}

static void const_line(Fmt *f, const void *what) {
    const Decl *decl = what;
    start_line(f);
    if (decl->is_pub) put(f, "pub ");
    fmt_type(f, decl->as.const_.type);
    put(f, " ");
    put_name(f, decl->as.const_.name);
    put(f, " = ");
    fmt_expr(f, decl->as.const_.value);
}

static void fmt_decl(Fmt *f, const Decl *decl) {
    begin_item(f, decl->offset);
    switch (decl->kind) {
    case DECL_IMPORT:
        start_line(f);
        put(f, "import ");
        fmt_path(f, decl->as.import);
        put(f, ";");
        end_line(f);
        break;
    case DECL_STRUCT:
    case DECL_CLASS:
    case DECL_INTERFACE:
        start_line(f);
        if (decl->is_pub) put(f, "pub ");
        put(f, decl->kind == DECL_STRUCT ? "struct " : decl->kind == DECL_CLASS ? "class " : "interface ");
        put_name(f, decl->as.type.name);
        fmt_type_params(f, decl->type_params);
        for (size_t i = 0; i < decl->as.type.base_count; i++) {
            put(f, i ? ", " : " : ");
            fmt_path(f, decl->as.type.bases[i]);
            if (decl->as.type.base_args) fmt_type_args(f, decl->as.type.base_args[i], decl->as.type.base_arg_counts[i]);
        }
        fmt_members(f, decl);
        break;
    case DECL_ENUM:
        start_line(f);
        if (decl->is_pub) put(f, "pub ");
        put(f, "enum ");
        put_name(f, decl->as.enum_.name);
        fmt_type_params(f, decl->type_params);
        fmt_enum(f, decl);
        break;
    case DECL_FN:
        fmt_function(f, &decl->as.fn);
        break;
    case DECL_EXTERN: {
        size_t end = decl->offset + decl->length - 1;
        start_line(f);
        if (decl->is_pub) put(f, "pub ");
        put(f, "extern ");
        put_name(f, decl->as.extern_.abi);
        put(f, " {");
        end_line(f);
        f->indent++;
        f->block_start = true;
        for (size_t i = 0; i < decl->as.extern_.fn_count; i++) {
            begin_item(f, decl->as.extern_.fns[i].offset);
            fmt_function(f, &decl->as.extern_.fns[i]);
        }
        flush_comments(f, end);
        f->indent--;
        start_line(f);
        put(f, "}");
        end_line(f);
        break;
    }
    case DECL_TEST:
        start_line(f);
        put(f, "test ");
        put_name(f, decl->as.test.name);
        put(f, " ");
        fmt_block(f, decl->as.test.body);
        end_line(f);
        break;
    case DECL_CONST:
        fit_line(f, const_line, decl, ";");
        end_line(f);
        break;
    }
}

/* ---- Interface files (spec §10.10) ----

   A .ui.sosu file becomes calls of widgets as it is parsed, so its syntax tree is not its
   source: it is formatted from its tokens and comments instead. Its lines stay where they are
   (one blank line at most between them); each is indented by the braces and the brackets open
   before it, and the spaces between its tokens follow the rules of `ui_space`, or else the
   source (whether it had a space). */

typedef struct {
    size_t offset;
    size_t length;
    TokenKind kind; /* TOK_EOF for a comment */
} UiPiece;

static int ui_piece_order(const void *a, const void *b) {
    size_t x = ((const UiPiece *)a)->offset, y = ((const UiPiece *)b)->offset;
    return x < y ? -1 : x > y;
}

static bool ui_opens(TokenKind k) { return k == TOK_LPAREN || k == TOK_LBRACKET || k == TOK_LBRACE; }
static bool ui_closes(TokenKind k) { return k == TOK_RPAREN || k == TOK_RBRACKET || k == TOK_RBRACE; }

/* Whether a space goes between `a` and `b` on a line; `before_a` is the piece before `a` (NULL:
   `a` starts the line); `spaced`: the source had one. */
static bool ui_space(const UiPiece *before_a, const UiPiece *a, const UiPiece *b, bool spaced) {
    if (b->kind == TOK_EOF || a->kind == TOK_EOF) return true;  /* comments */
    if (b->kind == TOK_COMMA || b->kind == TOK_RPAREN || b->kind == TOK_RBRACKET || b->kind == TOK_DOT ||
        b->kind == TOK_SEMICOLON) {
        return false;
    }
    if (a->kind == TOK_LPAREN || a->kind == TOK_LBRACKET || a->kind == TOK_DOT) return false;
    if (a->kind == TOK_COMMA || a->kind == TOK_SEMICOLON || a->kind == TOK_COLON) return true;
    if (b->kind == TOK_COLON) {  /* `name: value`, a property; else the `:` of `c ? a : b` or of a theme */
        bool property = a->kind == TOK_IDENT && (!before_a || before_a->kind == TOK_LPAREN ||
                                                 before_a->kind == TOK_COMMA || before_a->kind == TOK_LBRACE);
        return !property && spaced;
    }
    if (b->kind == TOK_LBRACE || a->kind == TOK_LBRACE || b->kind == TOK_RBRACE) return true;
    if (b->kind == TOK_LPAREN && (a->kind == TOK_IDENT || a->kind == TOK_RPAREN || a->kind == TOK_RBRACKET)) return false;
    if (b->kind == TOK_LBRACKET && (a->kind == TOK_IDENT || a->kind == TOK_RPAREN || a->kind == TOK_RBRACKET)) return false;
    return spaced;
}

static char *format_ui(const SourceFile *file, DiagList *diags) {
    TokenList tokens = {0};
    CommentList comments = {0};
    Arena arena = {0};
    size_t errors_before = diags->error_count;
    lex_file_comments(file, &tokens, &comments, diags);
    TokenList check = {0};
    DiagList ignored = {0};
    lex_file(file, &check, &ignored);
    parse_ui_module(file, &check, &arena, diags);  /* a file with errors is not formatted */
    token_list_free(&check);
    diag_free(&ignored);
    if (diags->error_count != errors_before) {
        arena_free(&arena);
        free(comments.items);
        token_list_free(&tokens);
        return NULL;
    }
    size_t count = 0;
    UiPiece *pieces = sosu_alloc((tokens.count + comments.count + 1) * sizeof *pieces);
    for (size_t i = 0; i < tokens.count; i++) {
        if (tokens.items[i].kind == TOK_EOF) continue;
        pieces[count++] = (UiPiece){ tokens.items[i].offset, tokens.items[i].length, tokens.items[i].kind };
    }
    for (size_t i = 0; i < comments.count; i++) {
        pieces[count++] = (UiPiece){ comments.items[i].offset, comments.items[i].length, TOK_EOF };
    }
    qsort(pieces, count, sizeof *pieces, ui_piece_order);

    Buf out = {0};
    put_n(&out, "", 0);
    int depth = 0;
    size_t last_line = 0;
    size_t i = 0;
    while (i < count) {
        size_t line = source_line_of(file, pieces[i].offset);
        if (last_line > 0 && line > last_line + 1) put_n(&out, "\n", 1);  /* one blank line at most */
        size_t end = i;  /* the pieces of this line */
        for (;;) {
            if (end >= count) break;
            if (source_line_of(file, pieces[end].offset) != line) break;
            end++;
        }
        size_t closing = 0;  /* the brackets that close at the start of the line are outdented */
        while (i + closing < end && ui_closes(pieces[i + closing].kind)) closing++;
        int indent = depth > (int)closing ? depth - (int)closing : 0;
        for (int k = 0; k < indent; k++) put_n(&out, "    ", 4);
        for (size_t k = i; k < end; k++) {
            if (k > i) {
                const UiPiece *a = &pieces[k - 1];
                bool spaced = pieces[k].offset > a->offset + a->length;
                if (pieces[k].kind == TOK_EOF && a->kind != TOK_EOF) put_n(&out, "  ", 2);  /* a comment after code */
                else if (ui_space(k - 1 > i ? &pieces[k - 2] : NULL, a, &pieces[k], spaced)) put_n(&out, " ", 1);
            }
            put_n(&out, file->text + pieces[k].offset, pieces[k].length);
            if (ui_opens(pieces[k].kind)) depth++;
            if (ui_closes(pieces[k].kind) && depth > 0) depth--;
        }
        put_n(&out, "\n", 1);
        size_t end_line, end_column;  /* a comment over several lines ends lower */
        source_position(file, pieces[end - 1].offset + pieces[end - 1].length, &end_line, &end_column);
        last_line = end_line;
        i = end;
    }
    free(pieces);
    arena_free(&arena);
    free(comments.items);
    token_list_free(&tokens);
    return out.items;
}

char *format_source(const SourceFile *file, DiagList *diags) {
    size_t path_length = strlen(file->path);
    if (path_length >= 8 && strcmp(file->path + path_length - 8, ".ui.sosu") == 0) return format_ui(file, diags);
    TokenList tokens = {0};
    CommentList comments = {0};
    Arena arena = {0};
    size_t errors_before = diags->error_count;
    lex_file_comments(file, &tokens, &comments, diags);
    Module *module = parse_module(file, &tokens, &arena, diags);

    char *result = NULL;
    if (diags->error_count == errors_before) {
        Fmt f = {0};
        f.file = file;
        f.comments = comments.items;
        f.comment_count = comments.count;
        f.block_start = true;
        f.wrap = -1;
        f.limit = LINE_WIDTH;
        put(&f, "");
        for (size_t i = 0; i < module->count; i++) fmt_decl(&f, module->decls[i]);
        flush_comments(&f, file->length + 1);
        while (f.out.count > 0 && f.out.items[f.out.count - 1] == '\n') f.out.items[--f.out.count] = '\0';
        if (f.out.count > 0) put(&f, "\n");
        result = f.out.items;
    }
    arena_free(&arena);
    free(comments.items);
    token_list_free(&tokens);
    return result;
}
