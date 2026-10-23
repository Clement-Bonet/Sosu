#include "json.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

/* ---- Reading ---- */

typedef struct {
    const char *text;
    size_t length;
    size_t pos;
    Arena *arena;
    int depth;
} Reader;

static bool parse_value(Reader *r, Json *out);

static void skip_space(Reader *r) {
    while (r->pos < r->length && strchr(" \t\r\n", r->text[r->pos])) r->pos++;
}

static bool take(Reader *r, char c) {
    skip_space(r);
    if (r->pos < r->length && r->text[r->pos] == c) {
        r->pos++;
        return true;
    }
    return false;
}

static bool take_word(Reader *r, const char *word) {
    size_t n = strlen(word);
    if (r->length - r->pos < n || memcmp(r->text + r->pos, word, n) != 0) return false;
    r->pos += n;
    return true;
}

static int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Four hexadecimal digits of a \u escape, or -1. */
static long read_hex4(Reader *r) {
    if (r->length - r->pos < 4) return -1;
    long value = 0;
    for (int i = 0; i < 4; i++) {
        int d = hex_digit(r->text[r->pos + i]);
        if (d < 0) return -1;
        value = value * 16 + d;
    }
    r->pos += 4;
    return value;
}

static size_t put_utf8(char *out, unsigned long c) {
    if (c < 0x80) {
        out[0] = (char)c;
        return 1;
    }
    if (c < 0x800) {
        out[0] = (char)(0xC0 | (c >> 6));
        out[1] = (char)(0x80 | (c & 0x3F));
        return 2;
    }
    if (c < 0x10000) {
        out[0] = (char)(0xE0 | (c >> 12));
        out[1] = (char)(0x80 | ((c >> 6) & 0x3F));
        out[2] = (char)(0x80 | (c & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (c >> 18));
    out[1] = (char)(0x80 | ((c >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((c >> 6) & 0x3F));
    out[3] = (char)(0x80 | (c & 0x3F));
    return 4;
}

/* A string, its opening quote already read. Its escapes never make it longer. */
static bool parse_string(Reader *r, char **text, size_t *length) {
    size_t start = r->pos;
    size_t end = start;
    while (end < r->length && r->text[end] != '"') end += r->text[end] == '\\' ? 2 : 1;
    if (end >= r->length) return false;
    char *out = arena_alloc(r->arena, end - start + 1);
    size_t n = 0;
    while (r->pos < end) {
        char c = r->text[r->pos++];
        if ((unsigned char)c < 0x20) return false;
        if (c != '\\') {
            out[n++] = c;
            continue;
        }
        char e = r->text[r->pos++];
        switch (e) {
        case '"': out[n++] = '"'; break;
        case '\\': out[n++] = '\\'; break;
        case '/': out[n++] = '/'; break;
        case 'b': out[n++] = '\b'; break;
        case 'f': out[n++] = '\f'; break;
        case 'n': out[n++] = '\n'; break;
        case 'r': out[n++] = '\r'; break;
        case 't': out[n++] = '\t'; break;
        case 'u': {
            long c1 = read_hex4(r);
            if (c1 < 0) return false;
            unsigned long code = (unsigned long)c1;
            if (c1 >= 0xD800 && c1 <= 0xDBFF) { /* a surrogate pair */
                if (r->pos + 2 > end || r->text[r->pos] != '\\' || r->text[r->pos + 1] != 'u') return false;
                r->pos += 2;
                long c2 = read_hex4(r);
                if (c2 < 0xDC00 || c2 > 0xDFFF) return false;
                code = 0x10000 + (((unsigned long)c1 - 0xD800) << 10) + ((unsigned long)c2 - 0xDC00);
            } else if (c1 >= 0xDC00 && c1 <= 0xDFFF) {
                return false;
            }
            n += put_utf8(out + n, code);
            break;
        }
        default:
            return false;
        }
    }
    r->pos = end + 1;
    out[n] = '\0';
    *text = out;
    *length = n;
    return true;
}

static bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

/* A number of the JSON grammar (no `+`, `inf`, hexadecimal...), then converted by strtod. */
static bool parse_number(Reader *r, double *out) {
    size_t start = r->pos, i = start;
    const char *t = r->text;
    if (i < r->length && t[i] == '-') i++;
    if (i >= r->length || !is_digit(t[i])) return false;
    if (t[i] == '0') i++;
    else while (i < r->length && is_digit(t[i])) i++;
    if (i < r->length && t[i] == '.') {
        if (++i >= r->length || !is_digit(t[i])) return false;
        while (i < r->length && is_digit(t[i])) i++;
    }
    if (i < r->length && (t[i] == 'e' || t[i] == 'E')) {
        i++;
        if (i < r->length && (t[i] == '+' || t[i] == '-')) i++;
        if (i >= r->length || !is_digit(t[i])) return false;
        while (i < r->length && is_digit(t[i])) i++;
    }
    char buffer[64];
    size_t n = i - start < sizeof buffer - 1 ? i - start : sizeof buffer - 1;
    memcpy(buffer, t + start, n);
    buffer[n] = '\0';
    *out = strtod(buffer, NULL);
    r->pos = i;
    return true;
}

/* Values and keys are first gathered in malloc'd lists, then copied into the arena. */
typedef struct {
    Json *items;
    size_t count, cap;
} JsonVec;
typedef struct {
    char **items;
    size_t count, cap;
} KeyVec;

static bool parse_members(Reader *r, Json *out, bool object) {
    JsonVec items = {0};
    KeyVec keys = {0};
    char closer = object ? '}' : ']';
    bool ok = true;
    if (!take(r, closer)) {
        do {
            if (object) {
                char *key;
                size_t key_length;
                if (!take(r, '"') || !parse_string(r, &key, &key_length) || !take(r, ':')) {
                    ok = false;
                    break;
                }
                SOSU_VEC_PUSH(&keys, key);
            }
            Json value;
            if (!parse_value(r, &value)) {
                ok = false;
                break;
            }
            SOSU_VEC_PUSH(&items, value);
        } while (take(r, ','));
        ok = ok && take(r, closer);
    }
    out->count = items.count;
    out->items = items.count ? arena_copy(r->arena, items.items, items.count * sizeof *items.items) : NULL;
    out->keys = keys.count ? arena_copy(r->arena, keys.items, keys.count * sizeof *keys.items) : NULL;
    free(items.items);
    free(keys.items);
    return ok;
}

static bool parse_value(Reader *r, Json *out) {
    memset(out, 0, sizeof *out);
    if (++r->depth > 200) return false; /* deeper than any message: refused */
    skip_space(r);
    bool ok = false;
    if (r->pos >= r->length) {
        ok = false;
    } else if (take(r, '{')) {
        out->kind = JSON_OBJECT;
        ok = parse_members(r, out, true);
    } else if (take(r, '[')) {
        out->kind = JSON_ARRAY;
        ok = parse_members(r, out, false);
    } else if (take(r, '"')) {
        out->kind = JSON_STRING;
        ok = parse_string(r, &out->string, &out->length);
    } else if (take_word(r, "true")) {
        out->kind = JSON_BOOL;
        out->boolean = true;
        ok = true;
    } else if (take_word(r, "false")) {
        out->kind = JSON_BOOL;
        ok = true;
    } else if (take_word(r, "null")) {
        out->kind = JSON_NULL;
        ok = true;
    } else {
        out->kind = JSON_NUMBER;
        ok = parse_number(r, &out->number);
    }
    r->depth--;
    return ok;
}

Json *json_parse(const char *text, size_t length, Arena *arena) {
    Reader r = { text, length, 0, arena, 0 };
    Json *value = arena_alloc(arena, sizeof *value);
    if (!parse_value(&r, value)) return NULL;
    skip_space(&r);
    return r.pos == length ? value : NULL;
}

const Json *json_get(const Json *object, const char *key) {
    if (!object || object->kind != JSON_OBJECT) return NULL;
    for (size_t i = 0; i < object->count; i++) {
        if (strcmp(object->keys[i], key) == 0) return &object->items[i];
    }
    return NULL;
}

const char *json_string(const Json *value, const char *fallback) {
    return value && value->kind == JSON_STRING ? value->string : fallback;
}

double json_number(const Json *value, double fallback) {
    return value && value->kind == JSON_NUMBER ? value->number : fallback;
}

bool json_bool(const Json *value, bool fallback) {
    return value && value->kind == JSON_BOOL ? value->boolean : fallback;
}

/* ---- Writing ---- */

static void out_add(JsonOut *out, const char *text, size_t length) {
    if (out->length + length + 1 > out->cap) {
        out->cap = out->cap ? out->cap * 2 : 256;
        while (out->length + length + 1 > out->cap) out->cap *= 2;
        out->text = sosu_realloc(out->text, out->cap);
    }
    memcpy(out->text + out->length, text, length);
    out->length += length;
    out->text[out->length] = '\0';
}

void json_raw(JsonOut *out, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    char *text = sosu_vformat(fmt, args);
    va_end(args);
    out_add(out, text, strlen(text));
    free(text);
}

void json_strn(JsonOut *out, const char *text, size_t length) {
    out_add(out, "\"", 1);
    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char)text[i];
        char escape[8];
        if (c == '"' || c == '\\') {
            escape[0] = '\\';
            escape[1] = (char)c;
            out_add(out, escape, 2);
        } else if (c == '\n') {
            out_add(out, "\\n", 2);
        } else if (c == '\r') {
            out_add(out, "\\r", 2);
        } else if (c == '\t') {
            out_add(out, "\\t", 2);
        } else if (c < 0x20 || c == 0x7F) {
            char *u = sosu_format("\\u%04x", c);
            out_add(out, u, 6);
            free(u);
        } else {
            out_add(out, (const char *)&text[i], 1);
        }
    }
    out_add(out, "\"", 1);
}

void json_str(JsonOut *out, const char *text) {
    json_strn(out, text, strlen(text));
}

void json_out_free(JsonOut *out) {
    free(out->text);
    memset(out, 0, sizeof *out);
}
