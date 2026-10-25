#include "i18n.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lexer.h"

/* The extraction works on tokens, not on the syntax tree: it needs no types, and a file with
   errors still gives what it can. */

typedef struct {
    char *items;
    size_t count;
    size_t cap;
} Buf;

static void buf_add(Buf *buf, const char *text, size_t length) {
    if (buf->count + length + 1 > buf->cap) {
        buf->cap = (buf->count + length + 1) * 2;
        buf->items = sosu_realloc(buf->items, buf->cap);
    }
    memcpy(buf->items + buf->count, text, length);
    buf->count += length;
    buf->items[buf->count] = '\0';
}

static void buf_puts(Buf *buf, const char *text) {
    buf_add(buf, text, strlen(text));
}

typedef struct {
    char *context; /* NULL: none */
    char *id;
    char *plural; /* NULL: none */
    Buf references; /* "file:line file:line" */
} Message;

struct I18nTemplate {
    Message *items;
    size_t count;
    size_t cap;
};

I18nTemplate *i18n_template_new(void) {
    I18nTemplate *t = sosu_alloc(sizeof *t);
    memset(t, 0, sizeof *t);
    return t;
}

void i18n_template_free(I18nTemplate *t) {
    for (size_t i = 0; i < t->count; i++) {
        free(t->items[i].context);
        free(t->items[i].id);
        free(t->items[i].plural);
        free(t->items[i].references.items);
    }
    free(t->items);
    free(t);
}

size_t i18n_template_count(const I18nTemplate *t) {
    return t->count;
}

static unsigned hex_value(char c) {
    if (c >= '0' && c <= '9') return (unsigned)(c - '0');
    if (c >= 'a' && c <= 'f') return (unsigned)(c - 'a' + 10);
    return (unsigned)(c - 'A' + 10);
}

/* The text of a string literal (checked by the lexer), decoded, then escaped as gettext does. */
static char *po_text(const SourceFile *file, const Token *token) {
    const char *text = file->text + token->offset + 1;
    size_t length = token->length - 2;
    Buf decoded = {0};
    for (size_t i = 0; i < length; i++) {
        if (text[i] != '\\') {
            buf_add(&decoded, text + i, 1);
            continue;
        }
        char e = text[++i];
        if (e == 'n') {
            buf_puts(&decoded, "\n");
        } else if (e == 't') {
            buf_puts(&decoded, "\t");
        } else if (e == 'r') {
            buf_puts(&decoded, "\r");
        } else if (e == 'x') {
            char byte = (char)(hex_value(text[i + 1]) * 16 + hex_value(text[i + 2]));
            buf_add(&decoded, &byte, 1);
            i += 2;
        } else if (e == 'u') {
            uint32_t value = 0;
            i += 2; /* u{ */
            while (text[i] != '}') value = value * 16 + hex_value(text[i++]);
            Buf *out = &decoded;
            char bytes[4];
            size_t n = 0;
            if (value < 0x80) {
                bytes[n++] = (char)value;
            } else if (value < 0x800) {
                bytes[n++] = (char)(0xC0 | value >> 6);
                bytes[n++] = (char)(0x80 | (value & 0x3F));
            } else if (value < 0x10000) {
                bytes[n++] = (char)(0xE0 | value >> 12);
                bytes[n++] = (char)(0x80 | (value >> 6 & 0x3F));
                bytes[n++] = (char)(0x80 | (value & 0x3F));
            } else {
                bytes[n++] = (char)(0xF0 | value >> 18);
                bytes[n++] = (char)(0x80 | (value >> 12 & 0x3F));
                bytes[n++] = (char)(0x80 | (value >> 6 & 0x3F));
                bytes[n++] = (char)(0x80 | (value & 0x3F));
            }
            buf_add(out, bytes, n);
        } else {
            buf_add(&decoded, &e, 1); /* \\ \' \" */
        }
    }
    Buf po = {0};
    buf_puts(&po, "");
    for (size_t i = 0; i < decoded.count; i++) {
        char c = decoded.items[i];
        if (c == '"') buf_puts(&po, "\\\"");
        else if (c == '\\') buf_puts(&po, "\\\\");
        else if (c == '\n') buf_puts(&po, "\\n");
        else if (c == '\t') buf_puts(&po, "\\t");
        else if (c == '\r') buf_puts(&po, "\\r");
        else buf_add(&po, &c, 1);
    }
    free(decoded.items);
    return po.items;
}

static bool same_or_null(const char *a, const char *b) {
    return (!a && !b) || (a && b && strcmp(a, b) == 0);
}

/* Adds a text (its context and plural may be NULL), or a reference to the text already there. */
static void add_message(I18nTemplate *t, char *context, char *id, char *plural, const char *path, size_t line) {
    Message *found = NULL;
    for (size_t i = 0; i < t->count; i++) {
        if (strcmp(t->items[i].id, id) == 0 && same_or_null(t->items[i].context, context)) found = &t->items[i];
    }
    if (!found) {
        Message m = { context, id, plural, { 0 } };
        SOSU_VEC_PUSH(t, m);
        found = &t->items[t->count - 1];
    } else {
        if (!found->plural && plural) {
            found->plural = plural;
        } else {
            free(plural);
        }
        free(context);
        free(id);
    }
    char *reference = sosu_format("%s%s:%zu", found->references.count ? " " : "", path, line);
    buf_puts(&found->references, reference);
    free(reference);
}

static bool is_ident(const SourceFile *file, const Token *token, const char *text) {
    return token->kind == TOK_IDENT && token->length == strlen(text) &&
           memcmp(file->text + token->offset, text, token->length) == 0;
}

/* The nodes whose first value is a text to translate. */
static bool is_text_node(const SourceFile *file, const Token *token) {
    static const char *const nodes[] = { "Text", "Title", "Button", "Checkbox", "Radio", "Option", "Menu", "MenuItem" };
    for (size_t i = 0; i < sizeof nodes / sizeof *nodes; i++) {
        if (is_ident(file, token, nodes[i])) return true;
    }
    return false;
}

static size_t line_at(const SourceFile *file, size_t offset) {
    size_t line, column;
    source_position(file, offset, &line, &column);
    return line;
}

/* The arguments of a node of a .ui.sosu file, from its `(` at `open`: its first value if it is a
   text (for a node of texts), `plural:`, `context:` and `placeholder:`. */
static void add_node(I18nTemplate *t, const SourceFile *file, const Token *tokens, size_t count, size_t open,
                     bool texts, const char *path) {
    const Token *text = NULL;
    const Token *plural = NULL;
    const Token *context = NULL;
    const Token *placeholder = NULL;
    if (texts && open + 2 < count && tokens[open + 1].kind == TOK_STRING &&
        (tokens[open + 2].kind == TOK_COMMA || tokens[open + 2].kind == TOK_RPAREN)) {
        text = &tokens[open + 1];
    }
    int depth = 0;
    for (size_t i = open; i < count; i++) {
        TokenKind kind = tokens[i].kind;
        if (kind == TOK_LPAREN || kind == TOK_LBRACKET) depth++;
        if (kind == TOK_RPAREN || kind == TOK_RBRACKET) depth--;
        if (depth == 0 || kind == TOK_EOF) break;
        if (depth == 1 && kind == TOK_IDENT && i + 2 < count && tokens[i + 1].kind == TOK_COLON &&
            tokens[i + 2].kind == TOK_STRING) {
            if (is_ident(file, &tokens[i], "plural")) plural = &tokens[i + 2];
            if (is_ident(file, &tokens[i], "context")) context = &tokens[i + 2];
            if (is_ident(file, &tokens[i], "placeholder")) placeholder = &tokens[i + 2];
        }
    }
    if (text) {
        add_message(t, context ? po_text(file, context) : NULL, po_text(file, text), plural ? po_text(file, plural) : NULL,
                    path, line_at(file, text->offset));
    }
    if (placeholder) add_message(t, NULL, po_text(file, placeholder), NULL, path, line_at(file, placeholder->offset));
}

/* `.tr("...")`, `.tr_in("context", "...")`, `.tr_plural("one", "other", n)`,
   `.tr_plural_in("context", "one", "other", n)` in a .sosu file, from the name at `at`. */
static void add_call(I18nTemplate *t, const SourceFile *file, const Token *tokens, size_t count, size_t at,
                     const char *path) {
    bool in = is_ident(file, &tokens[at], "tr_in") || is_ident(file, &tokens[at], "tr_plural_in");
    bool plural = is_ident(file, &tokens[at], "tr_plural") || is_ident(file, &tokens[at], "tr_plural_in");
    size_t needed = 1 + (in ? 1 : 0) + (plural ? 1 : 0); /* literal texts in a row */
    const Token *strings[3];
    size_t i = at + 2;
    for (size_t k = 0; k < needed; k++) {
        if (i >= count || tokens[i].kind != TOK_STRING) return; /* not literal: nothing to extract */
        strings[k] = &tokens[i];
        i += 1;
        if (k + 1 < needed) {
            if (i >= count || tokens[i].kind != TOK_COMMA) return;
            i += 1;
        }
    }
    const Token *context = in ? strings[0] : NULL;
    const Token *id = strings[in ? 1 : 0];
    const Token *other = plural ? strings[in ? 2 : 1] : NULL;
    add_message(t, context ? po_text(file, context) : NULL, po_text(file, id), other ? po_text(file, other) : NULL,
                path, line_at(file, id->offset));
}

void i18n_template_add(I18nTemplate *t, const SourceFile *file, const char *shown_path) {
    TokenList tokens = {0};
    DiagList diags = {0};
    lex_file(file, &tokens, &diags);
    size_t n = strlen(file->path);
    bool ui = n >= 8 && strcmp(file->path + n - 8, ".ui.sosu") == 0;
    const Token *list = tokens.items;
    for (size_t i = 0; i + 1 < tokens.count; i++) {
        if (ui) {
            if (list[i].kind == TOK_IDENT && list[i + 1].kind == TOK_LPAREN) {
                add_node(t, file, list, tokens.count, i + 1, is_text_node(file, &list[i]), shown_path);
            } else if (is_ident(file, &list[i], "title") && list[i + 1].kind == TOK_COLON && i + 2 < tokens.count &&
                       list[i + 2].kind == TOK_STRING) {
                add_message(t, NULL, po_text(file, &list[i + 2]), NULL, shown_path, line_at(file, list[i + 2].offset));
            }
        } else if (list[i].kind == TOK_DOT && i + 2 < tokens.count && list[i + 2].kind == TOK_LPAREN &&
                   (is_ident(file, &list[i + 1], "tr") || is_ident(file, &list[i + 1], "tr_in") ||
                    is_ident(file, &list[i + 1], "tr_plural") || is_ident(file, &list[i + 1], "tr_plural_in"))) {
            add_call(t, file, list, tokens.count, i + 1, shown_path);
        }
    }
    token_list_free(&tokens);
    diag_free(&diags);
}

/* A field of the template: `msgid "..."`. */
static void put_field(Buf *out, const char *keyword, const char *text) {
    buf_puts(out, keyword);
    buf_puts(out, " \"");
    buf_puts(out, text);
    buf_puts(out, "\"\n");
}

char *i18n_template_text(const I18nTemplate *t) {
    Buf out = {0};
    buf_puts(&out, "# The texts to translate, written by `sosuc i18n extract` (spec §10.7).\n");
    buf_puts(&out, "msgid \"\"\nmsgstr \"\"\n");
    buf_puts(&out, "\"Content-Type: text/plain; charset=UTF-8\\n\"\n");
    buf_puts(&out, "\"Plural-Forms: nplurals=2; plural=(n != 1);\\n\"\n");
    for (size_t i = 0; i < t->count; i++) {
        const Message *m = &t->items[i];
        buf_puts(&out, "\n#: ");
        buf_puts(&out, m->references.items ? m->references.items : "");
        buf_puts(&out, "\n");
        if (m->context) put_field(&out, "msgctxt", m->context);
        put_field(&out, "msgid", m->id);
        if (m->plural) {
            put_field(&out, "msgid_plural", m->plural);
            buf_puts(&out, "msgstr[0] \"\"\nmsgstr[1] \"\"\n");
        } else {
            buf_puts(&out, "msgstr \"\"\n");
        }
    }
    return out.items;
}
