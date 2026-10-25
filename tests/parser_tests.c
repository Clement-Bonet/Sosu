#include <stdlib.h>
#include <string.h>

#include "parser.h"
#include "test.h"

typedef struct {
    SourceFile file;
    TokenList tokens;
    DiagList diags;
    Arena arena;
    char *dump;
} Parsed;

/* Runs `print` into a string. */
static char *capture(void (*print)(const void *node, const SourceFile *file, FILE *out),
                     const void *node, const SourceFile *file) {
    FILE *out = tmpfile();
    if (!out) return sosu_strdup("<tmpfile failed>");
    print(node, file, out);
    long size = ftell(out);
    rewind(out);
    char *text = sosu_alloc((size_t)size + 1);
    size_t n = fread(text, 1, (size_t)size, out);
    text[n] = '\0';
    fclose(out);
    return text;
}

static void print_expr(const void *node, const SourceFile *file, FILE *out) { ast_print_expr(node, file, out); }
static void print_module(const void *node, const SourceFile *file, FILE *out) { ast_print_module(node, file, out); }

static void parse_text(Parsed *out, const char *text, bool expression) {
    memset(out, 0, sizeof *out);
    source_from_string(&out->file, "test.sosu", text);
    lex_file(&out->file, &out->tokens, &out->diags);
    if (expression) {
        Expr *expr = parse_expression(&out->file, &out->tokens, &out->arena, &out->diags);
        out->dump = capture(print_expr, expr, &out->file);
    } else {
        Module *module = parse_module(&out->file, &out->tokens, &out->arena, &out->diags);
        out->dump = capture(print_module, module, &out->file);
    }
}

static void parsed_free(Parsed *parsed) {
    free(parsed->dump);
    arena_free(&parsed->arena);
    diag_free(&parsed->diags);
    token_list_free(&parsed->tokens);
    source_free(&parsed->file);
}

static void expect_dump(const char *text, bool expression, const char *expected) {
    Parsed parsed;
    parse_text(&parsed, text, expression);
    CHECK(parsed.diags.count == 0, "`%s`: unexpected %s: %s", text,
          parsed.diags.count ? parsed.diags.items[0].code : "", parsed.diags.count ? parsed.diags.items[0].message : "");
    CHECK(strcmp(parsed.dump, expected) == 0, "`%s`:\n  got      %s\n  expected %s", text, parsed.dump, expected);
    parsed_free(&parsed);
}

static void expect_expr(const char *text, const char *expected) { expect_dump(text, true, expected); }
static void expect_module(const char *text, const char *expected) { expect_dump(text, false, expected); }

/* Checks the first diagnostic of a module and how many were reported. */
static void expect_error(const char *text, const char *code, size_t line, size_t column, size_t count) {
    Parsed parsed;
    parse_text(&parsed, text, false);
    CHECK(parsed.diags.count == count, "`%s`: %zu diagnostics, expected %zu", text, parsed.diags.count, count);
    if (parsed.diags.count > 0) {
        const Diagnostic *d = &parsed.diags.items[0];
        size_t l, c;
        source_position(&parsed.file, d->offset, &l, &c);
        CHECK(strcmp(d->code, code) == 0, "`%s`: expected %s, got %s (%s)", text, code, d->code, d->message);
        CHECK(l == line && c == column, "`%s`: expected %s at %zu:%zu, got %zu:%zu (%s)", text, code, line, column,
              l, c, d->message);
    }
    parsed_free(&parsed);
}

static void test_precedence(void) {
    expect_expr("1 + 2 * 3", "(+ 1 (* 2 3))");
    expect_expr("a - b - c", "(- (- a b) c)");
    expect_expr("a & b == c", "(== (& a b) c)");
    expect_expr("a | b ^ c & d", "(| a (^ b (& c d)))");
    expect_expr("x << 1 + 2", "(<< x (+ 1 2))");
    expect_expr("a || b && c == d", "(|| a (&& b (== c d)))");
    expect_expr("!a && -b < ~c", "(&& (! a) (< (- b) (~ c)))");
    expect_expr("c ? a : b ? d : e", "(?: c a (?: b d e))");
    expect_expr("0..n + 1", "(.. 0 (+ n 1))");
    expect_expr("1..=10", "(..= 1 10)");
    expect_expr("(a + b) * c", "(* (+ a b) c)");
    expect_expr("(a < b) == c", "(== (< a b) c)");
}

static void test_casts(void) {
    expect_expr("x as f32", "(as x f32)");
    expect_expr("-x as f32", "(as (- x) f32)");
    expect_expr("a * b as f32", "(* a (as b f32))");
    expect_expr("x as i32 * 2", "(* (as x i32) 2)");
    expect_expr("p as u8*", "(as p u8*)");
    expect_expr("(p as u8*) + 4", "(+ (as p u8*) 4)");
    expect_expr("w as? Button*", "(as? w Button*)");
    expect_expr("x as bool ? a : b", "(?: (as x bool) a b)");
    expect_expr("x as i32?", "(as x i32?)");
}

static void test_postfix(void) {
    expect_expr("a.b.c(1, 2)[i]", "(index (call (. (. a b) c) 1 2) i)");
    expect_expr("*(p + 4)", "(* (+ p 4))");
    expect_expr("&x.y", "(& (. x y))");
    expect_expr("try parse(s)", "(try (call parse s))");
    expect_expr("[1, 2, 3,]", "(array 1 2 3)");
    expect_expr("[0; 64]", "(repeat 0 64)");
    expect_expr("[a + 1, f(x)][i]", "(index (array (+ a 1) (call f x)) i)");
    expect_expr("Point{ x: 3.0, y: 4.0 }", "(lit Point (x 3.0) (y 4.0))");
    expect_expr("geo.Point{ x: 1, }", "(lit geo.Point (x 1))");
    expect_expr("arena.new(Button, \"OK\")", "(call (. arena new) Button \"OK\")");
    expect_expr("self.x + base.y", "(+ (. self x) (. base y))");
    expect_expr("s.bytes[0] == 'a'", "(== (index (. s bytes) 0) 'a')");
    expect_expr("match (s) { Circle(r) => r * r, Empty => 0.0, _ => 1.0 }",
                "(match s\n  (arm Circle(r) (* r r))\n  (arm Empty 0.0)\n  (arm _ 1.0))");
}

static void test_statements(void) {
    expect_module("i32 main() uses io {\n"
                  "    i32 n = 3;\n"
                  "    mut i32 total = 0;\n"
                  "    for (i32 i in 0..n) { total += add(i, 1); }\n"
                  "    auto p = Point{ x: 3.0, y: 4.0 };\n"
                  "    return 0;\n"
                  "}\n",
                  "(fn i32 main () (uses io)\n"
                  "  (block\n"
                  "    (var i32 n 3)\n"
                  "    (var mut i32 total 0)\n"
                  "    (for-in i32 i (.. 0 n)\n"
                  "      (block\n"
                  "        (+= total (call add i 1))))\n"
                  "    (var auto p (lit Point (x 3.0) (y 4.0)))\n"
                  "    (return 0)))\n");
    expect_module("void f() {\n"
                  "    if (a) { x = 1; } else if (b) { x = 2; } else { x = 3; }\n"
                  "    while (x < 10) { x *= 2; continue; }\n"
                  "    for (mut i32 i = 0; i < 3; i += 1) { break; }\n"
                  "    for (;;) { return; }\n"
                  "    defer arena.free();\n"
                  "    unsafe { u8 first = *(q + 4); }\n"
                  "}\n",
                  "(fn void f ()\n"
                  "  (block\n"
                  "    (if a\n"
                  "      (block\n"
                  "        (= x 1))\n"
                  "      (if b\n"
                  "        (block\n"
                  "          (= x 2))\n"
                  "        (block\n"
                  "          (= x 3))))\n"
                  "    (while (< x 10)\n"
                  "      (block\n"
                  "        (*= x 2)\n"
                  "        (continue)))\n"
                  "    (for (var mut i32 i 0) (< i 3) (+= i 1)\n"
                  "      (block\n"
                  "        (break)))\n"
                  "    (for _ _ _\n"
                  "      (block\n"
                  "        (return)))\n"
                  "    (defer (call (. arena free)))\n"
                  "    (unsafe (block\n"
                  "      (var u8 first (* (+ q 4)))))))\n");
    expect_module("void f() { Point* p = a.alloc(Point); u8[4] b = x; i32!E r = g(); fn(i32) -> i32 uses io h = k; }",
                  "(fn void f ()\n"
                  "  (block\n"
                  "    (var Point* p (call (. a alloc) Point))\n"
                  "    (var u8[4] b x)\n"
                  "    (var i32!E r (call g))\n"
                  "    (var fn(i32) -> i32 uses io h k)))\n");
}

static void test_declarations(void) {
    expect_module("import std.io;\n"
                  "pub struct Point { f32 x; pub mut f32 y = 0.0; f32 length() { return x; } }\n",
                  "(import std.io)\n"
                  "(struct pub Point\n"
                  "  (field f32 x)\n"
                  "  (field pub mut f32 y 0.0)\n"
                  "  (fn f32 length ()\n"
                  "    (block\n"
                  "      (return x))))\n");
    expect_module("interface Drawable { void draw(Render* r) uses io; }\n"
                  "class Button : Widget, Drawable {\n"
                  "    str label;\n"
                  "    virtual void on_click() uses io { }\n"
                  "    override void draw(Render* r) uses io { }\n"
                  "}\n",
                  "(interface Drawable\n"
                  "  (fn void draw ((Render* r)) (uses io)))\n"
                  "(class Button (: Widget Drawable)\n"
                  "  (field str label)\n"
                  "  (fn virtual void on_click () (uses io)\n"
                  "    (block))\n"
                  "  (fn override void draw ((Render* r)) (uses io)\n"
                  "    (block)))\n");
    expect_module("enum Shape { Circle(f32 radius), Rect(f32 w, f32 h), Empty, }",
                  "(enum Shape\n"
                  "  Circle((f32 radius))\n"
                  "  Rect((f32 w) (f32 h))\n"
                  "  Empty)\n");
    expect_module("i32 clamp(i32 x, i32 lo, i32 hi)\n"
                  "    requires lo <= hi\n"
                  "    ensures result >= lo && result <= hi\n"
                  "{ return x; }\n",
                  "(fn i32 clamp ((i32 x) (i32 lo) (i32 hi))\n"
                  "  (requires (<= lo hi))\n"
                  "  (ensures (&& (>= result lo) (<= result hi)))\n"
                  "  (block\n"
                  "    (return x)))\n");
    expect_module("extern \"C\" { i32 puts(u8* s); }\n"
                  "unsafe void poke(mut u8* p) { }\n"
                  "test \"works\" { assert(true); }\n",
                  "(extern \"C\"\n"
                  "  (fn i32 puts ((u8* s))))\n"
                  "(fn unsafe void poke ((mut u8* p))\n"
                  "  (block))\n"
                  "(test \"works\"\n"
                  "  (block\n"
                  "    (call assert true)))\n");
}

static void test_errors(void) {
    expect_error("void f() { x = 1 }", "E0200", 1, 18, 1);
    expect_error("void f() {\n    x = 1\n    y = 2;\n}", "E0200", 2, 10, 1);
    expect_error("void f() { g(; }", "E0201", 1, 14, 1);
    expect_error("void f() { Point* = 3; }", "E0201", 1, 19, 1);
    expect_error("void f() { auto x = 1 + ; }", "E0201", 1, 25, 1);
    expect_error("i32 x = 3;", "E0203", 1, 1, 1);
    expect_error("+ void", "E0203", 1, 1, 1);
    expect_error("void f() { if (a < b < c) { } }", "E0204", 1, 22, 1);
    expect_error("void f() { if (x = 1) { y = 2; } }", "E0205", 1, 18, 1);
    expect_error("void f() { a = b = c; }", "E0205", 1, 18, 1);
    expect_error("void f() { if (a) return; }", "E0206", 1, 19, 1);
    expect_error("void f() { while (a) x += 1; }", "E0206", 1, 22, 1);
    expect_error("void f() { i32 x; }", "E0207", 1, 16, 1);
    expect_error("void f() { auto g = (i32 a) => a + 1; }", "E0208", 1, 21, 1);
    expect_error("void f() { i32 a, b = 0; }", "E0209", 1, 17, 1);
    expect_error("void f() { p->x = 1; }", "E0210", 1, 13, 1);
    expect_error("void f() { i32 a[4] = x; }", "E0211", 1, 17, 1);
    expect_error("struct S { virtual void f() { } }", "E0212", 1, 12, 1);
    expect_error("pub import std.io;", "E0212", 1, 1, 1);
    expect_error("struct S : Base { }", "E0213", 1, 10, 1);
    expect_error("void f() { auto x = if (a) { 1 }; }", "E0201", 1, 21, 1);
}

static void test_recovery(void) {
    /* One error per broken statement, and the following ones are still parsed. */
    expect_error("void f() {\n    x = ;\n    y = 2;\n    z = );\n}\nvoid g() { }", "E0201", 2, 9, 2);
    expect_error("struct A { i32 }\nstruct B { f32 x; }\nvoid g() { h(; }", "E0200", 1, 16, 2);
    /* Lexer errors do not cascade into parser errors. */
    expect_error("void f() { x++; y = #; }", "E0110", 1, 13, 2);
}

/* ---- .ui.sosu files: what they become, and their errors ---- */

static void parse_ui(Parsed *out, const char *text) {
    memset(out, 0, sizeof *out);
    source_from_string(&out->file, "test.ui.sosu", text);
    lex_file(&out->file, &out->tokens, &out->diags);
    Module *module = parse_ui_module(&out->file, &out->tokens, &out->arena, &out->diags);
    out->dump = capture(print_module, module, &out->file);
}

static void expect_ui(const char *text, const char *expected) {
    Parsed parsed;
    parse_ui(&parsed, text);
    CHECK(parsed.diags.count == 0, "`%s`: unexpected %s: %s", text, parsed.diags.count ? parsed.diags.items[0].code : "",
          parsed.diags.count ? parsed.diags.items[0].message : "");
    CHECK(strcmp(parsed.dump, expected) == 0, "`%s`:\n  got\n%s  expected\n%s", text, parsed.dump, expected);
    parsed_free(&parsed);
}

static void expect_ui_error(const char *text, const char *code, size_t line, size_t column) {
    Parsed parsed;
    parse_ui(&parsed, text);
    CHECK(parsed.diags.count == 1, "`%s`: %zu diagnostics, expected 1", text, parsed.diags.count);
    if (parsed.diags.count > 0) {
        const Diagnostic *d = &parsed.diags.items[0];
        size_t l, c;
        source_position(&parsed.file, d->offset, &l, &c);
        CHECK(strcmp(d->code, code) == 0 && l == line && c == column, "`%s`: expected %s at %zu:%zu, got %s at %zu:%zu (%s)",
              text, code, line, column, d->code, l, c, d->message);
    }
    parsed_free(&parsed);
}

static void test_ui_files(void) {
    expect_ui("window W(S* s) {\n    title: \"Hi\"\n    Column(gap: 4) {\n        Text(\"a\")\n"
              "        Button(\"OK\", on_click: s.ok, enabled: s.ready)\n    }\n}\n",
              "(fn pub void W ((widgets.Frame* ui_frame) (S* s))\n"
              "  (block\n"
              "    (call (. ui_frame column) 4 (- 1.0))\n"
              "    (call (. ui_frame label) (call (. ui_frame tr) \"a\"))\n"
              "    (call (. ui_frame at) 80)\n"
              "    (if (call (. ui_frame button_enabled) (call (. ui_frame tr) \"OK\") (. s ready))\n"
              "      (block\n"
              "        (call (. s ok))))\n"
              "    (call (. ui_frame end))))\n"
              "(fn pub widgets.Window WConfig ()\n"
              "  (block\n"
              "    (return (lit widgets.Window (title \"Hi\")))))\n"
              "(import ui.widgets)\n");
    expect_ui("import ui.widgets;\ncomponent C(T* t) uses io {\n    if (t.on) {\n        Checkbox(bind: t.on)\n"
              "    } else {\n        TextField(bind: t.text, width: 90.5, on_enter: t.go(1))\n    }\n"
              "    for (i32 i in 0..t.n) {\n        Space(4)\n        other.Row(t, i)\n    }\n}\n",
              "(import ui.widgets)\n"
              "(fn pub void C ((widgets.Frame* ui_frame) (T* t)) (uses io)\n"
              "  (block\n"
              "    (if (. t on)\n"
              "      (block\n"
              "        (call (. ui_frame at) 71)\n"
              "        (call (. ui_frame checkbox_alone) (& (. t on))))\n"
              "      (block\n"
              "        (call (. ui_frame at) 113)\n"
              "        (if (call (. ui_frame text_field) (& (. t text)) 90.5)\n"
              "          (block\n"
              "            (call (. t go) 1)))))\n"
              "    (call (. ui_frame push_id) 179)\n"
              "    (for-in i32 i (.. 0 (. t n))\n"
              "      (block\n"
              "        (call (. ui_frame push_item))\n"
              "        (call (. ui_frame space) 4)\n"
              "        (call (. ui_frame push_id) 228)\n"
              "        (call (. other Row) ui_frame t i)\n"
              "        (call (. ui_frame pop_id))\n"
              "        (call (. ui_frame pop_id))))\n"
              "    (call (. ui_frame pop_id))))\n");
    expect_ui("component C(T* t) {\n    TextField(bind: t.date, mask: \"99/99/9999\", accept: Digits, max: 10)\n}\n",
              "(fn pub void C ((widgets.Frame* ui_frame) (T* t))\n"
              "  (block\n"
              "    (call (. ui_frame at) 24)\n"
              "    (call (. ui_frame text_field_with) (& (. t date)) 200.0 "
              "(lit widgets.Format (accept (. (. widgets Accept) Digits)) (max 10) (mask \"99/99/9999\")))))\n"
              "(import ui.widgets)\n");
    expect_ui("component C(T* t) {\n    Radio(\"S\", bind: t.size, value: Small)\n}\n",
              "(fn pub void C ((widgets.Frame* ui_frame) (T* t))\n"
              "  (block\n"
              "    (call (. ui_frame at) 24)\n"
              "    (if (&& (call (. ui_frame radio) (call (. ui_frame tr) \"S\") (== (. t size) Small)) (!= (. t size) Small))\n"
              "      (block\n"
              "        (= (. t size) Small)))))\n"
              "(import ui.widgets)\n");
    expect_ui_error("window W() {\n    Radio(\"S\", bind: x)\n}\n", "E0215", 2, 5);  /* no value */
    expect_ui("component C(T* t) {\n    List(height: 120, selected: t.index) {\n        Text(\"a\")\n    }\n"
              "    List { }\n}\n",
              "(fn pub void C ((widgets.Frame* ui_frame) (T* t))\n"
              "  (block\n"
              "    (call (. ui_frame at) 24)\n"
              "    (call (. ui_frame list) 300.0 120 (call some (& (. t index))))\n"
              "    (call (. ui_frame label) (call (. ui_frame tr) \"a\"))\n"
              "    (call (. ui_frame end_list))\n"
              "    (call (. ui_frame at) 91)\n"
              "    (call (. ui_frame list) 300.0 200.0 none)\n"
              "    (call (. ui_frame end_list))))\n"
              "(import ui.widgets)\n");
    expect_ui("window W(T* t) {\n    TitleBar(height: 36) {\n        Text(\"App\")\n"
              "        Dropdown(bind: t.mode, on_change: t.rebuild) {\n            Option(\"D\", value: Debug)\n"
              "            Option(\"R\", value: Release)\n        }\n    }\n    Text(\"under\")\n}\n",
              "(fn pub void W ((widgets.Frame* ui_frame) (T* t))\n"
              "  (block\n"
              "    (call (. ui_frame title_bar) 36 8.0 4.0)\n"
              "    (call (. ui_frame label) (call (. ui_frame tr) \"App\"))\n"
              "    (call (. ui_frame at) 72)\n"
              "    (call (. ui_frame dropdown) 160.0)\n"
              "    (if (&& (call (. ui_frame option) (call (. ui_frame tr) \"D\") (== (. t mode) Debug)) (!= (. t mode) Debug))\n"
              "      (block\n"
              "        (= (. t mode) Debug)\n"
              "        (call (. t rebuild))))\n"
              "    (if (&& (call (. ui_frame option) (call (. ui_frame tr) \"R\") (== (. t mode) Release)) (!= (. t mode) Release))\n"
              "      (block\n"
              "        (= (. t mode) Release)\n"
              "        (call (. t rebuild))))\n"
              "    (call (. ui_frame end_dropdown))\n"
              "    (call (. ui_frame end_title_bar))\n"
              "    (call (. ui_frame label) (call (. ui_frame tr) \"under\"))))\n"
              "(fn pub widgets.Window WConfig ()\n"
              "  (block\n"
              "    (return (lit widgets.Window (custom_title_bar true)))))\n"
              "(import ui.widgets)\n");
    expect_ui_error("window W() {\n    Text(\"a\")\n    TitleBar { }\n}\n", "E0215", 3, 5);  /* not first */
    expect_ui_error("component C() {\n    TitleBar { }\n}\n", "E0215", 2, 5);
    expect_ui_error("window W() {\n    Column { TitleBar { } }\n}\n", "E0215", 2, 14);
    expect_ui_error("window W() {\n    Option(\"a\", value: A)\n}\n", "E0215", 2, 5);  /* not in a Dropdown */
    expect_ui_error("window W(T* t) {\n    Dropdown(bind: t.m) { Text(\"a\") }\n}\n", "E0215", 2, 27);
    expect_ui_error("window W(T* t) {\n    Dropdown(bind: t.m) { Option(\"a\") }\n}\n", "E0215", 2, 27);
    expect_ui_error("window W() {\n    Dropdown { }\n}\n", "E0215", 2, 5);  /* no bind */

    expect_ui("window W(T* t) {\n    icon: \"app.png\"\n    Image(t.logo, size: 20)\n    Image(t.logo)\n}\n",
              "(fn pub void W ((widgets.Frame* ui_frame) (T* t))\n"
              "  (block\n"
              "    (call (. ui_frame image) (. t logo) 20)\n"
              "    (call (. ui_frame image) (. t logo) 0.0)))\n"
              "(fn pub widgets.Window WConfig ()\n"
              "  (block\n"
              "    (return (lit widgets.Window (icon \"app.png\")))))\n"
              "(import ui.widgets)\n");
    expect_ui_error("window W(T* t) {\n    Image()\n}\n", "E0215", 2, 5);
    expect_ui("component C(T* t) {\n    MenuBar {\n        Menu(\"File\") {\n"
              "            MenuItem(\"New\", on_click: t.new, shortcut: \"Ctrl+N\")\n            Separator\n"
              "            Menu(\"Recent\") {\n                MenuItem(\"a\", enabled: t.ok)\n            }\n"
              "            MenuItem(\"Quit\")\n        }\n    }\n"
              "    List { }\n    ContextMenu {\n        MenuItem(\"Remove\", on_click: t.remove, shortcut: \"Delete\", "
              "enabled: t.ok)\n    }\n}\n",
              "(fn pub void C ((widgets.Frame* ui_frame) (T* t))\n"
              "  (block\n"
              "    (call (. ui_frame menu_bar))\n"
              "    (call (. ui_frame at) 42)\n"
              "    (call (. ui_frame menu) (call (. ui_frame tr) \"File\"))\n"
              "    (if (call (. ui_frame menu_item_with) (call (. ui_frame tr) \"New\") \"Ctrl+N\" true)\n"
              "      (block\n"
              "        (call (. t new))))\n"
              "    (call (. ui_frame separator))\n"
              "    (call (. ui_frame at) 156)\n"
              "    (call (. ui_frame menu) (call (. ui_frame tr) \"Recent\"))\n"
              "    (call (. ui_frame menu_item_enabled) (call (. ui_frame tr) \"a\") (. t ok))\n"
              "    (call (. ui_frame end_menu))\n"
              "    (call (. ui_frame menu_item) (call (. ui_frame tr) \"Quit\"))\n"
              "    (call (. ui_frame end_menu))\n"
              "    (call (. ui_frame end_menu_bar))\n"
              "    (call (. ui_frame at) 281)\n"
              "    (call (. ui_frame list) 300.0 200.0 none)\n"
              "    (call (. ui_frame end_list))\n"
              "    (call (. ui_frame at) 294)\n"
              "    (call (. ui_frame context_menu))\n"
              "    (if (call (. ui_frame menu_item_with) (call (. ui_frame tr) \"Remove\") \"Delete\" (. t ok))\n"
              "      (block\n"
              "        (call (. t remove))))\n"
              "    (call (. ui_frame end_context_menu))))\n"
              "(import ui.widgets)\n");
    expect_ui_error("window W() {\n    MenuItem(\"a\")\n}\n", "E0215", 2, 5);  /* not in a menu */
    expect_ui_error("window W() {\n    Separator\n}\n", "E0215", 2, 5);
    expect_ui_error("window W() {\n    Menu(\"a\") { Text(\"b\") }\n}\n", "E0215", 2, 17);
    expect_ui_error("window W() {\n    MenuBar { Text(\"b\") }\n}\n", "E0215", 2, 15);
    expect_ui_error("window W() {\n    Menu { }\n}\n", "E0215", 2, 5);  /* no title */
    expect_ui_error("window W() {\n    ContextMenu { Column { } }\n}\n", "E0215", 2, 19);
    /* identities: a loop with `key:`, `focus:`, a `Scroll` */
    expect_ui("component C(T* t) {\n    for (Task* k in t.tasks, key: k.id) {\n        TextField(bind: k.name, focus: k.fresh)\n"
              "    }\n    Scroll(width: 300, height: 100) { }\n}\n",
              "(fn pub void C ((widgets.Frame* ui_frame) (T* t))\n"
              "  (block\n"
              "    (call (. ui_frame push_id) 24)\n"
              "    (for-in Task* k (. t tasks)\n"
              "      (block\n"
              "        (call (. ui_frame push_id) (as (. k id) u64))\n"
              "        (if (. k fresh)\n"
              "          (block\n"
              "            (call (. ui_frame focus_here))))\n"
              "        (call (. ui_frame at) 70)\n"
              "        (call (. ui_frame text_field) (& (. k name)) 200.0)\n"
              "        (call (. ui_frame pop_id))))\n"
              "    (call (. ui_frame pop_id))\n"
              "    (call (. ui_frame at) 120)\n"
              "    (call (. ui_frame scroll) 300 100)\n"
              "    (call (. ui_frame end_scroll))))\n"
              "(import ui.widgets)\n");
    expect_ui_error("window W(T* t) {\n    for (i32 i in 0..3, id: i) { }\n}\n", "E0215", 2, 25);  /* not key: */
    /* translations: literal texts, values in braces, plurals, contexts; data is not translated */
    expect_ui("component C(T* u) {\n    Text(\"Hello, {name}\", name: u.name)\n"
              "    Text(\"{count} file in {dir}\", plural: \"{count} files in {dir}\", count: u.n, dir: u.dir)\n"
              "    Button(\"Open\", context: \"verb\")\n    Text(u.title)\n}\n",
              "(fn pub void C ((widgets.Frame* ui_frame) (T* u))\n"
              "  (block\n"
              "    (call (. ui_frame label) (call (. ui_frame with1) (call (. ui_frame tr) \"Hello, {name}\") {name} (. u name)))\n"
              "    (call (. ui_frame label) (call (. ui_frame with2) (call (. ui_frame tr_plural) \"{count} file in {dir}\" "
              "\"{count} files in {dir}\" (as (. u n) i64)) {count} (call (. ui_frame decimal) (as (. u n) i64)) {dir} (. u dir)))\n"
              "    (call (. ui_frame at) 156)\n"
              "    (call (. ui_frame button) (call (. ui_frame tr_in) \"verb\" \"Open\"))\n"
              "    (call (. ui_frame label) (. u title))))\n"
              "(import ui.widgets)\n");
    expect_ui_error("component C(T* u) {\n    Text(\"Hi, {name}\")\n}\n", "E0215", 2, 15);  /* no value */
    expect_ui_error("component C(T* u) {\n    Text(\"Hi\", name: u.name)\n}\n", "E0215", 2, 16);  /* unused */
    expect_ui_error("component C(T* u) {\n    Text(\"a\", plural: \"b\")\n}\n", "E0215", 2, 23);  /* no count */
    expect_ui_error("component C(T* u) {\n    Text(\"a\", context: u.c)\n}\n", "E0215", 2, 24);
    expect_ui("component C(T* t) {\n    TextField(bind: t.q, placeholder: \"Search\")\n"
              "    Dropdown(bind: t.m, placeholder: \"Pick\") { }\n}\n",
              "(fn pub void C ((widgets.Frame* ui_frame) (T* t))\n"
              "  (block\n"
              "    (call (. ui_frame at) 24)\n"
              "    (call (. ui_frame text_field_with) (& (. t q)) 200.0 (lit widgets.Format (placeholder (call (. ui_frame tr) \"Search\"))))\n"
              "    (call (. ui_frame at) 72)\n"
              "    (call (. ui_frame dropdown_with) 160.0 (call (. ui_frame tr) \"Pick\"))\n"
              "    (call (. ui_frame end_dropdown))))\n"
              "(import ui.widgets)\n");
    expect_ui_error("window W() {\n    Scroll(height: 100) { }\n}\n", "E0215", 2, 5);  /* no width */

    expect_ui_error("window W() {\n    42\n}\n", "E0214", 2, 5);
    expect_ui_error("window W() {\n    Button(\"OK\", on_tap: f)\n}\n", "E0215", 2, 18);
    expect_ui_error("window W() {\n    Text(\"a\", \"b\")\n}\n", "E0215", 2, 5);
    expect_ui_error("window W() {\n    TextField(width: 9)\n}\n", "E0215", 2, 5);
    expect_ui_error("window W() {\n    Text(\"a\")\n    title: \"late\"\n}\n", "E0215", 3, 5);
    expect_ui_error("component C() {\n    width: 3\n}\n", "E0215", 2, 5);
    expect_ui_error("window W() {\n    Row(align: End) { }\n}\n", "E0216", 2, 9);
    expect_ui_error("window W() {\n    Mine(size: 2)\n}\n", "E0215", 2, 10);
    expect_ui_error("window W() {\n    Text(\"a\") { }\n}\n", "E0215", 2, 15);
    expect_ui_error("struct S { }\n", "E0203", 1, 1);
}

/* `theme` and `style` blocks, and `style:` on nodes (spec §10.7). */
static void test_ui_themes(void) {
    expect_ui("style Danger {\n    background: #F38BA8\n    radius: 6\n    hover { background: #EBA0AC, border_width: 2 }\n}\n",
              "(fn pub widgets.Style Danger ()\n"
              "  (block\n"
              "    (return (lit widgets.Style (normal (lit widgets.Props (background (call some (call (. widgets rgb) 243 139 168 255))) (radius (call some 6)))) (hover (lit widgets.Props (background (call some (call (. widgets rgb) 235 160 172 255))) (border_width (call some 2))))))))\n"
              "(import ui.widgets)\n");
    expect_ui("theme Dark {\n    accent: #89B4FA\n    surface: #313244\n    Window { background: #1E1E2E }\n"
              "    Button {\n        background: surface\n        focus { border: accent }\n    }\n}\n"
              "theme Light : Dark {\n    surface: #CCD0DA\n    Window { background: #EFF1F5 }\n}\n",
              "(fn pub widgets.Theme Dark ()\n"
              "  (block\n"
              "    (var auto accent (call (. widgets rgb) 137 180 250 255))\n"
              "    (var auto surface (call (. widgets rgb) 49 50 68 255))\n"
              "    (return (lit widgets.Theme (all (lit widgets.Props (accent (call some accent)))) (window (lit widgets.Style (normal (lit widgets.Props (background (call some (call (. widgets rgb) 30 30 46 255))))))) (button (lit widgets.Style (normal (lit widgets.Props (background (call some surface)))) (focus (lit widgets.Props (border (call some accent))))))))))\n"
              "(fn pub widgets.Theme Light ()\n"
              "  (block\n"
              "    (var auto accent (call (. widgets rgb) 137 180 250 255))\n"
              "    (var auto surface (call (. widgets rgb) 204 208 218 255))\n"
              "    (return (lit widgets.Theme (all (lit widgets.Props (accent (call some accent)))) (window (call (. widgets merge) (lit widgets.Style (normal (lit widgets.Props (background (call some (call (. widgets rgb) 30 30 46 255)))))) (lit widgets.Style (normal (lit widgets.Props (background (call some (call (. widgets rgb) 239 241 245 255)))))))) (button (lit widgets.Style (normal (lit widgets.Props (background (call some surface)))) (focus (lit widgets.Props (border (call some accent))))))))))\n"
              "(import ui.widgets)\n");
    expect_ui("window W(S* s) {\n    Column(style: Card) {\n        Text(\"a\", style: s.late ? theme.Danger : Normal)\n"
              "        Button(\"b\", style: [Card, s.mine])\n    }\n}\n",
              "(fn pub void W ((widgets.Frame* ui_frame) (S* s))\n"
              "  (block\n"
              "    (call (. ui_frame style) (call Card))\n"
              "    (call (. ui_frame column) (- 1.0) (- 1.0))\n"
              "    (call (. ui_frame style) (?: (. s late) (call (. theme Danger)) (lit widgets.Style)))\n"
              "    (call (. ui_frame label) (call (. ui_frame tr) \"a\"))\n"
              "    (call (. ui_frame style) (call Card))\n"
              "    (call (. ui_frame style) (. s mine))\n"
              "    (call (. ui_frame at) 108)\n"
              "    (call (. ui_frame button) (call (. ui_frame tr) \"b\"))\n"
              "    (call (. ui_frame end))))\n"
              "(fn pub widgets.Window WConfig ()\n"
              "  (block\n"
              "    (return (lit widgets.Window))))\n"
              "(import ui.widgets)\n");
    expect_ui_error("style S { colour: #FFFFFF }\n", "E0215", 1, 11);
    expect_ui_error("style S { active { color: #FFFFFF } }\n", "E0215", 1, 11);
    expect_ui_error("style S { radius: 2, radius: 3 }\n", "E0215", 1, 22);
    expect_ui_error("theme T { Label { color: #FFFFFF } }\n", "E0215", 1, 11);
    expect_ui_error("theme T : Missing { }\n", "E0215", 1, 11);
    expect_ui_error("theme T { a: 1\n    a: 2 }\n", "E0215", 2, 5);
    expect_ui_error("window W() {\n    Text(\"a\", style: c ? [A, B] : A)\n}\n", "E0215", 2, 26);
}

int main(void) {
    test_ui_files();
    test_ui_themes();
    test_precedence();
    test_casts();
    test_postfix();
    test_statements();
    test_declarations();
    test_errors();
    test_recovery();
    return test_summary("parser tests");
}
