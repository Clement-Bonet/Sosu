#ifndef SOSU_PARSER_INTERNAL_H
#define SOSU_PARSER_INTERNAL_H

/* Shared by the two parsers: parser.c (.sosu files) and ui_parser.c (.ui.sosu files, spec §10.2),
   which uses the helpers of the first one (tokens, errors, expressions, declarations). */

#include "parser.h"

typedef struct {
    const SourceFile *file;
    const Token *tokens;
    size_t count;
    size_t pos;
    Arena *arena;
    DiagList *diags;
    bool panic;
    /* The height of the tree being built, and whether it went beyond MAX_NESTING: the passes
       after the parser walk the tree by recursion, so a deeper one would exhaust the stack
       (spec §12). Then the rest of the file is skipped, without other errors. */
    size_t depth;
    bool too_deep;
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
void *finish_vec(Parser *p, void *items, size_t count, size_t size);

#define FINISH(p, vec) finish_vec((p), (vec).items, (vec).count, sizeof *(vec).items)

void init_parser(Parser *p, const SourceFile *file, const TokenList *tokens, Arena *arena, DiagList *diags);

/* ---- Tokens ---- */

const Token *cur(const Parser *p);
TokenKind kind_at(const Parser *p, size_t ahead);
bool check(const Parser *p, TokenKind kind);
const Token *advance(Parser *p);
bool parser_accept(Parser *p, TokenKind kind);
bool token_is(const Parser *p, const Token *token, const char *text);
Name make_name(const Parser *p, const Token *token);
size_t span_from(const Parser *p, size_t start);

/* ---- Diagnostics ---- */

Diagnostic *error_at(Parser *p, const char *code, size_t offset, size_t length, const char *fmt, ...);
void describe_token(const Parser *p, const Token *token, char *buffer, size_t size);
void report_expected(Parser *p, const char *code, const char *what, bool after_expression);
bool expect(Parser *p, TokenKind kind);
bool expect_name(Parser *p, Name *out, const char *what);
/* Expects a closing bracket. On error, skips to the matching one. */
void expect_closing(Parser *p, TokenKind closer);
void skip_balanced_braces(Parser *p);
/* After an error in a declaration header: skip to the next declaration. */
void sync_declaration(Parser *p);

/* ---- Nodes ---- */

/* One level more in the tree; false when there are too many (spec §12). */
bool deeper(Parser *p);
Path parse_path(Parser *p);
Path parse_effects(Parser *p, bool in_type);
TypeExpr *new_type(Parser *p, TypeKind kind, size_t start);
Expr *new_expr(Parser *p, ExprKind kind, size_t start);
Expr *parse_expr(Parser *p);
Expr *parse_condition(Parser *p);
Stmt *new_stmt(Parser *p, StmtKind kind, size_t start);
Stmt *close_stmt(Parser *p, Stmt *stmt);
void parse_binding(Parser *p, bool *is_mut, TypeExpr **type, Name *name);
void parse_params(Parser *p, Param **out, size_t *count, bool allow_mut);
Decl *parse_declaration(Parser *p);

#endif
