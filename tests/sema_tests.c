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
                 "i32!ParseError parse(str s) { return 0; }\n"
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

static void test_packages(void) {
    expect_package("tests/packages/app", "");
    expect_package("tests/packages/geometry", "");
    expect_package("tests/packages/visibility", "E0302 E0305 E0303 E0304");
}

int main(void) {
    test_valid();
    test_names();
    test_types();
    test_classes();
    test_duplicates();
    test_packages();
    return test_summary("sema tests");
}
