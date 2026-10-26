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
    bool half_gt;           /* the current token is `>>`, and its first `>` closed a list of type arguments */
    /* .ui.sosu files: the `bind:` and `on_change:` of the `Dropdown` whose options are being
       read (`ui_dropdown` NULL: not in one); whether the next item is the first node of a
       window, the place of a `TitleBar`, and whether the window has one. */
    Expr *ui_dropdown;
    Expr *ui_dropdown_change;
    bool ui_window_top;
    bool ui_title_bar;
    /* in the items of a `Menu` or a `ContextMenu`; in a `MenuBar` */
    bool ui_in_menu;
    bool ui_in_menu_bar;
    /* the items of a `Grid`, and the cells its nodes gave as literals, `row * 64 + col` (spec §10.8) */
    bool ui_in_grid;
    bool ui_in_tabs; /* the items of a `Tabs`: its `Tab` nodes only */
    bool ui_in_tree; /* the items of a `Tree` or a `Branch`: `Branch` and `Leaf` nodes only */
    bool ui_in_component; /* the items at the top of a component: a component can hold the items of a tree */
    /* the next node is at the top of a window or a component (a `Dialog` must be), and the dialogs of
       the view being read, emitted at its end so that they are drawn above the rest */
    bool ui_at_top;
    bool ui_node_at_top;
    void *ui_dialogs;
    struct { uint64_t *items; size_t count; size_t cap; } ui_cells;
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

/* Skips a list of type arguments `<...>` starting at `index` (on `<`), without reporting
   anything (spec §5.4). Returns the index just after it, or 0 if the tokens cannot be one:
   then `<` is a comparison. */
static size_t skip_type_args(const Parser *p, size_t index) {
    int angles = 0;
    int parens = 0;
    for (;;) {
        switch (token_at(p, index)->kind) {
        case TOK_LT: angles++; break;
        case TOK_GT: angles--; break;
        case TOK_SHR: angles -= 2; break;
        case TOK_LPAREN: parens++; break;
        case TOK_RPAREN:
            if (--parens < 0) return 0;
            break;
        case TOK_IDENT: case TOK_DOT: case TOK_COMMA: case TOK_STAR: case TOK_QUESTION: case TOK_LBRACKET:
        case TOK_RBRACKET: case TOK_INT: case TOK_BANG: case TOK_ARROW: case TOK_KW_USES:
            break;
        default:
            return 0;
        }
        index++;
        if (angles == 0) return parens == 0 ? index : 0;
        if (angles == -1 && parens == 0) return index - 1; /* `>>` closes this list and the outer one */
        if (angles < 0) return 0;
    }
}

/* Skips a list of type parameters `<T: Ordered, uses E>` (on `<`); 0 if there is none. */
static size_t skip_type_params(const Parser *p, size_t index) {
    int angles = 0;
    for (;;) {
        TokenKind kind = token_at(p, index)->kind;
        if (kind == TOK_LT) angles++;
        else if (kind == TOK_GT) angles--;
        else if (kind == TOK_SHR) angles -= 2;
        else if (kind == TOK_EOF || kind == TOK_LBRACE || kind == TOK_SEMICOLON) return 0;
        index++;
        if (angles <= 0) return angles == 0 ? index : 0;
    }
}

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
        if (token_at(p, index)->kind == TOK_LT) {
            size_t after = skip_type_args(p, index);
            if (after) index = after;
        }
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
    if (after == 0 || token_at(p, after)->kind != TOK_IDENT) return false;
    after++;
    if (token_at(p, after)->kind == TOK_LT) after = skip_type_params(p, after);
    return after != 0 && token_at(p, after)->kind == TOK_LPAREN;
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

/* `>` closing a list of type arguments; `>>` closes two, one at a time. */
static void close_angle(Parser *p) {
    if (p->half_gt) {
        p->half_gt = false;
        advance(p);
    } else if (check(p, TOK_SHR)) {
        p->half_gt = true;
    } else {
        expect(p, TOK_GT);
    }
}

/* `<A, B>`, after a generic name (the caller checked it with skip_type_args). */
static TypeExpr **parse_type_args(Parser *p, size_t *count) {
    advance(p); /* < */
    VEC(TypeExpr *) args = {0};
    do {
        TypeExpr *arg = parse_type(p, false);
        SOSU_VEC_PUSH(&args, arg);
    } while (!p->half_gt && accept(p, TOK_COMMA));
    close_angle(p);
    *count = args.count;
    return FINISH(p, args);
}

/* `<T, K: Hash + Equal, uses E>` after the name of a declaration (spec §5.4). */
static void parse_type_params(Parser *p, TypeParams *out) {
    if (!check(p, TOK_LT)) return;
    advance(p);
    VEC(TypeParam) params = {0};
    do {
        TypeParam param = {0};
        param.is_effect = accept(p, TOK_KW_USES);
        expect_name(p, &param.name, param.is_effect ? "the name of an effect parameter" : "the name of a type parameter");
        if (!param.is_effect && accept(p, TOK_COLON)) {
            VEC(Path) constraints = {0};
            do {
                Path constraint = parse_path(p);
                SOSU_VEC_PUSH(&constraints, constraint);
            } while (accept(p, TOK_PLUS));
            param.constraint_count = constraints.count;
            param.constraints = FINISH(p, constraints);
        }
        SOSU_VEC_PUSH(&params, param);
    } while (accept(p, TOK_COMMA));
    close_angle(p);
    out->count = params.count;
    out->items = FINISH(p, params);
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
        if (check(p, TOK_LT) && skip_type_args(p, p->pos)) type->args = parse_type_args(p, &type->arg_count);
    } else {
        report_expected(p, "E0202", "a type", false);
    }
    type->length = span_from(p, start);
    return type;
}

static bool starts_operand(TokenKind kind) {
    switch (kind) {
    case TOK_IDENT: case TOK_INT: case TOK_FLOAT: case TOK_STRING: case TOK_CHAR: case TOK_COLOR: case TOK_LPAREN:
    case TOK_KW_TRUE: case TOK_KW_FALSE: case TOK_KW_SELF: case TOK_KW_BASE:
    case TOK_KW_TRY: case TOK_KW_MATCH:
    case TOK_MINUS: case TOK_BANG: case TOK_TILDE: case TOK_AMP: case TOK_STAR:
        return true;
    default:
        return false;
    }
}

/* After `as`, `*` and `?` are type suffixes unless an operand follows:
   `x as i32 * 2` multiplies, `p as ui8*;` converts to a pointer. */
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
    type->args = type_expr->type_args;
    type->arg_count = type_expr->type_arg_count;
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
    case TOK_COLOR: {
        /* `#89B4FA` in a .ui.sosu file: `widgets.rgb(0x89, 0xB4, 0xFA, 0xFF)` (spec §10.7) */
        static const char *const parts[2] = { "widgets", "rgb" };
        Expr *module = new_expr(p, EXPR_NAME, token->offset);
        module->length = token->length;
        module->as.name.text = parts[0];
        module->as.name.length = 7;
        module->as.name.offset = token->offset;
        Expr *callee = new_expr(p, EXPR_FIELD, token->offset);
        callee->length = token->length;
        callee->as.field.object = module;
        callee->as.field.name.text = parts[1];
        callee->as.field.name.length = 3;
        callee->as.field.name.offset = token->offset;
        expr = new_expr(p, EXPR_CALL, token->offset);
        expr->as.call.callee = callee;
        expr->as.call.arg_count = 4;
        expr->as.call.args = arena_alloc(p->arena, 4 * sizeof(Expr *));
        for (int i = 0; i < 4; i++) {
            Expr *byte = new_expr(p, EXPR_INT, token->offset); /* length 0: a number not in the source */
            byte->as.int_value = token->int_value >> (24 - 8 * i) & 0xFF;
            expr->as.call.args[i] = byte;
        }
        break;
    }
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

/* After a name in an expression, `<` starts type arguments when they are followed by `(`, `{`,
   `.`, or end an argument (`arena.new(Vec<i32>, a)`): the other reading, two comparisons in a
   row, is an error anyway (spec §5.4). */
static bool type_args_follow(const Parser *p) {
    size_t after = skip_type_args(p, p->pos);
    if (!after) return false;
    TokenKind next = token_at(p, after)->kind;
    return next == TOK_LPAREN || next == TOK_DOT || next == TOK_COMMA || next == TOK_RPAREN ||
           (next == TOK_LBRACE && !p->no_struct_literal);
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
        } else if (check(p, TOK_LT) && is_type_path(expr) && expr->type_arg_count == 0 && type_args_follow(p)) {
            expr->type_args = parse_type_args(p, &expr->type_arg_count);
            expr->length = span_from(p, expr->offset);
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
    parse_type_params(p, &fn->type_params);
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
        parse_type_params(p, &decl->type_params);
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
        parse_type_params(p, &decl->type_params);
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

/* ---- .ui.sosu files (spec §10.2) ----
   A `window` or a `component` becomes a function that describes it with the widgets of
   `ui.widgets`, run at each frame (immediate mode):

       window Hello(State* s) {               pub void Hello(widgets.Frame* ui_frame, State* s) {
           Column(gap: 8) {                       ui_frame.column(8 as f32, 0 as f32);
               Text("Hi")                         ui_frame.label("Hi");
               Button("OK", on_click: s.ok)       if (ui_frame.button("OK")) { s.ok(); }
           }                                      ui_frame.end();
       }                                      }

   A window also gets `pub widgets.Window HelloConfig()`, from its properties (`title`,
   `width`, `height`). The nodes keep the positions of the .ui.sosu file: errors found
   later (types, effects) point to it. */

typedef VEC(Stmt *) StmtVec;
typedef VEC(Expr *) ExprVec;

/* `name: value` in the arguments of a node */
typedef struct {
    Name name;
    Expr *value;
} UiProp;

typedef struct {
    ExprVec values; /* the values given in order */
    VEC(UiProp) props;
} UiArgs;

/* The widgets written in a .ui.sosu file, and what they accept. */
typedef struct {
    const char *name;
    size_t values;        /* values in order, before the properties */
    bool optional;        /* the value can be left out (a checkbox without a label) */
    const char *props[16]; /* NULL-terminated */
    bool children;        /* `{ ... }` after the arguments */
} UiNodeKind;

static const UiNodeKind ui_nodes[] = {
    { "Grid", 0, false, { "columns", "rows", "gap", "padding", "style", NULL }, true },
    { "Column", 0, false, { "gap", "padding", "style", NULL }, true },
    { "Row", 0, false, { "gap", "padding", "style", NULL }, true },
    { "List", 0, false, { "width", "height", "selected", "selection", "count", "item", "on_drop", "focus", "style", NULL }, true },
    { "Table", 0, false, { "count", "item", "width", "height", "selected", "selection", "sort", "on_activate", "focus", "style", NULL }, true },
    { "Scroll", 0, false, { "width", "height", "style", NULL }, true },
    { "Text", 1, false, { "plural", "count", "context", "style", NULL }, false },
    { "Title", 1, false, { "plural", "count", "context", "style", NULL }, false },
    { "Space", 1, false, { NULL }, false },
    { "Button", 1, false, { "on_click", "enabled", "focus", "plural", "count", "context", "style", NULL }, false },
    { "TextField", 0, false, { "bind", "width", "on_enter", "accept", "max", "mask", "placeholder", "focus", "style", NULL }, false },
    { "TextArea", 0, false, { "bind", "width", "rows", "wrap", "tabs", "placeholder", "on_enter", "focus", "style", NULL }, false },
    { "Slider", 0, false, { "bind", "min", "max", "step", "width", "on_change", "focus", "style", NULL }, false },
    { "Progress", 0, false, { "value", "max", "width", "style", NULL }, false },
    { "DateField", 0, false, { "bind", "width", "format", "monday_first", "on_change", "focus", "style", NULL }, false },
    { "Dialog", 1, false, { "open", "width", "plural", "count", "context", NULL }, true },
    { "NumberField", 0, false, { "bind", "width", "min", "max", "step", "decimals", "decimal", "group", "on_change", "focus", "style", NULL }, false },
    { "MoneyField", 0, false, { "bind", "width", "min", "max", "step", "decimals", "decimal", "group", "symbol", "symbol_first", "on_change", "focus", "style", NULL }, false },
    { "Checkbox", 1, true, { "bind", "on_change", "plural", "count", "context", "style", NULL }, false },
    { "Radio", 1, false, { "bind", "value", "on_change", "plural", "count", "context", "style", NULL }, false },
    { "TitleBar", 0, false, { "height", "gap", "padding", "style", NULL }, true },
    { "Dropdown", 0, false, { "bind", "width", "on_change", "placeholder", "focus", "style", NULL }, true },
    { "Option", 1, false, { "value", "plural", "count", "context", NULL }, false },
    { "Image", 1, false, { "size", "style", NULL }, false },
    { "Group", 1, true, { "gap", "padding", "collapsible", "open", "style", NULL }, true },
    { "Tabs", 0, false, { "bind", "placement", "focus", "style", NULL }, true },
    { "Tab", 1, false, { "closable", "on_close", "enabled", "plural", "count", "context", NULL }, true },
    { "Tree", 0, false, { "selected", "width", "height", "count", "item", "depth", "branch", "open", "on_toggle", "focus", "style", NULL }, true },
    { "Branch", 1, true, { "open", "icon", "template", "plural", "count", "context", NULL }, true },
    { "Leaf", 1, true, { "on_click", "icon", "template", "plural", "count", "context", NULL }, false },
    { "Item", 0, false, { "gap", "padding", "vertical", "enabled", "style", NULL }, true },
    { "MenuBar", 0, false, { "style", NULL }, true },
    { "Menu", 1, false, { "plural", "count", "context", "style", NULL }, true },
    { "MenuItem", 1, false, { "on_click", "shortcut", "enabled", "plural", "count", "context", NULL }, false },
    { "Separator", 0, false, { NULL }, false },
    { "ContextMenu", 0, false, { NULL }, true },
};

#define UI_CONTAINERS                                                                                      \
    "only `Column`, `Row`, `Grid`, `Group`, `List`, `Scroll`, `Tabs`, `Tab`, `Tree`, `Branch`, `TitleBar`, " \
    "`Dropdown`, `MenuBar`, `Menu` and `ContextMenu` contain other nodes"

/* The widgets that have an identity (spec §10.6): `f.at(site)` comes before them. */
static const char *const ui_identified[] = { "Item", "Button", "TextField", "TextArea", "NumberField", "MoneyField", "Slider", "DateField", "Dialog", "Table", "Checkbox", "Radio", "List", "Group", "Tabs", "Tree", "Branch", "Leaf",
                                             "Scroll", "Dropdown", "Menu", "ContextMenu" };

static void ui_parse_children(Parser *p, StmtVec *out);

static bool name_text_is(Name name, const char *text) {
    return name.length == strlen(text) && memcmp(name.text, text, name.length) == 0;
}

static bool names_equal(Name a, Name b) {
    return a.length == b.length && memcmp(a.text, b.text, a.length) == 0;
}

/* A name that is not in the source (`ui_frame`, `label`...), placed at `offset`. */
static Name ui_name(const char *text, size_t offset) {
    Name name;
    name.text = text;
    name.length = strlen(text);
    name.offset = offset;
    return name;
}

static Expr *ui_expr(Parser *p, ExprKind kind, size_t offset, size_t length) {
    Expr *expr = new_expr(p, kind, offset);
    expr->length = length;
    return expr;
}

/* `name`, or `module.name` */
static TypeExpr *ui_type(Parser *p, size_t offset, const char *module, const char *name) {
    TypeExpr *type = new_type(p, TYPE_NAME, offset);
    Name names[2] = { ui_name(module ? module : name, offset), ui_name(name, offset) };
    type->as.name.count = module ? 2 : 1;
    type->as.name.items = arena_copy(p->arena, module ? names : names + 1, type->as.name.count * sizeof *names);
    return type;
}

/* `ui_frame.method(args...)`, placed at the node `at_offset`, `at_length` */
static Expr *ui_frame_call(Parser *p, size_t at_offset, size_t at_length, const char *method, Expr **args,
                           size_t count) {
    Expr *frame = ui_expr(p, EXPR_NAME, at_offset, at_length);
    frame->as.name = ui_name("ui_frame", at_offset);
    Expr *callee = ui_expr(p, EXPR_FIELD, at_offset, at_length);
    callee->as.field.object = frame;
    callee->as.field.name = ui_name(method, at_offset);
    Expr *call = ui_expr(p, EXPR_CALL, at_offset, at_length);
    call->as.call.callee = callee;
    call->as.call.arg_count = count;
    call->as.call.args = arena_alloc(p->arena, (count ? count : 1) * sizeof *args);
    if (count) memcpy(call->as.call.args, args, count * sizeof *args);
    return call;
}

/* An integer that is not in the source: its value is in `int_value` (see codegen). */
static Expr *ui_int(Parser *p, size_t offset, uint64_t value) {
    Expr *literal = ui_expr(p, EXPR_INT, offset, 0);
    literal->as.int_value = value;
    return literal;
}

/* A number of pixels: `8` stands for `8.0`; `fallback` when it is absent. An added float
   literal has no text in the source: its value is in `int_value` (see codegen). */
static Expr *ui_float(Parser *p, Expr *value, size_t offset, uint64_t fallback) {
    if (!value) {
        value = ui_expr(p, EXPR_FLOAT, offset, 0);
        value->as.int_value = fallback;
    } else if (value->kind == EXPR_INT) {
        value->kind = EXPR_FLOAT; /* its text, `8`, is a valid C number */
    }
    return value;
}

static Expr *ui_address(Parser *p, Expr *value) {
    Expr *address = ui_expr(p, EXPR_UNARY, value->offset, value->length);
    address->as.unary.op = TOK_AMP;
    address->as.unary.operand = value;
    return address;
}

static Stmt *ui_stmt(Parser *p, Expr *expr) {
    Stmt *stmt = new_stmt(p, STMT_EXPR, expr->offset);
    stmt->length = expr->length;
    stmt->as.expr = expr;
    return stmt;
}

/* `ui_frame.method(value)` as a statement, or `ui_frame.method()` when `value` is NULL. */
static Stmt *ui_frame_stmt(Parser *p, size_t offset, size_t length, const char *method, Expr *value) {
    return ui_stmt(p, ui_frame_call(p, offset, length, method, &value, value ? 1 : 0));
}

static Stmt *ui_block(Parser *p, StmtVec *items, size_t offset, size_t length) {
    Stmt *block = new_stmt(p, STMT_BLOCK, offset);
    block->length = length;
    block->as.block.count = items->count;
    block->as.block.items = FINISH(p, *items);
    return block;
}

/* `if (widget) { handler(); }`: `on_click: s.ok` calls `s.ok()`; `s.ok(1)` is called as is. */
static Stmt *ui_when(Parser *p, Expr *widget, Expr *handler) {
    if (!handler) return ui_stmt(p, widget);
    Expr *call = handler;
    if (handler->kind != EXPR_CALL) {
        call = ui_expr(p, EXPR_CALL, handler->offset, handler->length);
        call->as.call.callee = handler;
        call->as.call.args = arena_alloc(p->arena, sizeof(Expr *));
    }
    StmtVec items = {0};
    SOSU_VEC_PUSH(&items, ui_stmt(p, call));
    Stmt *stmt = new_stmt(p, STMT_IF, widget->offset);
    stmt->length = widget->length;
    stmt->as.if_.cond = widget;
    stmt->as.if_.then_block = ui_block(p, &items, handler->offset, handler->length);
    return stmt;
}

/* `(value, name: value, ...)` */
static void ui_parse_args(Parser *p, UiArgs *args) {
    memset(args, 0, sizeof *args);
    if (!expect(p, TOK_LPAREN)) return;
    if (!check(p, TOK_RPAREN)) {
        do {
            if (check(p, TOK_IDENT) && kind_at(p, 1) == TOK_COLON) {
                UiProp prop;
                prop.name = make_name(p, advance(p));
                advance(p);
                prop.value = parse_expr(p);
                SOSU_VEC_PUSH(&args->props, prop);
            } else {
                Expr *value = parse_expr(p);
                SOSU_VEC_PUSH(&args->values, value);
            }
        } while (accept(p, TOK_COMMA));
    }
    expect_closing(p, TOK_RPAREN);
}

/* The value of the property `name`, or NULL. */
static Expr *ui_prop(const UiArgs *args, const char *name) {
    for (size_t i = 0; i < args->props.count; i++) {
        if (name_text_is(args->props.items[i].name, name)) return args->props.items[i].value;
    }
    return NULL;
}

/* Checks the values and properties given to a widget. */
/* ---- Translations (spec §10.7) ---- */

/* Whether the literal `text` holds `{name}`. */
static bool ui_has_placeholder(const Parser *p, const Expr *text, const char *name, size_t length) {
    if (!text || text->kind != EXPR_STRING) return false;
    const char *s = p->file->text + text->offset;
    for (size_t i = 0; i + length + 1 < text->length; i++) {
        if (s[i] == '{' && s[i + length + 1] == '}' && memcmp(s + i + 1, name, length) == 0) return true;
    }
    return false;
}

/* The value of the property named `length` bytes at `name`, or NULL. */
static Expr *ui_prop_named(const UiArgs *args, const char *name, size_t length) {
    for (size_t i = 0; i < args->props.count; i++) {
        Name prop = args->props.items[i].name;
        if (prop.length == length && memcmp(prop.text, name, length) == 0) return args->props.items[i].value;
    }
    return NULL;
}

static bool is_name_char(char c) {
    return c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

/* `{name}` in a literal: the offset of its `{` in the file, and the length of the name. */
typedef struct {
    size_t offset;
    size_t length;
} UiPlaceholder;
typedef VEC(UiPlaceholder) UiPlaceholders;

/* Adds the placeholders of the literal `text` to `out`, each name once. */
static void ui_placeholders(const Parser *p, const Expr *text, UiPlaceholders *out) {
    if (!text || text->kind != EXPR_STRING) return;
    const char *file = p->file->text;
    for (size_t i = text->offset + 1; i + 1 < text->offset + text->length; i++) {
        if (file[i] != '{') continue;
        size_t end = i + 1;
        while (end < text->offset + text->length && is_name_char(file[end])) end++;
        if (end == i + 1 || file[end] != '}') continue;
        bool seen = false;
        for (size_t k = 0; k < out->count; k++) {
            UiPlaceholder q = out->items[k];
            seen = seen || (q.length == end - i - 1 && memcmp(file + q.offset + 1, file + i + 1, q.length) == 0);
        }
        if (!seen) {
            UiPlaceholder found = { i, end - i - 1 };
            SOSU_VEC_PUSH(out, found);
        }
        i = end;
    }
}

/* A literal text of a node, translated: `ui_frame.tr("...")`, or `tr_in`, `tr_plural`,
   `tr_plural_in` with `context:` and `plural:` (and `count:`); then its values in braces, given
   by the properties of the same names, filled by `with1` to `with4`. Anything else is data, and
   is returned as it is. */
static Expr *ui_translate(Parser *p, const UiArgs *args, Expr *text) {
    if (!text || text->kind != EXPR_STRING) return text;
    size_t at = text->offset;
    size_t length = text->length;
    Expr *context = ui_prop(args, "context");
    Expr *plural = ui_prop(args, "plural");
    Expr *count = ui_prop(args, "count");
    if (context && context->kind != EXPR_STRING) {
        error_at(p, "E0215", context->offset, context->length, "`context:` is a literal text, read by the translators");
        context = NULL;
    }
    if (plural && plural->kind != EXPR_STRING) {
        error_at(p, "E0215", plural->offset, plural->length, "`plural:` is a literal text, read by the translators");
        plural = NULL;
    }
    if (plural && !count) {
        Diagnostic *d = error_at(p, "E0215", plural->offset, plural->length, "`plural:` needs `count:`");
        diag_help(d, "give the number that chooses the form: `count: files.len`");
        plural = NULL;
    }
    Expr *number = NULL;
    if (count) {
        number = ui_expr(p, EXPR_CAST, count->offset, count->length);
        number->as.cast.value = count;
        number->as.cast.type = ui_type(p, count->offset, NULL, "i64");
    }
    Expr *call_args[4];
    size_t n = 0;
    if (context) call_args[n++] = context;
    call_args[n++] = text;
    if (plural) {
        call_args[n++] = plural;
        call_args[n++] = number;
    }
    const char *method = plural ? (context ? "tr_plural_in" : "tr_plural") : (context ? "tr_in" : "tr");
    Expr *result = ui_frame_call(p, at, length, method, call_args, n);

    UiPlaceholders names = {0};
    ui_placeholders(p, text, &names);
    ui_placeholders(p, plural, &names);
    Expr *pairs[8];
    size_t pair_count = 0;
    for (size_t k = 0; k <= names.count; k++) {
        if (k < names.count) {
            UiPlaceholder q = names.items[k];
            const char *name = p->file->text + q.offset + 1;
            Expr *value = ui_prop_named(args, name, q.length);
            if (q.length == 5 && memcmp(name, "count", 5) == 0 && number) {
                value = ui_frame_call(p, count->offset, count->length, "decimal", &number, 1);
            }
            if (!value) {
                Diagnostic *d = error_at(p, "E0215", q.offset, q.length + 2, "`{%.*s}` has no value", (int)q.length, name);
                diag_help(d, "give it after the text: `%.*s: value` (a `str`)", (int)q.length, name);
                continue;
            }
            /* its name, a string whose quotes are the braces of the placeholder */
            Expr *key = ui_expr(p, EXPR_STRING, q.offset, q.length + 2);
            pairs[pair_count++] = key;
            pairs[pair_count++] = value;
        }
        if (pair_count == 8 || (k == names.count && pair_count > 0)) {
            static const char *const fills[] = { "with1", "with2", "with3", "with4" };
            Expr *fill_args[9];
            fill_args[0] = result;
            memcpy(fill_args + 1, pairs, pair_count * sizeof *pairs);
            result = ui_frame_call(p, at, length, fills[pair_count / 2 - 1], fill_args, pair_count + 1);
            pair_count = 0;
        }
    }
    free(names.items);
    return result;
}

static bool ui_check_args(Parser *p, const UiNodeKind *kind, Name node, const UiArgs *args) {
    if (args->values.count != kind->values && !(kind->optional && args->values.count == 0)) {
        Diagnostic *d = error_at(p, "E0215", node.offset, node.length,
                                 "`%s` takes %zu value%s before its properties, not %zu", kind->name, kind->values,
                                 kind->values == 1 ? "" : "s", args->values.count);
        if (kind->values == 1) diag_help(d, "write `%s(value, ...)`", kind->name);
        else diag_help(d, "give it properties only: `%s(name: value)`", kind->name);
        return false;
    }
    for (size_t i = 0; i < args->props.count; i++) {
        Name prop = args->props.items[i].name;
        for (size_t j = 0; j < i; j++) {
            if (names_equal(args->props.items[j].name, prop)) {
                error_at(p, "E0215", prop.offset, prop.length, "`%.*s` is given twice", (int)prop.length, prop.text);
                return false;
            }
        }
        bool known = false;
        for (size_t j = 0; kind->props[j]; j++) known = known || name_text_is(prop, kind->props[j]);
        if (known) continue;
        Expr *first = args->values.count ? args->values.items[0] : NULL;
        if (ui_has_placeholder(p, first, prop.text, prop.length) ||
            ui_has_placeholder(p, ui_prop(args, "plural"), prop.text, prop.length) ||
            ui_has_placeholder(p, ui_prop(args, "placeholder"), prop.text, prop.length)) {
            continue;  /* a value in braces: `Text("Hello, {name}", name: n)` */
        }
        Diagnostic *d = error_at(p, "E0215", prop.offset, prop.length, "`%s` has no property `%.*s`", kind->name,
                                 (int)prop.length, prop.text);
        char list[128] = "";
        for (size_t j = 0; kind->props[j]; j++) {
            if (j) strcat(list, ", ");
            strcat(list, kind->props[j]);
        }
        if (list[0]) diag_help(d, "its properties are: %s", list);
        else diag_help(d, "it has no properties");
        return false;
    }
    bool binds = strcmp(kind->name, "TextField") == 0 || strcmp(kind->name, "TextArea") == 0 ||
                 strcmp(kind->name, "NumberField") == 0 || strcmp(kind->name, "MoneyField") == 0 ||
                 strcmp(kind->name, "Slider") == 0 || strcmp(kind->name, "DateField") == 0 || strcmp(kind->name, "Checkbox") == 0 ||
                 strcmp(kind->name, "Radio") == 0 || strcmp(kind->name, "Dropdown") == 0;
    if (binds && !ui_prop(args, "bind")) {
        Diagnostic *d = error_at(p, "E0215", node.offset, node.length, "`%s` needs `bind:`", kind->name);
        diag_help(d, "bind it to a `mut` field: `%s(bind: state.value)`", kind->name);
        return false;
    }
    if (strcmp(kind->name, "Radio") == 0 && !ui_prop(args, "value")) {
        Diagnostic *d = error_at(p, "E0215", node.offset, node.length, "`Radio` needs `value:`");
        diag_help(d, "give the value it chooses: `Radio(\"Small\", bind: state.size, value: Small)`");
        return false;
    }
    if (strcmp(kind->name, "Dialog") == 0 && !ui_prop(args, "open")) {
        Diagnostic *d = error_at(p, "E0215", node.offset, node.length, "`Dialog` needs `open:`");
        diag_help(d, "bind it to a `mut bool` that says whether it is open: `Dialog(\"Delete?\", open: state.asking)`");
        return false;
    }
    if (strcmp(kind->name, "Grid") == 0 && !ui_prop(args, "columns")) {
        Diagnostic *d = error_at(p, "E0215", node.offset, node.length, "`Grid` needs `columns:`");
        diag_help(d, "give the size of each column: `Grid(columns: [Auto, Fill]) { ... }`");
        return false;
    }
    if (strcmp(kind->name, "Scroll") == 0 && (!ui_prop(args, "width") || !ui_prop(args, "height"))) {
        Diagnostic *d = error_at(p, "E0215", node.offset, node.length, "`Scroll` needs `width:` and `height:`");
        diag_help(d, "give the size of the area: `Scroll(width: 300, height: 200) { ... }`");
        return false;
    }
    if (strcmp(kind->name, "Option") == 0 && !ui_prop(args, "value")) {
        Diagnostic *d = error_at(p, "E0215", node.offset, node.length, "`Option` needs `value:`");
        diag_help(d, "give the value it chooses: `Option(\"Debug\", value: Debug)`");
        return false;
    }
    return true;
}

/* Where a node may go: a `TitleBar` first in a window, an `Option` in a `Dropdown`, and only
   options there. Reports the error otherwise. */
static bool ui_check_place(Parser *p, const UiNodeKind *kind, Name node, bool window_top) {
    bool title_bar = kind && strcmp(kind->name, "TitleBar") == 0;
    bool option = kind && strcmp(kind->name, "Option") == 0;
    if (p->ui_dropdown && !option) {
        Diagnostic *d = error_at(p, "E0215", node.offset, node.length, "a `Dropdown` contains only `Option` nodes");
        diag_help(d, "one option per value: `Option(\"Debug\", value: Debug)`");
        return false;
    }
    if (option && !p->ui_dropdown) {
        Diagnostic *d = error_at(p, "E0215", node.offset, node.length, "an `Option` goes in a `Dropdown`");
        diag_help(d, "`Dropdown(bind: state.mode) { Option(\"Debug\", value: Debug) }`");
        return false;
    }
    bool menu = kind && strcmp(kind->name, "Menu") == 0;
    bool entry = kind && (strcmp(kind->name, "MenuItem") == 0 || strcmp(kind->name, "Separator") == 0);
    if (p->ui_in_menu && !menu && !entry) {
        Diagnostic *d = error_at(p, "E0215", node.offset, node.length,
                                 "a menu contains only `MenuItem`, `Separator` and `Menu` nodes");
        diag_help(d, "`Menu(\"File\") { MenuItem(\"Save\", on_click: state.save, shortcut: \"Ctrl+S\") }`");
        return false;
    }
    if (entry && !p->ui_in_menu) {
        Diagnostic *d = error_at(p, "E0215", node.offset, node.length, "a `%s` goes in a `Menu` or a `ContextMenu`",
                                 kind->name);
        diag_help(d, "`Menu(\"File\") { MenuItem(\"Save\", on_click: state.save) }`");
        return false;
    }
    if (p->ui_in_menu_bar && !menu) {
        Diagnostic *d = error_at(p, "E0215", node.offset, node.length, "a `MenuBar` contains only `Menu` nodes");
        diag_help(d, "`MenuBar { Menu(\"File\") { ... } Menu(\"Edit\") { ... } }`");
        return false;
    }
    bool item = kind && (strcmp(kind->name, "Branch") == 0 || strcmp(kind->name, "Leaf") == 0);
    if (p->ui_in_tree && kind && !item) {  /* a component may describe items */
        Diagnostic *d = error_at(p, "E0215", node.offset, node.length, "a tree contains only `Branch` and `Leaf` nodes");
        diag_help(d, "`Tree(selected: state.item) { Branch(\"src\") { Leaf(\"main.sosu\") } }`");
        return false;
    }
    if (item && !p->ui_in_tree && !p->ui_in_component) {
        Diagnostic *d = error_at(p, "E0215", node.offset, node.length, "a `%s` goes in a `Tree` or a `Branch`", kind->name);
        diag_help(d, "`Tree(selected: state.item) { Branch(\"src\") { Leaf(\"main.sosu\") } }`");
        return false;
    }
    bool tab = kind && strcmp(kind->name, "Tab") == 0;
    if (p->ui_in_tabs && !tab) {
        Diagnostic *d = error_at(p, "E0215", node.offset, node.length, "a `Tabs` contains only `Tab` nodes");
        diag_help(d, "one tab per page: `Tabs(bind: state.page) { Tab(\"General\") { ... } }`");
        return false;
    }
    if (tab && !p->ui_in_tabs) {
        Diagnostic *d = error_at(p, "E0215", node.offset, node.length, "a `Tab` goes in a `Tabs`");
        diag_help(d, "`Tabs(bind: state.page) { Tab(\"General\") { ... } Tab(\"Files\") { ... } }`");
        return false;
    }
    if (title_bar && (!window_top || p->ui_title_bar)) {
        Diagnostic *d = error_at(p, "E0215", node.offset, node.length, "a `TitleBar` is the first node of a window");
        diag_help(d, "a window has one title bar, at its top: move it before the other nodes");
        return false;
    }
    return true;
}

/* `if (f.method(label, x == v) && x != v) { x = v; handler(); }`, for `Radio` and `Option`:
   clicking the value already chosen changes nothing. */
static Stmt *ui_choice(Parser *p, const char *method, Expr *label, Expr *bound, Expr *chosen, Expr *handler,
                       size_t start, size_t length) {
    Expr *same = ui_expr(p, EXPR_BINARY, chosen->offset, chosen->length);
    same->as.binary.op = TOK_EQ_EQ;
    same->as.binary.left = bound;
    same->as.binary.right = chosen;
    Expr *other = ui_expr(p, EXPR_BINARY, chosen->offset, chosen->length);
    other->as.binary.op = TOK_BANG_EQ;
    other->as.binary.left = bound;
    other->as.binary.right = chosen;
    Expr *call_args[2] = { label, same };
    Expr *clicked = ui_frame_call(p, start, length, method, call_args, 2);
    Expr *choice = ui_expr(p, EXPR_BINARY, start, length);
    choice->as.binary.op = TOK_AMP_AMP;
    choice->as.binary.left = clicked;
    choice->as.binary.right = other;
    Stmt *choose = new_stmt(p, STMT_ASSIGN, chosen->offset);
    choose->length = chosen->length;
    choose->as.assign.op = TOK_EQ;
    choose->as.assign.target = bound;
    choose->as.assign.value = chosen;
    Stmt *when = ui_when(p, choice, handler);
    StmtVec items = {0};
    SOSU_VEC_PUSH(&items, choose);
    if (when->kind == STMT_IF) {  /* the handler is called after the choice */
        SOSU_VEC_PUSH(&items, when->as.if_.then_block->as.block.items[0]);
    } else {
        when = new_stmt(p, STMT_IF, choice->offset);
        when->length = choice->length;
        when->as.if_.cond = choice;
    }
    when->as.if_.then_block = ui_block(p, &items, chosen->offset, chosen->length);
    return when;
}

/* `widgets.Format{ accept: widgets.Accept.Digits, max: 10, mask: "..." }` from the properties of a
   TextField, or NULL when it has none of them. `accept: Digits` names a variant of
   `widgets.Accept`: it is qualified here, since the type is known. */
static Expr *ui_format(Parser *p, const UiArgs *args, size_t offset, size_t length) {
    static const char *const names[] = { "accept", "max", "mask", "placeholder" };
    VEC(FieldInit) fields = {0};
    for (size_t i = 0; i < 4; i++) {
        Expr *value = ui_prop(args, names[i]);
        if (!value) continue;
        if (i == 3) value = ui_translate(p, args, value);  /* the placeholder */
        if (i == 0 && value->kind == EXPR_NAME) {
            Expr *module = ui_expr(p, EXPR_NAME, value->offset, value->length);
            module->as.name = ui_name("widgets", value->offset);
            Expr *type = ui_expr(p, EXPR_FIELD, value->offset, value->length);
            type->as.field.object = module;
            type->as.field.name = ui_name("Accept", value->offset);
            Expr *variant = ui_expr(p, EXPR_FIELD, value->offset, value->length);
            variant->as.field.object = type;
            variant->as.field.name = value->as.name;
            value = variant;
        }
        FieldInit field;
        field.name = ui_name(names[i], value->offset);
        field.value = value;
        SOSU_VEC_PUSH(&fields, field);
    }
    if (!fields.count) return NULL;
    Expr *literal = ui_expr(p, EXPR_STRUCT_LIT, offset, length);
    literal->as.struct_lit.type = ui_type(p, offset, "widgets", "Format");
    literal->as.struct_lit.field_count = fields.count;
    literal->as.struct_lit.fields = FINISH(p, fields);
    return literal;
}

/* A component used as a node: `TaskRow(t)` calls `TaskRow(ui_frame, t)`. */
static void ui_component(Parser *p, StmtVec *out, Path path, UiArgs *args, size_t start, size_t length) {
    if (args->props.count) {
        Name prop = args->props.items[0].name;
        Diagnostic *d = error_at(p, "E0215", prop.offset, prop.length, "a component takes its arguments in order");
        diag_help(d, "remove `%.*s:`, and give the values in the order of its parameters", (int)prop.length, prop.text);
    }
    if (check(p, TOK_LBRACE)) {
        error_at(p, "E0215", cur(p)->offset, cur(p)->length, UI_CONTAINERS);
        skip_balanced_braces(p);
    }
    Expr *callee = ui_expr(p, EXPR_NAME, path.items[0].offset, path.items[0].length);
    callee->as.name = path.items[0];
    for (size_t i = 1; i < path.count; i++) {
        Expr *field = ui_expr(p, EXPR_FIELD, start, path.items[i].offset + path.items[i].length - start);
        field->as.field.object = callee;
        field->as.field.name = path.items[i];
        callee = field;
    }
    Expr *frame = ui_expr(p, EXPR_NAME, start, length);
    frame->as.name = ui_name("ui_frame", start);
    ExprVec values = {0};
    SOSU_VEC_PUSH(&values, frame);
    for (size_t i = 0; i < args->values.count; i++) SOSU_VEC_PUSH(&values, args->values.items[i]);
    Expr *call = ui_expr(p, EXPR_CALL, start, length);
    call->as.call.callee = callee;
    call->as.call.arg_count = values.count;
    call->as.call.args = FINISH(p, values);
    /* in a scope of its own, named by its place: two uses of a component are distinct */
    SOSU_VEC_PUSH(out, ui_frame_stmt(p, start, length, "push_id", ui_int(p, start, start)));
    SOSU_VEC_PUSH(out, ui_stmt(p, call));
    SOSU_VEC_PUSH(out, ui_frame_stmt(p, start, length, "pop_id", NULL));
}

static Expr *ui_call(Parser *p, Expr *callee, Expr *arg, size_t offset, size_t length);
static Expr *ui_some(Parser *p, Expr *value);
typedef VEC(FieldInit) UiFields;
static Expr *ui_struct(Parser *p, const char *type, UiFields *fields, size_t offset, size_t length);

/* A style given to a node: `Danger` and `theme.Danger` are called, `Normal` changes nothing
   (`widgets.Style{ }`), a choice `c ? a : b` is a choice between styles; any other value is a
   `widgets.Style` already. */
static Expr *ui_style_of(Parser *p, Expr *value) {
    if (value->kind == EXPR_TERNARY) {
        value->as.ternary.then_expr = ui_style_of(p, value->as.ternary.then_expr);
        value->as.ternary.else_expr = ui_style_of(p, value->as.ternary.else_expr);
        return value;
    }
    if (value->kind == EXPR_ARRAY) {
        Diagnostic *d = error_at(p, "E0215", value->offset, value->length, "several styles are not a choice");
        diag_help(d, "give the styles one by one: `style: [Card, c ? Danger : Normal]`");
        return value;
    }
    if (value->kind == EXPR_CALL) {  /* `Size(22)`: a style with parameters, its numbers of pixels f32 */
        for (size_t i = 0; i < value->as.call.arg_count; i++) {
            value->as.call.args[i] = ui_float(p, value->as.call.args[i], value->offset, 0);
        }
        return value;
    }
    const Expr *path = value;
    while (path->kind == EXPR_FIELD) path = path->as.field.object;
    if (path->kind != EXPR_NAME) return value;
    Name last = value->kind == EXPR_FIELD ? value->as.field.name : value->as.name;
    if (name_text_is(last, "Normal")) return ui_struct(p, "Style", NULL, value->offset, value->length);
    if (last.length && last.text[0] >= 'A' && last.text[0] <= 'Z') {
        return ui_call(p, value, NULL, value->offset, value->length);
    }
    return value;
}

/* `style: s` or `style: [a, b]`: `ui_frame.style(...)` for each, in order, before the node. */
static void ui_node_styles(Parser *p, StmtVec *out, Expr *styles) {
    if (!styles) return;
    if (styles->kind == EXPR_ARRAY && !styles->as.array.repeat) {
        for (size_t i = 0; i < styles->as.array.count; i++) ui_node_styles(p, out, styles->as.array.items[i]);
        return;
    }
    SOSU_VEC_PUSH(out, ui_frame_stmt(p, styles->offset, styles->length, "style", ui_style_of(p, styles)));
}

/* The gap or the padding of a `Column` or a `Row`: `-1.0` when it is not given, that of the
   theme and the styles. */
static Expr *ui_layout_size(Parser *p, Expr *value, size_t offset) {
    if (value) return ui_float(p, value, offset, 0);
    Expr *minus = ui_expr(p, EXPR_UNARY, offset, 0);
    minus->as.unary.op = TOK_MINUS;
    minus->as.unary.operand = ui_float(p, NULL, offset, 1);
    return minus;
}

/* ---- Grids (spec §10.8) ---- */

/* `widgets.type.name`, placed at `at` */
static Expr *ui_variant(Parser *p, const Expr *at, const char *type, Name name) {
    Expr *module = ui_expr(p, EXPR_NAME, at->offset, at->length);
    module->as.name = ui_name("widgets", at->offset);
    Expr *of = ui_expr(p, EXPR_FIELD, at->offset, at->length);
    of->as.field.object = module;
    of->as.field.name = ui_name(type, at->offset);
    Expr *variant = ui_expr(p, EXPR_FIELD, at->offset, at->length);
    variant->as.field.object = of;
    variant->as.field.name = name;
    return variant;
}

static bool ui_is_name(const Expr *value, const char *name) {
    return value && value->kind == EXPR_NAME && name_text_is(value->as.name, name);
}

/* `Fill(2)`, `Pixels(120)`: a call of `name` with one value */
static bool ui_is_call_of(const Expr *value, const char *name) {
    return value && value->kind == EXPR_CALL && value->as.call.arg_count == 1 && ui_is_name(value->as.call.callee, name);
}

/* A size of a column or a row: `120` is `widgets.Size.Pixels(120.0)`, `Auto` and `Fill` are
   qualified, `Fill` alone weighs 1; another expression is a `widgets.Size` already. */
static Expr *ui_size(Parser *p, Expr *value) {
    if (value->kind == EXPR_INT || value->kind == EXPR_FLOAT) {
        Name pixels = ui_name("Pixels", value->offset);
        return ui_call(p, ui_variant(p, value, "Size", pixels), ui_float(p, value, value->offset, 0), value->offset,
                       value->length);
    }
    if (ui_is_name(value, "Auto")) return ui_variant(p, value, "Size", value->as.name);
    if (ui_is_name(value, "Fill")) {
        return ui_call(p, ui_variant(p, value, "Size", value->as.name), ui_float(p, NULL, value->offset, 1),
                       value->offset, value->length);
    }
    if (ui_is_call_of(value, "Fill") || ui_is_call_of(value, "Pixels")) {
        Expr *callee = value->as.call.callee;
        value->as.call.callee = ui_variant(p, callee, "Size", callee->as.name);
        value->as.call.args[0] = ui_float(p, value->as.call.args[0], value->offset, 0);
    }
    return value;
}

/* A width or a height in pixels: `Fill` is what is left (`ui_frame.fill_width()`), `Auto` the
   size of the widget, `fallback`. */
static Expr *ui_extent(Parser *p, Expr *value, size_t offset, uint64_t fallback, bool horizontal) {
    if (ui_is_name(value, "Fill") || ui_is_call_of(value, "Fill")) {
        return ui_frame_call(p, value->offset, value->length, horizontal ? "fill_width" : "fill_height", NULL, 0);
    }
    if (ui_is_name(value, "Auto")) return ui_float(p, NULL, value->offset, fallback);
    return ui_float(p, value, offset, fallback);
}

/* `-1` */
static Expr *ui_minus_one(Parser *p, size_t offset) {
    Expr *minus = ui_expr(p, EXPR_UNARY, offset, 0);
    minus->as.unary.op = TOK_MINUS;
    minus->as.unary.operand = ui_int(p, offset, 1);
    return minus;
}

static bool ui_cell_property(Name name) {
    return name_text_is(name, "row") || name_text_is(name, "col") || name_text_is(name, "span") ||
           name_text_is(name, "row_span") || name_text_is(name, "align");
}

/* The cell of a node in a `Grid`: its properties `row:`, `col:`, `span:`, `row_span:` and
   `align:` are taken out of `args`, and become `ui_frame.cell(row, col, span, row_span, align)`
   before it; NULL when it gives none. Outside a grid, `align:` alone becomes
   `ui_frame.align(...)`, and the others are an error (`*valid` false). */
static Stmt *ui_take_cell(Parser *p, UiArgs *args, size_t start, size_t length, bool *valid) {
    Expr *row = NULL, *col = NULL, *span = NULL, *row_span = NULL, *align = NULL;
    size_t kept = 0;
    for (size_t i = 0; i < args->props.count; i++) {
        UiProp prop = args->props.items[i];
        if (!ui_cell_property(prop.name)) {
            args->props.items[kept++] = prop;
            continue;
        }
        if (!p->ui_in_grid && !name_text_is(prop.name, "align")) {
            Diagnostic *d = error_at(p, "E0215", prop.name.offset, prop.name.length, "`%.*s:` places a node in a `Grid`",
                                     (int)prop.name.length, prop.name.text);
            diag_help(d, "put the node in a grid: `Grid(columns: [Auto, Fill]) { ... }`");
            *valid = false;
            return NULL;
        }
        Expr **slot = name_text_is(prop.name, "row") ? &row : name_text_is(prop.name, "col") ? &col
                    : name_text_is(prop.name, "span") ? &span : name_text_is(prop.name, "row_span") ? &row_span : &align;
        if (*slot) {
            error_at(p, "E0215", prop.name.offset, prop.name.length, "`%.*s` is given twice", (int)prop.name.length,
                     prop.name.text);
            *valid = false;
            return NULL;
        }
        *slot = prop.value;
    }
    args->props.count = kept;
    if (!p->ui_in_grid) {  /* `align:` in a column or a row: `ui_frame.align(...)` */
        if (!align) return NULL;
        if (align->kind == EXPR_NAME) align = ui_variant(p, align, "Align", align->as.name);
        return ui_frame_stmt(p, align->offset, align->length, "align", align);
    }
    if (!row && !col && !span && !row_span && !align) return NULL;
    if (row && col && row->kind == EXPR_INT && col->kind == EXPR_INT) {
        uint64_t key = row->as.int_value * 64 + col->as.int_value;
        for (size_t i = 0; i < p->ui_cells.count; i++) {
            if (p->ui_cells.items[i] == key) {
                Diagnostic *d = error_at(p, "E0215", row->offset, col->offset + col->length - row->offset,
                                         "the cell (%llu, %llu) is given twice", (unsigned long long)row->as.int_value,
                                         (unsigned long long)col->as.int_value);
                diag_help(d, "a cell holds one node: put several in a `Column` or a `Row` in it");
                *valid = false;
                return NULL;
            }
        }
        SOSU_VEC_PUSH(&p->ui_cells, key);
    }
    if (align && align->kind == EXPR_NAME) align = ui_variant(p, align, "Align", align->as.name);
    Expr *cell_args[5] = { row ? row : ui_minus_one(p, start), col ? col : ui_minus_one(p, start),
                           span ? span : ui_int(p, start, 1), row_span ? row_span : ui_int(p, start, 1),
                           align ? align : ui_variant(p, ui_expr(p, EXPR_NAME, start, length), "Align", ui_name("Start", start)) };
    return ui_stmt(p, ui_frame_call(p, start, length, "cell", cell_args, 5));
}

/* `columns: [...]` or `rows: [...]`: `ui_frame.grid_column(size)` or `grid_row(size)` for each. */
static void ui_grid_sizes(Parser *p, StmtVec *out, Expr *sizes, const char *method) {
    if (!sizes) return;
    if (sizes->kind != EXPR_ARRAY || sizes->as.array.repeat) {
        Diagnostic *d = error_at(p, "E0215", sizes->offset, sizes->length, "the sizes of a grid are a list");
        diag_help(d, "one size per column or row: `[120, Auto, Fill, Fill(2)]`");
        return;
    }
    for (size_t i = 0; i < sizes->as.array.count; i++) {
        Expr *size = ui_size(p, sizes->as.array.items[i]);
        SOSU_VEC_PUSH(out, ui_frame_stmt(p, size->offset, size->length, method, size));
    }
}

/* `template: FileItem(f)` (spec §10.10): the component `FileItem` called with the frame, as a
   node, in a scope of its own: `ui_frame.push_id(site); FileItem(ui_frame, f); ui_frame.pop_id();`. */
static void ui_template(Parser *p, StmtVec *out, Expr *call) {
    if (call->kind != EXPR_CALL) {
        Diagnostic *d = error_at(p, "E0215", call->offset, call->length, "a template is a component and its values");
        diag_help(d, "`template: FileItem(f)`, where `component FileItem(state.File* f) { Item { ... } }`");
        p->panic = false;
        return;
    }
    Expr *frame = ui_expr(p, EXPR_NAME, call->offset, call->length);
    frame->as.name = ui_name("ui_frame", call->offset);
    size_t count = call->as.call.arg_count + 1;
    Expr **args = arena_alloc(p->arena, count * sizeof *args);
    args[0] = frame;
    for (size_t i = 1; i < count; i++) args[i] = call->as.call.args[i - 1];
    call->as.call.args = args;
    call->as.call.arg_count = count;
    SOSU_VEC_PUSH(out, ui_frame_stmt(p, call->offset, call->length, "push_id", ui_int(p, call->offset, call->offset)));
    SOSU_VEC_PUSH(out, ui_stmt(p, call));
    SOSU_VEC_PUSH(out, ui_frame_stmt(p, call->offset, call->length, "pop_id", NULL));
}

/* The items in view of a long list or a table: `for (i32 item in ui_frame.first_shown()..
   ui_frame.last_shown()) { ui_frame.push_id(item as ui64); the items; ui_frame.pop_id(); }`. */
static void ui_shown_loop(Parser *p, StmtVec *out, Expr *item, StmtVec *items, size_t start, size_t length) {
    if (!item || item->kind != EXPR_NAME) {
        Diagnostic *d = error_at(p, "E0215", item ? item->offset : start, item ? item->length : length,
                                 "`count:` needs `item:`, the name of the index of the item described");
        diag_help(d, "`List(count: s.count, item: i) { Text(s.names[i]) }`");
        p->panic = false;
        free(items->items);
        return;
    }
    Expr *range = ui_expr(p, EXPR_RANGE, start, length);
    range->as.range.start = ui_frame_call(p, start, length, "first_shown", NULL, 0);
    range->as.range.end = ui_frame_call(p, start, length, "last_shown", NULL, 0);
    range->as.range.inclusive = false;
    Expr *use = ui_expr(p, EXPR_NAME, item->offset, item->length);
    use->as.name = item->as.name;
    Expr *key = ui_expr(p, EXPR_CAST, item->offset, item->length);
    key->as.cast.value = use;
    key->as.cast.type = ui_type(p, item->offset, NULL, "ui64");
    StmtVec body = {0};
    SOSU_VEC_PUSH(&body, ui_frame_stmt(p, item->offset, item->length, "push_id", key));
    for (size_t i = 0; i < items->count; i++) SOSU_VEC_PUSH(&body, items->items[i]);
    free(items->items);
    SOSU_VEC_PUSH(&body, ui_frame_stmt(p, start, length, "pop_id", NULL));
    Stmt *loop = new_stmt(p, STMT_FOR_IN, start);
    loop->length = length;
    loop->as.for_in.is_mut = false;
    loop->as.for_in.type = ui_type(p, item->offset, NULL, "i32");
    loop->as.for_in.name = item->as.name;
    loop->as.for_in.iterable = range;
    loop->as.for_in.body = ui_block(p, &body, start, length);
    SOSU_VEC_PUSH(out, loop);
}

/* The columns of a `Table`: `Column(title, width: size, sortable: true, align: End) { its cell }`,
   each giving `ui_frame.table_column(...)` to `header` and its cell, after `ui_frame.table_cell()`,
   to `cells`. */
static void ui_table_columns(Parser *p, StmtVec *header, StmtVec *cells) {
    if (!expect(p, TOK_LBRACE)) return;
    while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
        size_t before = p->pos;
        size_t start = cur(p)->offset;
        if (!check(p, TOK_IDENT) || !token_is(p, cur(p), "Column")) {
            Diagnostic *d = error_at(p, "E0215", cur(p)->offset, cur(p)->length, "a `Table` contains only `Column` nodes");
            diag_help(d, "`Table(count: s.count, item: i) { Column(\"Name\", width: Fill) { Text(s.names[i]) } }`");
            skip_balanced_braces(p);
            p->panic = false;
            if (p->pos == before) advance(p);
            continue;
        }
        advance(p);
        UiArgs args;
        if (check(p, TOK_LPAREN)) ui_parse_args(p, &args);
        else memset(&args, 0, sizeof args);
        size_t length = span_from(p, start);
        Expr *title = args.values.count ? ui_translate(p, &args, args.values.items[0]) : NULL;
        if (!title) {
            error_at(p, "E0215", start, length, "a `Column` of a table has a title: `Column(\"Name\") { ... }`");
            p->panic = false;
            title = ui_expr(p, EXPR_NAME, start, length);
            title->as.name = ui_name("none", start);
        }
        Expr *width = ui_prop(&args, "width");
        Expr *sortable = ui_prop(&args, "sortable");
        Expr *align = ui_prop(&args, "align");
        if (!sortable) {
            sortable = ui_expr(p, EXPR_BOOL, start, length);
            sortable->as.bool_value = false;
        }
        if (align && align->kind == EXPR_NAME) align = ui_variant(p, align, "Align", align->as.name);
        Expr *column_args[4] = { title, width ? ui_size(p, width) : ui_variant(p, ui_expr(p, EXPR_NAME, start, length), "Size", ui_name("Auto", start)),
                                 sortable, align ? align : ui_variant(p, ui_expr(p, EXPR_NAME, start, length), "Align", ui_name("Start", start)) };
        SOSU_VEC_PUSH(header, ui_stmt(p, ui_frame_call(p, start, length, "table_column", column_args, 4)));
        SOSU_VEC_PUSH(cells, ui_stmt(p, ui_frame_call(p, start, length, "table_cell", NULL, 0)));
        if (check(p, TOK_LBRACE)) ui_parse_children(p, cells);
        free(args.values.items);
        free(args.props.items);
        if (p->pos == before) advance(p);
    }
    expect(p, TOK_RBRACE);
}

/* A node: a widget of `ui_nodes`, or a component (`TaskRow(t)`, `tasks.TaskRow(t)`). */
static void ui_parse_node(Parser *p, StmtVec *out, bool window_top) {
    size_t start = cur(p)->offset;
    Path path = parse_path(p);
    Name node = path.items[path.count - 1];
    bool at_top = p->ui_node_at_top;
    p->ui_node_at_top = false;
    if (path.count == 1 && name_text_is(node, "Dialog")) {
        if (!at_top || !p->ui_dialogs) {
            Diagnostic *d = error_at(p, "E0215", node.offset, node.length, "a `Dialog` is at the top of a window or a component");
            diag_help(d, "it is drawn above the rest: put it beside the other nodes of the window, not inside one");
            if (check(p, TOK_LPAREN)) {
                UiArgs skipped;
                ui_parse_args(p, &skipped);
                free(skipped.values.items);
                free(skipped.props.items);
            }
            if (check(p, TOK_LBRACE)) skip_balanced_braces(p);
            return;
        }
        out = (StmtVec *)p->ui_dialogs;  /* emitted at the end of the view */
    }
    UiArgs args;
    if (check(p, TOK_LPAREN)) {
        ui_parse_args(p, &args);
    } else {
        memset(&args, 0, sizeof args);  /* `List { ... }`: no arguments, no parentheses */
    }
    size_t length = span_from(p, start);

    const UiNodeKind *kind = NULL;
    for (size_t i = 0; path.count == 1 && i < sizeof ui_nodes / sizeof *ui_nodes; i++) {
        if (name_text_is(node, ui_nodes[i].name)) kind = &ui_nodes[i];
    }
    if (!ui_check_place(p, kind, node, window_top)) {
        if (check(p, TOK_LBRACE)) skip_balanced_braces(p);
        free(args.values.items);
        free(args.props.items);
        return;
    }
    bool in_grid = p->ui_in_grid;
    bool in_tabs = p->ui_in_tabs;
    bool in_tree = p->ui_in_tree;
    bool in_component = p->ui_in_component;
    bool placed = true;
    Stmt *cell = ui_take_cell(p, &args, start, length, &placed);
    if (cell) SOSU_VEC_PUSH(out, cell);
    Expr *tip = NULL;  /* `tooltip:`, on any node: `ui_frame.tooltip(text)` after it */
    size_t kept_props = 0;
    for (size_t i = 0; i < args.props.count; i++) {
        if (name_text_is(args.props.items[i].name, "tooltip")) tip = args.props.items[i].value;
        else args.props.items[kept_props++] = args.props.items[i];
    }
    args.props.count = kept_props;
    p->ui_in_grid = false;  /* the children of a node are not cells, unless it is a grid */
    p->ui_in_tabs = false;
    p->ui_in_tree = false;
    p->ui_in_component = false;
    if (!kind) {
        ui_component(p, out, path, &args, start, length);
    } else {
        bool valid = placed && ui_check_args(p, kind, node, &args);
        if (kind->children != check(p, TOK_LBRACE)) {
            if (kind->children) {
                report_expected(p, "E0215", "`{` and the nodes it contains", false);
            } else {
                error_at(p, "E0215", cur(p)->offset, cur(p)->length, UI_CONTAINERS);
                skip_balanced_braces(p);
            }
            valid = false;
        }
        Expr *value = args.values.count ? args.values.items[0] : NULL;
        static const char *const ui_texts[] = { "Text", "Title", "Button", "Checkbox", "Radio", "Option", "Menu", "MenuItem", "Group", "Tab", "Branch", "Leaf", "Dialog" };
        for (size_t i = 0; valid && value && i < sizeof ui_texts / sizeof *ui_texts; i++) {
            if (strcmp(kind->name, ui_texts[i]) == 0) value = ui_translate(p, &args, value);
        }
        bool identified = false;
        for (size_t i = 0; i < sizeof ui_identified / sizeof *ui_identified; i++) {
            identified = identified || strcmp(kind->name, ui_identified[i]) == 0;
        }
        if (valid) ui_node_styles(p, out, ui_prop(&args, "style"));
        if (valid && identified) {
            /* `if (focus) { f.focus_here(); }`, then `f.at(site)`: its place in the file */
            Expr *focus = ui_prop(&args, "focus");
            if (focus) {
                StmtVec take = {0};
                SOSU_VEC_PUSH(&take, ui_frame_stmt(p, focus->offset, focus->length, "focus_here", NULL));
                Stmt *when = new_stmt(p, STMT_IF, focus->offset);
                when->length = focus->length;
                when->as.if_.cond = focus;
                when->as.if_.then_block = ui_block(p, &take, focus->offset, focus->length);
                SOSU_VEC_PUSH(out, when);
            }
            SOSU_VEC_PUSH(out, ui_frame_stmt(p, start, length, "at", ui_int(p, start, start)));
        }
        if (!valid) {
            if (kind->children && check(p, TOK_LBRACE)) ui_parse_children(p, out);
        } else if (name_text_is(node, "List")) {
            /* f.list(width, height, some(&selected) or none), the items, f.end_list() */
            Expr *several = ui_prop(&args, "selection");  /* several choices: f.multiple(&selection) first */
            if (several) SOSU_VEC_PUSH(out, ui_frame_stmt(p, several->offset, several->length, "multiple", ui_address(p, several)));
            Expr *on_drop = ui_prop(&args, "on_drop");  /* items dragged: f.reorderable() first */
            if (on_drop) SOSU_VEC_PUSH(out, ui_frame_stmt(p, on_drop->offset, on_drop->length, "reorderable", NULL));
            Expr *selected = ui_prop(&args, "selected");
            Expr *choice;
            if (selected) {
                choice = ui_expr(p, EXPR_CALL, selected->offset, selected->length);
                choice->as.call.callee = ui_expr(p, EXPR_NAME, selected->offset, selected->length);
                choice->as.call.callee->as.name = ui_name("some", selected->offset);
                choice->as.call.arg_count = 1;
                choice->as.call.args = arena_alloc(p->arena, sizeof(Expr *));
                choice->as.call.args[0] = ui_address(p, selected);
            } else {
                choice = ui_expr(p, EXPR_NAME, start, length);
                choice->as.name = ui_name("none", start);
            }
            Expr *list_args[4] = { ui_extent(p, ui_prop(&args, "width"), start, 300, true),
                                   ui_extent(p, ui_prop(&args, "height"), start, 200, false), choice, NULL };
            Expr *count = ui_prop(&args, "count");
            if (count) {  /* a long list: f.list_of(count, ...), and its items in view only */
                Expr *long_args[4] = { count, list_args[0], list_args[1], choice };
                SOSU_VEC_PUSH(out, ui_stmt(p, ui_frame_call(p, start, length, "list_of", long_args, 4)));
                StmtVec items = {0};
                ui_parse_children(p, &items);
                ui_shown_loop(p, out, ui_prop(&args, "item"), &items, start, length);
            } else {
                SOSU_VEC_PUSH(out, ui_stmt(p, ui_frame_call(p, start, length, "list", list_args, 3)));
                ui_parse_children(p, out);
            }
            SOSU_VEC_PUSH(out, ui_stmt(p, ui_frame_call(p, start, length, "end_list", NULL, 0)));
            if (on_drop) {  /* if (f.dropped()) { on_drop(f.drop_from(), f.drop_to()); } */
                Expr *call = ui_expr(p, EXPR_CALL, on_drop->offset, on_drop->length);
                call->as.call.callee = on_drop;
                call->as.call.arg_count = 2;
                call->as.call.args = arena_alloc(p->arena, 2 * sizeof(Expr *));
                call->as.call.args[0] = ui_frame_call(p, on_drop->offset, on_drop->length, "drop_from", NULL, 0);
                call->as.call.args[1] = ui_frame_call(p, on_drop->offset, on_drop->length, "drop_to", NULL, 0);
                SOSU_VEC_PUSH(out, ui_when(p, ui_frame_call(p, start, length, "dropped", NULL, 0), call));
            }
        } else if (name_text_is(node, "Table")) {
            /* f.table(count, width, height, selected, sort), f.table_column(...) for each column,
               f.table_rows(), the rows in view (a cell per column), if (f.end_table()) { on_activate } */
            Expr *selected = ui_prop(&args, "selected");
            Expr *sort = ui_prop(&args, "sort");
            Expr *several = ui_prop(&args, "selection");
            if (several) SOSU_VEC_PUSH(out, ui_frame_stmt(p, several->offset, several->length, "multiple", ui_address(p, several)));
            Expr *none_selected = ui_expr(p, EXPR_NAME, start, length);
            none_selected->as.name = ui_name("none", start);
            Expr *none_sort = ui_expr(p, EXPR_NAME, start, length);
            none_sort->as.name = ui_name("none", start);
            Expr *count = ui_prop(&args, "count");
            Expr *table_args[5] = { count ? count : ui_int(p, start, 0), ui_extent(p, ui_prop(&args, "width"), start, 400, true),
                                    ui_extent(p, ui_prop(&args, "height"), start, 240, false),
                                    selected ? ui_some(p, ui_address(p, selected)) : none_selected,
                                    sort ? ui_some(p, ui_address(p, sort)) : none_sort };
            SOSU_VEC_PUSH(out, ui_stmt(p, ui_frame_call(p, start, length, "table", table_args, 5)));
            StmtVec cells = {0};
            ui_table_columns(p, out, &cells);
            SOSU_VEC_PUSH(out, ui_stmt(p, ui_frame_call(p, start, length, "table_rows", NULL, 0)));
            ui_shown_loop(p, out, ui_prop(&args, "item"), &cells, start, length);
            SOSU_VEC_PUSH(out, ui_when(p, ui_frame_call(p, start, length, "end_table", NULL, 0), ui_prop(&args, "on_activate")));
        } else if (name_text_is(node, "Scroll")) {
            /* f.scroll(width, height), the nodes, f.end_scroll() */
            Expr *size[2] = { ui_extent(p, ui_prop(&args, "width"), start, 0, true),
                              ui_extent(p, ui_prop(&args, "height"), start, 0, false) };
            SOSU_VEC_PUSH(out, ui_stmt(p, ui_frame_call(p, start, length, "scroll", size, 2)));
            ui_parse_children(p, out);
            SOSU_VEC_PUSH(out, ui_stmt(p, ui_frame_call(p, start, length, "end_scroll", NULL, 0)));
        } else if (name_text_is(node, "Column") || name_text_is(node, "Row")) {
            Expr *layout[2] = { ui_layout_size(p, ui_prop(&args, "gap"), start),
                                ui_layout_size(p, ui_prop(&args, "padding"), start) };
            const char *open = name_text_is(node, "Row") ? "row_with" : "column";
            SOSU_VEC_PUSH(out, ui_stmt(p, ui_frame_call(p, start, length, open, layout, 2)));
            ui_parse_children(p, out);
            SOSU_VEC_PUSH(out, ui_stmt(p, ui_frame_call(p, start, length, "end", NULL, 0)));
        } else if (name_text_is(node, "Text") || name_text_is(node, "Title")) {
            const char *method = name_text_is(node, "Text") ? "label" : "title";
            SOSU_VEC_PUSH(out, ui_stmt(p, ui_frame_call(p, start, length, method, &value, 1)));
        } else if (name_text_is(node, "MenuBar") || name_text_is(node, "Menu") || name_text_is(node, "ContextMenu")) {
            /* f.menu_bar(), f.menu(title) or f.context_menu(), the nodes, then the matching end */
            bool bar = name_text_is(node, "MenuBar");
            bool context = name_text_is(node, "ContextMenu");
            const char *open = bar ? "menu_bar" : context ? "context_menu" : "menu";
            const char *close = bar ? "end_menu_bar" : context ? "end_context_menu" : "end_menu";
            SOSU_VEC_PUSH(out, ui_stmt(p, ui_frame_call(p, start, length, open, &value, bar || context ? 0 : 1)));
            bool in_menu = p->ui_in_menu;
            bool in_bar = p->ui_in_menu_bar;
            p->ui_in_menu = !bar;
            p->ui_in_menu_bar = bar;
            ui_parse_children(p, out);
            p->ui_in_menu = in_menu;
            p->ui_in_menu_bar = in_bar;
            SOSU_VEC_PUSH(out, ui_stmt(p, ui_frame_call(p, start, length, close, NULL, 0)));
        } else if (name_text_is(node, "MenuItem")) {
            /* f.menu_item(s), f.menu_item_enabled(s, c) or f.menu_item_with(s, key, c) */
            Expr *key = ui_prop(&args, "shortcut");
            Expr *enabled = ui_prop(&args, "enabled");
            Expr *item;
            if (key) {
                if (!enabled) {
                    enabled = ui_expr(p, EXPR_BOOL, start, length);
                    enabled->as.bool_value = true;
                }
                Expr *call_args[3] = { value, key, enabled };
                item = ui_frame_call(p, start, length, "menu_item_with", call_args, 3);
            } else {
                Expr *call_args[2] = { value, enabled };
                item = ui_frame_call(p, start, length, enabled ? "menu_item_enabled" : "menu_item", call_args,
                                     enabled ? 2 : 1);
            }
            SOSU_VEC_PUSH(out, ui_when(p, item, ui_prop(&args, "on_click")));
        } else if (name_text_is(node, "Separator")) {
            SOSU_VEC_PUSH(out, ui_stmt(p, ui_frame_call(p, start, length, "separator", NULL, 0)));
        } else if (name_text_is(node, "Image")) {
            Expr *image_args[2] = { value, ui_float(p, ui_prop(&args, "size"), start, 0) };
            SOSU_VEC_PUSH(out, ui_stmt(p, ui_frame_call(p, start, length, "image", image_args, 2)));
        } else if (name_text_is(node, "Space")) {
            Expr *size = ui_float(p, value, start, 0);
            SOSU_VEC_PUSH(out, ui_stmt(p, ui_frame_call(p, start, length, "space", &size, 1)));
        } else if (name_text_is(node, "Button")) {
            Expr *enabled = ui_prop(&args, "enabled");
            Expr *call_args[2] = { value, enabled };
            Expr *button = ui_frame_call(p, start, length, enabled ? "button_enabled" : "button", call_args, enabled ? 2 : 1);
            SOSU_VEC_PUSH(out, ui_when(p, button, ui_prop(&args, "on_click")));
        } else if (name_text_is(node, "DateField")) {
            /* f.date_field(&bind, width, format, monday_first); on_change when it changed */
            Expr *format = ui_prop(&args, "format");
            Expr *monday = ui_prop(&args, "monday_first");
            if (!format) {  /* `widgets.default_date_format()`: dd/mm/yyyy */
                Expr *module = ui_expr(p, EXPR_NAME, start, length);
                module->as.name = ui_name("widgets", start);
                Expr *callee = ui_expr(p, EXPR_FIELD, start, length);
                callee->as.field.object = module;
                callee->as.field.name = ui_name("default_date_format", start);
                format = ui_call(p, callee, NULL, start, length);
            }
            if (!monday) {
                monday = ui_expr(p, EXPR_BOOL, start, length);
                monday->as.bool_value = true;
            }
            Expr *date_args[4] = { ui_address(p, ui_prop(&args, "bind")), ui_extent(p, ui_prop(&args, "width"), start, 160, true),
                                   format, monday };
            SOSU_VEC_PUSH(out, ui_when(p, ui_frame_call(p, start, length, "date_field", date_args, 4), ui_prop(&args, "on_change")));
        } else if (name_text_is(node, "Dialog")) {
            /* if (f.dialog(title, width, &open)) { the nodes; f.end_dialog(); }, at the end of the view */
            Expr *dialog_args[3] = { value, ui_float(p, ui_prop(&args, "width"), start, 400), ui_address(p, ui_prop(&args, "open")) };
            StmtVec inner = {0};
            size_t body_start = cur(p)->offset;
            ui_parse_children(p, &inner);
            SOSU_VEC_PUSH(&inner, ui_stmt(p, ui_frame_call(p, start, length, "end_dialog", NULL, 0)));
            Stmt *when = new_stmt(p, STMT_IF, start);
            when->length = length;
            when->as.if_.cond = ui_frame_call(p, start, length, "dialog", dialog_args, 3);
            when->as.if_.then_block = ui_block(p, &inner, body_start, span_from(p, body_start));
            SOSU_VEC_PUSH(out, when);
        } else if (name_text_is(node, "Slider")) {
            /* f.slider(&bind, width, min, max, step); on_change when it changed */
            Expr *low = ui_prop(&args, "min");
            Expr *high = ui_prop(&args, "max");
            Expr *step = ui_prop(&args, "step");
            Expr *slider_args[5] = { ui_address(p, ui_prop(&args, "bind")), ui_extent(p, ui_prop(&args, "width"), start, 200, true),
                                     low ? low : ui_int(p, start, 0), high ? high : ui_int(p, start, 100),
                                     step ? step : ui_int(p, start, 1) };
            SOSU_VEC_PUSH(out, ui_when(p, ui_frame_call(p, start, length, "slider", slider_args, 5), ui_prop(&args, "on_change")));
        } else if (name_text_is(node, "Progress")) {
            /* f.progress(value, max, width), or f.progress_busy(width) without a value */
            Expr *done = ui_prop(&args, "value");
            Expr *total = ui_prop(&args, "max");
            Expr *width = ui_extent(p, ui_prop(&args, "width"), start, 200, true);
            Expr *progress_args[3] = { done, total ? total : ui_int(p, start, 100), width };
            SOSU_VEC_PUSH(out, ui_stmt(p, done ? ui_frame_call(p, start, length, "progress", progress_args, 3)
                                               : ui_frame_call(p, start, length, "progress_busy", &width, 1)));
        } else if (name_text_is(node, "NumberField") || name_text_is(node, "MoneyField")) {
            /* f.number_field(&bind, width, widgets.Number{ ... }) or f.money_field(...); on_change when it changed */
            bool money = name_text_is(node, "MoneyField");
            static const char *const names[] = { "min", "max", "step", "decimals", "decimal", "group", "symbol", "symbol_first" };
            UiFields fields = {0};
            for (size_t i = 0; i < 8; i++) {
                Expr *option = ui_prop(&args, names[i]);
                if (!option) continue;
                FieldInit field = { ui_name(names[i], option->offset), i < 2 ? ui_some(p, option) : option };
                SOSU_VEC_PUSH(&fields, field);
            }
            if (money && !ui_prop(&args, "decimals")) {  /* money has cents unless it says otherwise */
                FieldInit field = { ui_name("decimals", start), ui_int(p, start, 2) };
                SOSU_VEC_PUSH(&fields, field);
            }
            Expr *number_args[3] = { ui_address(p, ui_prop(&args, "bind")), ui_extent(p, ui_prop(&args, "width"), start, 160, true),
                                     ui_struct(p, "Number", &fields, start, length) };
            Expr *call = ui_frame_call(p, start, length, money ? "money_field" : "number_field", number_args, 3);
            SOSU_VEC_PUSH(out, ui_when(p, call, ui_prop(&args, "on_change")));
        } else if (name_text_is(node, "TextArea")) {
            /* f.text_area(&bind, width, rows, widgets.Area{ wrap, tabs, placeholder }), and on_enter on Ctrl+Enter */
            static const char *const names[] = { "wrap", "tabs", "placeholder" };
            UiFields fields = {0};
            for (size_t i = 0; i < 3; i++) {
                Expr *option = ui_prop(&args, names[i]);
                if (!option) continue;
                FieldInit field = { ui_name(names[i], option->offset), i == 2 ? ui_translate(p, &args, option) : option };
                SOSU_VEC_PUSH(&fields, field);
            }
            Expr *rows = ui_prop(&args, "rows");
            Expr *area_args[4] = { ui_address(p, ui_prop(&args, "bind")), ui_extent(p, ui_prop(&args, "width"), start, 300, true),
                                   rows ? rows : ui_int(p, start, 5), ui_struct(p, "Area", &fields, start, length) };
            SOSU_VEC_PUSH(out, ui_when(p, ui_frame_call(p, start, length, "text_area", area_args, 4), ui_prop(&args, "on_enter")));
        } else if (name_text_is(node, "TextField")) {
            Expr *call_args[3] = { ui_address(p, ui_prop(&args, "bind")), ui_extent(p, ui_prop(&args, "width"), start, 200, true),
                                   ui_format(p, &args, start, length) };
            Expr *field = ui_frame_call(p, start, length, call_args[2] ? "text_field_with" : "text_field", call_args,
                                        call_args[2] ? 3 : 2);
            SOSU_VEC_PUSH(out, ui_when(p, field, ui_prop(&args, "on_enter")));
        } else if (name_text_is(node, "Checkbox")) {
            Expr *bound = ui_address(p, ui_prop(&args, "bind"));
            Expr *call_args[2] = { value, bound };
            Expr *box = value ? ui_frame_call(p, start, length, "checkbox", call_args, 2)
                              : ui_frame_call(p, start, length, "checkbox_alone", &bound, 1);
            SOSU_VEC_PUSH(out, ui_when(p, box, ui_prop(&args, "on_change")));
        } else if (name_text_is(node, "Radio")) {
            SOSU_VEC_PUSH(out, ui_choice(p, "radio", value, ui_prop(&args, "bind"), ui_prop(&args, "value"),
                                         ui_prop(&args, "on_change"), start, length));
        } else if (name_text_is(node, "Option")) {
            SOSU_VEC_PUSH(out, ui_choice(p, "option", value, p->ui_dropdown, ui_prop(&args, "value"),
                                         p->ui_dropdown_change, start, length));
        } else if (name_text_is(node, "TitleBar")) {
            /* f.title_bar(height, gap, padding), the nodes, f.end_title_bar() */
            Expr *bar[3] = { ui_float(p, ui_prop(&args, "height"), start, 40), ui_float(p, ui_prop(&args, "gap"), start, 8),
                             ui_float(p, ui_prop(&args, "padding"), start, 4) };
            p->ui_title_bar = true;
            SOSU_VEC_PUSH(out, ui_stmt(p, ui_frame_call(p, start, length, "title_bar", bar, 3)));
            ui_parse_children(p, out);
            SOSU_VEC_PUSH(out, ui_stmt(p, ui_frame_call(p, start, length, "end_title_bar", NULL, 0)));
        } else if (name_text_is(node, "Dropdown")) {
            /* f.dropdown(width) or f.dropdown_with(width, placeholder), the options, f.end_dropdown() */
            Expr *width = ui_extent(p, ui_prop(&args, "width"), start, 160, true);
            Expr *hint = ui_translate(p, &args, ui_prop(&args, "placeholder"));
            Expr *drop_args[2] = { width, hint };
            const char *open = hint ? "dropdown_with" : "dropdown";
            SOSU_VEC_PUSH(out, ui_stmt(p, ui_frame_call(p, start, length, open, drop_args, hint ? 2 : 1)));
            p->ui_dropdown = ui_prop(&args, "bind");
            p->ui_dropdown_change = ui_prop(&args, "on_change");
            ui_parse_children(p, out);
            p->ui_dropdown = NULL;
            p->ui_dropdown_change = NULL;
            SOSU_VEC_PUSH(out, ui_stmt(p, ui_frame_call(p, start, length, "end_dropdown", NULL, 0)));
        } else if (name_text_is(node, "Tree")) {
            /* f.tree(width, height, some(&selected) or none), the branches and leaves, f.end_tree() */
            Expr *selected = ui_prop(&args, "selected");
            Expr *choice = ui_expr(p, EXPR_NAME, start, length);
            choice->as.name = ui_name("none", start);
            if (selected) choice = ui_some(p, ui_address(p, selected));
            Expr *tree_args[3] = { ui_extent(p, ui_prop(&args, "width"), start, 300, true),
                                   ui_extent(p, ui_prop(&args, "height"), start, 200, false), choice };
            Expr *count = ui_prop(&args, "count");
            if (count) {  /* a long tree: f.tree_of(count, ...), each row in view after f.tree_row(depth, branch, open) */
                Expr *long_args[4] = { count, tree_args[0], tree_args[1], choice };
                SOSU_VEC_PUSH(out, ui_stmt(p, ui_frame_call(p, start, length, "tree_of", long_args, 4)));
                Expr *depth = ui_prop(&args, "depth");
                Expr *branch = ui_prop(&args, "branch");
                Expr *open = ui_prop(&args, "open");
                if (!branch) {
                    branch = ui_expr(p, EXPR_BOOL, start, length);
                    branch->as.bool_value = false;
                }
                if (!open) {
                    open = ui_expr(p, EXPR_BOOL, start, length);
                    open->as.bool_value = false;
                }
                Expr *row_args[3] = { depth ? depth : ui_int(p, start, 0), branch, open };
                StmtVec row = {0};
                SOSU_VEC_PUSH(&row, ui_when(p, ui_frame_call(p, start, length, "tree_row", row_args, 3), ui_prop(&args, "on_toggle")));
                ui_parse_children(p, &row);  /* the widgets of a row */
                SOSU_VEC_PUSH(&row, ui_stmt(p, ui_frame_call(p, start, length, "end_tree_row", NULL, 0)));
                ui_shown_loop(p, out, ui_prop(&args, "item"), &row, start, length);
            } else {
                SOSU_VEC_PUSH(out, ui_stmt(p, ui_frame_call(p, start, length, "tree", tree_args, 3)));
                p->ui_in_tree = true;
                ui_parse_children(p, out);
                p->ui_in_tree = false;
            }
            SOSU_VEC_PUSH(out, ui_stmt(p, ui_frame_call(p, start, length, "end_tree", NULL, 0)));
        } else if (name_text_is(node, "Branch")) {
            /* if (f.branch(text, some(&open) or none)) { the branches and leaves; f.end_branch(); } */
            Expr *open = ui_prop(&args, "open");
            Expr *state = ui_expr(p, EXPR_NAME, start, length);
            state->as.name = ui_name("none", start);
            if (open) state = ui_some(p, ui_address(p, open));
            Expr *template = ui_prop(&args, "template");
            if (template) {  /* f.branch_open(open), its template, if (f.branch_close()) { children; f.end_branch(); } */
                SOSU_VEC_PUSH(out, ui_frame_stmt(p, start, length, "branch_open", state));
                ui_template(p, out, template);
            } else if (!value) {
                error_at(p, "E0215", node.offset, node.length, "a `Branch` has a text or a `template:`");
                p->panic = false;
            }
            Expr *icon = ui_prop(&args, "icon");
            Expr *branch_args[3] = { value, state, icon ? ui_some(p, icon) : NULL };
            StmtVec inner = {0};
            size_t body_start = cur(p)->offset;
            p->ui_in_tree = true;
            ui_parse_children(p, &inner);
            p->ui_in_tree = false;
            SOSU_VEC_PUSH(&inner, ui_stmt(p, ui_frame_call(p, start, length, "end_branch", NULL, 0)));
            Stmt *when = new_stmt(p, STMT_IF, start);
            when->length = length;
            when->as.if_.cond = template ? ui_frame_call(p, start, length, "branch_close", NULL, 0)
                                         : ui_frame_call(p, start, length, icon ? "branch_with" : "branch", branch_args, icon ? 3 : 2);
            when->as.if_.then_block = ui_block(p, &inner, body_start, span_from(p, body_start));
            SOSU_VEC_PUSH(out, when);
        } else if (name_text_is(node, "Leaf")) {
            Expr *template = ui_prop(&args, "template");
            Expr *icon = ui_prop(&args, "icon");
            Expr *leaf_args[2] = { value, icon ? ui_some(p, icon) : NULL };
            Expr *leaf = NULL;
            if (template) {  /* f.leaf_open(), its template, if (f.leaf_close()) { on_click } */
                SOSU_VEC_PUSH(out, ui_frame_stmt(p, start, length, "leaf_open", NULL));
                ui_template(p, out, template);
                leaf = ui_frame_call(p, start, length, "leaf_close", NULL, 0);
            } else if (value) {
                leaf = ui_frame_call(p, start, length, icon ? "leaf_with" : "leaf", leaf_args, icon ? 2 : 1);
            } else {
                error_at(p, "E0215", node.offset, node.length, "a `Leaf` has a text or a `template:`");
                p->panic = false;
            }
            if (leaf) SOSU_VEC_PUSH(out, ui_when(p, leaf, ui_prop(&args, "on_click")));
        } else if (name_text_is(node, "Item")) {
            /* f.item(horizontal, gap, padding, enabled), the widgets, f.end_item() */
            Expr *vertical = ui_prop(&args, "vertical");
            Expr *horizontal = ui_expr(p, EXPR_UNARY, start, length);
            horizontal->as.unary.op = TOK_BANG;
            horizontal->as.unary.operand = vertical;
            if (!vertical) {
                horizontal = ui_expr(p, EXPR_BOOL, start, length);
                horizontal->as.bool_value = true;
            }
            Expr *enabled = ui_prop(&args, "enabled");
            if (!enabled) {
                enabled = ui_expr(p, EXPR_BOOL, start, length);
                enabled->as.bool_value = true;
            }
            Expr *item_args[4] = { horizontal, ui_layout_size(p, ui_prop(&args, "gap"), start),
                                   ui_layout_size(p, ui_prop(&args, "padding"), start), enabled };
            SOSU_VEC_PUSH(out, ui_stmt(p, ui_frame_call(p, start, length, "item", item_args, 4)));
            ui_parse_children(p, out);
            SOSU_VEC_PUSH(out, ui_stmt(p, ui_frame_call(p, start, length, "end_item", NULL, 0)));
        } else if (name_text_is(node, "Tabs")) {
            /* f.tabs(some(&page) or none), the tabs, f.end_tabs() */
            Expr *bound = ui_prop(&args, "bind");
            Expr *chosen = ui_expr(p, EXPR_NAME, start, length);
            chosen->as.name = ui_name("none", start);
            if (bound) chosen = ui_some(p, ui_address(p, bound));
            Expr *placement = ui_prop(&args, "placement");
            if (placement && placement->kind == EXPR_NAME) placement = ui_variant(p, placement, "Side", placement->as.name);
            Expr *tabs_args[2] = { chosen, placement };
            SOSU_VEC_PUSH(out, ui_stmt(p, ui_frame_call(p, start, length, placement ? "tabs_at" : "tabs", tabs_args,
                                                        placement ? 2 : 1)));
            p->ui_in_tabs = true;
            ui_parse_children(p, out);
            p->ui_in_tabs = false;
            SOSU_VEC_PUSH(out, ui_stmt(p, ui_frame_call(p, start, length, "end_tabs", NULL, 0)));
        } else if (name_text_is(node, "Tab")) {
            /* if (f.tab(title, closable, enabled)) { the nodes }, then if (f.tab_closed()) { on_close(); } */
            Expr *on_close = ui_prop(&args, "on_close");
            Expr *closable = ui_prop(&args, "closable");
            Expr *enabled = ui_prop(&args, "enabled");
            if (!closable) {
                closable = ui_expr(p, EXPR_BOOL, start, length);
                closable->as.bool_value = on_close != NULL;  /* a tab the program closes has a cross */
            }
            if (!enabled) {
                enabled = ui_expr(p, EXPR_BOOL, start, length);
                enabled->as.bool_value = true;
            }
            Expr *tab_args[3] = { value, closable, enabled };
            StmtVec inner = {0};
            size_t body_start = cur(p)->offset;
            ui_parse_children(p, &inner);
            Stmt *when = new_stmt(p, STMT_IF, start);
            when->length = length;
            when->as.if_.cond = ui_frame_call(p, start, length, "tab", tab_args, 3);
            when->as.if_.then_block = ui_block(p, &inner, body_start, span_from(p, body_start));
            SOSU_VEC_PUSH(out, when);
            if (on_close) SOSU_VEC_PUSH(out, ui_when(p, ui_frame_call(p, start, length, "tab_closed", NULL, 0), on_close));
        } else if (name_text_is(node, "Group")) {
            /* f.group(title, gap, padding), or f.group_untitled(gap, padding), the nodes, f.end_group();
               folding: if (f.group_folding(title, gap, padding, some(&open) or none)) { the nodes } */
            Expr *gap = ui_layout_size(p, ui_prop(&args, "gap"), start);
            Expr *padding = ui_layout_size(p, ui_prop(&args, "padding"), start);
            Expr *collapsible = ui_prop(&args, "collapsible");
            Expr *open = ui_prop(&args, "open");
            if (collapsible && collapsible->kind != EXPR_BOOL) {
                Diagnostic *d = error_at(p, "E0215", collapsible->offset, collapsible->length,
                                         "`collapsible:` is `true` or `false`");
                diag_help(d, "bind its state instead: `open: state.options_open`");
                p->panic = false;
            }
            bool folding = open || (collapsible && collapsible->kind == EXPR_BOOL && collapsible->as.bool_value);
            if (folding && !value) {
                error_at(p, "E0215", node.offset, node.length, "a group that folds has a title");
                p->panic = false;
                folding = false;
            }
            if (folding) {
                Expr *state = ui_expr(p, EXPR_NAME, start, length);
                state->as.name = ui_name("none", start);
                if (open) state = ui_some(p, ui_address(p, open));
                Expr *group_args[4] = { value, gap, padding, state };
                StmtVec inner = {0};
                size_t body_start = cur(p)->offset;
                ui_parse_children(p, &inner);
                Stmt *when = new_stmt(p, STMT_IF, start);
                when->length = length;
                when->as.if_.cond = ui_frame_call(p, start, length, "group_folding", group_args, 4);
                when->as.if_.then_block = ui_block(p, &inner, body_start, span_from(p, body_start));
                SOSU_VEC_PUSH(out, when);
            } else {
                Expr *group_args[3] = { value, gap, padding };
                SOSU_VEC_PUSH(out, ui_stmt(p, value ? ui_frame_call(p, start, length, "group", group_args, 3)
                                                   : ui_frame_call(p, start, length, "group_untitled", group_args + 1, 2)));
                ui_parse_children(p, out);
            }
            SOSU_VEC_PUSH(out, ui_stmt(p, ui_frame_call(p, start, length, "end_group", NULL, 0)));
        } else if (name_text_is(node, "Grid")) {
            /* f.grid(gap, padding), f.grid_column(size)..., f.grid_row(size)..., the cells, f.end_grid() */
            Expr *layout[2] = { ui_layout_size(p, ui_prop(&args, "gap"), start),
                                ui_layout_size(p, ui_prop(&args, "padding"), start) };
            SOSU_VEC_PUSH(out, ui_stmt(p, ui_frame_call(p, start, length, "grid", layout, 2)));
            ui_grid_sizes(p, out, ui_prop(&args, "columns"), "grid_column");
            ui_grid_sizes(p, out, ui_prop(&args, "rows"), "grid_row");
            p->panic = false;
            VEC(uint64_t) cells = { p->ui_cells.items, p->ui_cells.count, p->ui_cells.cap };
            memset(&p->ui_cells, 0, sizeof p->ui_cells);
            p->ui_in_grid = true;
            ui_parse_children(p, out);
            free(p->ui_cells.items);
            p->ui_cells.items = cells.items;
            p->ui_cells.count = cells.count;
            p->ui_cells.cap = cells.cap;
            SOSU_VEC_PUSH(out, ui_stmt(p, ui_frame_call(p, start, length, "end_grid", NULL, 0)));
        }
    }
    if (tip && placed) SOSU_VEC_PUSH(out, ui_frame_stmt(p, tip->offset, tip->length, "tooltip", ui_translate(p, &args, tip)));
    p->ui_in_grid = in_grid;
    p->ui_in_tabs = in_tabs;
    p->ui_in_tree = in_tree;
    p->ui_in_component = in_component;
    free(args.values.items);
    free(args.props.items);
}

/* A node, an `if` or a `for` in a window, a component, a `Column` or a `Row`. */
static void ui_parse_item(Parser *p, StmtVec *out) {
    bool window_top = p->ui_window_top;
    p->ui_window_top = false;
    bool at_top = p->ui_at_top;
    p->ui_at_top = false;
    if (check(p, TOK_KW_IF)) {
        Stmt *stmt = new_stmt(p, STMT_IF, advance(p)->offset);
        stmt->as.if_.cond = parse_condition(p);
        StmtVec then_items = {0};
        size_t then_start = cur(p)->offset;
        ui_parse_children(p, &then_items);
        stmt->as.if_.then_block = ui_block(p, &then_items, then_start, span_from(p, then_start));
        if (accept(p, TOK_KW_ELSE)) {
            StmtVec else_items = {0};
            size_t else_start = cur(p)->offset;
            if (check(p, TOK_KW_IF)) ui_parse_item(p, &else_items);
            else ui_parse_children(p, &else_items);
            stmt->as.if_.else_stmt = ui_block(p, &else_items, else_start, span_from(p, else_start));
        }
        SOSU_VEC_PUSH(out, close_stmt(p, stmt));
        return;
    }
    if (check(p, TOK_KW_FOR)) {
        /* f.push_id(site), then the loop, each iteration in a scope: f.push_item() (its number) or
           f.push_id(key as ui64), its nodes, f.pop_id(); then f.pop_id() (spec §10.6) */
        size_t start = advance(p)->offset;
        Stmt *stmt = new_stmt(p, STMT_FOR_IN, start);
        expect(p, TOK_LPAREN);
        parse_binding(p, &stmt->as.for_in.is_mut, &stmt->as.for_in.type, &stmt->as.for_in.name);
        if (!accept(p, TOK_KW_IN)) report_expected(p, "E0200", "`in` (a .ui.sosu file loops over a range or a slice)", false);
        stmt->as.for_in.iterable = parse_expr(p);
        Expr *key = NULL;
        if (accept(p, TOK_COMMA)) {
            if (check(p, TOK_IDENT) && token_is(p, cur(p), "key") && kind_at(p, 1) == TOK_COLON) {
                advance(p);
                advance(p);
                key = parse_expr(p);
            } else {
                report_expected(p, "E0215", "`key:` (what names an item of the loop)", false);
            }
        }
        expect_closing(p, TOK_RPAREN);
        size_t length = span_from(p, start);
        StmtVec items = {0};
        if (key) {
            Expr *cast = ui_expr(p, EXPR_CAST, key->offset, key->length);
            cast->as.cast.value = key;
            cast->as.cast.type = ui_type(p, key->offset, NULL, "ui64");
            SOSU_VEC_PUSH(&items, ui_frame_stmt(p, key->offset, key->length, "push_id", cast));
        } else {
            SOSU_VEC_PUSH(&items, ui_frame_stmt(p, start, length, "push_item", NULL));
        }
        size_t body_start = cur(p)->offset;
        ui_parse_children(p, &items);
        SOSU_VEC_PUSH(&items, ui_frame_stmt(p, start, length, "pop_id", NULL));
        stmt->as.for_in.body = ui_block(p, &items, body_start, span_from(p, body_start));
        SOSU_VEC_PUSH(out, ui_frame_stmt(p, start, length, "push_id", ui_int(p, start, start)));
        SOSU_VEC_PUSH(out, close_stmt(p, stmt));
        SOSU_VEC_PUSH(out, ui_frame_stmt(p, start, length, "pop_id", NULL));
        return;
    }
    if (check(p, TOK_IDENT)) {
        p->ui_node_at_top = at_top;
        ui_parse_node(p, out, window_top);
        return;
    }
    const Token *token = cur(p);
    char found[64];
    describe_token(p, token, found, sizeof found);
    Diagnostic *d = error_at(p, "E0214", token->offset, token->length, "expected a node, `if` or `for`, found %s", found);
    diag_help(d, "a node is written `Type(arguments)`: `Text(\"Hello\")`, `Column(gap: 8) { ... }`");
}

static size_t line_of(const Parser *p, size_t offset) {
    size_t line, column;
    source_position(p->file, offset, &line, &column);
    return line;
}

/* After an error in an item: skip to the next line, where the next item starts. */
static void ui_sync(Parser *p, size_t line) {
    p->panic = false;
    while (!check(p, TOK_EOF) && !check(p, TOK_RBRACE) && line_of(p, cur(p)->offset) <= line) {
        if (check(p, TOK_LBRACE)) skip_balanced_braces(p);
        else advance(p);
    }
}

/* `{ items }` */
static void ui_parse_children(Parser *p, StmtVec *out) {
    if (!expect(p, TOK_LBRACE)) return;
    while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
        size_t before = p->pos;
        size_t line = line_of(p, cur(p)->offset);
        ui_parse_item(p, out);
        if (p->panic) ui_sync(p, line);
        if (p->pos == before) advance(p);
    }
    expect(p, TOK_RBRACE);
}

/* `name: value` in the body of a window. Returns false if it is not one of its properties. */
static bool ui_window_property(Parser *p, bool is_window, bool after_nodes, FieldInit *setting) {
    setting->name = make_name(p, advance(p));
    advance(p);
    setting->value = parse_expr(p);
    Name name = setting->name;
    if (!is_window) {
        Diagnostic *d = error_at(p, "E0215", name.offset, name.length, "a component has no properties");
        diag_help(d, "only a `window` has properties: title, width, height, icon");
        return false;
    }
    if (!name_text_is(name, "title") && !name_text_is(name, "width") && !name_text_is(name, "height") &&
        !name_text_is(name, "icon")) {
        Diagnostic *d = error_at(p, "E0215", name.offset, name.length, "a window has no property `%.*s`",
                                 (int)name.length, name.text);
        diag_help(d, "its properties are: title, width, height, icon");
        return false;
    }
    if (after_nodes) {
        error_at(p, "E0215", name.offset, name.length, "the properties of a window come before its nodes");
        return false;
    }
    return true;
}

/* `pub widgets.Window NameConfig() { return widgets.Window{ properties }; }` */
static Decl *ui_window_config(Parser *p, const Decl *window, FieldInit *settings, size_t count) {
    const FnDecl *fn = &window->as.fn;
    Decl *decl = arena_alloc(p->arena, sizeof *decl);
    *decl = *window;
    FnDecl *config = &decl->as.fn;
    char *name = arena_alloc(p->arena, fn->name.length + sizeof "Config");
    memcpy(name, fn->name.text, fn->name.length);
    memcpy(name + fn->name.length, "Config", sizeof "Config");
    config->name = ui_name(name, fn->name.offset);
    config->ret = ui_type(p, fn->offset, "widgets", "Window");
    config->params = NULL;
    config->param_count = 0;
    memset(&config->effects, 0, sizeof config->effects);

    Expr *literal = ui_expr(p, EXPR_STRUCT_LIT, fn->name.offset, fn->name.length);
    literal->as.struct_lit.type = ui_type(p, fn->name.offset, "widgets", "Window");
    literal->as.struct_lit.field_count = count;
    literal->as.struct_lit.fields = arena_alloc(p->arena, (count ? count : 1) * sizeof *settings);
    if (count) memcpy(literal->as.struct_lit.fields, settings, count * sizeof *settings);
    Stmt *ret = new_stmt(p, STMT_RETURN, fn->name.offset);
    ret->length = fn->name.length;
    ret->as.expr = literal;
    StmtVec body = {0};
    SOSU_VEC_PUSH(&body, ret);
    config->body = ui_block(p, &body, fn->body->offset, fn->body->length);
    return decl;
}

/* `window Name(params) [uses ...] { properties, then nodes }`, or `component ...` without properties. */
static void ui_parse_view(Parser *p, Decl ***decls, size_t *count, size_t *cap) {
    const Token *keyword = advance(p);
    bool is_window = token_is(p, keyword, "window");
    Decl *decl = arena_alloc(p->arena, sizeof *decl);
    decl->kind = DECL_FN;
    decl->is_pub = true;
    decl->offset = keyword->offset;
    FnDecl *fn = &decl->as.fn;
    fn->is_pub = true;
    fn->offset = keyword->offset;
    fn->ret = ui_type(p, keyword->offset, NULL, "void");
    expect_name(p, &fn->name, is_window ? "a window name" : "a component name");
    Param *params;
    size_t param_count;
    parse_params(p, &params, &param_count, false);
    if (accept(p, TOK_KW_USES)) fn->effects = parse_effects(p, false);
    /* the first parameter: `widgets.Frame* ui_frame` */
    fn->param_count = param_count + 1;
    fn->params = arena_alloc(p->arena, fn->param_count * sizeof *fn->params);
    fn->params[0].type = new_type(p, TYPE_POINTER, keyword->offset);
    fn->params[0].type->as.inner = ui_type(p, keyword->offset, "widgets", "Frame");
    fn->params[0].name = ui_name("ui_frame", keyword->offset);
    if (param_count) memcpy(fn->params + 1, params, param_count * sizeof *params);

    VEC(FieldInit) settings = {0};
    StmtVec items = {0};
    size_t nodes = 0;
    StmtVec dialogs = {0};
    p->ui_title_bar = false;
    size_t body_start = cur(p)->offset;
    p->ui_in_component = !is_window;
    p->ui_dialogs = &dialogs;
    if (expect(p, TOK_LBRACE)) {
        while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
            size_t before = p->pos;
            size_t line = line_of(p, cur(p)->offset);
            if (check(p, TOK_IDENT) && kind_at(p, 1) == TOK_COLON) {
                FieldInit setting;
                if (ui_window_property(p, is_window, items.count > 0, &setting)) SOSU_VEC_PUSH(&settings, setting);
            } else {
                p->ui_window_top = is_window && nodes == 0;
                p->ui_at_top = true;
                ui_parse_item(p, &items);
                nodes++;
            }
            if (p->panic) ui_sync(p, line);
            if (p->pos == before) advance(p);
        }
        expect(p, TOK_RBRACE);
    }
    p->ui_in_component = false;
    p->ui_dialogs = NULL;
    p->ui_at_top = false;
    for (size_t i = 0; i < dialogs.count; i++) SOSU_VEC_PUSH(&items, dialogs.items[i]);  /* above the rest */
    free(dialogs.items);
    fn->body = ui_block(p, &items, body_start, span_from(p, body_start));
    fn->length = span_from(p, fn->offset);
    decl->length = fn->length;
    if (is_window && p->ui_title_bar) {  /* `custom_title_bar: true`, for `platform` */
        FieldInit setting;
        setting.name = ui_name("custom_title_bar", fn->name.offset);
        setting.value = ui_expr(p, EXPR_BOOL, fn->name.offset, fn->name.length);
        setting.value->as.bool_value = true;
        SOSU_VEC_PUSH(&settings, setting);
    }

    VEC(Decl *) out = { *decls, *count, *cap };
    SOSU_VEC_PUSH(&out, decl);
    if (is_window) SOSU_VEC_PUSH(&out, ui_window_config(p, decl, settings.items, settings.count));
    *decls = out.items;
    *count = out.count;
    *cap = out.cap;
    free(settings.items);
}

/* ---- Themes and styles (spec §10.7) ---- */

static const char *const ui_style_props[] = { "color", "background", "border", "border_width", "radius",
                                              "padding", "gap", "height", "font_size", "accent", NULL };
#define UI_STYLE_PROPS "color, background, border, border_width, radius, padding, gap, height, font_size, accent"

/* The states and parts of a style: the fields of `widgets.Style` after `normal`. */
static const char *const ui_style_states[] = { "hover", "pressed", "focus", "disabled", "checked",
                                               "open", "placeholder", "selection", "bar", NULL };
#define UI_STYLE_STATES "hover, pressed, focus, disabled, checked, open, placeholder, selection, bar, title"
#define UI_STYLE_PARTS 10 /* `normal` and the states */

/* The selectors of a theme: a node, and its field in `widgets.Theme`. */
static const char *const ui_theme_selectors[][2] = {
    { "Window", "window" }, { "Text", "text" }, { "Title", "title" }, { "Button", "button" },
    { "TextField", "text_field" }, { "Checkbox", "checkbox" }, { "Radio", "radio" }, { "Dropdown", "dropdown" },
    { "List", "list" }, { "Scroll", "scroll" }, { "Column", "column" }, { "Row", "row" },
    { "TitleBar", "title_bar" }, { "MenuBar", "menu_bar" }, { "Menu", "menu" }, { "Image", "image" },
    { "Grid", "grid" }, { "Group", "group" }, { "Tabs", "tabs" }, { "Tree", "tree" },
    { "Slider", "slider" }, { "Progress", "progress" }, { "Tooltip", "tooltip" }, { "Dialog", "dialog" },
    { "DateField", "date" }, { "Table", "table" }, { "Item", "item" },
};
#define UI_SELECTORS (sizeof ui_theme_selectors / sizeof *ui_theme_selectors)
#define UI_SELECTOR_NAMES \
    "Window, Text, Title, Button, TextField, Checkbox, Radio, Dropdown, List, Scroll, Column, Row, TitleBar, MenuBar, Menu, " \
    "Image, Grid, Group, Tabs, Tree, Slider, Progress, Tooltip, Dialog, DateField, Table, Item"

/* The properties a theme gives every widget, in `all`: those that are inherited. */
static const char *const ui_theme_all[] = { "color", "font_size", "accent" };


static bool ui_color_property(Name name) {
    return name_text_is(name, "color") || name_text_is(name, "background") || name_text_is(name, "border") ||
           name_text_is(name, "accent");
}

/* `name(arg)`, or `name()` when `arg` is NULL */
static Expr *ui_call(Parser *p, Expr *callee, Expr *arg, size_t offset, size_t length) {
    Expr *call = ui_expr(p, EXPR_CALL, offset, length);
    call->as.call.callee = callee;
    call->as.call.arg_count = arg ? 1 : 0;
    call->as.call.args = arena_alloc(p->arena, sizeof(Expr *));
    call->as.call.args[0] = arg;
    return call;
}

static Expr *ui_some(Parser *p, Expr *value) {
    Expr *some = ui_expr(p, EXPR_NAME, value->offset, value->length);
    some->as.name = ui_name("some", value->offset);
    return ui_call(p, some, value, value->offset, value->length);
}

/* `widgets.type{ fields }` */
static Expr *ui_struct(Parser *p, const char *type, UiFields *fields, size_t offset, size_t length) {
    Expr *literal = ui_expr(p, EXPR_STRUCT_LIT, offset, length);
    literal->as.struct_lit.type = ui_type(p, offset, "widgets", type);
    literal->as.struct_lit.field_count = fields ? fields->count : 0;
    literal->as.struct_lit.fields = fields && fields->count ? FINISH(p, *fields) : arena_alloc(p->arena, sizeof(FieldInit));
    return literal;
}

static bool ui_field_given(Parser *p, const UiFields *fields, Name name) {
    for (size_t i = 0; i < fields->count; i++) {
        if (names_equal(fields->items[i].name, name)) {
            error_at(p, "E0215", name.offset, name.length, "`%.*s` is given twice", (int)name.length, name.text);
            return true;
        }
    }
    return false;
}

/* A value of a style or a theme: a color, `#RRGGBB`, or a number of pixels (`6` stands for `6.0`). */
static Expr *ui_style_value(Parser *p, bool color) {
    bool no_struct_literal = p->no_struct_literal;
    p->no_struct_literal = true;  /* `Button {` on the next line starts a selector */
    Expr *value = parse_expr(p);
    p->no_struct_literal = no_struct_literal;
    return color ? value : ui_float(p, value, value->offset, 0);
}

/* `name: value` in a style, as the field `name: some(value)` of `widgets.Props`. */
static void ui_parse_style_prop(Parser *p, UiFields *props) {
    Name name = make_name(p, advance(p));
    advance(p);  /* `:` */
    bool known = false;
    for (size_t i = 0; ui_style_props[i]; i++) known = known || name_text_is(name, ui_style_props[i]);
    if (!known) {
        Diagnostic *d = error_at(p, "E0215", name.offset, name.length, "a style has no property `%.*s`",
                                 (int)name.length, name.text);
        diag_help(d, "its properties are: " UI_STYLE_PROPS);
        return;
    }
    if (ui_field_given(p, props, name)) return;
    FieldInit field;
    field.name = name;
    field.value = ui_some(p, ui_style_value(p, ui_color_property(name)));
    SOSU_VEC_PUSH(props, field);
}

/* `{ properties, state { properties } ... }`: a `widgets.Style{ ... }`. */
static Expr *ui_parse_style_body(Parser *p, size_t offset, size_t length) {
    UiFields parts[UI_STYLE_PARTS];
    memset(parts, 0, sizeof parts);
    bool given[UI_STYLE_PARTS] = {0};
    if (expect(p, TOK_LBRACE)) {
        while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
            size_t before = p->pos;
            size_t line = line_of(p, cur(p)->offset);
            if (check(p, TOK_IDENT) && kind_at(p, 1) == TOK_COLON) {
                ui_parse_style_prop(p, &parts[0]);
            } else if (check(p, TOK_IDENT) && kind_at(p, 1) == TOK_LBRACE) {
                Name state = make_name(p, advance(p));
                size_t index = 0;
                for (size_t i = 0; ui_style_states[i]; i++) {
                    if (name_text_is(state, ui_style_states[i])) index = i + 1;
                }
                if (name_text_is(state, "title")) index = 7;  /* the title of a `Group`: the part `placeholder` */
                if (!index || given[index]) {
                    if (!index) {
                        Diagnostic *d = error_at(p, "E0215", state.offset, state.length,
                                                 "`%.*s` is not a state of a widget", (int)state.length, state.text);
                        diag_help(d, "the states and parts are: " UI_STYLE_STATES);
                    } else {
                        error_at(p, "E0215", state.offset, state.length, "`%.*s` is given twice", (int)state.length,
                                 state.text);
                    }
                    skip_balanced_braces(p);
                } else {
                    given[index] = true;
                    advance(p);  /* `{` */
                    while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
                        size_t inner = p->pos;
                        size_t inner_line = line_of(p, cur(p)->offset);
                        if (check(p, TOK_IDENT) && kind_at(p, 1) == TOK_COLON) {
                            ui_parse_style_prop(p, &parts[index]);
                        } else {
                            report_expected(p, "E0215", "a property, `name: value`", false);
                        }
                        accept(p, TOK_COMMA);
                        if (p->panic) ui_sync(p, inner_line);
                        if (p->pos == inner) advance(p);
                    }
                    expect(p, TOK_RBRACE);
                }
            } else {
                report_expected(p, "E0215", "a property, `name: value`, or a state, `hover { ... }`", false);
            }
            accept(p, TOK_COMMA);
            if (p->panic) ui_sync(p, line);
            if (p->pos == before) advance(p);
        }
        expect(p, TOK_RBRACE);
    }
    UiFields fields = {0};
    for (size_t i = 0; i < UI_STYLE_PARTS; i++) {
        if (!parts[i].count) continue;
        FieldInit field;
        field.name = ui_name(i ? ui_style_states[i - 1] : "normal", offset);
        field.value = ui_struct(p, "Props", &parts[i], offset, length);
        SOSU_VEC_PUSH(&fields, field);
    }
    return ui_struct(p, "Style", &fields, offset, length);
}

/* `pub widgets.type name() { body }` */
static Decl *ui_function(Parser *p, size_t offset, Name name, const char *type, StmtVec *body, size_t body_offset,
                         Param *params, size_t param_count) {
    Decl *decl = arena_alloc(p->arena, sizeof *decl);
    decl->kind = DECL_FN;
    decl->is_pub = true;
    decl->offset = offset;
    FnDecl *fn = &decl->as.fn;
    fn->is_pub = true;
    fn->offset = offset;
    fn->name = name;
    fn->ret = ui_type(p, offset, "widgets", type);
    fn->params = params;
    fn->param_count = param_count;
    fn->body = ui_block(p, body, body_offset, span_from(p, body_offset));
    fn->length = span_from(p, offset);
    decl->length = fn->length;
    return decl;
}

static Stmt *ui_return(Parser *p, Expr *value) {
    Stmt *ret = new_stmt(p, STMT_RETURN, value->offset);
    ret->length = value->length;
    ret->as.expr = value;
    return ret;
}

/* `style Name { ... }`: `pub widgets.Style Name() { return widgets.Style{ ... }; }` */
static Decl *ui_parse_style(Parser *p) {
    size_t offset = advance(p)->offset;
    Name name;
    expect_name(p, &name, "a style name");
    Param *params = NULL;  /* `style Size(f32 px)`: a style with parameters */
    size_t param_count = 0;
    if (check(p, TOK_LPAREN)) parse_params(p, &params, &param_count, false);
    size_t body_offset = cur(p)->offset;
    Expr *style = ui_parse_style_body(p, name.offset, name.length);
    StmtVec body = {0};
    SOSU_VEC_PUSH(&body, ui_return(p, style));
    return ui_function(p, offset, name, "Style", &body, body_offset, params, param_count);
}

/* A name of a theme, `accent: #89B4FA`; `own`: given by the body being read, not its parent. */
typedef struct {
    Name name;
    Expr *value;
    bool own;
} UiThemeName;

/* What a theme says, with what it inherits. */
typedef struct {
    VEC(UiThemeName) names;
    Expr *selectors[UI_SELECTORS]; /* `widgets.Style` values */
    bool own[UI_SELECTORS];
} UiThemeBody;

/* A theme of the file: its name, the place of its body, its parent (SIZE_MAX: none), and
   whether it was read without errors (a theme can start from it). */
typedef struct {
    Name name;
    size_t body;
    size_t parent;
    bool valid;
    Param *params;  /* `theme Brand(canvas.Color accent)`: its parameters */
    size_t param_count;
    ExprVec parent_args;  /* `: Brand(#D20F39)`: what it gives its parent */
} UiTheme;

typedef VEC(UiTheme) UiThemes;

/* The body of a theme at the current place, over `out`, which holds what it inherits. */
static void ui_parse_theme_body(Parser *p, UiThemeBody *out) {
    for (size_t i = 0; i < out->names.count; i++) out->names.items[i].own = false;
    memset(out->own, 0, sizeof out->own);
    if (!expect(p, TOK_LBRACE)) return;
    while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
        size_t before = p->pos;
        size_t line = line_of(p, cur(p)->offset);
        if (check(p, TOK_IDENT) && kind_at(p, 1) == TOK_COLON) {
            /* a name: a color or a number used below, `color`, `font_size` and `accent` for every widget */
            Name name = make_name(p, advance(p));
            advance(p);  /* `:` */
            Expr *value = ui_style_value(p, true);
            if (value->kind == EXPR_INT) value = ui_float(p, value, value->offset, 0);
            UiThemeName *found = NULL;
            for (size_t i = 0; i < out->names.count; i++) {
                if (names_equal(out->names.items[i].name, name)) found = &out->names.items[i];
            }
            if (found && found->own) {
                error_at(p, "E0215", name.offset, name.length, "`%.*s` is given twice", (int)name.length, name.text);
            } else if (found) {
                found->value = value;  /* the parent's, changed */
                found->own = true;
            } else {
                UiThemeName entry = { name, value, true };
                SOSU_VEC_PUSH(&out->names, entry);
            }
        } else if (check(p, TOK_IDENT) && kind_at(p, 1) == TOK_LBRACE) {
            Name node = make_name(p, cur(p));
            size_t index = UI_SELECTORS;
            for (size_t i = 0; i < UI_SELECTORS; i++) {
                if (name_text_is(node, ui_theme_selectors[i][0])) index = i;
            }
            if (index == UI_SELECTORS || out->own[index]) {
                if (index == UI_SELECTORS) {
                    Diagnostic *d = error_at(p, "E0215", node.offset, node.length, "a theme cannot style `%.*s`",
                                             (int)node.length, node.text);
                    diag_help(d, "its selectors are: " UI_SELECTOR_NAMES);
                } else {
                    error_at(p, "E0215", node.offset, node.length, "`%.*s` is given twice", (int)node.length, node.text);
                }
                advance(p);
                skip_balanced_braces(p);
            } else {
                advance(p);
                Expr *style = ui_parse_style_body(p, node.offset, node.length);
                Expr *inherited = out->selectors[index];
                if (inherited) {
                    /* `widgets.merge(parent, style)`: what the theme says, over what it inherits */
                    Expr *module = ui_expr(p, EXPR_NAME, node.offset, node.length);
                    module->as.name = ui_name("widgets", node.offset);
                    Expr *merge = ui_expr(p, EXPR_FIELD, node.offset, node.length);
                    merge->as.field.object = module;
                    merge->as.field.name = ui_name("merge", node.offset);
                    Expr *call = ui_call(p, merge, inherited, node.offset, node.length);
                    Expr **args = arena_alloc(p->arena, 2 * sizeof *args);
                    args[0] = inherited;
                    args[1] = style;
                    call->as.call.args = args;
                    call->as.call.arg_count = 2;
                    style = call;
                }
                out->selectors[index] = style;
                out->own[index] = true;
            }
        } else {
            report_expected(p, "E0215", "a name, `accent: #89B4FA`, or a selector, `Button { ... }`", false);
        }
        accept(p, TOK_COMMA);
        if (p->panic) ui_sync(p, line);
        if (p->pos == before) advance(p);
    }
    expect(p, TOK_RBRACE);
}

/* What the theme `index` says, with its parents: their bodies are read again, so that the
   names a theme changes apply to the selectors it inherits (`theme Light : Dark`). */
static void ui_inherit(Parser *p, const UiThemes *themes, size_t index, const ExprVec *args, UiThemeBody *out) {
    const UiTheme *theme = &themes->items[index];
    if (theme->parent != SIZE_MAX) ui_inherit(p, themes, theme->parent, &theme->parent_args, out);
    /* its parameters: names of the theme, given by the theme that starts from it */
    for (size_t i = 0; i < theme->param_count && i < args->count; i++) {
        UiThemeName entry = { theme->params[i].name, ui_float(p, args->items[i], args->items[i]->offset, 0), false };
        SOSU_VEC_PUSH(&out->names, entry);
    }
    size_t pos = p->pos;
    p->pos = theme->body;
    ui_parse_theme_body(p, out);
    p->pos = pos;
}

/* `theme Name [: Parent] { ... }`: `pub widgets.Theme Name() { auto name = value; ...
   return widgets.Theme{ all: ..., selector: style, ... }; }` */
static Decl *ui_parse_theme(Parser *p, UiThemes *themes) {
    size_t offset = advance(p)->offset;
    size_t errors = p->diags->error_count;
    Name name;
    expect_name(p, &name, "a theme name");
    Param *params = NULL;  /* `theme Brand(canvas.Color accent)` */
    size_t param_count = 0;
    if (check(p, TOK_LPAREN)) parse_params(p, &params, &param_count, false);
    UiThemeBody body;
    memset(&body, 0, sizeof body);
    size_t parent = SIZE_MAX;
    ExprVec parent_args = {0};
    if (accept(p, TOK_COLON)) {
        Name base;
        if (expect_name(p, &base, "the theme it starts from")) {
            if (accept(p, TOK_LPAREN)) {  /* `: Brand(#D20F39)` */
                if (!check(p, TOK_RPAREN)) {
                    do {
                        Expr *arg = parse_expr(p);
                        SOSU_VEC_PUSH(&parent_args, arg);
                    } while (accept(p, TOK_COMMA));
                }
                expect_closing(p, TOK_RPAREN);
            }
            for (size_t i = 0; i < themes->count; i++) {
                if (names_equal(themes->items[i].name, base)) parent = i;
            }
            if (parent == SIZE_MAX) {
                Diagnostic *d = error_at(p, "E0215", base.offset, base.length, "no theme `%.*s` before this one",
                                         (int)base.length, base.text);
                diag_help(d, "a theme starts from a theme written above it, in the same file");
                p->panic = false;
            } else if (themes->items[parent].param_count != parent_args.count) {
                Diagnostic *d = error_at(p, "E0215", base.offset, base.length, "`%.*s` takes %zu value%s, not %zu",
                                         (int)base.length, base.text, themes->items[parent].param_count,
                                         themes->items[parent].param_count == 1 ? "" : "s", parent_args.count);
                diag_help(d, "give the values of its parameters: `theme Red : %.*s(#D20F39) { }`", (int)base.length, base.text);
                p->panic = false;
            } else if (themes->items[parent].valid) {
                ui_inherit(p, themes, parent, &parent_args, &body);
            }
        }
    }
    UiTheme theme = { name, p->pos, parent, false, params, param_count, parent_args };
    size_t body_offset = cur(p)->offset;
    ui_parse_theme_body(p, &body);
    theme.valid = p->diags->error_count == errors;
    SOSU_VEC_PUSH(themes, theme);

    StmtVec items = {0};
    UiFields all = {0};
    for (size_t i = 0; i < body.names.count; i++) {
        UiThemeName entry = body.names.items[i];
        Stmt *var = new_stmt(p, STMT_VAR, entry.name.offset);
        var->length = entry.name.length;
        var->as.var.is_mut = false;
        var->as.var.type = NULL;  /* `auto` */
        var->as.var.name = entry.name;
        var->as.var.init = entry.value;
        SOSU_VEC_PUSH(&items, var);
        for (size_t j = 0; j < sizeof ui_theme_all / sizeof *ui_theme_all; j++) {
            if (!name_text_is(entry.name, ui_theme_all[j])) continue;
            Expr *use = ui_expr(p, EXPR_NAME, entry.name.offset, entry.name.length);
            use->as.name = entry.name;
            FieldInit field = { entry.name, ui_some(p, use) };
            SOSU_VEC_PUSH(&all, field);
        }
    }
    free(body.names.items);
    UiFields fields = {0};
    if (all.count) {
        FieldInit field = { ui_name("all", name.offset), ui_struct(p, "Props", &all, name.offset, name.length) };
        SOSU_VEC_PUSH(&fields, field);
    }
    for (size_t i = 0; i < UI_SELECTORS; i++) {
        if (!body.selectors[i]) continue;
        FieldInit field = { ui_name(ui_theme_selectors[i][1], name.offset), body.selectors[i] };
        SOSU_VEC_PUSH(&fields, field);
    }
    SOSU_VEC_PUSH(&items, ui_return(p, ui_struct(p, "Theme", &fields, name.offset, name.length)));
    return ui_function(p, offset, name, "Theme", &items, body_offset, params, param_count);
}

static bool ui_imports_widgets(const Decl *decl) {
    return decl->kind == DECL_IMPORT && decl->as.import.count == 2 && name_text_is(decl->as.import.items[0], "ui") &&
           name_text_is(decl->as.import.items[1], "widgets");
}

Module *parse_ui_module(const SourceFile *file, const TokenList *tokens, Arena *arena, DiagList *diags) {
    Parser p;
    init_parser(&p, file, tokens, arena, diags);
    VEC(Decl *) decls = {0};
    bool widgets = false;
    UiThemes themes = {0};  /* those of the file, which a theme can start from */
    while (!check(&p, TOK_EOF)) {
        size_t before = p.pos;
        const Token *token = cur(&p);
        if (check(&p, TOK_KW_IMPORT)) {
            Decl *decl = parse_declaration(&p);
            if (decl) {
                widgets = widgets || ui_imports_widgets(decl);
                SOSU_VEC_PUSH(&decls, decl);
            }
        } else if (token_is(&p, token, "window") || token_is(&p, token, "component")) {
            ui_parse_view(&p, &decls.items, &decls.count, &decls.cap);
        } else if (token_is(&p, token, "style") && kind_at(&p, 1) == TOK_IDENT) {
            SOSU_VEC_PUSH(&decls, ui_parse_style(&p));
            p.panic = false;
        } else if (token_is(&p, token, "theme") && kind_at(&p, 1) == TOK_IDENT) {
            SOSU_VEC_PUSH(&decls, ui_parse_theme(&p, &themes));
            p.panic = false;
        } else {
            char found[64];
            describe_token(&p, token, found, sizeof found);
            Diagnostic *d = error_at(&p, "E0203", token->offset, token->length, "expected a declaration, found %s", found);
            diag_help(d, "a .ui.sosu file contains `import`, `window`, `component`, `theme` and `style`");
            /* skip it whole: up to its block, included */
            while (!check(&p, TOK_EOF) && !check(&p, TOK_LBRACE) && !check(&p, TOK_KW_IMPORT)) advance(&p);
            if (check(&p, TOK_LBRACE)) skip_balanced_braces(&p);
            p.panic = false;
        }
        if (p.panic) sync_declaration(&p);
        if (p.pos == before) advance(&p);
    }
    if (!widgets) {
        /* the generated code uses `widgets.Frame`: `import ui.widgets;` is implied */
        Decl *import = arena_alloc(arena, sizeof *import);
        import->kind = DECL_IMPORT;
        Name names[2] = { ui_name("ui", 0), ui_name("widgets", 0) };
        import->as.import.count = 2;
        import->as.import.items = arena_copy(arena, names, sizeof names);
        SOSU_VEC_PUSH(&decls, import);
    }
    for (size_t i = 0; i < themes.count; i++) free(themes.items[i].parent_args.items);
    free(themes.items);
    Module *module = arena_alloc(arena, sizeof *module);
    module->count = decls.count;
    module->decls = finish_vec(&p, decls.items, decls.count, sizeof *decls.items);
    return module;
}
