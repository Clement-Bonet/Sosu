#ifndef SOSU_LEXER_H
#define SOSU_LEXER_H

#include "diag.h"
#include "source.h"
#include "token.h"

typedef struct {
    Token *items;
    size_t count;
    size_t cap;
} TokenList;

/* Splits a source file into tokens, always ending with TOK_EOF. Invalid input is
   reported to `diags` and becomes a TOK_ERROR token, so lexing never stops early.
   Returns true if no error was reported. */
bool lex_file(const SourceFile *file, TokenList *tokens, DiagList *diags);
void token_list_free(TokenList *tokens);

#endif
