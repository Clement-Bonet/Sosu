#include "parser.h"

#include <stdlib.h>
#include <string.h>

/* Recursive descent parser. Binary operators use precedence climbing (spec §5.1).

   Error recovery: the first error sets `panic`, which silences further errors until the
   parser resynchronizes (at the end of a statement, member or declaration, or at a
   closing bracket), so one mistake gives one diagnostic. */

typedef struct {
    const SourceFile *file;
    const Token *tokens;
    size_t count;
    size_t pos;
    Arena *arena;
    DiagList *diags;
    bool panic;
    bool no_struct_literal; /* `Name {` starts a body, not a literal (contracts) */
} Parser;

#define VEC(T) struct { T *items; size_t count; size_t cap; }

typedef VEC(Name) NameVec;

/* Moves a temporary vector into the arena. */
static void *finish_vec(Parser *p, void *items, size_t count, size_t size) {
    void *result = arena_copy(p->arena, items, count * size);
    free(items);
    return result;
}

#define FINISH(p, vec) finish_vec((p), (vec).items, (vec).count, sizeof *(vec).items)

static Stmt *parse_statement(Parser *p);
static Stmt *parse_block(Parser *p);
static Stmt *parse_if(Parser *p);
static Expr *parse_expr(Parser *p);
static TypeExpr *parse_type(Parser *p, bool in_cast);

/* ---- Tokens ---- */

static const Token *token_at(const Parser *p, size_t index) {
    return &p->tokens[index < p->count ? index : p->count - 1]; /* the last token is EOF */
}

static const Token *cur(const Parser *p) { return token_at(p, p->pos); }
static TokenKind kind_at(const Parser *p, size_t ahead) { return token_at(p, p->pos + ahead)->kind; }
static bool check(const Parser *p, TokenKind kind) { return cur(p)->kind == kind; }
static const Token *previous(const Parser *p) { return token_at(p, p->pos ? p->pos - 1 : 0); }

static const Token *advance(Parser *p) {
    const Token *token = cur(p);
    if (token->kind != TOK_EOF) p->pos++;
    return token;
}

static bool accept(Parser *p, TokenKind kind) {
    if (!check(p, kind)) return false;
    advance(p);
    return true;
}

/* Invalid tokens were reported by the lexer: step over them silently. */
static void skip_error_tokens(Parser *p) {
    while (check(p, TOK_ERROR)) advance(p);
}

static bool token_is(const Parser *p, const Token *token, const char *text) {
    size_t length = strlen(text);
    return token->kind == TOK_IDENT && token->length == length &&
           memcmp(p->file->text + token->offset, text, length) == 0;
}

static bool is_effect_name(const Parser *p, const Token *token) {
    return token_is(p, token, "io") || token_is(p, token, "time") ||
           token_is(p, token, "random") || token_is(p, token, "threads");
}

static Name make_name(const Parser *p, const Token *token) {
    Name name;
    name.text = p->file->text + token->offset;
    name.length = token->length;
    name.offset = token->offset;
    return name;
}

static size_t span_from(const Parser *p, size_t start) {
    const Token *last = previous(p);
    size_t end = last->offset + last->length;
    return end > start ? end - start : 0;
}

/* ---- Diagnostics ---- */

static Diagnostic *error_at(Parser *p, const char *code, size_t offset, size_t length, const char *fmt, ...) {
    if (p->panic) return NULL;
    p->panic = true;
    va_list args;
    va_start(args, fmt);
    Diagnostic *diag = diag_verror(p->diags, code, p->file, offset, length, fmt, args);
    va_end(args);
    return diag;
}

static void describe_token(const Parser *p, const Token *token, char *buffer, size_t size) {
    switch (token->kind) {
    case TOK_IDENT:
    case TOK_INT:
    case TOK_FLOAT:
    case TOK_STRING:
    case TOK_CHAR: {
        int length = token->length > 32 ? 32 : (int)token->length;
        snprintf(buffer, size, "`%.*s%s`", length, p->file->text + token->offset, token->length > 32 ? "..." : "");
        break;
    }
    default:
        snprintf(buffer, size, "%s", token_kind_describe(token->kind));
        break;
    }
}

static bool is_assign_op(TokenKind kind) {
    switch (kind) {
    case TOK_EQ: case TOK_PLUS_EQ: case TOK_MINUS_EQ: case TOK_STAR_EQ: case TOK_SLASH_EQ:
    case TOK_PERCENT_EQ: case TOK_AMP_EQ: case TOK_PIPE_EQ: case TOK_CARET_EQ:
    case TOK_SHL_EQ: case TOK_SHR_EQ:
        return true;
    default:
        return false;
    }
}

static bool on_later_line(const Parser *p, const Token *before, const Token *after) {
    size_t line_before, line_after, column;
    source_position(p->file, before->offset, &line_before, &column);
    source_position(p->file, after->offset, &line_after, &column);
    return line_after > line_before;
}

/* Reports "expected <what>, found <current token>". `after_expression` is set where an
   expression may just have ended, so a stray `=` is explained as an assignment. */
static void report_expected(Parser *p, const char *code, const char *what, bool after_expression) {
    const Token *token = cur(p);
    if (after_expression && is_assign_op(token->kind)) {
        Diagnostic *d = error_at(p, "E0205", token->offset, token->length, "assignment is a statement, not an expression");
        diag_help(d, token->kind == TOK_EQ ? "use `==` to compare values" : "move the assignment to its own statement");
        return;
    }
    char found[64];
    describe_token(p, token, found, sizeof found);
    /* A token missing at the end of a line is reported right after the previous token. */
    size_t offset = token->offset;
    size_t length = token->length;
    if (p->pos > 0 && on_later_line(p, previous(p), token)) {
        offset = previous(p)->offset + previous(p)->length;
        length = 0;
    }
    error_at(p, code, offset, length, "expected %s, found %s", what, found);
}

static bool expect(Parser *p, TokenKind kind) {
    skip_error_tokens(p);
    if (accept(p, kind)) return true;
    bool after_expression = kind == TOK_RPAREN || kind == TOK_RBRACKET || kind == TOK_COMMA ||
                            kind == TOK_SEMICOLON;
    report_expected(p, "E0200", token_kind_describe(kind), after_expression);
    return false;
}

static bool expect_name(Parser *p, Name *out, const char *what) {
    skip_error_tokens(p);
    if (check(p, TOK_IDENT)) {
        *out = make_name(p, advance(p));
        return true;
    }
    report_expected(p, "E0200", what, false);
    out->text = "";
    out->length = 0;
    out->offset = cur(p)->offset;
    return false;
}

static bool is_opening(TokenKind kind) { return kind == TOK_LPAREN || kind == TOK_LBRACKET || kind == TOK_LBRACE; }
static bool is_closing(TokenKind kind) { return kind == TOK_RPAREN || kind == TOK_RBRACKET || kind == TOK_RBRACE; }

/* Expects a closing bracket. On error, skips to the matching one so that parsing
   resumes in a known place, and ends the panic if it is found. */
static void expect_closing(Parser *p, TokenKind closer) {
    if (expect(p, closer)) return;
    size_t depth = 0;
    while (!check(p, TOK_EOF)) {
        TokenKind kind = cur(p)->kind;
        if (depth == 0 && kind == closer) {
            advance(p);
            p->panic = false;
            return;
        }
        if (is_opening(kind)) {
            depth++;
        } else if (is_closing(kind)) {
            if (depth == 0) return;
            depth--;
        } else if (kind == TOK_SEMICOLON && depth == 0 && closer != TOK_RBRACE) {
            return;
        }
        advance(p);
    }
}

static void skip_balanced_braces(Parser *p) {
    size_t depth = 0;
    while (!check(p, TOK_EOF)) {
        TokenKind kind = advance(p)->kind;
        if (kind == TOK_LBRACE) depth++;
        else if (kind == TOK_RBRACE && --depth == 0) return;
    }
}

static bool starts_statement(TokenKind kind) {
    switch (kind) {
    case TOK_KW_IF: case TOK_KW_WHILE: case TOK_KW_FOR: case TOK_KW_RETURN: case TOK_KW_BREAK:
    case TOK_KW_CONTINUE: case TOK_KW_DEFER: case TOK_KW_UNSAFE: case TOK_KW_MATCH:
    case TOK_KW_MUT: case TOK_KW_AUTO:
        return true;
    default:
        return false;
    }
}

static bool starts_declaration(TokenKind kind) {
    switch (kind) {
    case TOK_KW_STRUCT: case TOK_KW_CLASS: case TOK_KW_INTERFACE: case TOK_KW_ENUM:
    case TOK_KW_IMPORT: case TOK_KW_EXTERN: case TOK_KW_PUB: case TOK_KW_TEST:
        return true;
    default:
        return false;
    }
}

/* After an error in a statement or member: skip to its end. */
static void sync_statement(Parser *p) {
    p->panic = false;
    while (!check(p, TOK_EOF)) {
        TokenKind before = previous(p)->kind;
        if (p->pos > 0 && (before == TOK_SEMICOLON || before == TOK_RBRACE)) return;
        TokenKind kind = cur(p)->kind;
        if (kind == TOK_RBRACE || starts_statement(kind)) return;
        if (kind == TOK_LBRACE) skip_balanced_braces(p);
        else advance(p);
    }
}

/* After an error in a declaration header: skip to the next declaration. */
static void sync_declaration(Parser *p) {
    p->panic = false;
    while (!check(p, TOK_EOF)) {
        TokenKind kind = cur(p)->kind;
        if (starts_declaration(kind)) return;
        if (kind == TOK_LBRACE) {
            skip_balanced_braces(p);
            return;
        }
        advance(p);
        if (kind == TOK_SEMICOLON) return;
    }
}

/* ---- Lookahead ---- */

/* Skips the tokens of a type starting at `index`, without reporting anything.
   Returns the index just after the type, or 0 if there is no type there. */
static size_t skip_type(const Parser *p, size_t index) {
    const Token *token = token_at(p, index);
    if (token->kind != TOK_IDENT) return 0;

    if (token_is(p, token, "fn") && token_at(p, index + 1)->kind == TOK_LPAREN) {
        size_t depth = 0;
        index++;
        do {
            TokenKind kind = token_at(p, index)->kind;
            if (kind == TOK_LPAREN) depth++;
            else if (kind == TOK_RPAREN) depth--;
            else if (kind == TOK_EOF) return 0;
            index++;
        } while (depth > 0);
        if (token_at(p, index)->kind != TOK_ARROW) return 0;
        index = skip_type(p, index + 1);
        if (index == 0) return 0;
        if (token_at(p, index)->kind == TOK_KW_USES) {
            if (token_at(p, index + 1)->kind != TOK_IDENT) return 0;
            index += 2;
            while (token_at(p, index)->kind == TOK_COMMA && is_effect_name(p, token_at(p, index + 1))) index += 2;
        }
    } else {
        index++;
        while (token_at(p, index)->kind == TOK_DOT && token_at(p, index + 1)->kind == TOK_IDENT) index += 2;
    }

    for (;;) {
        TokenKind kind = token_at(p, index)->kind;
        if (kind == TOK_STAR || kind == TOK_QUESTION) {
            index++;
        } else if (kind == TOK_LBRACKET) {
            if (token_at(p, index + 1)->kind == TOK_RBRACKET) index += 2;
            else if (token_at(p, index + 1)->kind == TOK_INT && token_at(p, index + 2)->kind == TOK_RBRACKET) index += 3;
            else return index;
        } else if (kind == TOK_BANG && token_at(p, index + 1)->kind == TOK_IDENT) {
            index += 2;
            while (token_at(p, index)->kind == TOK_DOT && token_at(p, index + 1)->kind == TOK_IDENT) index += 2;
        } else {
            return index;
        }
    }
}

/* Spec §5: a statement starting with `Type Ident` is a declaration. */
static bool at_variable_declaration(const Parser *p) {
    if (check(p, TOK_KW_MUT) || check(p, TOK_KW_AUTO)) return true;
    size_t after = skip_type(p, p->pos);
    return after != 0 && token_at(p, after)->kind == TOK_IDENT;
}

static bool at_function(const Parser *p) {
    if (check(p, TOK_KW_UNSAFE)) return true;
    size_t after = skip_type(p, p->pos);
    return after != 0 && token_at(p, after)->kind == TOK_IDENT && token_at(p, after + 1)->kind == TOK_LPAREN;
}

/* ---- Names and types ---- */

static Path parse_path(Parser *p) {
    NameVec names = {0};
    Name name;
    expect_name(p, &name, "a name");
    SOSU_VEC_PUSH(&names, name);
    while (check(p, TOK_DOT) && kind_at(p, 1) == TOK_IDENT) {
        advance(p);
        SOSU_VEC_PUSH(&names, make_name(p, advance(p)));
    }
    Path path;
    path.count = names.count;
    path.items = FINISH(p, names);
    return path;
}

/* `uses io, time`. In a function type, a comma may also end the type (`fn() -> void
   uses io, i32 x`), so the list only continues there with a predefined effect name. */
static Path parse_effects(Parser *p, bool in_type) {
    NameVec names = {0};
    for (;;) {
        Name name;
        expect_name(p, &name, "an effect name");
        SOSU_VEC_PUSH(&names, name);
        if (!check(p, TOK_COMMA)) break;
        if (in_type && !is_effect_name(p, token_at(p, p->pos + 1))) break;
        advance(p);
    }
    Path path;
    path.count = names.count;
    path.items = FINISH(p, names);
    return path;
}

static TypeExpr *new_type(Parser *p, TypeKind kind, size_t start) {
    TypeExpr *type = arena_alloc(p->arena, sizeof *type);
    type->kind = kind;
    type->offset = start;
    return type;
}

static TypeExpr *parse_base_type(Parser *p) {
    skip_error_tokens(p);
    size_t start = cur(p)->offset;
    if (token_is(p, cur(p), "fn") && kind_at(p, 1) == TOK_LPAREN) {
        TypeExpr *type = new_type(p, TYPE_FN, start);
        advance(p);
        advance(p);
        VEC(TypeExpr *) params = {0};
        if (!check(p, TOK_RPAREN)) {
            do {
                TypeExpr *param = parse_type(p, false);
                SOSU_VEC_PUSH(&params, param);
            } while (accept(p, TOK_COMMA));
        }
        expect_closing(p, TOK_RPAREN);
        type->as.fn.param_count = params.count;
        type->as.fn.params = FINISH(p, params);
        expect(p, TOK_ARROW);
        type->as.fn.ret = parse_type(p, false);
        if (accept(p, TOK_KW_USES)) type->as.fn.effects = parse_effects(p, true);
        type->length = span_from(p, start);
        return type;
    }
    TypeExpr *type = new_type(p, TYPE_NAME, start);
    if (check(p, TOK_IDENT)) {
        type->as.name = parse_path(p);
    } else {
        report_expected(p, "E0202", "a type", false);
    }
    type->length = span_from(p, start);
    return type;
}

static bool starts_operand(TokenKind kind) {
    switch (kind) {
    case TOK_IDENT: case TOK_INT: case TOK_FLOAT: case TOK_STRING: case TOK_CHAR: case TOK_LPAREN:
    case TOK_KW_TRUE: case TOK_KW_FALSE: case TOK_KW_SELF: case TOK_KW_BASE:
    case TOK_KW_TRY: case TOK_KW_MATCH:
    case TOK_MINUS: case TOK_BANG: case TOK_TILDE: case TOK_AMP: case TOK_STAR:
        return true;
    default:
        return false;
    }
}

/* After `as`, `*` and `?` are type suffixes unless an operand follows:
   `x as i32 * 2` multiplies, `p as u8*;` converts to a pointer. */
static bool suffix_is_operator(const Parser *p) {
    size_t ahead = 1;
    while (kind_at(p, ahead) == TOK_STAR) ahead++;
    return starts_operand(kind_at(p, ahead));
}

static TypeExpr *parse_type(Parser *p, bool in_cast) {
    size_t start = cur(p)->offset;
    TypeExpr *type = parse_base_type(p);
    for (;;) {
        TokenKind kind = cur(p)->kind;
        TypeExpr *outer = NULL;
        if (kind == TOK_STAR || kind == TOK_QUESTION) {
            if (in_cast && suffix_is_operator(p)) break;
            advance(p);
            outer = new_type(p, kind == TOK_STAR ? TYPE_POINTER : TYPE_OPTION, start);
            outer->as.inner = type;
        } else if (kind == TOK_LBRACKET) {
            advance(p);
            if (check(p, TOK_INT)) {
                outer = new_type(p, TYPE_ARRAY, start);
                outer->as.array.element = type;
                outer->as.array.size = advance(p)->int_value;
            } else {
                outer = new_type(p, TYPE_SLICE, start);
                outer->as.inner = type;
            }
            expect_closing(p, TOK_RBRACKET);
        } else if (kind == TOK_BANG) {
            advance(p);
            outer = new_type(p, TYPE_RESULT, start);
            outer->as.result.value = type;
            outer->as.result.error = parse_base_type(p);
        } else {
            break;
        }
        outer->length = span_from(p, start);
        type = outer;
    }
    return type;
}

/* ---- Expressions ---- */

enum {
    PREC_NONE,
    PREC_OR,      /* || */
    PREC_AND,     /* && */
    PREC_COMPARE, /* == != < <= > >= (not chainable) */
    PREC_BIT_OR,  /* | */
    PREC_BIT_XOR, /* ^ */
    PREC_BIT_AND, /* & */
    PREC_SHIFT,   /* << >> */
    PREC_ADD,     /* + - */
    PREC_MUL,     /* * / % */
};

static int binary_precedence(TokenKind kind) {
    switch (kind) {
    case TOK_PIPE_PIPE: return PREC_OR;
    case TOK_AMP_AMP: return PREC_AND;
    case TOK_EQ_EQ: case TOK_BANG_EQ: case TOK_LT: case TOK_LT_EQ: case TOK_GT: case TOK_GT_EQ: return PREC_COMPARE;
    case TOK_PIPE: return PREC_BIT_OR;
    case TOK_CARET: return PREC_BIT_XOR;
    case TOK_AMP: return PREC_BIT_AND;
    case TOK_SHL: case TOK_SHR: return PREC_SHIFT;
    case TOK_PLUS: case TOK_MINUS: return PREC_ADD;
    case TOK_STAR: case TOK_SLASH: case TOK_PERCENT: return PREC_MUL;
    default: return PREC_NONE;
    }
}

static Expr *new_expr(Parser *p, ExprKind kind, size_t start) {
    Expr *expr = arena_alloc(p->arena, sizeof *expr);
    expr->kind = kind;
    expr->offset = start;
    return expr;
}

static Expr *close_expr(Parser *p, Expr *expr) {
    expr->length = span_from(p, expr->offset);
    return expr;
}

static bool is_type_path(const Expr *expr) {
    if (expr->kind == EXPR_NAME) return true;
    return expr->kind == EXPR_FIELD && is_type_path(expr->as.field.object);
}

static void collect_path(const Expr *expr, NameVec *names) {
    if (expr->kind == EXPR_FIELD) {
        collect_path(expr->as.field.object, names);
        SOSU_VEC_PUSH(names, expr->as.field.name);
    } else {
        SOSU_VEC_PUSH(names, expr->as.name);
    }
}

static Expr *parse_struct_literal(Parser *p, Expr *type_expr) {
    Expr *expr = new_expr(p, EXPR_STRUCT_LIT, type_expr->offset);
    TypeExpr *type = new_type(p, TYPE_NAME, type_expr->offset);
    type->length = type_expr->length;
    NameVec names = {0};
    collect_path(type_expr, &names);
    type->as.name.count = names.count;
    type->as.name.items = FINISH(p, names);
    expr->as.struct_lit.type = type;

    advance(p); /* { */
    bool saved = p->no_struct_literal;
    p->no_struct_literal = false;
    VEC(FieldInit) fields = {0};
    while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
        FieldInit field;
        expect_name(p, &field.name, "a field name");
        expect(p, TOK_COLON);
        field.value = parse_expr(p);
        SOSU_VEC_PUSH(&fields, field);
        if (!accept(p, TOK_COMMA)) break;
    }
    expect_closing(p, TOK_RBRACE);
    p->no_struct_literal = saved;
    expr->as.struct_lit.field_count = fields.count;
    expr->as.struct_lit.fields = FINISH(p, fields);
    return close_expr(p, expr);
}

static void parse_pattern(Parser *p, Pattern *pattern) {
    skip_error_tokens(p);
    pattern->offset = cur(p)->offset;
    if (token_is(p, cur(p), "_")) {
        advance(p);
        pattern->kind = PATTERN_WILDCARD;
    } else if (check(p, TOK_IDENT)) {
        pattern->kind = PATTERN_VARIANT;
        pattern->path = parse_path(p);
        if (accept(p, TOK_LPAREN)) {
            pattern->has_parens = true;
            NameVec bindings = {0};
            if (!check(p, TOK_RPAREN)) {
                do {
                    Name name;
                    expect_name(p, &name, "a binding name");
                    SOSU_VEC_PUSH(&bindings, name);
                } while (accept(p, TOK_COMMA));
            }
            expect_closing(p, TOK_RPAREN);
            pattern->binding_count = bindings.count;
            pattern->bindings = FINISH(p, bindings);
        }
    } else {
        report_expected(p, "E0200", "a pattern (`_`, `Variant` or `Variant(a, b)`)", false);
    }
    pattern->length = span_from(p, pattern->offset);
}

static Expr *parse_match(Parser *p) {
    Expr *expr = new_expr(p, EXPR_MATCH, advance(p)->offset);
    bool saved = p->no_struct_literal;
    p->no_struct_literal = false;
    expect(p, TOK_LPAREN);
    expr->as.match.subject = parse_expr(p);
    expect_closing(p, TOK_RPAREN);
    expect(p, TOK_LBRACE);

    VEC(MatchArm) arms = {0};
    while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
        MatchArm arm = {0};
        parse_pattern(p, &arm.pattern);
        expect(p, TOK_FAT_ARROW);
        if (check(p, TOK_LBRACE)) arm.block = parse_block(p);
        else arm.expr = parse_expr(p);
        SOSU_VEC_PUSH(&arms, arm);
        if (!accept(p, TOK_COMMA)) break;
    }
    expect_closing(p, TOK_RBRACE);
    p->no_struct_literal = saved;
    expr->as.match.arm_count = arms.count;
    expr->as.match.arms = FINISH(p, arms);
    return close_expr(p, expr);
}

/* `[a, b, c]` (trailing comma allowed) or `[value; count]` with a literal count. */
static Expr *parse_array(Parser *p) {
    Expr *expr = new_expr(p, EXPR_ARRAY, advance(p)->offset);
    bool saved = p->no_struct_literal;
    p->no_struct_literal = false;
    VEC(Expr *) items = {0};
    if (!check(p, TOK_RBRACKET)) {
        Expr *first = parse_expr(p);
        if (accept(p, TOK_SEMICOLON)) {
            expr->as.array.repeat = first;
            if (check(p, TOK_INT)) expr->as.array.repeat_count = advance(p)->int_value;
            else report_expected(p, "E0200", "the number of elements (an integer literal)", false);
        } else {
            SOSU_VEC_PUSH(&items, first);
            while (accept(p, TOK_COMMA) && !check(p, TOK_RBRACKET)) {
                Expr *item = parse_expr(p);
                SOSU_VEC_PUSH(&items, item);
            }
        }
    }
    expect_closing(p, TOK_RBRACKET);
    p->no_struct_literal = saved;
    expr->as.array.count = items.count;
    expr->as.array.items = FINISH(p, items);
    return close_expr(p, expr);
}

/* `(...) =>` starts a lambda, planned for v1. */
static bool at_lambda(const Parser *p) {
    size_t depth = 0;
    size_t index = p->pos;
    for (;;) {
        TokenKind kind = token_at(p, index)->kind;
        if (kind == TOK_EOF) return false;
        if (kind == TOK_LPAREN) depth++;
        else if (kind == TOK_RPAREN && --depth == 0) break;
        index++;
    }
    return token_at(p, index + 1)->kind == TOK_FAT_ARROW;
}

static Expr *parse_lambda(Parser *p) {
    size_t start = cur(p)->offset;
    Diagnostic *d = error_at(p, "E0208", start, 1, "lambdas are not available in v0");
    diag_help(d, "lambdas are planned for v1; declare a named function instead");
    size_t depth = 0;
    do {
        TokenKind kind = advance(p)->kind;
        if (kind == TOK_LPAREN) depth++;
        else if (kind == TOK_RPAREN) depth--;
    } while (depth > 0);
    advance(p); /* => */
    if (check(p, TOK_LBRACE)) skip_balanced_braces(p);
    else parse_expr(p);
    return close_expr(p, new_expr(p, EXPR_ERROR, start));
}

static Expr *parse_primary(Parser *p) {
    const Token *token = cur(p);
    Expr *expr;
    switch (token->kind) {
    case TOK_INT:
        expr = new_expr(p, EXPR_INT, token->offset);
        expr->as.int_value = token->int_value;
        break;
    case TOK_FLOAT:
        expr = new_expr(p, EXPR_FLOAT, token->offset);
        break;
    case TOK_STRING:
        expr = new_expr(p, EXPR_STRING, token->offset);
        break;
    case TOK_CHAR:
        expr = new_expr(p, EXPR_CHAR, token->offset);
        expr->as.char_value = token->char_value;
        break;
    case TOK_KW_TRUE:
    case TOK_KW_FALSE:
        expr = new_expr(p, EXPR_BOOL, token->offset);
        expr->as.bool_value = token->kind == TOK_KW_TRUE;
        break;
    case TOK_KW_SELF: expr = new_expr(p, EXPR_SELF, token->offset); break;
    case TOK_KW_BASE: expr = new_expr(p, EXPR_BASE, token->offset); break;
    case TOK_IDENT:
        expr = new_expr(p, EXPR_NAME, token->offset);
        expr->as.name = make_name(p, token);
        break;
    case TOK_ERROR:
        expr = new_expr(p, EXPR_ERROR, token->offset);
        break;
    case TOK_LPAREN: {
        if (at_lambda(p)) return parse_lambda(p);
        advance(p);
        bool saved = p->no_struct_literal;
        p->no_struct_literal = false;
        expr = parse_expr(p);
        expect_closing(p, TOK_RPAREN);
        p->no_struct_literal = saved;
        return expr;
    }
    case TOK_KW_MATCH:
        return parse_match(p);
    case TOK_LBRACKET:
        return parse_array(p);
    case TOK_KW_IF: {
        Diagnostic *d = error_at(p, "E0201", token->offset, token->length, "`if` is a statement, not an expression");
        diag_help(d, "use the conditional operator: `cond ? a : b`");
        parse_if(p); /* skipped silently, so that it is not parsed again as a statement */
        return close_expr(p, new_expr(p, EXPR_ERROR, token->offset));
    }
    default:
        report_expected(p, "E0201", "an expression", false);
        return new_expr(p, EXPR_ERROR, token->offset);
    }
    advance(p);
    return close_expr(p, expr);
}

static Expr *parse_call(Parser *p, Expr *callee) {
    Expr *expr = new_expr(p, EXPR_CALL, callee->offset);
    expr->as.call.callee = callee;
    advance(p); /* ( */
    bool saved = p->no_struct_literal;
    p->no_struct_literal = false;
    VEC(Expr *) args = {0};
    while (!check(p, TOK_RPAREN) && !check(p, TOK_EOF)) {
        Expr *arg = parse_expr(p);
        SOSU_VEC_PUSH(&args, arg);
        if (!accept(p, TOK_COMMA)) break;
    }
    expect_closing(p, TOK_RPAREN);
    p->no_struct_literal = saved;
    expr->as.call.arg_count = args.count;
    expr->as.call.args = FINISH(p, args);
    return close_expr(p, expr);
}

static Expr *parse_postfix(Parser *p) {
    Expr *expr = parse_primary(p);
    for (;;) {
        if (check(p, TOK_LPAREN)) {
            expr = parse_call(p, expr);
        } else if (check(p, TOK_LBRACKET)) {
            advance(p);
            Expr *index = new_expr(p, EXPR_INDEX, expr->offset);
            bool saved = p->no_struct_literal;
            p->no_struct_literal = false;
            index->as.index.object = expr;
            index->as.index.index = parse_expr(p);
            expect_closing(p, TOK_RBRACKET);
            p->no_struct_literal = saved;
            expr = close_expr(p, index);
        } else if (check(p, TOK_DOT) || (check(p, TOK_ARROW) && kind_at(p, 1) == TOK_IDENT)) {
            const Token *op = advance(p);
            if (op->kind == TOK_ARROW) {
                Diagnostic *d = error_at(p, "E0210", op->offset, op->length, "`->` is not used to access members");
                diag_help(d, "use `.`: pointers are dereferenced automatically");
            }
            Expr *field = new_expr(p, EXPR_FIELD, expr->offset);
            field->as.field.object = expr;
            expect_name(p, &field->as.field.name, "a member name");
            expr = close_expr(p, field);
        } else if (check(p, TOK_LBRACE) && !p->no_struct_literal && is_type_path(expr)) {
            expr = parse_struct_literal(p, expr);
        } else {
            return expr;
        }
    }
}

static Expr *parse_prefix(Parser *p) {
    const Token *token = cur(p);
    switch (token->kind) {
    case TOK_MINUS: case TOK_BANG: case TOK_TILDE: case TOK_AMP: case TOK_STAR: {
        advance(p);
        Expr *expr = new_expr(p, EXPR_UNARY, token->offset);
        expr->as.unary.op = token->kind;
        expr->as.unary.operand = parse_prefix(p);
        return close_expr(p, expr);
    }
    case TOK_KW_TRY: {
        advance(p);
        Expr *expr = new_expr(p, EXPR_TRY, token->offset);
        expr->as.inner = parse_prefix(p);
        return close_expr(p, expr);
    }
    default:
        return parse_postfix(p);
    }
}

/* `as` has the precedence of the unary operators and applies to their result:
   `-x as f32` is `(-x) as f32`. */
static Expr *parse_cast(Parser *p) {
    Expr *expr = parse_prefix(p);
    while (accept(p, TOK_KW_AS)) {
        Expr *cast = new_expr(p, accept(p, TOK_QUESTION) ? EXPR_TRY_CAST : EXPR_CAST, expr->offset);
        cast->as.cast.value = expr;
        cast->as.cast.type = parse_type(p, true);
        expr = close_expr(p, cast);
    }
    return expr;
}

static Expr *parse_binary(Parser *p, int min_precedence) {
    Expr *left = parse_cast(p);
    bool left_is_comparison = false;
    for (;;) {
        TokenKind op = cur(p)->kind;
        int precedence = binary_precedence(op);
        if (precedence == PREC_NONE || precedence < min_precedence) return left;
        const Token *op_token = advance(p);
        if (precedence == PREC_COMPARE && left_is_comparison) {
            Diagnostic *d = error_at(p, "E0204", op_token->offset, op_token->length,
                                     "comparison operators cannot be chained");
            diag_help(d, "combine the comparisons with `&&`, e.g. `a < b && b < c`");
        }
        Expr *expr = new_expr(p, EXPR_BINARY, left->offset);
        expr->as.binary.op = op;
        expr->as.binary.left = left;
        expr->as.binary.right = parse_binary(p, precedence + 1);
        left = close_expr(p, expr);
        left_is_comparison = precedence == PREC_COMPARE;
    }
}

static Expr *parse_range(Parser *p) {
    Expr *left = parse_binary(p, PREC_OR);
    if (!check(p, TOK_DOT_DOT) && !check(p, TOK_DOT_DOT_EQ)) return left;
    Expr *expr = new_expr(p, EXPR_RANGE, left->offset);
    expr->as.range.inclusive = advance(p)->kind == TOK_DOT_DOT_EQ;
    expr->as.range.start = left;
    expr->as.range.end = parse_binary(p, PREC_OR);
    return close_expr(p, expr);
}

static Expr *parse_expr(Parser *p) {
    Expr *cond = parse_range(p);
    if (!accept(p, TOK_QUESTION)) return cond;
    Expr *expr = new_expr(p, EXPR_TERNARY, cond->offset);
    expr->as.ternary.cond = cond;
    expr->as.ternary.then_expr = parse_expr(p);
    expect(p, TOK_COLON);
    expr->as.ternary.else_expr = parse_expr(p);
    return close_expr(p, expr);
}

/* ---- Statements ---- */

static Stmt *new_stmt(Parser *p, StmtKind kind, size_t start) {
    Stmt *stmt = arena_alloc(p->arena, sizeof *stmt);
    stmt->kind = kind;
    stmt->offset = start;
    return stmt;
}

static Stmt *close_stmt(Parser *p, Stmt *stmt) {
    stmt->length = span_from(p, stmt->offset);
    return stmt;
}

static Stmt *parse_block(Parser *p) {
    Stmt *block = new_stmt(p, STMT_BLOCK, cur(p)->offset);
    if (!expect(p, TOK_LBRACE)) return close_stmt(p, block);
    bool saved = p->no_struct_literal;
    p->no_struct_literal = false;

    VEC(Stmt *) items = {0};
    while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
        size_t before = p->pos;
        Stmt *stmt = parse_statement(p);
        SOSU_VEC_PUSH(&items, stmt);
        if (p->panic) sync_statement(p);
        if (p->pos == before) advance(p);
    }
    expect(p, TOK_RBRACE);
    p->no_struct_literal = saved;
    block->as.block.count = items.count;
    block->as.block.items = FINISH(p, items);
    return close_stmt(p, block);
}

/* Spec §5.1: braces are mandatory after `if`, `else`, `while` and `for`. */
static Stmt *parse_body(Parser *p, const char *keyword) {
    if (check(p, TOK_LBRACE)) return parse_block(p);
    Diagnostic *d = error_at(p, "E0206", cur(p)->offset, cur(p)->length, "braces are required after `%s`", keyword);
    diag_help(d, "wrap the body in `{ }`");
    Stmt *block = new_stmt(p, STMT_BLOCK, cur(p)->offset);
    Stmt *inner = parse_statement(p);
    block->as.block.count = 1;
    block->as.block.items = arena_copy(p->arena, &inner, sizeof inner);
    return close_stmt(p, block);
}

static Expr *parse_condition(Parser *p) {
    expect(p, TOK_LPAREN);
    bool saved = p->no_struct_literal;
    p->no_struct_literal = false;
    Expr *cond = parse_expr(p);
    expect_closing(p, TOK_RPAREN);
    p->no_struct_literal = saved;
    return cond;
}

/* An expression or assignment, without the final `;`. */
static Stmt *parse_simple_statement(Parser *p) {
    size_t start = cur(p)->offset;
    Expr *target = parse_expr(p);
    if (is_assign_op(cur(p)->kind)) {
        Stmt *stmt = new_stmt(p, STMT_ASSIGN, start);
        stmt->as.assign.op = advance(p)->kind;
        stmt->as.assign.target = target;
        stmt->as.assign.value = parse_expr(p);
        return close_stmt(p, stmt);
    }
    Stmt *stmt = new_stmt(p, STMT_EXPR, start);
    stmt->as.expr = target;
    return close_stmt(p, stmt);
}

/* `[mut] (Type | auto) name` */
static void parse_binding(Parser *p, bool *is_mut, TypeExpr **type, Name *name) {
    *is_mut = accept(p, TOK_KW_MUT);
    *type = accept(p, TOK_KW_AUTO) ? NULL : parse_type(p, false);
    expect_name(p, name, "a variable name");
}

/* `[mut] Type name = value`, without the final `;`. */
static Stmt *parse_variable(Parser *p) {
    Stmt *stmt = new_stmt(p, STMT_VAR, cur(p)->offset);
    parse_binding(p, &stmt->as.var.is_mut, &stmt->as.var.type, &stmt->as.var.name);
    const Token *token = cur(p);
    if (token->kind == TOK_LBRACKET) {
        Diagnostic *d = error_at(p, "E0211", token->offset, token->length, "the array size belongs to the type");
        diag_help(d, "write the type first, e.g. `i32[4] values`");
    } else if (token->kind == TOK_COMMA) {
        Diagnostic *d = error_at(p, "E0209", token->offset, token->length, "only one variable can be declared at a time");
        diag_help(d, "declare each variable in its own statement");
    } else if (token->kind != TOK_EQ && !p->panic) {
        Name name = stmt->as.var.name;
        Diagnostic *d = error_at(p, "E0207", name.offset, name.length, "variable `%.*s` has no initial value",
                                 (int)name.length, name.text);
        diag_help(d, "variables are initialized where they are declared: add `= value`");
    }
    if (accept(p, TOK_EQ)) stmt->as.var.init = parse_expr(p);
    return close_stmt(p, stmt);
}

static Stmt *parse_if(Parser *p) {
    Stmt *stmt = new_stmt(p, STMT_IF, advance(p)->offset);
    stmt->as.if_.cond = parse_condition(p);
    stmt->as.if_.then_block = parse_body(p, "if");
    if (accept(p, TOK_KW_ELSE)) {
        stmt->as.if_.else_stmt = check(p, TOK_KW_IF) ? parse_if(p) : parse_body(p, "else");
    }
    return close_stmt(p, stmt);
}

static Stmt *parse_for(Parser *p) {
    size_t start = advance(p)->offset;
    expect(p, TOK_LPAREN);
    bool saved = p->no_struct_literal;
    p->no_struct_literal = false;

    Stmt *init = NULL;
    if (at_variable_declaration(p)) {
        size_t decl_start = cur(p)->offset;
        bool is_mut;
        TypeExpr *type;
        Name name;
        parse_binding(p, &is_mut, &type, &name);
        if (accept(p, TOK_KW_IN)) {
            Stmt *stmt = new_stmt(p, STMT_FOR_IN, start);
            stmt->as.for_in.is_mut = is_mut;
            stmt->as.for_in.type = type;
            stmt->as.for_in.name = name;
            stmt->as.for_in.iterable = parse_expr(p);
            expect_closing(p, TOK_RPAREN);
            p->no_struct_literal = saved;
            stmt->as.for_in.body = parse_body(p, "for");
            return close_stmt(p, stmt);
        }
        init = new_stmt(p, STMT_VAR, decl_start);
        init->as.var.is_mut = is_mut;
        init->as.var.type = type;
        init->as.var.name = name;
        expect(p, TOK_EQ);
        init->as.var.init = parse_expr(p);
        close_stmt(p, init);
    } else if (!check(p, TOK_SEMICOLON)) {
        init = parse_simple_statement(p);
    }

    Stmt *stmt = new_stmt(p, STMT_FOR, start);
    stmt->as.for_.init = init;
    expect(p, TOK_SEMICOLON);
    if (!check(p, TOK_SEMICOLON)) stmt->as.for_.cond = parse_expr(p);
    expect(p, TOK_SEMICOLON);
    if (!check(p, TOK_RPAREN)) stmt->as.for_.step = parse_simple_statement(p);
    expect_closing(p, TOK_RPAREN);
    p->no_struct_literal = saved;
    stmt->as.for_.body = parse_body(p, "for");
    return close_stmt(p, stmt);
}

static Stmt *parse_statement(Parser *p) {
    skip_error_tokens(p);
    size_t start = cur(p)->offset;
    Stmt *stmt;
    switch (cur(p)->kind) {
    case TOK_LBRACE:
        return parse_block(p);
    case TOK_KW_IF:
        return parse_if(p);
    case TOK_KW_WHILE:
        stmt = new_stmt(p, STMT_WHILE, advance(p)->offset);
        stmt->as.while_.cond = parse_condition(p);
        stmt->as.while_.body = parse_body(p, "while");
        return close_stmt(p, stmt);
    case TOK_KW_FOR:
        return parse_for(p);
    case TOK_KW_RETURN:
        stmt = new_stmt(p, STMT_RETURN, advance(p)->offset);
        if (!check(p, TOK_SEMICOLON)) stmt->as.expr = parse_expr(p);
        break;
    case TOK_KW_BREAK:
    case TOK_KW_CONTINUE:
        stmt = new_stmt(p, advance(p)->kind == TOK_KW_BREAK ? STMT_BREAK : STMT_CONTINUE, start);
        break;
    case TOK_KW_DEFER:
        advance(p);
        stmt = new_stmt(p, STMT_DEFER, start);
        if (check(p, TOK_LBRACE)) {
            stmt->as.inner = parse_block(p);
            return close_stmt(p, stmt);
        }
        stmt->as.inner = parse_simple_statement(p);
        break;
    case TOK_KW_UNSAFE:
        advance(p);
        stmt = new_stmt(p, STMT_UNSAFE, start);
        stmt->as.inner = parse_block(p);
        return close_stmt(p, stmt);
    case TOK_KW_MATCH:
        /* A `match` statement ends with its `}`. */
        stmt = new_stmt(p, STMT_EXPR, start);
        stmt->as.expr = parse_expr(p);
        accept(p, TOK_SEMICOLON);
        return close_stmt(p, stmt);
    default:
        stmt = at_variable_declaration(p) ? parse_variable(p) : parse_simple_statement(p);
        break;
    }
    expect(p, TOK_SEMICOLON);
    return close_stmt(p, stmt);
}

/* ---- Declarations ---- */

static void parse_params(Parser *p, Param **out, size_t *count, bool allow_mut) {
    expect(p, TOK_LPAREN);
    VEC(Param) params = {0};
    if (!check(p, TOK_RPAREN)) {
        do {
            Param param = {0};
            const Token *mut = cur(p);
            if (accept(p, TOK_KW_MUT) && !allow_mut) {
                error_at(p, "E0212", mut->offset, mut->length, "`mut` is not allowed here");
            }
            param.is_mut = mut->kind == TOK_KW_MUT;
            param.type = parse_type(p, false);
            expect_name(p, &param.name, "a parameter name");
            SOSU_VEC_PUSH(&params, param);
        } while (accept(p, TOK_COMMA));
    }
    expect_closing(p, TOK_RPAREN);
    *count = params.count;
    *out = FINISH(p, params);
}

/* `[unsafe] Type name(params) [uses ...] { contract } body`, or a signature ending with `;`. */
static void parse_function(Parser *p, FnDecl *fn, bool signature) {
    fn->offset = cur(p)->offset;
    fn->is_unsafe = accept(p, TOK_KW_UNSAFE);
    fn->ret = parse_type(p, false);
    expect_name(p, &fn->name, "a function name");
    parse_params(p, &fn->params, &fn->param_count, true);
    if (accept(p, TOK_KW_USES)) fn->effects = parse_effects(p, false);

    if (signature) {
        expect(p, TOK_SEMICOLON);
        fn->length = span_from(p, fn->offset);
        return;
    }
    VEC(Contract) contracts = {0};
    while (check(p, TOK_KW_REQUIRES) || check(p, TOK_KW_ENSURES)) {
        Contract contract;
        contract.kind = advance(p)->kind == TOK_KW_REQUIRES ? CONTRACT_REQUIRES : CONTRACT_ENSURES;
        bool saved = p->no_struct_literal;
        p->no_struct_literal = true; /* `ensures x > lo {`: the brace opens the body */
        contract.expr = parse_expr(p);
        p->no_struct_literal = saved;
        SOSU_VEC_PUSH(&contracts, contract);
    }
    fn->contract_count = contracts.count;
    fn->contracts = FINISH(p, contracts);
    fn->body = parse_block(p);
    fn->length = span_from(p, fn->offset);
}

static Path *parse_bases(Parser *p, size_t *count) {
    VEC(Path) bases = {0};
    do {
        Path base = parse_path(p);
        SOSU_VEC_PUSH(&bases, base);
    } while (accept(p, TOK_COMMA));
    *count = bases.count;
    return FINISH(p, bases);
}

/* Members of a struct or class. */
static void parse_members(Parser *p, Decl *decl) {
    bool is_class = decl->kind == DECL_CLASS;
    VEC(Field) fields = {0};
    VEC(FnDecl) methods = {0};
    if (!expect(p, TOK_LBRACE)) return;
    while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
        size_t before = p->pos;
        skip_error_tokens(p);
        bool is_pub = accept(p, TOK_KW_PUB);
        MethodKind method_kind = METHOD_PLAIN;
        const Token *modifier = cur(p);
        if (accept(p, TOK_KW_VIRTUAL)) method_kind = METHOD_VIRTUAL;
        else if (accept(p, TOK_KW_OVERRIDE)) method_kind = METHOD_OVERRIDE;
        if (method_kind != METHOD_PLAIN && !is_class) {
            Diagnostic *d = error_at(p, "E0212", modifier->offset, modifier->length,
                                     "`%s` is only allowed on class methods", token_kind_spelling(modifier->kind));
            diag_help(d, "structs have no inheritance: use a class");
        }

        if (at_function(p)) {
            FnDecl method = {0};
            method.is_pub = is_pub;
            method.method_kind = method_kind;
            parse_function(p, &method, false);
            SOSU_VEC_PUSH(&methods, method);
        } else {
            if (method_kind != METHOD_PLAIN) {
                error_at(p, "E0212", modifier->offset, modifier->length, "`%s` only applies to methods",
                         token_kind_spelling(modifier->kind));
            }
            Field field = {0};
            field.is_pub = is_pub;
            field.is_mut = accept(p, TOK_KW_MUT);
            field.type = parse_type(p, false);
            expect_name(p, &field.name, "a field name");
            if (accept(p, TOK_EQ)) field.init = parse_expr(p);
            expect(p, TOK_SEMICOLON);
            SOSU_VEC_PUSH(&fields, field);
        }
        if (p->panic) sync_statement(p);
        if (p->pos == before) advance(p);
    }
    expect(p, TOK_RBRACE);
    decl->as.type.field_count = fields.count;
    decl->as.type.fields = FINISH(p, fields);
    decl->as.type.method_count = methods.count;
    decl->as.type.methods = FINISH(p, methods);
}

/* Signatures of an interface or an extern block. */
static FnDecl *parse_signatures(Parser *p, size_t *count) {
    VEC(FnDecl) fns = {0};
    *count = 0;
    if (!expect(p, TOK_LBRACE)) return NULL;
    while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
        size_t before = p->pos;
        FnDecl fn = {0};
        parse_function(p, &fn, true);
        SOSU_VEC_PUSH(&fns, fn);
        if (p->panic) sync_statement(p);
        if (p->pos == before) advance(p);
    }
    expect(p, TOK_RBRACE);
    *count = fns.count;
    return FINISH(p, fns);
}

static void parse_enum_body(Parser *p, Decl *decl) {
    VEC(Variant) variants = {0};
    if (!expect(p, TOK_LBRACE)) return;
    while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
        Variant variant = {0};
        expect_name(p, &variant.name, "a variant name");
        if (check(p, TOK_LPAREN)) {
            variant.has_parens = true;
            parse_params(p, &variant.fields, &variant.field_count, false);
        }
        SOSU_VEC_PUSH(&variants, variant);
        if (!accept(p, TOK_COMMA)) break;
    }
    expect_closing(p, TOK_RBRACE);
    decl->as.enum_.variant_count = variants.count;
    decl->as.enum_.variants = FINISH(p, variants);
}

static Decl *parse_declaration(Parser *p) {
    skip_error_tokens(p);
    if (check(p, TOK_EOF)) return NULL;
    Decl *decl = arena_alloc(p->arena, sizeof *decl);
    decl->offset = cur(p)->offset;
    const Token *pub = cur(p);
    decl->is_pub = accept(p, TOK_KW_PUB);
    const Token *token = cur(p);

    switch (token->kind) {
    case TOK_KW_IMPORT:
        decl->kind = DECL_IMPORT;
        advance(p);
        decl->as.import = parse_path(p);
        expect(p, TOK_SEMICOLON);
        break;
    case TOK_KW_STRUCT:
    case TOK_KW_CLASS:
    case TOK_KW_INTERFACE:
        decl->kind = token->kind == TOK_KW_STRUCT ? DECL_STRUCT : token->kind == TOK_KW_CLASS ? DECL_CLASS : DECL_INTERFACE;
        advance(p);
        expect_name(p, &decl->as.type.name, "a type name");
        if (check(p, TOK_COLON)) {
            const Token *colon = advance(p);
            if (decl->kind != DECL_CLASS) {
                Diagnostic *d = error_at(p, "E0213", colon->offset, colon->length,
                                         "only classes can have a parent class or interfaces");
                diag_help(d, decl->kind == DECL_STRUCT ? "use a class to inherit or implement an interface"
                                                       : "interfaces cannot extend other interfaces in v0");
            }
            decl->as.type.bases = parse_bases(p, &decl->as.type.base_count);
        }
        if (decl->kind == DECL_INTERFACE) {
            decl->as.type.methods = parse_signatures(p, &decl->as.type.method_count);
        } else {
            parse_members(p, decl);
        }
        break;
    case TOK_KW_ENUM:
        decl->kind = DECL_ENUM;
        advance(p);
        expect_name(p, &decl->as.enum_.name, "an enum name");
        parse_enum_body(p, decl);
        break;
    case TOK_KW_EXTERN:
        decl->kind = DECL_EXTERN;
        advance(p);
        if (check(p, TOK_STRING)) decl->as.extern_.abi = make_name(p, advance(p));
        else report_expected(p, "E0200", "an ABI name such as `\"C\"`", false);
        decl->as.extern_.fns = parse_signatures(p, &decl->as.extern_.fn_count);
        break;
    case TOK_KW_TEST:
        decl->kind = DECL_TEST;
        advance(p);
        if (check(p, TOK_STRING)) decl->as.test.name = make_name(p, advance(p));
        else report_expected(p, "E0200", "a test name (a string)", false);
        decl->as.test.body = parse_block(p);
        break;
    default:
        if (at_function(p)) {
            decl->kind = DECL_FN;
            decl->as.fn.is_pub = decl->is_pub;
            parse_function(p, &decl->as.fn, false);
            break;
        }
        if (at_variable_declaration(p)) {
            Diagnostic *d = error_at(p, "E0203", token->offset, token->length, "global variables are not supported");
            diag_help(d, "pass the value as a parameter, or store it in a struct");
        } else {
            char found[64];
            describe_token(p, token, found, sizeof found);
            Diagnostic *d = error_at(p, "E0203", token->offset, token->length, "expected a declaration, found %s", found);
            diag_help(d, "a file contains `import`, `struct`, `class`, `interface`, `enum`, `extern`, `test` and functions");
        }
        return NULL;
    }
    if (decl->is_pub && (decl->kind == DECL_IMPORT || decl->kind == DECL_EXTERN || decl->kind == DECL_TEST)) {
        error_at(p, "E0212", pub->offset, pub->length, "`pub` is not allowed on `%s`", token_kind_spelling(token->kind));
    }
    decl->length = span_from(p, decl->offset);
    return decl;
}

static void init_parser(Parser *p, const SourceFile *file, const TokenList *tokens, Arena *arena, DiagList *diags) {
    memset(p, 0, sizeof *p);
    p->file = file;
    p->tokens = tokens->items;
    p->count = tokens->count;
    p->arena = arena;
    p->diags = diags;
}

Module *parse_module(const SourceFile *file, const TokenList *tokens, Arena *arena, DiagList *diags) {
    Parser p;
    init_parser(&p, file, tokens, arena, diags);
    VEC(Decl *) decls = {0};
    while (!check(&p, TOK_EOF)) {
        size_t before = p.pos;
        Decl *decl = parse_declaration(&p);
        if (decl) SOSU_VEC_PUSH(&decls, decl);
        if (p.panic) sync_declaration(&p);
        if (p.pos == before) advance(&p);
    }
    Module *module = arena_alloc(arena, sizeof *module);
    module->count = decls.count;
    module->decls = finish_vec(&p, decls.items, decls.count, sizeof *decls.items);
    return module;
}

Expr *parse_expression(const SourceFile *file, const TokenList *tokens, Arena *arena, DiagList *diags) {
    Parser p;
    init_parser(&p, file, tokens, arena, diags);
    Expr *expr = parse_expr(&p);
    if (!check(&p, TOK_EOF)) report_expected(&p, "E0200", "end of expression", true);
    return expr;
}
