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
   - long lines are not broken (v0). */

#define NAME_ARGS(name) (int)(name).length, (name).text

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
    size_t line, column;
    source_position(f->file, offset, &line, &column);
    return line;
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
            fmt_type(f, type->as.fn.params[i]);
        }
        put(f, ") -> ");
        fmt_type(f, type->as.fn.ret);
        fmt_effects(f, type->as.fn.effects);
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

static void fmt_args(Fmt *f, Expr **args, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (i) put(f, ", ");
        fmt_expr(f, args[i]);
    }
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
        start_line(f);
        fmt_pattern(f, &arm->pattern);
        put(f, " => ");
        if (arm->block) fmt_block(f, arm->block);
        else fmt_expr(f, arm->expr);
        put(f, ",");
        end_line(f);
    }
    flush_comments(f, e->offset + e->length - 1);
    f->indent--;
    start_line(f);
    put(f, "}");
}

/* An array literal written over several lines: one element per line (tables of data). */
static void fmt_array_lines(Fmt *f, const Expr *e) {
    put(f, "[");
    end_line(f);
    f->indent++;
    f->block_start = true;
    for (size_t i = 0; i < e->as.array.count; i++) {
        begin_item(f, e->as.array.items[i]->offset);
        start_line(f);
        fmt_expr(f, e->as.array.items[i]);
        put(f, ",");
        end_line(f);
    }
    flush_comments(f, e->offset + e->length - 1);
    f->indent--;
    start_line(f);
    put(f, "]");
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
    case EXPR_TRY:
        put(f, "try ");
        fmt_operand(f, e->as.inner, precedence(e->as.inner) < P_PREFIX || is_cast(e->as.inner));
        break;
    case EXPR_BINARY: {
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
    case EXPR_TERNARY:
        fmt_operand(f, e->as.ternary.cond, precedence(e->as.ternary.cond) <= P_TERNARY || open_cast(e->as.ternary.cond));
        put(f, " ? ");
        fmt_expr(f, e->as.ternary.then_expr);
        put(f, " : ");
        fmt_expr(f, e->as.ternary.else_expr);
        break;
    case EXPR_RANGE:
        fmt_operand(f, e->as.range.start, precedence(e->as.range.start) < P_OR);
        put(f, e->as.range.inclusive ? "..=" : "..");
        fmt_operand(f, e->as.range.end, precedence(e->as.range.end) < P_OR);
        break;
    case EXPR_CALL:
        fmt_operand(f, e->as.call.callee, precedence(e->as.call.callee) < P_POSTFIX);
        put(f, "(");
        fmt_args(f, e->as.call.args, e->as.call.arg_count);
        put(f, ")");
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
        put(f, "{ ");
        for (size_t i = 0; i < e->as.struct_lit.field_count; i++) {
            if (i) put(f, ", ");
            put_name(f, e->as.struct_lit.fields[i].name);
            put(f, ": ");
            fmt_expr(f, e->as.struct_lit.fields[i].value);
        }
        put(f, " }");
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
        put(f, "[");
        if (e->as.array.repeat) {
            char count[32];
            fmt_expr(f, e->as.array.repeat);
            snprintf(count, sizeof count, "; %llu", (unsigned long long)e->as.array.repeat_count);
            put(f, count);
        } else {
            fmt_args(f, e->as.array.items, e->as.array.count);
        }
        put(f, "]");
        break;
    case EXPR_LAMBDA:
        put(f, "(");
        for (size_t i = 0; i < e->as.lambda.param_count; i++) {
            const Param *param = &e->as.lambda.params[i];
            if (i) put(f, ", ");
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

static void fmt_if(Fmt *f, const Stmt *stmt) {
    put(f, "if (");
    fmt_expr(f, stmt->as.if_.cond);
    put(f, ") ");
    fmt_block(f, stmt->as.if_.then_block);
    const Stmt *other = stmt->as.if_.else_stmt;
    if (!other) return;
    put(f, " else ");
    if (other->kind == STMT_IF) fmt_if(f, other);
    else fmt_block(f, other);
}

static void fmt_stmt(Fmt *f, const Stmt *stmt) {
    begin_item(f, stmt->offset);
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
    end_line(f);
}

/* ---- Declarations ---- */

static void fmt_params(Fmt *f, const Param *params, size_t count) {
    put(f, "(");
    for (size_t i = 0; i < count; i++) {
        if (i) put(f, ", ");
        if (params[i].is_mut) put(f, "mut ");
        fmt_type(f, params[i].type);
        put(f, " ");
        put_name(f, params[i].name);
    }
    put(f, ")");
}

static void fmt_function(Fmt *f, const FnDecl *fn) {
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
    fmt_effects(f, fn->effects);
    if (!fn->body) {
        put(f, ";");
        end_line(f);
        return;
    }
    if (fn->contract_count == 0) {
        put(f, " ");
    } else {
        end_line(f);
        f->indent++;
        for (size_t i = 0; i < fn->contract_count; i++) {
            start_line(f);
            put(f, fn->contracts[i].kind == CONTRACT_REQUIRES ? "requires " : "ensures ");
            fmt_expr(f, fn->contracts[i].expr);
            end_line(f);
        }
        f->indent--;
        start_line(f);
    }
    fmt_block(f, fn->body);
    end_line(f);
}

/* A member of a struct or class: a field or a method, in the order of the source. */
typedef struct {
    size_t offset;
    const Field *field;
    const FnDecl *method;
} Member;

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
        const Field *field = members[i].field;
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
        put(f, ";");
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
        start_line(f);
        if (decl->is_pub) put(f, "pub ");
        fmt_type(f, decl->as.const_.type);
        put(f, " ");
        put_name(f, decl->as.const_.name);
        put(f, " = ");
        fmt_expr(f, decl->as.const_.value);
        put(f, ";");
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
        size_t line, column;
        source_position(file, pieces[i].offset, &line, &column);
        if (last_line > 0 && line > last_line + 1) put_n(&out, "\n", 1);  /* one blank line at most */
        size_t end = i;  /* the pieces of this line */
        for (;;) {
            if (end >= count) break;
            size_t piece_line, piece_column;
            source_position(file, pieces[end].offset, &piece_line, &piece_column);
            if (piece_line != line) break;
            end++;
        }
        int closing = 0;  /* the brackets that close at the start of the line are outdented */
        while (i + closing < end && ui_closes(pieces[i + closing].kind)) closing++;
        int indent = depth - closing > 0 ? depth - closing : 0;
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
