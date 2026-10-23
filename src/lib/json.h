#ifndef SOSU_JSON_H
#define SOSU_JSON_H

#include "arena.h"
#include "common.h"

/* JSON (RFC 8259), read into a tree in an arena and written into a growing string: what the
   language server (lsp.c) needs, with the libc only. */

typedef enum {
    JSON_NULL,
    JSON_BOOL,
    JSON_NUMBER,
    JSON_STRING,
    JSON_ARRAY,
    JSON_OBJECT,
} JsonKind;

typedef struct Json Json;
struct Json {
    JsonKind kind;
    bool boolean;
    double number;
    char *string; /* UTF-8, NUL-terminated (it may also contain NUL: see `length`) */
    size_t length;
    Json *items; /* ARRAY: the values; OBJECT: the values, `keys` their names */
    char **keys;
    size_t count;
};

/* The value in `text`, allocated in `arena`, or NULL if it is not valid JSON. */
Json *json_parse(const char *text, size_t length, Arena *arena);

/* The member `key` of an object, or NULL (missing, or `object` is not an object). */
const Json *json_get(const Json *object, const char *key);
/* Shortcuts that accept NULL: the value when it has the right kind, else the fallback. */
const char *json_string(const Json *value, const char *fallback);
double json_number(const Json *value, double fallback);
bool json_bool(const Json *value, bool fallback);

/* A JSON text being written. */
typedef struct {
    char *text;
    size_t length;
    size_t cap;
} JsonOut;

/* Appends text as it is (punctuation, numbers, literals...). */
void json_raw(JsonOut *out, const char *fmt, ...);
/* Appends a JSON string: quotes, and escapes for `"`, `\` and control characters. */
void json_str(JsonOut *out, const char *text);
void json_strn(JsonOut *out, const char *text, size_t length);
void json_out_free(JsonOut *out);

#endif
