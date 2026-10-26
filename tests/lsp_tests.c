#if !defined(_WIN32) && !defined(_XOPEN_SOURCE)
#define _XOPEN_SOURCE 700 /* realpath */
#endif

#include <stdlib.h>
#include <string.h>

#include "json.h"
#include "lsp.h"
#include "test.h"

/* Exchanges with the language server, on the package tests/packages/app whose main file
   is open with other text. Run from the repository root. */

static LspServer *server;
static LspMessages out;
static char *main_uri;

static void exchange(const char *message) {
    lsp_messages_free(&out);
    lsp_handle(server, message, strlen(message), &out);
}

/* The message sent back that contains `needle`, or NULL. */
static const char *answer(const char *needle) {
    for (size_t i = 0; i < out.count; i++) {
        if (strstr(out.items[i], needle)) return out.items[i];
    }
    return NULL;
}

static char *json_text(const char *text) {
    JsonOut o = {0};
    json_str(&o, text);
    return o.text;
}

static void open_main(const char *text) {
    char *quoted = json_text(text);
    char *message = sosu_format("{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\",\"params\":{\"textDocument\":"
                                "{\"uri\":\"%s\",\"languageId\":\"sosu\",\"version\":1,\"text\":%s}}}",
                                main_uri, quoted);
    exchange(message);
    free(message);
    free(quoted);
}

static void change_main(const char *text) {
    char *quoted = json_text(text);
    char *message = sosu_format("{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didChange\",\"params\":{\"textDocument\":"
                                "{\"uri\":\"%s\",\"version\":2},\"contentChanges\":[{\"text\":%s}]}}",
                                main_uri, quoted);
    exchange(message);
    free(message);
    free(quoted);
}

/* Sends a request at the position of `needle` in `text`, plus `delta` characters. */
static void request(const char *method, const char *text, const char *needle, size_t delta) {
    const char *at = strstr(text, needle);
    CHECK(at != NULL, "`%s` is in the text", needle);
    if (!at) return;
    size_t line = 0, character = 0;
    for (const char *c = text; c < at; c++) {
        if (*c == '\n') {
            line++;
            character = 0;
        } else {
            character++;
        }
    }
    char *message = sosu_format("{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"%s\",\"params\":{\"textDocument\":{\"uri\":\"%s\"},"
                                "\"position\":{\"line\":%zu,\"character\":%zu}}}",
                                method, main_uri, line, character + delta);
    exchange(message);
    free(message);
}

static const char *const good =
    "import std.io;\n"
    "import app.shapes;\n"
    "import geometry.vector;\n"
    "\n"
    "vector.Vec2 origin() {\n"
    "    vector.Vec2 v = vector.Vec2{ x: 0.0, y: 0.0 };\n"
    "    return v;\n"
    "}\n"
    "\n"
    "T pick<T>(T a, T b) {\n"
    "    return a;\n"
    "}\n"
    "\n"
    "shapes.Shape unit_circle() {\n"
    "    return shapes.Shape.Circle(1.0);\n"
    "}\n"
    "\n"
    "i32 main() uses io {\n"
    "    i32 count = 2;\n"
    "    vector.Vec2 o = origin();\n"
    "    i32 n = pick(count, 3);\n"
    "    return count + 1;\n"
    "}\n";

static void test_session(void) {
    exchange("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"capabilities\":{}}}");
    CHECK(answer("\"hoverProvider\":true") && answer("\"id\":1"), "initialize: %s", out.count ? out.items[0] : "nothing");
    exchange("{\"jsonrpc\":\"2.0\",\"method\":\"initialized\",\"params\":{}}");
    CHECK(out.count == 0, "initialized needs no answer");

    /* diagnostics while typing: an error, then none */
    open_main("i32 main() {\n    i32 count = \"two\";\n    return count;\n}\n");
    const char *published = answer(main_uri);
    CHECK(published && strstr(published, "publishDiagnostics") && strstr(published, "\"severity\":1") &&
              strstr(published, "\"line\":1"),
          "an error on line 2: %s", published ? published : "nothing");
    change_main(good);
    published = answer(main_uri);
    CHECK(published && strstr(published, "\"diagnostics\":[]"), "no more errors: %s", published ? published : "nothing");

    /* hover */
    request("textDocument/hover", good, "count + 1", 2);
    CHECK(answer("i32 count"), "hover on a local: %s", out.count ? out.items[0] : "nothing");
    request("textDocument/hover", good, "origin();", 1);
    CHECK(answer("vector.Vec2 origin()"), "hover on a call: %s", out.count ? out.items[0] : "nothing");
    request("textDocument/hover", good, "pick(count", 1);
    CHECK(answer("T pick<T>(T a, T b)") && answer("here: `T` is `i32`"), "hover on a generic call: %s",
          out.count ? out.items[0] : "nothing");
    request("textDocument/hover", good, "Circle(1.0)", 1);
    CHECK(answer("Shape.Circle(f32)"), "hover on a variant: %s", out.count ? out.items[0] : "nothing");
    request("textDocument/hover", good, "x: 0.0", 0);
    CHECK(answer("f32 x"), "hover on a field of a literal: %s", out.count ? out.items[0] : "nothing");

    /* go to definition */
    request("textDocument/definition", good, "count + 1", 0);
    CHECK(answer("\"start\":{\"line\":18,\"character\":8}"), "a local: %s", out.count ? out.items[0] : "nothing");
    request("textDocument/definition", good, "Vec2 origin", 1);
    CHECK(answer("geometry/src/vector.sosu") && answer("\"start\":{\"line\":0,\"character\":11}"), "a type of a module: %s",
          out.count ? out.items[0] : "nothing");
    request("textDocument/definition", good, "Circle(1.0)", 0);
    CHECK(answer("app/src/shapes.sosu") && answer("\"line\":3"), "a variant: %s", out.count ? out.items[0] : "nothing");
    request("textDocument/definition", good, "origin();", 0);
    CHECK(answer("\"start\":{\"line\":4,\"character\":12}"), "a function: %s", out.count ? out.items[0] : "nothing");

    /* completion */
    const char *member =
        "import geometry.vector;\n"
        "import app.shapes;\n"
        "i32 main() {\n"
        "    vector.Vec2 v = vector.Vec2{ x: 0.0, y: 0.0 };\n"
        "    f32 a = v.\n"
        "    return 0;\n"
        "}\n";
    change_main(member);
    request("textDocument/completion", member, "v.\n", 2);
    CHECK(answer("\"label\":\"x\"") && answer("\"label\":\"y\""), "the fields of a struct: %s",
          out.count ? out.items[0] : "nothing");
    const char *variants = "import app.shapes;\ni32 main() {\n    auto s = shapes.Shape.\n    return 0;\n}\n";
    change_main(variants);
    request("textDocument/completion", variants, "Shape.\n", 6);
    CHECK(answer("\"label\":\"Circle\"") && answer("\"label\":\"Empty\""), "the variants of an enum: %s",
          out.count ? out.items[0] : "nothing");
    const char *module = "import app.shapes;\ni32 main() {\n    auto s = shapes.Shape.Empty;\n    return 0;\n}\n";
    change_main(module);
    request("textDocument/completion", module, "shapes.Shape", 7);
    CHECK(answer("\"label\":\"Shape\""), "the names of a module: %s", out.count ? out.items[0] : "nothing");
    change_main(good);
    request("textDocument/completion", good, "count + 1", 0);
    CHECK(answer("\"label\":\"count\"") && answer("\"label\":\"origin\"") && answer("\"label\":\"return\"") &&
              answer("\"label\":\"vector\"") && answer("\"label\":\"i32\""),
          "locals, functions, modules, keywords and types: %s", out.count ? out.items[0] : "nothing");

    exchange("{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"textDocument/rename\",\"params\":{}}");
    CHECK(answer("-32601"), "an unknown request is an error");
    exchange("{\"jsonrpc\":\"2.0\",\"id\":10,\"method\":\"shutdown\"}");
    CHECK(answer("\"id\":10,\"result\":null"), "shutdown");
    exchange("{\"jsonrpc\":\"2.0\",\"method\":\"exit\"}");
    CHECK(lsp_exit_code(server) == 0, "exit after shutdown");
}

static void test_uris(void) {
    char *path = lsp_uri_to_path("file:///d%3A/Docs/My%20App/src/main.sosu");
    CHECK(path && strcmp(path, "d:/Docs/My App/src/main.sosu") == 0, "Windows URI: %s", path ? path : "NULL");
    free(path);
    path = lsp_uri_to_path("file:///home/ada/app/main.sosu");
    CHECK(path && strcmp(path, "/home/ada/app/main.sosu") == 0, "POSIX URI");
    free(path);
    CHECK(lsp_uri_to_path("untitled:Untitled-1") == NULL, "not a file");
    char *uri = lsp_path_to_uri("D:/Docs/My App/é.sosu");
    CHECK(strcmp(uri, "file:///D%3A/Docs/My%20App/%C3%A9.sosu") == 0, "path to URI: %s", uri);
    free(uri);
}

int main(void) {
    test_uris();
#ifdef _WIN32
    char *root = _fullpath(NULL, "tests/packages/app/src/main.sosu", 0);
#else
    char *root = realpath("tests/packages/app/src/main.sosu", NULL);
#endif
    if (!root) {
        fputs("lsp_tests: run from the repository root\n", stderr);
        return 1;
    }
    main_uri = lsp_path_to_uri(root);
    free(root);
    server = lsp_new("lib");
    test_session();
    lsp_free(server);
    lsp_messages_free(&out);
    free(main_uri);
    return test_summary("lsp_tests");
}
