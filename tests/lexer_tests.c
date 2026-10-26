#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lexer.h"
#include "test.h"

typedef struct {
    SourceFile file;
    TokenList tokens;
    DiagList diags;
} Lexed;

static void lex_text(Lexed *out, const char *text) {
    memset(out, 0, sizeof *out);
    source_from_string(&out->file, "test.sosu", text);
    lex_file(&out->file, &out->tokens, &out->diags);
}

static void lexed_free(Lexed *lexed) {
    diag_free(&lexed->diags);
    token_list_free(&lexed->tokens);
    source_free(&lexed->file);
}

/* Checks the token kinds of `text`; `kinds` ends with TOK_EOF. */
static void expect_kinds(const char *text, const TokenKind *kinds) {
    Lexed lx;
    lex_text(&lx, text);
    CHECK(lx.diags.count == 0, "`%s`: unexpected diagnostic %s: %s", text,
          lx.diags.count ? lx.diags.items[0].code : "", lx.diags.count ? lx.diags.items[0].message : "");
    size_t i = 0;
    for (;; i++) {
        TokenKind actual = i < lx.tokens.count ? lx.tokens.items[i].kind : TOK_COUNT;
        CHECK(actual == kinds[i], "`%s`: token %zu is %s, expected %s", text, i,
              token_kind_name(actual), token_kind_name(kinds[i]));
        if (actual != kinds[i] || kinds[i] == TOK_EOF) break;
    }
    lexed_free(&lx);
}

/* Checks that `text` produces the error `code` as its first diagnostic, at line:column. */
static void expect_error(const char *text, const char *code, size_t line, size_t column) {
    Lexed lx;
    lex_text(&lx, text);
    CHECK(lx.diags.count > 0, "`%s`: expected %s, got no diagnostic", text, code);
    if (lx.diags.count > 0) {
        const Diagnostic *d = &lx.diags.items[0];
        size_t l, c;
        source_position(&lx.file, d->offset, &l, &c);
        CHECK(strcmp(d->code, code) == 0, "`%s`: expected %s, got %s (%s)", text, code, d->code, d->message);
        CHECK(l == line && c == column, "`%s`: expected %s at %zu:%zu, got %zu:%zu", text, code, line, column, l, c);
    }
    CHECK(lx.tokens.count > 0 && lx.tokens.items[lx.tokens.count - 1].kind == TOK_EOF,
          "`%s`: tokens must end with EOF", text);
    lexed_free(&lx);
}

static void test_basics(void) {
    expect_kinds("", (TokenKind[]){ TOK_EOF });
    expect_kinds("  \t\r\n ", (TokenKind[]){ TOK_EOF });
    expect_kinds("\xEF\xBB\xBFx", (TokenKind[]){ TOK_IDENT, TOK_EOF });
    expect_kinds("struct Point mut x _ some window comptimeX result",
                 (TokenKind[]){ TOK_KW_STRUCT, TOK_IDENT, TOK_KW_MUT, TOK_IDENT, TOK_IDENT, TOK_IDENT,
                                TOK_IDENT, TOK_IDENT, TOK_IDENT, TOK_EOF }); /* `result` is contextual */
    expect_kinds("a // comment\n b /* block\n comment */ c",
                 (TokenKind[]){ TOK_IDENT, TOK_IDENT, TOK_IDENT, TOK_EOF });
    expect_kinds("// é ü 😀", (TokenKind[]){ TOK_EOF });
}

static void test_numbers(void) {
    expect_kinds("42 0xFF 0b1010 0", (TokenKind[]){ TOK_INT, TOK_INT, TOK_INT, TOK_INT, TOK_EOF });
    expect_kinds("3.14 1e5 2.5e-3 0.5", (TokenKind[]){ TOK_FLOAT, TOK_FLOAT, TOK_FLOAT, TOK_FLOAT, TOK_EOF });
    expect_kinds("0..n", (TokenKind[]){ TOK_INT, TOK_DOT_DOT, TOK_IDENT, TOK_EOF });
    expect_kinds("0..=10", (TokenKind[]){ TOK_INT, TOK_DOT_DOT_EQ, TOK_INT, TOK_EOF });
    expect_kinds("3.abs()", (TokenKind[]){ TOK_INT, TOK_DOT, TOK_IDENT, TOK_LPAREN, TOK_RPAREN, TOK_EOF });

    Lexed lx;
    lex_text(&lx, "42 0xFF 0b1010 18446744073709551615");
    CHECK(lx.tokens.items[0].int_value == 42, "42");
    CHECK(lx.tokens.items[1].int_value == 255, "0xFF");
    CHECK(lx.tokens.items[2].int_value == 10, "0b1010");
    CHECK(lx.tokens.items[3].int_value == UINT64_MAX, "ui64 max");
    lexed_free(&lx);
}

static void test_symbols(void) {
    expect_kinds("<<= >>= << >> <= >= == != => -> && || ..= .. . = < >",
                 (TokenKind[]){ TOK_SHL_EQ, TOK_SHR_EQ, TOK_SHL, TOK_SHR, TOK_LT_EQ, TOK_GT_EQ, TOK_EQ_EQ,
                                TOK_BANG_EQ, TOK_FAT_ARROW, TOK_ARROW, TOK_AMP_AMP, TOK_PIPE_PIPE,
                                TOK_DOT_DOT_EQ, TOK_DOT_DOT, TOK_DOT, TOK_EQ, TOK_LT, TOK_GT, TOK_EOF });
    expect_kinds("+= -= *= /= %= &= |= ^= + - * / % & | ^ ~ !",
                 (TokenKind[]){ TOK_PLUS_EQ, TOK_MINUS_EQ, TOK_STAR_EQ, TOK_SLASH_EQ, TOK_PERCENT_EQ,
                                TOK_AMP_EQ, TOK_PIPE_EQ, TOK_CARET_EQ, TOK_PLUS, TOK_MINUS, TOK_STAR,
                                TOK_SLASH, TOK_PERCENT, TOK_AMP, TOK_PIPE, TOK_CARET, TOK_TILDE, TOK_BANG,
                                TOK_EOF });
    expect_kinds("Button* back = w as? Button*;",
                 (TokenKind[]){ TOK_IDENT, TOK_STAR, TOK_IDENT, TOK_EQ, TOK_IDENT, TOK_KW_AS, TOK_QUESTION,
                                TOK_IDENT, TOK_STAR, TOK_SEMICOLON, TOK_EOF });
    expect_kinds("x - -1", (TokenKind[]){ TOK_IDENT, TOK_MINUS, TOK_MINUS, TOK_INT, TOK_EOF });
}

static void test_strings_and_chars(void) {
    expect_kinds("\"\" \"hello\" \"h\xC3\xA9\\n\\t\\\\\\\"\\x41\\u{1F600}\"",
                 (TokenKind[]){ TOK_STRING, TOK_STRING, TOK_STRING, TOK_EOF });
    expect_kinds("extern \"C\"", (TokenKind[]){ TOK_KW_EXTERN, TOK_STRING, TOK_EOF });

    Lexed lx;
    lex_text(&lx, "'a' '\\n' '\xC3\xA9' '\\u{1F600}' '\\''");
    CHECK(lx.diags.count == 0, "char literals: unexpected diagnostic");
    CHECK(lx.tokens.count == 6, "char literals: %zu tokens", lx.tokens.count);
    if (lx.tokens.count == 6) {
        CHECK(lx.tokens.items[0].char_value == 'a', "'a'");
        CHECK(lx.tokens.items[1].char_value == '\n', "'\\n'");
        CHECK(lx.tokens.items[2].char_value == 0xE9, "'é'");
        CHECK(lx.tokens.items[3].char_value == 0x1F600, "'\\u{1F600}'");
        CHECK(lx.tokens.items[4].char_value == '\'', "'\\''");
    }
    lexed_free(&lx);
}

static void test_errors(void) {
    expect_error("\xFF", "E0100", 1, 1);
    expect_error("\"a\xC3\x28\"", "E0100", 1, 3);
    expect_error("x # y", "E0101", 1, 3);
    expect_error("caf\xC3\xA9", "E0101", 1, 4);
    expect_error("a\n/* open", "E0102", 2, 1);
    expect_error("a\n  \"abc", "E0103", 2, 3);
    expect_error("\"abc\ndef\"", "E0103", 1, 1);
    expect_error("'a", "E0104", 1, 1);
    expect_error("''", "E0105", 1, 1);
    expect_error("'ab'", "E0106", 1, 1);
    expect_error("\"\\q\"", "E0107", 1, 2);
    expect_error("\"\\xFF\"", "E0107", 1, 2);
    expect_error("\"\\x4\"", "E0107", 1, 2);
    expect_error("0b102", "E0108", 1, 5);
    expect_error("12abc", "E0108", 1, 3);
    expect_error("1.5f32", "E0108", 1, 4);
    expect_error("0x", "E0108", 1, 1);
    expect_error("18446744073709551616", "E0109", 1, 1);
    expect_error("x++", "E0110", 1, 2);
    expect_error("x--", "E0110", 1, 2);
    expect_error("std::io", "E0111", 1, 4);
    expect_error("comptime", "E0112", 1, 1);
    expect_error("\"\\u{D800}\"", "E0113", 1, 2);
    expect_error("\"\\u{110000}\"", "E0113", 1, 2);
    expect_error("\"\\u41\"", "E0113", 1, 2);
    expect_error("007", "E0114", 1, 1);
}

static void test_error_recovery(void) {
    /* Lexing continues after an error, so later errors are reported too. */
    Lexed lx;
    lex_text(&lx, "x++ # 0b2 ok");
    CHECK(lx.diags.count == 3, "recovery: %zu diagnostics, expected 3", lx.diags.count);
    CHECK(lx.tokens.items[lx.tokens.count - 2].kind == TOK_IDENT, "recovery: last token should be `ok`");
    lexed_free(&lx);
}

static void test_text_rendering(void) {
    Lexed lx;
    lex_text(&lx, "i32 x = 1;\n\tstr s = \"abc;\n");
    FILE *out = tmpfile();
    CHECK(out != NULL, "tmpfile");
    if (!out) {
        lexed_free(&lx);
        return;
    }
    diag_print_text(&lx.diags, out);
    rewind(out);
    char buffer[1024];
    size_t n = fread(buffer, 1, sizeof buffer - 1, out);
    buffer[n] = '\0';
    fclose(out);

    const char *expected =
        "error[E0103]: unterminated string literal\n"
        " --> test.sosu:2:10\n"
        "  |\n"
        "2 | \tstr s = \"abc;\n"
        "  | \t        ^\n"
        "  = help: close the string with `\"` on the same line; write `\\n` for a line break\n"
        "\n";
    CHECK(strcmp(buffer, expected) == 0, "text rendering:\n%s\nexpected:\n%s", buffer, expected);
    lexed_free(&lx);
}

int main(void) {
    test_basics();
    test_numbers();
    test_symbols();
    test_strings_and_chars();
    test_errors();
    test_error_recovery();
    test_text_rendering();

    return test_summary("lexer tests");
}
