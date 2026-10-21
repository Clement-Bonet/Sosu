#include "lexer.h"

#include <stdlib.h>
#include <string.h>

#include "utf8.h"

typedef struct {
    const SourceFile *file;
    const char *src;
    size_t len;
    size_t pos;
    TokenList *tokens;
    DiagList *diags;
} Lexer;

typedef struct {
    const char *text;
    TokenKind kind;
} Spelling;

static const Spelling keywords[] = {
#define X(name, text) { text, TOK_##name },
    SOSU_KEYWORDS(X)
#undef X
};

static const Spelling punctuation[] = {
#define X(name, text) { text, TOK_##name },
    SOSU_PUNCTUATION(X)
#undef X
};

static const char *const numeric_types[] = {
    "i8", "i16", "i32", "i64", "u8", "u16", "u32", "u64", "isize", "usize", "f32", "f64",
};

#define COUNT_OF(array) (sizeof(array) / sizeof((array)[0]))

/* ---- Character helpers ---- */

static bool is_digit(char c) { return c >= '0' && c <= '9'; }
static bool is_hex_digit(char c) { return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
static bool is_ident_start(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
static bool is_ident_char(char c) { return is_ident_start(c) || is_digit(c); }

static unsigned digit_value(char c) {
    if (is_digit(c)) return (unsigned)(c - '0');
    if (c >= 'a' && c <= 'f') return (unsigned)(c - 'a' + 10);
    return (unsigned)(c - 'A' + 10);
}

static bool at_end(const Lexer *lx) { return lx->pos >= lx->len; }

/* Character `ahead` bytes after the current position, or '\0' past the end. */
static char peek(const Lexer *lx, size_t ahead) {
    size_t i = lx->pos + ahead;
    return i < lx->len ? lx->src[i] : '\0';
}

static Token *emit(Lexer *lx, TokenKind kind, size_t start) {
    Token token = {0};
    token.kind = kind;
    token.offset = start;
    token.length = lx->pos - start;
    SOSU_VEC_PUSH(lx->tokens, token);
    return &lx->tokens->items[lx->tokens->count - 1];
}

/* Advances over one code point, reporting E0100 if the bytes are not valid UTF-8.
   Returns the code point, or -1 if invalid. */
static int32_t advance_char(Lexer *lx) {
    int32_t cp;
    size_t n = utf8_decode(lx->src + lx->pos, lx->len - lx->pos, &cp);
    if (cp < 0) {
        Diagnostic *d = diag_error(lx->diags, "E0100", lx->file, lx->pos, n, "invalid UTF-8 byte sequence");
        diag_help(d, "source files must be encoded in UTF-8");
    }
    lx->pos += n;
    return cp;
}

/* ---- Whitespace and comments ---- */

static void skip_trivia(Lexer *lx) {
    while (!at_end(lx)) {
        char c = lx->src[lx->pos];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            lx->pos++;
        } else if (c == '/' && peek(lx, 1) == '/') {
            lx->pos += 2;
            while (!at_end(lx) && lx->src[lx->pos] != '\n') advance_char(lx);
        } else if (c == '/' && peek(lx, 1) == '*') {
            size_t start = lx->pos;
            lx->pos += 2;
            for (;;) {
                if (at_end(lx)) {
                    Diagnostic *d = diag_error(lx->diags, "E0102", lx->file, start, 2, "unterminated block comment");
                    diag_help(d, "close the comment with `*/` (block comments do not nest)");
                    return;
                }
                if (lx->src[lx->pos] == '*' && peek(lx, 1) == '/') {
                    lx->pos += 2;
                    break;
                }
                advance_char(lx);
            }
        } else {
            return;
        }
    }
}

/* ---- Identifiers and keywords ---- */

static void lex_identifier(Lexer *lx) {
    size_t start = lx->pos;
    while (!at_end(lx) && is_ident_char(lx->src[lx->pos])) lx->pos++;
    size_t length = lx->pos - start;

    TokenKind kind = TOK_IDENT;
    for (size_t i = 0; i < COUNT_OF(keywords); i++) {
        if (strlen(keywords[i].text) == length && memcmp(keywords[i].text, lx->src + start, length) == 0) {
            kind = keywords[i].kind;
            break;
        }
    }
    if (kind == TOK_KW_COMPTIME) {
        Diagnostic *d = diag_error(lx->diags, "E0112", lx->file, start, length,
                                   "`comptime` is reserved for a future version of Sosu");
        diag_help(d, "choose another name");
    }
    emit(lx, kind, start);
}

/* ---- Numbers ---- */

static bool is_numeric_type(const char *text, size_t length) {
    for (size_t i = 0; i < COUNT_OF(numeric_types); i++) {
        if (strlen(numeric_types[i]) == length && memcmp(numeric_types[i], text, length) == 0) return true;
    }
    return false;
}

static void skip_ident_chars(Lexer *lx) {
    while (!at_end(lx) && is_ident_char(lx->src[lx->pos])) lx->pos++;
}

/* Reports letters or digits glued to a number literal (`12abc`, `0b102`, `1.5f32`).
   Returns true if there were any. */
static bool reject_number_suffix(Lexer *lx, size_t start, const char *base_name) {
    if (at_end(lx) || !is_ident_char(lx->src[lx->pos])) return false;
    size_t suffix = lx->pos;
    skip_ident_chars(lx);
    size_t length = lx->pos - suffix;
    const char *text = lx->src + suffix;

    if (is_digit(text[0])) {
        diag_error(lx->diags, "E0108", lx->file, suffix, 1, "invalid digit `%c` in %s literal", text[0], base_name);
        return true;
    }
    Diagnostic *d = diag_error(lx->diags, "E0108", lx->file, suffix, length,
                               "invalid suffix `%.*s` on number literal", (int)length, text);
    if (is_numeric_type(text, length)) {
        diag_help(d, "conversions are explicit: write `%.*s as %.*s`",
                  (int)(suffix - start), lx->src + start, (int)length, text);
    }
    return true;
}

static void finish_integer(Lexer *lx, size_t start, uint64_t value, bool overflow, bool invalid,
                           const char *base_name) {
    if (reject_number_suffix(lx, start, base_name)) {
        invalid = true;
    } else if (overflow) {
        Diagnostic *d = diag_error(lx->diags, "E0109", lx->file, start, lx->pos - start, "integer literal is too large");
        diag_help(d, "the largest integer literal is 18446744073709551615 (the maximum of `u64`)");
        invalid = true;
    }
    Token *token = emit(lx, invalid ? TOK_ERROR : TOK_INT, start);
    token->int_value = value;
}

static void lex_prefixed_integer(Lexer *lx) {
    size_t start = lx->pos;
    unsigned base = peek(lx, 1) == 'x' ? 16 : 2;
    const char *base_name = base == 16 ? "hexadecimal" : "binary";
    lx->pos += 2;

    size_t digits = lx->pos;
    uint64_t value = 0;
    bool overflow = false;
    while (!at_end(lx)) {
        char c = lx->src[lx->pos];
        if (base == 16 ? !is_hex_digit(c) : (c != '0' && c != '1')) break;
        unsigned d = digit_value(c);
        if (value > (UINT64_MAX - d) / base) overflow = true;
        else value = value * base + d;
        lx->pos++;
    }
    if (lx->pos == digits) {
        diag_error(lx->diags, "E0108", lx->file, start, 2, "expected %s digits after `%.2s`",
                   base_name, lx->src + start);
        skip_ident_chars(lx);
        emit(lx, TOK_ERROR, start);
        return;
    }
    finish_integer(lx, start, value, overflow, false, base_name);
}

static void lex_number(Lexer *lx) {
    size_t start = lx->pos;
    if (lx->src[start] == '0' && (peek(lx, 1) == 'x' || peek(lx, 1) == 'b')) {
        lex_prefixed_integer(lx);
        return;
    }

    uint64_t value = 0;
    bool overflow = false;
    while (!at_end(lx) && is_digit(lx->src[lx->pos])) {
        unsigned d = digit_value(lx->src[lx->pos]);
        if (value > (UINT64_MAX - d) / 10) overflow = true;
        else value = value * 10 + d;
        lx->pos++;
    }
    size_t int_end = lx->pos;

    /* `0..n` is a range and `3.abs()` a method call: a fraction needs a digit after the dot. */
    bool is_float = false;
    if (peek(lx, 0) == '.' && is_digit(peek(lx, 1))) {
        lx->pos++;
        while (!at_end(lx) && is_digit(lx->src[lx->pos])) lx->pos++;
        is_float = true;
    }
    if (peek(lx, 0) == 'e' &&
        (is_digit(peek(lx, 1)) || ((peek(lx, 1) == '+' || peek(lx, 1) == '-') && is_digit(peek(lx, 2))))) {
        lx->pos += 2;
        while (!at_end(lx) && is_digit(lx->src[lx->pos])) lx->pos++;
        is_float = true;
    }

    /* A leading zero means octal in C: forbid it rather than guess. */
    bool invalid = false;
    if (lx->src[start] == '0' && int_end - start > 1) {
        size_t first = start;
        while (first < int_end - 1 && lx->src[first] == '0') first++;
        Diagnostic *d = diag_error(lx->diags, "E0114", lx->file, start, int_end - start,
                                   "leading zeros are not allowed in number literals");
        diag_help(d, "write `%.*s` (there are no octal literals)", (int)(int_end - first), lx->src + first);
        invalid = true;
    }

    if (is_float) {
        if (reject_number_suffix(lx, start, "decimal")) invalid = true;
        emit(lx, invalid ? TOK_ERROR : TOK_FLOAT, start);
        return;
    }
    finish_integer(lx, start, value, overflow, invalid, "decimal");
}

/* ---- Strings and characters ---- */

static bool lex_hex_escape(Lexer *lx, size_t start, uint32_t *out) {
    lx->pos++; /* x */
    if (!is_hex_digit(peek(lx, 0)) || !is_hex_digit(peek(lx, 1))) {
        diag_error(lx->diags, "E0107", lx->file, start, lx->pos - start, "expected two hexadecimal digits after `\\x`");
        return false;
    }
    uint32_t value = digit_value(peek(lx, 0)) * 16 + digit_value(peek(lx, 1));
    lx->pos += 2;
    if (value > 0x7F) {
        Diagnostic *d = diag_error(lx->diags, "E0107", lx->file, start, lx->pos - start,
                                   "`\\x` escapes are limited to ASCII (`\\x00` to `\\x7F`)");
        diag_help(d, "text is UTF-8: write `\\u{%X}` for the code point U+%04X", value, value);
        return false;
    }
    *out = value;
    return true;
}

static bool lex_unicode_escape(Lexer *lx, size_t start, uint32_t *out) {
    lx->pos++; /* u */
    if (peek(lx, 0) != '{') {
        Diagnostic *d = diag_error(lx->diags, "E0113", lx->file, start, lx->pos - start, "expected `{` after `\\u`");
        diag_help(d, "write the code point in hexadecimal between braces, e.g. `\\u{E9}`");
        return false;
    }
    lx->pos++;
    uint32_t value = 0;
    size_t digits = 0;
    while (is_hex_digit(peek(lx, 0))) {
        if (digits < 8) value = value * 16 + digit_value(peek(lx, 0));
        digits++;
        lx->pos++;
    }
    bool closed = peek(lx, 0) == '}';
    if (closed) lx->pos++;
    if (!closed || digits == 0 || digits > 6) {
        Diagnostic *d = diag_error(lx->diags, "E0113", lx->file, start, lx->pos - start, "malformed unicode escape");
        diag_help(d, "write 1 to 6 hexadecimal digits between braces, e.g. `\\u{E9}`");
        return false;
    }
    if (value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF)) {
        Diagnostic *d = diag_error(lx->diags, "E0113", lx->file, start, lx->pos - start,
                                   "`\\u{%X}` is not a valid Unicode scalar value", value);
        diag_help(d, "valid code points are 0 to D7FF and E000 to 10FFFF");
        return false;
    }
    *out = value;
    return true;
}

/* Lexes the escape sequence starting at the backslash. Returns false if it is invalid
   (already reported), or cut by a line end (reported by the caller as unterminated). */
static bool lex_escape(Lexer *lx, uint32_t *out) {
    size_t start = lx->pos++;
    *out = 0;
    if (at_end(lx) || lx->src[lx->pos] == '\n') return false;

    char c = lx->src[lx->pos];
    switch (c) {
    case 'n': *out = '\n'; break;
    case 't': *out = '\t'; break;
    case 'r': *out = '\r'; break;
    case '0': *out = 0; break;
    case '\\': *out = '\\'; break;
    case '\'': *out = '\''; break;
    case '"': *out = '"'; break;
    case 'x': return lex_hex_escape(lx, start, out);
    case 'u': return lex_unicode_escape(lx, start, out);
    default: {
        if ((unsigned char)c < 0x80) lx->pos++;
        else if (advance_char(lx) < 0) return false;
        Diagnostic *d = diag_error(lx->diags, "E0107", lx->file, start, lx->pos - start,
                                   "unknown escape sequence `%.*s`", (int)(lx->pos - start), lx->src + start);
        diag_help(d, "valid escapes are \\n \\t \\r \\0 \\\\ \\' \\\" \\xNN (ASCII) and \\u{NNNN}");
        return false;
    }
    }
    lx->pos++;
    return true;
}

static void lex_string(Lexer *lx) {
    size_t start = lx->pos++;
    bool valid = true;
    for (;;) {
        if (at_end(lx) || lx->src[lx->pos] == '\n') {
            Diagnostic *d = diag_error(lx->diags, "E0103", lx->file, start, 1, "unterminated string literal");
            diag_help(d, "close the string with `\"` on the same line; write `\\n` for a line break");
            emit(lx, TOK_ERROR, start);
            return;
        }
        char c = lx->src[lx->pos];
        if (c == '"') {
            lx->pos++;
            break;
        }
        if (c == '\\') {
            uint32_t ignored;
            if (!lex_escape(lx, &ignored)) valid = false;
        } else if (advance_char(lx) < 0) {
            valid = false;
        }
    }
    emit(lx, valid ? TOK_STRING : TOK_ERROR, start);
}

static void lex_char(Lexer *lx) {
    size_t start = lx->pos++;
    if (peek(lx, 0) == '\'') {
        lx->pos++;
        Diagnostic *d = diag_error(lx->diags, "E0105", lx->file, start, 2, "empty character literal");
        diag_help(d, "a character literal holds exactly one code point, e.g. `'a'`");
        emit(lx, TOK_ERROR, start);
        return;
    }

    bool valid = true;
    uint32_t value = 0;
    if (!at_end(lx) && lx->src[lx->pos] == '\\') {
        valid = lex_escape(lx, &value);
    } else if (!at_end(lx) && lx->src[lx->pos] != '\n') {
        int32_t cp = advance_char(lx);
        if (cp < 0) valid = false;
        else value = (uint32_t)cp;
    }

    if (peek(lx, 0) == '\'') {
        lx->pos++;
        Token *token = emit(lx, valid ? TOK_CHAR : TOK_ERROR, start);
        token->char_value = value;
        return;
    }

    size_t scan = lx->pos;
    while (scan < lx->len && lx->src[scan] != '\'' && lx->src[scan] != '\n') scan++;
    if (scan < lx->len && lx->src[scan] == '\'') {
        lx->pos = scan + 1;
        Diagnostic *d = diag_error(lx->diags, "E0106", lx->file, start, lx->pos - start,
                                   "character literal contains more than one character");
        diag_help(d, "use a string for text: `\"%.*s\"`", (int)(lx->pos - start - 2), lx->src + start + 1);
    } else {
        Diagnostic *d = diag_error(lx->diags, "E0104", lx->file, start, 1, "unterminated character literal");
        diag_help(d, "close the character with `'`");
    }
    emit(lx, TOK_ERROR, start);
}

/* ---- Symbols ---- */

static void lex_unexpected(Lexer *lx) {
    size_t start = lx->pos;
    unsigned char c = (unsigned char)lx->src[start];
    if (c >= 0x80) {
        int32_t cp = advance_char(lx); /* reports invalid UTF-8 itself */
        if (cp >= 0) {
            Diagnostic *d = diag_error(lx->diags, "E0101", lx->file, start, lx->pos - start,
                                       "unexpected character `%.*s` (U+%04X)",
                                       (int)(lx->pos - start), lx->src + start, (unsigned)cp);
            diag_help(d, "identifiers are ASCII; other characters may only appear in strings, characters and comments");
        }
    } else {
        lx->pos++;
        if (c < 0x20 || c == 0x7F) {
            diag_error(lx->diags, "E0101", lx->file, start, 1, "unexpected control character U+%04X", (unsigned)c);
        } else {
            Diagnostic *d = diag_error(lx->diags, "E0101", lx->file, start, 1, "unexpected character `%c`", c);
            if (c == '#') diag_help(d, "Sosu has no preprocessor: use `import` for modules");
        }
    }
    emit(lx, TOK_ERROR, start);
}

static void lex_symbol(Lexer *lx) {
    size_t start = lx->pos;
    const char *s = lx->src + start;
    size_t rest = lx->len - start;

    if (rest >= 2 && (s[0] == '+' || s[0] == '-') && s[1] == s[0]) {
        lx->pos += 2;
        Diagnostic *d = diag_error(lx->diags, "E0110", lx->file, start, 2, "`%.2s` does not exist in Sosu", s);
        diag_help(d, "write `x %c= 1` as its own statement", s[0]);
        emit(lx, TOK_ERROR, start);
        return;
    }
    if (rest >= 2 && s[0] == ':' && s[1] == ':') {
        lx->pos += 2;
        Diagnostic *d = diag_error(lx->diags, "E0111", lx->file, start, 2, "`::` does not exist in Sosu");
        diag_help(d, "use `.` for modules and members, e.g. `std.io`");
        emit(lx, TOK_ERROR, start);
        return;
    }

    /* Longest match: `<<=` before `<<` before `<`. */
    size_t best_length = 0;
    TokenKind best = TOK_ERROR;
    for (size_t i = 0; i < COUNT_OF(punctuation); i++) {
        size_t n = strlen(punctuation[i].text);
        if (n > best_length && n <= rest && memcmp(punctuation[i].text, s, n) == 0) {
            best_length = n;
            best = punctuation[i].kind;
        }
    }
    if (best_length == 0) {
        lex_unexpected(lx);
        return;
    }
    lx->pos += best_length;
    emit(lx, best, start);
}

/* ---- Entry point ---- */

bool lex_file(const SourceFile *file, TokenList *tokens, DiagList *diags) {
    Lexer lx = {0};
    lx.file = file;
    lx.src = file->text;
    lx.len = file->length;
    lx.tokens = tokens;
    lx.diags = diags;
    size_t errors_before = diags->error_count;

    if (lx.len >= 3 && memcmp(lx.src, "\xEF\xBB\xBF", 3) == 0) lx.pos = 3;

    for (;;) {
        skip_trivia(&lx);
        if (at_end(&lx)) break;
        char c = lx.src[lx.pos];
        if (is_ident_start(c)) lex_identifier(&lx);
        else if (is_digit(c)) lex_number(&lx);
        else if (c == '"') lex_string(&lx);
        else if (c == '\'') lex_char(&lx);
        else lex_symbol(&lx);
    }
    lx.pos = lx.len;
    emit(&lx, TOK_EOF, lx.len);
    return diags->error_count == errors_before;
}

void token_list_free(TokenList *tokens) {
    free(tokens->items);
    memset(tokens, 0, sizeof *tokens);
}
