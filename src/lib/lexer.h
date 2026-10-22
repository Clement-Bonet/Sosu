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

typedef struct {
    size_t offset; /* span of the comment, `//` or `/*` included */
    size_t length;
    bool own_line; /* nothing but spaces before it on its line */
} Comment;

typedef struct {
    Comment *items;
    size_t count;
    size_t cap;
} CommentList;

/* lex_file that also records the comments, in order (used by the formatter). */
bool lex_file_comments(const SourceFile *file, TokenList *tokens, CommentList *comments, DiagList *diags);

#endif
