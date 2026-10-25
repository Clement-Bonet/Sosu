#ifndef SOSU_TOKEN_H
#define SOSU_TOKEN_H

#include "common.h"

/* Keywords (spec §3). `comptime` is reserved: the lexer reports it. `result` is not a
   keyword: it names the returned value only in `ensures` (see sema_body.c). */
#define SOSU_KEYWORDS(X)                                                          \
    X(KW_STRUCT, "struct") X(KW_CLASS, "class") X(KW_INTERFACE, "interface")       \
    X(KW_ENUM, "enum") X(KW_IMPORT, "import") X(KW_EXTERN, "extern")               \
    X(KW_PUB, "pub") X(KW_RETURN, "return") X(KW_IF, "if") X(KW_ELSE, "else")      \
    X(KW_WHILE, "while") X(KW_FOR, "for") X(KW_IN, "in") X(KW_BREAK, "break")      \
    X(KW_CONTINUE, "continue") X(KW_DEFER, "defer") X(KW_MUT, "mut")               \
    X(KW_AUTO, "auto") X(KW_TRUE, "true") X(KW_FALSE, "false") X(KW_AS, "as")      \
    X(KW_MATCH, "match") X(KW_TRY, "try") X(KW_REQUIRES, "requires")              \
    X(KW_ENSURES, "ensures") X(KW_TEST, "test") X(KW_USES, "uses")                 \
    X(KW_UNSAFE, "unsafe") X(KW_VIRTUAL, "virtual")                                \
    X(KW_OVERRIDE, "override") X(KW_SELF, "self") X(KW_BASE, "base")               \
    X(KW_COMPTIME, "comptime")

/* Symbols (spec §5.1). `++`, `--` and `::` are rejected by the lexer. */
#define SOSU_PUNCTUATION(X)                                                       \
    X(LPAREN, "(") X(RPAREN, ")") X(LBRACE, "{") X(RBRACE, "}")                    \
    X(LBRACKET, "[") X(RBRACKET, "]") X(COMMA, ",") X(SEMICOLON, ";")              \
    X(COLON, ":") X(DOT, ".") X(DOT_DOT, "..") X(DOT_DOT_EQ, "..=")                \
    X(QUESTION, "?") X(FAT_ARROW, "=>") X(ARROW, "->")                             \
    X(PLUS, "+") X(MINUS, "-") X(STAR, "*") X(SLASH, "/") X(PERCENT, "%")          \
    X(AMP, "&") X(PIPE, "|") X(CARET, "^") X(TILDE, "~") X(BANG, "!")              \
    X(SHL, "<<") X(SHR, ">>") X(AMP_AMP, "&&") X(PIPE_PIPE, "||")                  \
    X(EQ_EQ, "==") X(BANG_EQ, "!=") X(LT, "<") X(LT_EQ, "<=") X(GT, ">")           \
    X(GT_EQ, ">=") X(EQ, "=") X(PLUS_EQ, "+=") X(MINUS_EQ, "-=")                   \
    X(STAR_EQ, "*=") X(SLASH_EQ, "/=") X(PERCENT_EQ, "%=") X(AMP_EQ, "&=")         \
    X(PIPE_EQ, "|=") X(CARET_EQ, "^=") X(SHL_EQ, "<<=") X(SHR_EQ, ">>=")

typedef enum {
    TOK_EOF,
    TOK_ERROR, /* invalid input, already reported */
    TOK_IDENT,
    TOK_INT,
    TOK_FLOAT,
    TOK_STRING,
    TOK_CHAR,
    TOK_COLOR, /* #RRGGBB or #RRGGBBAA, in .ui.sosu files only: int_value is 0xRRGGBBAA */
#define X(name, text) TOK_##name,
    SOSU_KEYWORDS(X)
    SOSU_PUNCTUATION(X)
#undef X
    TOK_COUNT
} TokenKind;

typedef struct {
    TokenKind kind;
    size_t offset; /* byte span in the source file */
    size_t length;
    uint64_t int_value;  /* TOK_INT */
    uint32_t char_value; /* TOK_CHAR: code point */
} Token;

/* Enum-style name, e.g. "KW_STRUCT" (for dumps). */
const char *token_kind_name(TokenKind kind);
/* Human-readable form for messages, e.g. "`struct`" or "identifier". */
const char *token_kind_describe(TokenKind kind);
/* Source text of a keyword or symbol, e.g. "<<="; NULL for other kinds. */
const char *token_kind_spelling(TokenKind kind);

#endif
