#ifndef SOSU_PARSER_H
#define SOSU_PARSER_H

#include "arena.h"
#include "ast.h"
#include "diag.h"
#include "lexer.h"

/* Parses the tokens of a file (from lex_file) into an AST allocated in `arena`.
   Errors are reported to `diags`; the parser recovers and always returns a module. */
Module *parse_module(const SourceFile *file, const TokenList *tokens, Arena *arena, DiagList *diags);

/* Parses a `.ui.sosu` file (spec §10.2): each `window` and `component` becomes a function
   that describes it with the widgets of `ui.widgets`, in the same AST as a `.sosu` file. */
Module *parse_ui_module(const SourceFile *file, const TokenList *tokens, Arena *arena, DiagList *diags);

/* Parses a single expression that must span all the tokens (tests and tools). */
Expr *parse_expression(const SourceFile *file, const TokenList *tokens, Arena *arena, DiagList *diags);

#endif
