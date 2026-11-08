#include <stdlib.h>
#include <string.h>

#include "fmt.h"
#include "parser.h"
#include "test.h"

/* Run from the repository root: the second part formats the Sosu files of the repository. */

static char *format_text(const char *text) {
    SourceFile file;
    source_from_string(&file, "test.sosu", text);
    DiagList diags = {0};
    char *result = format_source(&file, &diags);
    diag_free(&diags);
    source_free(&file);
    return result;
}

static void expect_format(const char *input, const char *expected) {
    char *result = format_text(input);
    CHECK(result && strcmp(result, expected) == 0, "format:\n%s\n--- got:\n%s--- expected:\n%s", input,
          result ? result : "(error)\n", expected);
    free(result);
}

static void test_canonical(void) {
    expect_format("struct  P{f32 x;f32 y;}", "struct P {\n    f32 x;\n    f32 y;\n}\n");
    expect_format("void f(){}", "void f() { }\n");
    expect_format("void f(  keeps  T* x,mut keeps str s){}", "void f(keeps T* x, mut keeps str s) { }\n");
    expect_format("void f(fn(  keeps  T*,i32,keeps  U)->void g){}", "void f(fn(keeps T*, i32, keeps U) -> void g) { }\n");
    expect_format("void f(){auto g=(  keeps  T* x,y)=>x;}", "void f() {\n    auto g = (keeps T* x, y) => x;\n}\n");
    expect_format("void m()  keeps   self  uses io{}", "void m() keeps self uses io { }\n");
    expect_format("interface I{void m()keeps self;}", "interface I {\n    void m() keeps self;\n}\n");
    expect_format("i32 f(){return (a+b)*c-(d-e);}", "i32 f() {\n    return (a + b) * c - (d - e);\n}\n");
    expect_format("i32 f(){return ((a*b))+(c);}", "i32 f() {\n    return a * b + c;\n}\n");
    expect_format("void f(){auto x=-(y as f32);auto p=(q as ui8*)+1;auto z=(a<b)==c;}",
                  "void f() {\n    auto x = -(y as f32);\n    auto p = (q as ui8*) + 1;\n    auto z = (a < b) == c;\n}\n");
    expect_format("void f(){x=c?a:(b?d:e);y=(c?a:b)?d:e;z=*(p+4);w=(*p).x;}",
                  "void f() {\n    x = c ? a : b ? d : e;\n    y = (c ? a : b) ? d : e;\n    z = *(p + 4);\n    w = (*p).x;\n}\n");
    expect_format("enum E { A, B(i32 x) }", "enum E { A, B(i32 x) }\n");
    expect_format("pub  ui32 A=0x20|(B);\nusize B=4;\nstruct S{ui8[B] b;}",
                  "pub ui32 A = 0x20 | B;\nusize B = 4;\nstruct S {\n    ui8[B] b;\n}\n");
    expect_format("enum E {\nA,\nB }", "enum E {\n    A,\n    B,\n}\n");
    expect_format("i32 clamp(i32 x) requires x > 0 ensures result > 0 { return x; }",
                  "i32 clamp(i32 x)\n    requires x > 0\n    ensures result > 0\n{\n    return x;\n}\n");
    /* generics (spec §5.4) */
    expect_format("struct Pair<A,B>{A first;B second;}", "struct Pair<A, B> {\n    A first;\n    B second;\n}\n");
    expect_format("T max_of<T:Ordered+Hash,uses E>(T a,T b)uses E{return a>b?a:b;}",
                  "T max_of<T: Ordered + Hash, uses E>(T a, T b) uses E {\n    return a > b ? a : b;\n}\n");
    expect_format("void f(){Vec<Vec<i32>*>* v=vec.new<Vec<i32>*>(&a);auto t=Tree<i32>.Leaf(1);auto p=Pair<str,i32>{first:\"x\",second:1};}",
                  "void f() {\n    Vec<Vec<i32>*>* v = vec.new<Vec<i32>*>(&a);\n    auto t = Tree<i32>.Leaf(1);\n"
                  "    auto p = Pair<str, i32>{ first: \"x\", second: 1 };\n}\n");
    expect_format("void f(E e) { match (e) { A => g(), B(x) => { h(x); } } }",
                  "void f(E e) {\n    match (e) {\n        A => g(),\n        B(x) => {\n            h(x);\n        },\n"
                  "    }\n}\n");
    expect_format("void f() { for (mut i32 i = 0; i < 3; i += 1) { } for (;;) { break; } }",
                  "void f() {\n    for (mut i32 i = 0; i < 3; i += 1) { }\n    for (;;) {\n        break;\n    }\n}\n");
    expect_format("void f() { auto a = [1,2,3]; auto b = [0;8]; Point p = Point{x:1.0,y:2.0}; }",
                  "void f() {\n    auto a = [1, 2, 3];\n    auto b = [0; 8];\n    Point p = Point{ x: 1.0, y: 2.0 };\n}\n");
    expect_format("void f() { auto t = [1,\n 2, // two\n\n 3]; }",
                  "void f() {\n    auto t = [\n        1,\n        2,  // two\n\n        3,\n    ];\n}\n");
    expect_format("void f() { auto t = [\n1, 2]; auto u = [[1, 2], [3, 4]]; }",
                  "void f() {\n    auto t = [\n        1, 2,\n    ];\n    auto u = [[1, 2], [3, 4]];\n}\n");
    expect_format("void f() { auto t = [1,\n2, 3,\n4]; }", "void f() {\n    auto t = [\n        1,\n        2, 3,\n        4,\n    ];\n}\n");
    expect_format("class B : A, I { override void f() uses io { } }",
                  "class B : A, I {\n    override void f() uses io { }\n}\n");
    /* lambdas (spec §5.5) */
    expect_format("void f() { auto g=(i32 a,b)=>a+b; h((x)=>{return x;}); auto k=((x)=>x)(3); }",
                  "void f() {\n    auto g = (i32 a, b) => a + b;\n    h((x) => {\n        return x;\n    });\n"
                  "    auto k = ((x) => x)(3);\n}\n");
    expect_format("void f((fn(i32)->i32)[] fs, (fn()->void)? g) { }", "void f((fn(i32) -> i32)[] fs, (fn() -> void)? g) { }\n");
}

static void test_comments_and_blank_lines(void) {
    expect_format("// top\nimport std.io;   // io\n\n\n\n// main\ni32 main() uses io { // start\n\n    return 0; // done\n"
                  "    // end of main\n}\n// last\n",
                  "// top\nimport std.io;  // io\n\n// main\ni32 main() uses io {  // start\n    return 0;  // done\n"
                  "    // end of main\n}\n// last\n");
    expect_format("struct S {\n    f32 a;\n\n    // b\n    f32 b; /* inline */\n}\n",
                  "struct S {\n    f32 a;\n\n    // b\n    f32 b;  /* inline */\n}\n");
    expect_format("void f() {\n    // only a comment\n}\n", "void f() {\n    // only a comment\n}\n");
    char *broken = format_text("void f( {");
    CHECK(broken == NULL, "a file with errors is not formatted");
    free(broken);
}

/* Properties on real files: formatting keeps the syntax tree and every comment, and a
   formatted file stays as it is. */

static char *dump(const char *path, const char *text) {
    SourceFile file;
    source_from_string(&file, path, text);
    TokenList tokens = {0};
    DiagList diags = {0};
    Arena arena = {0};
    lex_file(&file, &tokens, &diags);
    Module *module = parse_module(&file, &tokens, &arena, &diags);
    char *result = NULL;
    FILE *out = tmpfile();
    if (out && diags.error_count == 0) {
        ast_print_module(module, &file, out);
        long size = ftell(out);
        rewind(out);
        result = sosu_alloc((size_t)size + 1);
        size_t n = fread(result, 1, (size_t)size, out);
        result[n] = '\0';
    }
    if (out) fclose(out);
    arena_free(&arena);
    diag_free(&diags);
    token_list_free(&tokens);
    source_free(&file);
    return result;
}

static size_t comment_count(const char *text) {
    SourceFile file;
    source_from_string(&file, "c.sosu", text);
    TokenList tokens = {0};
    CommentList comments = {0};
    DiagList diags = {0};
    lex_file_comments(&file, &tokens, &comments, &diags);
    size_t count = comments.count;
    free(comments.items);
    diag_free(&diags);
    token_list_free(&tokens);
    source_free(&file);
    return count;
}

static const char *const files[] = {
    "tests/programs/basics/src/main.sosu", "tests/programs/basics/src/defaults.sosu",
    "tests/programs/classes/src/main.sosu", "packages/ui/src/widgets.sosu", "packages/ui/src/lib.sosu",
    "examples/form/src/main.sosu", "examples/form/src/state.sosu", "tests/programs/screens/src/main.sosu",
    "tests/programs/screens/src/state.sosu", "tests/packages/bad_ui/src/main.sosu",
    "tests/programs/files/src/main.sosu", "tests/programs/clock_random/src/main.sosu",
    "tests/programs/arrays/src/main.sosu", "tests/programs/copies/src/main.sosu",
    "tests/programs/linking/src/main.sosu", "tests/programs/testing/src/lib.sosu", "tests/programs/lambdas/src/main.sosu",
    "tests/programs/iterators/src/main.sosu", "tests/programs/threads/src/main.sosu", "lib/std/src/thread.sosu",
    "lib/std/src/channel.sosu", "lib/std/src/math.sosu", "tests/programs/mathematics/src/main.sosu",
    "lib/std/src/sort.sosu", "tests/programs/sorting/src/main.sosu", "lib/std/src/text.sosu",
    "tests/programs/texts/src/main.sosu", "tests/programs/calendar/src/main.sosu", "lib/std/src/os.sosu",
    "lib/std/src/fs.sosu", "tests/programs/system/src/main.sosu", "lib/std/src/cli.sosu", "lib/std/src/log.sosu",
    "lib/std/src/json.sosu", "tests/programs/formats/src/main.sosu", "tests/programs/readme/src/main.sosu",
    "packages/platform/src/app.sosu", "packages/platform/src/sdl.sosu", "packages/render/src/canvas.sosu",
    "packages/render/src/font.sosu", "packages/render/src/truetype.sosu", "packages/render/src/lib.sosu",
    "packages/image/src/lib.sosu", "packages/image/src/image.sosu", "packages/image/src/png.sosu",
    "packages/image/src/jpeg.sosu", "packages/image/src/ops.sosu", "tests/programs/images/src/lib.sosu",
    "packages/crypto/src/lib.sosu", "packages/crypto/src/sha256.sosu",
    "packages/crypto/src/sha1.sosu", "packages/crypto/src/hmac.sosu", "packages/crypto/src/base64.sosu",
    "packages/crypto/src/hex.sosu", "packages/crypto/src/secure.sosu", "tests/programs/hashes/src/main.sosu",
    "packages/compress/src/lib.sosu", "packages/compress/src/checksum.sosu", "packages/compress/src/deflate.sosu",
    "packages/compress/src/zlib.sosu", "packages/compress/src/gzip.sosu", "tests/programs/compression/src/main.sosu",
    "packages/math/src/lib.sosu", "packages/math/src/linear.sosu", "tests/programs/geometry/src/main.sosu",
    "examples/window/src/main.sosu",
    "tests/window/src/main.sosu", "lib/std/src/io.sosu", "lib/std/src/time.sosu", "lib/std/src/random.sosu",
    "lib/core/src/prelude.sosu", "tests/samples/hello.sosu", "tests/samples/classes.sosu",
    "tests/samples/reliability.sosu", "tests/samples/events.sosu",
};

static void test_repository_files(void) {
    for (size_t i = 0; i < sizeof files / sizeof *files; i++) {
        SourceFile file;
        if (!source_load(&file, files[i])) {
            CHECK(false, "cannot read %s", files[i]);
            continue;
        }
        char *once = format_text(file.text);
        CHECK(once != NULL, "%s does not format", files[i]);
        if (once) {
            char *twice = format_text(once);
            CHECK(twice && strcmp(once, twice) == 0, "%s: formatting twice changes it", files[i]);
            char *before = dump(files[i], file.text);
            char *after = dump(files[i], once);
            CHECK(before && after && strcmp(before, after) == 0, "%s: formatting changes the syntax tree", files[i]);
            CHECK(comment_count(file.text) == comment_count(once), "%s: formatting loses comments", files[i]);
            CHECK(strcmp(once, file.text) == 0, "%s is not formatted: run `sosuc fmt` on it", files[i]);
            free(before);
            free(after);
            free(twice);
        }
        free(once);
        source_free(&file);
    }
}

/* Long lines (spec §7.6): broken at their outermost list, chain of operators or conditional,
   and formatted again unchanged. */
static void test_long_lines(void) {
    const char *input =
        "void calls() {\n"
        "    draw_rectangle(target_surface_now, first_corner_x, first_corner_y, second_corner_x, second_corner_y_of_it, line_color);\n"
        "    handler.on_event(Resized(window_width_in_pixels, window_height_in_pixels, screen_density_scale, more_arguments_here));\n"
        "    Rect r = Rect{ x: left_margin + padding, y: top_margin + padding, w: width - 2 * padding, h: line_height_in_pixels_now };\n"
        "    i32 value = kind == 0 ? x : kind == 1 ? x + left : kind == 2 ? x + above : kind == 3 ? x + above_left : kind == 4 ? x + paeth : -1;\n"
        "    if (first_condition_holds && second_condition_holds && third_condition_holds || fourth_condition_holds_too_now_and) {\n"
        "        return;\n"
        "    } else if (first_condition_holds && second_condition_holds && third_condition_holds || fourth_condition_holds_too) {\n"
        "        return;\n"
        "    }\n"
        "    auto table = [16, 11, 10, 16, 24, 40, 51, 61, 12, 12, 14, 19, 26, 58, 60, 55, 14, 13, 16, 24, 40, 57, 69, 56, 14, 17, 22, 29];\n"
        "}\n"
        "\n"
        "i32 clamp(i32 x) requires x > 0 && x < 1000000 && x != 42 && x != 4242 && x != 424242 && x != 4242424242 + offset_value { return x; }\n"
        "\n"
        "void connect(Arena* arena, str host_name, ui16 port_number, Duration timeout, bool keep_alive, i32 retries_count) uses io { }\n";
    const char *expected =
        "void calls() {\n"
        "    draw_rectangle(\n"
        "        target_surface_now,\n"
        "        first_corner_x,\n"
        "        first_corner_y,\n"
        "        second_corner_x,\n"
        "        second_corner_y_of_it,\n"
        "        line_color\n"
        "    );\n"
        "    handler.on_event(Resized(\n"
        "        window_width_in_pixels,\n"
        "        window_height_in_pixels,\n"
        "        screen_density_scale,\n"
        "        more_arguments_here\n"
        "    ));\n"
        "    Rect r = Rect{\n"
        "        x: left_margin + padding,\n"
        "        y: top_margin + padding,\n"
        "        w: width - 2 * padding,\n"
        "        h: line_height_in_pixels_now\n"
        "    };\n"
        "    i32 value = kind == 0 ? x\n"
        "            : kind == 1 ? x + left\n"
        "            : kind == 2 ? x + above\n"
        "            : kind == 3 ? x + above_left\n"
        "            : kind == 4 ? x + paeth\n"
        "            : -1;\n"
        "    if (first_condition_holds && second_condition_holds && third_condition_holds\n"
        "            || fourth_condition_holds_too_now_and) {\n"
        "        return;\n"
        "    } else if (first_condition_holds && second_condition_holds && third_condition_holds || fourth_condition_holds_too) {\n"
        "        return;\n"
        "    }\n"
        "    auto table = [\n"
        "        16, 11, 10, 16, 24, 40, 51, 61, 12, 12, 14, 19, 26, 58, 60, 55, 14, 13, 16, 24, 40, 57, 69, 56, 14, 17, 22, 29,\n"
        "    ];\n"
        "}\n"
        "\n"
        "i32 clamp(i32 x)\n"
        "    requires x > 0 && x < 1000000 && x != 42 && x != 4242 && x != 424242 && x != 4242424242 + offset_value\n"
        "{\n"
        "    return x;\n"
        "}\n"
        "\n"
        "void connect(\n"
        "    Arena* arena,\n"
        "    str host_name,\n"
        "    ui16 port_number,\n"
        "    Duration timeout,\n"
        "    bool keep_alive,\n"
        "    i32 retries_count\n"
        ") uses io { }\n";
    expect_format(input, expected);
    char *again = format_text(expected);
    CHECK(again && strcmp(again, expected) == 0, "long lines: formatting twice changes them:\n%s", again ? again : "(error)\n");
    free(again);
    /* a line that cannot be broken stays as it is */
    expect_format("void f() { g(\"a string literal much longer than the width of a line, which the formatter never breaks, so it stays\"); }",
                  "void f() {\n    g(\"a string literal much longer than the width of a line, which the formatter never breaks, so it stays\");\n}\n");
}

/* `.ui.sosu` files (spec §10.10): from their tokens, the lines kept, indented by their brackets. */
static char *format_ui_text(const char *text) {
    SourceFile file;
    source_from_string(&file, "screen.ui.sosu", text);
    DiagList diags = {0};
    char *result = format_source(&file, &diags);
    diag_free(&diags);
    source_free(&file);
    return result;
}

static void expect_ui_format(const char *input, const char *expected) {
    char *result = format_ui_text(input);
    CHECK(result && strcmp(result, expected) == 0, "format:\n%s\n--- got:\n%s--- expected:\n%s", input,
          result ? result : "(error)\n", expected);
    if (result) {  /* formatting again changes nothing */
        char *again = format_ui_text(result);
        CHECK(again && strcmp(again, result) == 0, "format twice:\n%s--- got:\n%s", result, again ? again : "(error)\n");
        free(again);
    }
    free(result);
}

static void test_interface_files(void) {
    expect_ui_format("window W(S* s) {\ntitle:\"Hi\"\n   Column(gap:8 ,padding: 4){\n  Text( \"a\" ,style:s.big ? Big : Normal)   // a comment\n"
                     "\n\n\n        Button(\"OK\",on_click : s.ok)\n}\n}\n",
                     "window W(S* s) {\n    title: \"Hi\"\n    Column(gap: 8, padding: 4) {\n        Text(\"a\", style: s.big ? Big : Normal)  // a comment\n"
                     "\n        Button(\"OK\", on_click: s.ok)\n    }\n}\n");
    expect_ui_format("theme Dark {\n  accent: #89B4FA\nButton { background: accent\nhover { color: #FFFFFF } }\n}\n",
                     "theme Dark {\n    accent: #89B4FA\n    Button { background: accent\n        hover { color: #FFFFFF } }\n}\n");
    expect_ui_format("window W() {\n    Text(\"a\",\n        context: \"b\")\n}\n", "window W() {\n    Text(\"a\",\n        context: \"b\")\n}\n");
    char *broken = format_ui_text("window W( {\n");
    CHECK(broken == NULL, "a file with errors is not formatted");
    free(broken);
}

int main(void) {
    test_canonical();
    test_comments_and_blank_lines();
    test_long_lines();
    test_interface_files();
    test_repository_files();
    return test_summary("fmt tests");
}
