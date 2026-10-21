#include "token.h"

static const char *const names[TOK_COUNT] = {
    "EOF", "ERROR", "IDENT", "INT", "FLOAT", "STRING", "CHAR",
#define X(name, text) #name,
    SOSU_KEYWORDS(X)
    SOSU_PUNCTUATION(X)
#undef X
};

static const char *const descriptions[TOK_COUNT] = {
    "end of file", "invalid token", "identifier", "integer literal",
    "float literal", "string literal", "character literal",
#define X(name, text) "`" text "`",
    SOSU_KEYWORDS(X)
    SOSU_PUNCTUATION(X)
#undef X
};

static const char *const spellings[TOK_COUNT] = {
    NULL, NULL, NULL, NULL, NULL, NULL, NULL,
#define X(name, text) text,
    SOSU_KEYWORDS(X)
    SOSU_PUNCTUATION(X)
#undef X
};

const char *token_kind_spelling(TokenKind kind) {
    return kind < TOK_COUNT ? spellings[kind] : NULL;
}

const char *token_kind_name(TokenKind kind) {
    return kind < TOK_COUNT ? names[kind] : "?";
}

const char *token_kind_describe(TokenKind kind) {
    return kind < TOK_COUNT ? descriptions[kind] : "?";
}
