#include <stdlib.h>
#include <string.h>

#include "json.h"
#include "test.h"

static Json *parse(Arena *arena, const char *text) {
    return json_parse(text, strlen(text), arena);
}

static void test_values(void) {
    Arena arena = {0};
    Json *v = parse(&arena, " {\"id\": 3, \"ok\": true, \"no\": false, \"x\": null, \"f\": -1.5e2,"
                            " \"list\": [1, [], {}], \"s\": \"a\\\"b\\\\c\\n\"} ");
    CHECK(v && v->kind == JSON_OBJECT && v->count == 7, "an object of 7 members");
    CHECK(json_number(json_get(v, "id"), 0) == 3, "id");
    CHECK(json_bool(json_get(v, "ok"), false) && !json_bool(json_get(v, "no"), true), "booleans");
    CHECK(json_get(v, "x") && json_get(v, "x")->kind == JSON_NULL, "null");
    CHECK(json_number(json_get(v, "f"), 0) == -150, "exponent");
    const Json *list = json_get(v, "list");
    CHECK(list && list->kind == JSON_ARRAY && list->count == 3 && list->items[1].kind == JSON_ARRAY
              && list->items[2].kind == JSON_OBJECT && list->items[2].count == 0,
          "nested values");
    CHECK(strcmp(json_string(json_get(v, "s"), ""), "a\"b\\c\n") == 0, "escapes");
    CHECK(json_get(v, "missing") == NULL && json_string(json_get(v, "id"), "fb")[0] == 'f', "fallbacks");
    arena_free(&arena);
}

static void test_unicode(void) {
    Arena arena = {0};
    Json *v = parse(&arena, "\"\\u00e9\\u20ac\\ud83d\\ude00 \xc3\xa9\"");
    CHECK(v && strcmp(v->string, "\xc3\xa9\xe2\x82\xac\xf0\x9f\x98\x80 \xc3\xa9") == 0, "\\u escapes to UTF-8");
    Json *nul = parse(&arena, "\"a\\u0000b\"");
    CHECK(nul && nul->length == 3 && nul->string[1] == '\0', "a NUL keeps the length");
    arena_free(&arena);
}

static void test_invalid(void) {
    const char *bad[] = { "", "{", "[1,]", "{\"a\" 1}", "01", "1.", "+1", "tru", "\"\\x\"", "\"a",
                          "\"\\ud83d\"", "[1] 2", "nan", "{\"a\":1,}" };
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
        Arena arena = {0};
        CHECK(parse(&arena, bad[i]) == NULL, "invalid: %s", bad[i]);
        arena_free(&arena);
    }
}

static void test_writing(void) {
    JsonOut out = {0};
    json_raw(&out, "{\"id\":%d,\"s\":", 7);
    json_str(&out, "q\"\\\n\t\x01 \xc3\xa9");
    json_raw(&out, "}");
    CHECK(strcmp(out.text, "{\"id\":7,\"s\":\"q\\\"\\\\\\n\\t\\u0001 \xc3\xa9\"}") == 0, "written: %s", out.text);
    Arena arena = {0};
    Json *back = json_parse(out.text, out.length, &arena);
    CHECK(back && strcmp(json_string(json_get(back, "s"), ""), "q\"\\\n\t\x01 \xc3\xa9") == 0, "read back");
    arena_free(&arena);
    json_out_free(&out);
}

int main(void) {
    test_values();
    test_unicode();
    test_invalid();
    test_writing();
    return test_summary("json_tests");
}
