#include <stdlib.h>
#include <string.h>

#include "sema.h"
#include "test.h"

/* Run from the repository root: the standard library is in lib/. */

static void codes_of(const DiagList *diags, char *codes, size_t size) {
    codes[0] = '\0';
    for (size_t i = 0; i < diags->count && strlen(codes) + 8 < size; i++) {
        if (i) strcat(codes, " ");
        strcat(codes, diags->items[i].code);
    }
}

/* Loads `text` as a module, checks it, and compares the error codes ("" for none). */
static void expect_codes(const char *text, const char *expected) {
    Program program;
    bool loaded = program_load_text(&program, text, "lib");
    CHECK(loaded, "`%s`: does not load: %s", text, program.diags.count ? program.diags.items[0].message : "");
    if (loaded) {
        Sema sema;
        sema_check(&sema, &program);
        sema_free(&sema);
    }
    char codes[256];
    codes_of(&program.diags, codes, sizeof codes);
    CHECK(strcmp(codes, expected) == 0, "`%s`:\n  got [%s], expected [%s]%s%s", text, codes, expected,
          program.diags.count ? "\n  first: " : "", program.diags.count ? program.diags.items[0].message : "");
    program_free(&program);
}

/* Same, and checks the help of the first diagnostic. */
static void expect_help(const char *text, const char *code, const char *help) {
    Program program;
    program_load_text(&program, text, "lib");
    Sema sema;
    sema_check(&sema, &program);
    sema_free(&sema);
    const Diagnostic *d = program.diags.count ? &program.diags.items[0] : NULL;
    CHECK(d && strcmp(d->code, code) == 0, "`%s`: expected %s", text, code);
    CHECK(d && d->help && strcmp(d->help, help) == 0, "`%s`: help is `%s`, expected `%s`", text,
          d && d->help ? d->help : "(none)", help);
    program_free(&program);
}

static void expect_package(const char *dir, const char *expected) {
    Program program;
    bool loaded = program_load(&program, dir, "lib");
    CHECK(loaded, "%s does not load", dir);
    if (loaded) {
        Sema sema;
        sema_check(&sema, &program);
        sema_free(&sema);
    }
    char codes[256];
    codes_of(&program.diags, codes, sizeof codes);
    CHECK(strcmp(codes, expected) == 0, "%s: got [%s], expected [%s]%s%s", dir, codes, expected,
          program.diags.count ? "; first: " : "", program.diags.count ? program.diags.items[0].message : "");
    program_free(&program);
}

static void test_valid(void) {
    expect_codes("", "");
    expect_codes("struct Point { f32 x; f32 y; f32 length() { return x; } }\n"
                 "enum ParseError { Empty, NotANumber(str text) }\n"
                 "i32!ParseError parse(str s) { return ok(0); }\n"
                 "i32? find(i32[] items, i32 target) { return none; }\n"
                 "void apply(fn(i32) -> i32 uses io f, u8[4] bytes, Point*? p) { }\n"
                 "struct Node { i32 value; Node* next; }\n",
                 "");
    expect_codes("struct Render { }\n"
                 "interface Drawable { void draw(Render* r) uses io; }\n"
                 "class Widget : Drawable {\n"
                 "    f32 x;\n"
                 "    virtual void on_click() uses io { }\n"
                 "    override void draw(Render* r) uses io { }\n"
                 "}\n"
                 "class Button : Widget {\n"
                 "    str label;\n"
                 "    override void on_click() { }\n"
                 "}\n"
                 "void show(Drawable* d, Widget* w) { }\n",
                 "");
    expect_codes("import std.io;\nvoid log(str message) uses io, time { }\n", "");
}

static void test_names(void) {
    expect_codes("struct A { }\nenum A { X }", "E0301");
    expect_codes("void f() { }\nvoid f() { }", "E0301");
    expect_codes("struct i32 { }", "E0301");
    expect_codes("import std.io;\nstruct io { }", "E0302");
    expect_codes("struct A { Pont p; }", "E0303");
    expect_help("struct A { i33 x; }", "E0303", "did you mean `i32`?");
    expect_help("struct Point { }\nstruct A { Pont p; }", "E0303", "did you mean `Point`?");
    expect_codes("struct A { a.B x; }", "E0303");
    expect_codes("import std.io;\nstruct A { io.File f; }", "E0303");
    expect_codes("import std.io;\nstruct A { std.io.File f; }", "E0303");
    expect_codes("void f() { }\nstruct A { f x; }", "E0304");
    expect_codes("import std.io;\nstruct A { io x; }", "E0304");
}

static void test_types(void) {
    expect_codes("struct A { void x; }", "E0306");
    expect_codes("void f(void x) { }", "E0306");
    expect_codes("struct E { }\ni32!E f() { return 0; }", "E0307");
    expect_codes("i32!str f() { return 0; }", "E0307");
    expect_codes("struct A { i32[0] x; }", "E0308");
    expect_codes("struct A { A a; }", "E0309");
    expect_codes("struct A { B b; }\nstruct B { A? a; }", "E0309");
    expect_codes("struct A { A[2] items; }", "E0309");
    expect_codes("enum List { Cons(i32 value, List next), Nil }", "E0309");
    expect_codes("enum List { Cons(i32 value, List* next), Nil }", "");
    expect_codes("void f() uses disk { }", "E0320");
    expect_codes("class W { }\nstruct A { W w; }", "E0321");
    expect_codes("class W { }\nvoid f(W w) { }", "E0321");
    expect_codes("interface I { }\nI make() { return none; }", "E0321");
    expect_codes("class W { }\nvoid f(W* w, W*? maybe, W*[] all) { }", "");
}

static void test_classes(void) {
    expect_codes("struct S { }\nclass C : S { }", "E0310");
    expect_codes("interface I { }\nclass P { }\nclass C : I, P { }", "E0310");
    expect_codes("class A : B { }\nclass B : A { }", "E0311");
    expect_codes("class A : A { }", "E0311");
    expect_codes("class A { override void f() { } }", "E0313");
    expect_codes("class A { virtual void f() { } }\nclass B : A { void f() { } }", "E0314");
    expect_codes("class A { virtual void f() { } }\nclass B : A { virtual void f() { } }", "E0314");
    expect_codes("class A { void f() { } }\nclass B : A { override void f() { } }", "E0315");
    expect_codes("class A { virtual void f(i32 x) { } }\nclass B : A { override void f(i64 x) { } }", "E0316");
    expect_codes("class A { virtual i32 f() { return 0; } }\nclass B : A { override void f() { } }", "E0316");
    expect_codes("class A { virtual void f() { } }\nclass B : A { override void f() uses io { } }", "E0317");
    expect_codes("class A { virtual void f() uses io { } }\nclass B : A { override void f() { } }", "");
    expect_codes("interface I { void f(); }\nclass C : I { }", "E0318");
    expect_codes("interface I { void f(); }\nclass P { virtual void f() { } }\nclass C : P, I { }", "");
    expect_codes("interface I { void f(); }\nclass C : I { void f() { } }", "E0314");
    expect_codes("class A { virtual void f() { } }\nclass B : A { }\nclass C : B { override void f() { } }", "");
}

static void test_duplicates(void) {
    expect_codes("struct A { i32 x; f32 x; }", "E0319");
    expect_codes("struct A { i32 x; void x() { } }", "E0319");
    expect_codes("class A { i32 x; }\nclass B : A { i32 x; }", "E0319");
    expect_codes("enum E { X, X }", "E0319");
    expect_codes("enum E { X(i32 a, i32 a) }", "E0319");
    expect_codes("void f(i32 a, i32 a) { }", "E0319");
    expect_codes("interface I { }\nclass C : I, I { }", "E0319");
}

static void test_valid_bodies(void) {
    expect_codes("import std.io;\n"
                 "enum Shape { Circle(f32 radius), Rect(f32 w, f32 h), Empty, }\n"
                 "f32 area(Shape s) {\n"
                 "    return match (s) {\n"
                 "        Circle(r) => 3.14159 * r * r,\n"
                 "        Rect(w, h) => w * h,\n"
                 "        Empty => 0.0,\n"
                 "    };\n"
                 "}\n"
                 "Shape unit() { return Circle(1.0); }\n"
                 "Shape nothing() { return Shape.Empty; }\n"
                 "enum ParseError { Empty, NotANumber }\n"
                 "i32!ParseError parse(str s) {\n"
                 "    if (s.len == 0) { return err(Empty); }\n"
                 "    return ok(42);\n"
                 "}\n"
                 "i32!ParseError twice(str s) {\n"
                 "    i32 n = try parse(s);\n"
                 "    return ok(n * 2);\n"
                 "}\n"
                 "void!ParseError validate(str s) { i32 n = try parse(s); return ok(); }\n"
                 "i32? find(i32[] items, i32 target) {\n"
                 "    for (mut usize i = 0; i < items.len; i += 1) {\n"
                 "        if (items[i] == target) { return some(i as i32); }\n"
                 "    }\n"
                 "    return none;\n"
                 "}\n"
                 "i32 clamp(i32 x, i32 lo, i32 hi)\n"
                 "    requires lo <= hi\n"
                 "    ensures result >= lo && result <= hi\n"
                 "{\n"
                 "    if (x < lo) { return lo; }\n"
                 "    if (x > hi) { return hi; }\n"
                 "    return x;\n"
                 "}\n"
                 "test \"clamp stays in range\" { assert(clamp(15, 0, 10) == 10); }\n"
                 "i32 sum(i32[] values) {\n"
                 "    mut i32 total = 0;\n"
                 "    for (i32 v in values) { total += v; }\n"
                 "    for (i32 i in 0..10) { total += i; }\n"
                 "    return total;\n"
                 "}\n"
                 "struct Point { mut f32 x; f32 y; f32 length_squared() { return x * x + y * y; } }\n"
                 "void nudge(Point* p) { p.x = p.x + 1.0; }\n"
                 "void literals() uses io {\n"
                 "    u8 small = 255;\n"
                 "    i64 big = 3000000000;\n"
                 "    i8 negative = -128;\n"
                 "    auto inferred = 3;\n"
                 "    f32 f = 2.5;\n"
                 "    u32 mask = (0xFF & 0b1010) << 2;\n"
                 "    char c = 'a';\n"
                 "    i32 code = c as i32;\n"
                 "    bool flag = !(small > 3) || big == 0;\n"
                 "    i32 m = max(code, 10);\n"
                 "    auto ratio = f as f64 / 2.0;\n"
                 "    mut Point p = Point{ x: 3.0, y: 4.0 };\n"
                 "    p.x = 1.0;\n"
                 "    io.print(\"{} {} {}\", p.length_squared(), flag, ratio);\n"
                 "}\n"
                 "void raw(i32* p) { unsafe { u8* q = p as u8*; u8 first = *(q + 4); } }\n",
                 "");
    expect_codes("import std.io;\n"
                 "class Widget {\n"
                 "    f32 x;\n"
                 "    void init(f32 x) { self.x = x; }\n"
                 "    virtual void on_click() uses io { io.print(\"clicked at {}\", x); }\n"
                 "}\n"
                 "class Button : Widget {\n"
                 "    str label;\n"
                 "    void init(str label) { base.init(0.0); self.label = label; }\n"
                 "    override void on_click() uses io { base.on_click(); }\n"
                 "}\n"
                 "void use(Button* b) uses io {\n"
                 "    Widget* w = b as Widget*;\n"
                 "    w.on_click();\n"
                 "    Button*? back = w as? Button*;\n"
                 "}\n",
                 "");
}

static void test_body_errors(void) {
    expect_codes("void f() { g(); }", "E0330");
    expect_help("void f() { i32 count = 1; i32 x = cuont; }", "E0330", "did you mean `count`?");
    expect_codes("void f() { i32 x = true; }", "E0331");
    expect_help("void f() { i64 a = 1; i32 b = a; }", "E0331", "conversions are explicit: write `... as i32`");
    expect_codes("void f() { f32 x = 3; }", "E0331");
    expect_codes("void f() { u8 x = 256; }", "E0332");
    expect_codes("void f() { i8 x = -129; }", "E0332");
    expect_codes("void f() { auto x = 3000000000; }", "E0332");
    expect_codes("struct P { }\nvoid f() { auto x = P; }", "E0333");
    expect_codes("void f() { i32 a = 1; i64 b = 2; auto c = a + b; }", "E0334");
    expect_codes("void f() { auto c = true + 1; }", "E0334");
    expect_codes("void f() { str s = \"a\"; auto c = s[0]; }", "E0334");
    expect_codes("void f() { i32 x = 1; x = 2; }", "E0335");
    expect_codes("struct P { f32 x; }\nvoid f(P* p) { p.x = 1.0; }", "E0335");
    expect_codes("struct P { mut f32 x; }\nvoid f() { P p = P{ x: 1.0 }; p.x = 2.0; }", "E0335");
    expect_codes("i32 g(i32 a) { return a; }\nvoid f() { g(1, 2); }", "E0336");
    expect_codes("struct P { f32 x; }\nf32 f(P p) { return p.y; }", "E0337");
    expect_codes("struct P { }\nvoid f(P p) { p.go(); }", "E0337");
    expect_codes("void f() { i32 x = 1; x(); }", "E0338");
    expect_codes("void f() { bool b = 3.0 as bool; }", "E0339");
    expect_codes("import std.io;\nvoid f() { io.print(\"hi\"); }", "E0340");
    expect_help("void g() uses time { }\nvoid f() { g(); }", "E0340", "add `uses time` to the signature of `f` (spec §7.4)");
    expect_codes("unsafe void g() { }\nvoid f() { g(); }", "E0341");
    expect_codes("extern \"C\" { i32 puts(u8* s); }\nvoid f(u8* s) { puts(s); }", "E0341");
    expect_codes("void f(u8* p) { u8* q = p + 1; }", "E0341");
    expect_codes("void f(i32* p) { u8* q = p as u8*; }", "E0341");
    expect_codes("void f() { auto x = none; }", "E0342");
    expect_codes("void f() { auto r = ok(1); }", "E0342");
    expect_codes("enum E { A, B }\nvoid g() { }\nvoid f(E e) { match (e) { A => g(), C => g(), _ => g() } }", "E0343");
    expect_codes("void g() { }\nvoid f(i32 x) { match (x) { _ => g() } }", "E0343");

    /* a variant without its type, where an enum is expected (spec §10.2); `==` on enums without data */
    expect_codes("enum Size { Small, Large }\nstruct S { mut Size size; }\n"
                 "Size f(S* s, Size other) { s.size = Large; Size t = Small; bool same = other == Small; return Small; }",
                 "");
    expect_codes("enum Size { Small, Large }\nvoid f() { auto x = Small; }", "E0330");  /* nothing expected */
    expect_codes("enum Size { Small, Large }\nvoid f() { Size x = Medium; }", "E0330");
    expect_codes("enum Shape { Circle(f32 r), Dot }\nvoid f() { Shape s = Circle; }", "E0336");
    expect_codes("enum E { A }\nvoid g() { }\nvoid f(E e) { match (e) { A => g(), _ => g() } }", "E0343");
    expect_codes("enum E { A(i32 x) }\nvoid g() { }\nvoid f(E e) { match (e) { A => g() } }", "E0343");
    expect_codes("enum E { A, B }\ni32 f(E e) { return match (e) { A => 1, B => { } }; }", "E0343");
    expect_codes("enum E { A, B, C }\ni32 f(E e) { return match (e) { A => 1, B => 2 }; }", "E0312");
    expect_codes("i32 f(i32? x) { return match (x) { some(v) => v }; }", "E0312");
    expect_codes("void f() { break; }", "E0344");
    expect_codes("void f() { defer { return; } }", "E0344");
    expect_codes("i32 g() { return 1; }\nvoid f() { auto x = try g(); }", "E0345");
    expect_codes("enum A { X }\nenum B { Y }\ni32!A g() { return ok(1); }\ni32!B f() { i32 x = try g(); return ok(x); }",
                 "E0345");
    expect_codes("i32 f(bool c) { if (c) { return 1; } }", "E0346");
    expect_codes("void f() { return 1; }", "E0346");
    expect_codes("i32 f() { return; }", "E0346");
    expect_codes("i32 f() { while (true) { break; } }", "E0346");
    expect_codes("i32 f() { while (true) { } }", "");
    expect_codes("void f(str s) { auto c = s.chars(); }", "E0352");
    expect_codes("void f(str s) { for (char c in s.chars()) { } }", "");
    expect_codes("void f(str s) { for (u8 c in s.chars()) { } }", "E0331");
    expect_codes("void f(i32 x) { i32 x = 2; }", "E0347");
    expect_codes("void f() { i32 a = 1; { i32 a = 2; } }", "E0347");
    expect_codes("void f() { i32 a = 1; } void g() { i32 a = 2; }", "");
    expect_codes("void f(i32 x) { x == 1; }", "E0348");
    expect_codes("enum E { A }\ni32!E g() { return ok(1); }\nvoid f() { g(); }", "E0348");
    expect_codes("void f() { auto s = self; }", "E0349");
    expect_codes("void f() { auto r = result; }", "E0330");          /* contextual: only in `ensures` */
    expect_codes("i32 f() { i32 result = 3; return result; }", "");
    expect_codes("void f() ensures result > 0 { }", "E0349");
    expect_codes("struct P { f32 x; f32 y; }\nvoid f() { P p = P{ x: 1.0 }; }", "E0350");
    expect_codes("struct P { f32 x; }\nvoid f() { P p = P{ x: 1.0, z: 2.0 }; }", "E0350");
    expect_codes("struct P { f32 x; f32 y = 0.0; }\nvoid f() { P p = P{ x: 1.0 }; }", "");
    expect_codes("import std.io;\nvoid f() uses io { io.print(\"{} {}\", 1); }", "E0351");
    expect_codes("import std.io;\nstruct P { }\nvoid f(P p) uses io { io.print(\"{}\", p); }", "E0351");
    expect_codes("void f() { auto r = 0..10; }", "E0352");
    expect_codes("void f(str s) { for (char c in s) { } }", "E0352");
    expect_codes("i32 g() { return 1; }\nvoid f() { i32* p = &g(); }", "E0353");

    /* arenas */
    expect_codes("struct P { f32 x; }\nclass C { i32 n; void init(i32 n) { self.n = n; } }\n"
                 "void f() {\n"
                 "    Arena a = Arena.new(64 * KB);\n"
                 "    defer a.free();\n"
                 "    P* p = a.alloc(P);\n"
                 "    P*? q = a.try_alloc(P);\n"
                 "    C* c = a.new(C, 3);\n"
                 "    i32* n = a.alloc(i32);\n"
                 "}\n",
                 "");
    expect_codes("struct P { }\nvoid f(Arena* a) { P* p = a.alloc(3); }", "E0354");
    expect_codes("struct N { N* next; }\nvoid f(Arena* a) { N* n = a.alloc(N); }", "E0354");
    expect_codes("class C { }\nvoid f(Arena* a) { C* c = a.alloc(C); }", "E0354");
    expect_codes("class C { void init(i32 n) { } }\nvoid f(Arena* a) { C* c = a.new(C); }", "E0336");
    expect_codes("class C { }\nvoid f(Arena* a) { C* c = a.new(C, 1); }", "E0336");
    expect_codes("void f(Arena* a) { a.reset(); }", "E0337");
    expect_codes("void f() { auto x = MB(); }", "E0338");

    /* text (spec §4.1) and the prelude */
    expect_codes("void f(Arena* a, u8[] data) {\n"
                 "    u8* c = \"abc\".to_cstr(a);\n"
                 "    str!Utf8Error text = str.from_bytes(data);\n"
                 "    u8[] buffer = a.alloc_slice(u8, 16);\n"
                 "}\n",
                 "");
    expect_codes("void f(u8[] data) { auto c = \"abc\".to_cstr(data); }", "E0336");
    expect_codes("void f() { auto t = str.from_bytes(\"abc\"); }", "E0331");
    expect_codes("void f(Arena* a) { auto s = a.alloc_slice(u8); }", "E0336");
    expect_codes("enum Utf8Error { Mine }\nUtf8Error f() { return Mine; }", "");

    /* array literals */
    expect_codes("void f() { i32[3] a = [1, 2, 3]; u8[64] b = [0; 64]; auto c = [1.5, 2.5]; f64 x = c[1]; }", "");
    expect_codes("void f() { auto grid = [[1, 2], [3, 4]]; i32 x = grid[1][0]; }", "");
    expect_codes("void f() { i32[3] a = [1, 2]; }", "E0355");
    expect_codes("void f() { auto a = []; }", "E0308");
    expect_codes("void f() { auto a = [1, 2.0]; }", "E0331");

    /* `&x` needs `mut x` */
    expect_codes("void f() { i32 x = 1; i32* p = &x; }", "E0335");
    expect_codes("void f() { mut i32 x = 1; i32* p = &x; }", "");
    expect_codes("struct P { f32 x; }\nvoid f(P* p) { f32* q = &p.x; }", "E0335");

    /* moves of values owning a resource */
    expect_codes("void f() { Arena a = Arena.new(1 * KB); Arena b = a; a.free(); }", "E0356");
    expect_codes("void f() { mut Arena a = Arena.new(1 * KB); Arena b = a; a = Arena.new(1 * KB); a.free(); b.free(); }", "");
    expect_codes("void g(Arena a) { }\nvoid f() { Arena a = Arena.new(1 * KB); while (true) { g(a); } }", "E0356");
    expect_codes("struct H { Arena a; }\nvoid f(H* h) { Arena x = h.a; }", "E0356");
    expect_codes("void g(Arena* a) { }\nvoid f() { mut Arena a = Arena.new(1 * KB); g(&a); g(&a); a.free(); }", "");
    expect_codes("struct P { f32 x; }\nvoid f() { P a = P{ x: 1.0 }; P b = a; P c = a; }", "");   /* copied */

    /* views x[a..b] */
    expect_codes("void f() { mut i32[3] a = [1, 2, 3]; i32[] v = a[0..2]; i32[] w = v[1..2]; }", "");
    expect_codes("void f(i32[3]* p, u8[] s) { i32[] v = p[1..3]; u8[] t = s[0..s.len]; }", "");
    expect_codes("void f() { i32[3] a = [1, 2, 3]; i32[] v = a[0..2]; }", "E0335");  /* `a` is not mut */
    expect_codes("void f(i32[] s) { s[0..1] = s; }", "E0335");
    expect_codes("void f(str s) { u8[] b = s[0..1]; }", "E0334");
    expect_codes("void f(i32[] s, i32 n) { i32[] v = s[0..n]; }", "E0331");
    expect_codes("void f(i32[] s) { i32[] v = s[0..=1]; }", "E0352");

    /* x.copy(): new arenas, and the original is not moved */
    expect_codes("struct H { Arena a; i32 n; }\nvoid f(H h) { H c = h.copy(); H d = h; }", "");
    expect_codes("struct H { Arena a; }\nvoid f(H* h) { H c = h.copy(); }", "");
    expect_codes("void f() { mut Arena a = Arena.new(1 * KB); while (true) { Arena c = a.copy(); } }", "");
    expect_codes("void f(H?[2] h) { H?[2] c = h.copy(); }\nstruct H { Arena a; }", "");
    expect_codes("void f() { Arena a = Arena.new(1 * KB); Arena b = a; Arena c = a.copy(); }", "E0356");
    expect_codes("void f(Arena a) { Arena c = a.copy(1); }", "E0336");
    expect_codes("struct P { f32 x; }\nvoid f(P p) { P c = p.copy(); }", "E0337");   /* copied by value already */
    expect_codes("void f(i32 x) { i32 c = x.copy(); }", "E0337");
    expect_codes("struct H { Arena a; }\nvoid f(H h) { auto c = h.copy(); H d = c; }", "");
    expect_help("struct P { f32 x; }\nvoid f(P p) { P c = p.copy(); }", "E0337",
                "a value without an `Arena` is copied by value: use it directly (spec §6)");

    /* wrapping operations (spec §5.1), std.time and std.random effects */
    expect_codes("u64 f(u64 a) { return wrapping_mul(wrapping_add(a, 1), 3); }", "");
    expect_codes("f64 f(f64 a) { return wrapping_add(a, 1.0); }", "E0334");
    expect_codes("void f(u8 a, u16 b) { auto c = wrapping_sub(a, b); }", "E0334");
    expect_codes("import std.time;\nvoid f() { time.sleep_ms(10); }", "E0340");
    expect_codes("import std.random;\nu64 f() { random.Rng r = random.seeded(1); return r.next(); }", "");
    expect_codes("import std.random;\nvoid f() { random.Rng r = random.from_system(); }", "E0340");
}

static void test_packages(void) {
    expect_package("tests/packages/app", "");
    expect_package("tests/packages/geometry", "");
    expect_package("tests/packages/visibility", "E0302 E0305 E0303 E0304");
    expect_package("tests/packages/bad_ui", "E0331 E0338");  /* in screen.ui.sosu */
    expect_package("tests/programs/screens", "");
}

int main(void) {
    test_valid();
    test_names();
    test_types();
    test_classes();
    test_duplicates();
    test_valid_bodies();
    test_body_errors();
    test_packages();
    return test_summary("sema tests");
}
