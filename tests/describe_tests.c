/* `sosuc describe` (describe.c, spec 11.4): the comments read, the module written, each error. */
#include <stdlib.h>
#include <string.h>

#include "describe.h"
#include "test.h"

typedef struct {
    char *text;   /* the module written, or NULL */
    DiagList diags;
    size_t components;
} Result;

/* The module written for a package `game` of a module `parts` of text `code`, and of a module `lib`
   that imports it (as `sozo_setup` does) unless `alone`. */
static Result describe_in(const char *code, bool alone) {
    SourceFile file, lib;
    source_from_string(&file, "parts.sosu", code);
    source_from_string(&lib, "lib.sosu", "import game.described;\n");
    DescribeSource sources[2] = { { "parts", &file }, { "lib", &lib } };
    Result r;
    memset(&r, 0, sizeof r);
    r.text = describe_package("game", sources, alone ? 1 : 2, &r.diags, &r.components);
    source_free(&file);
    source_free(&lib);
    return r;
}

static Result describe_one(const char *code) {
    return describe_in(code, false);
}

static bool has(const char *text, const char *part) {
    return text && strstr(text, part) != NULL;
}

/* Whether the first diagnostic has `code` and its message holds `part`. */
static bool said(const Result *r, const char *code, const char *part) {
    return r->diags.count > 0 && strcmp(r->diags.items[0].code, code) == 0 && strstr(r->diags.items[0].message, part);
}

static void done(Result *r) {
    free(r->text);
    diag_free(&r->diags);
}

static void test_component_and_fields(void) {
    Result r = describe_one("import math.linear;\n"
                            "\n"
                            "pub enum Mode {\n"
                            "    Slow,\n"
                            "    VeryFast,\n"
                            "}\n"
                            "\n"
                            "// A door that locks.\n"
                            "// component\n"
                            "pub struct DoorLock {\n"
                            "    // label \"Open speed\"; range 0..10; unit \"m/s\"\n"
                            "    pub mut f32 speed = 1.0;\n"
                            "    pub mut Mode mode = Mode.Slow;  // the way it opens\n"
                            "    // group \"Lock\"\n"
                            "    pub mut i32 code = 0;  // range -5..5\n"
                            "    pub mut str key = \"\";  // asset; tooltip \"The \\\"key\\\"\"\n"
                            "    pub mut linear.Vec3 glow = linear.Vec3{ x: 1.0, y: 0.0, z: 0.0 };  // color; hidden\n"
                            "    pub mut f32 timer = 0.0;  // transient\n"
                            "    pub f32 made = 0.0;  // transient\n"
                            "    f32 inner = 0.0;\n"
                            "}\n");
    CHECK(r.text && r.diags.count == 0 && r.components == 1, "a component described");
    CHECK(has(r.text, "import game.parts;") && has(r.text, "import math.linear;") && has(r.text, "import engine.host;"),
          "its imports");
    CHECK(has(r.text, "return \"door_lock\";"), "its table: the name of the struct in lower case, with `_`");
    CHECK(has(r.text, "pub class DoorLockFields : describe.Describe<parts.DoorLock>, save.Codec<parts.DoorLock> {"),
          "its class of fields");
    CHECK(has(r.text, "pub class DoorLockKind : kinds.Kind, host.Saved {"), "its kind");
    CHECK(has(r.text, "pub DoorLockKind* door_lock_kind(keeps Arena* arena, keeps scene.World* world) {"),
          "the function that makes its kind");
    CHECK(has(r.text, "return 5;"), "five fields: not the transient ones, nor the private one");
    CHECK(has(r.text, "label: \"Open speed\"") && has(r.text, "low: 0.0") && has(r.text, "high: 10.0") &&
              has(r.text, "unit: \"m/s\""),
          "the words of the comment above a field");
    CHECK(has(r.text, "label: \"Mode\"") && has(r.text, "kind: describe.Kind.Choice"),
          "a label made of its name; a comment that says no word is prose");
    CHECK(has(r.text, "made_1[1] = \"very_fast\";"), "the choices: the names of the variants, in lower case");
    CHECK(has(r.text, "group: \"Lock\"") && has(r.text, "low: -5.0"), "a group, and a range at the end of the line");
    CHECK(has(r.text, "kind: describe.Kind.Asset") && has(r.text, "tooltip: \"The \\\"key\\\"\""),
          "an asset, and a text with quotes");
    CHECK(has(r.text, "kind: describe.Kind.Color") && has(r.text, "hidden: true"), "a color, hidden");
    CHECK(has(r.text, "c.code = describe.i32_of(v, c.code);"), "an i32 held within its type");
    CHECK(has(r.text, "c.mode = parts_mode_at(describe.choice_of(v, parts_mode_index(c.mode)));"),
          "an enum through its index");
    CHECK(has(r.text, "    l.add(k0 as kinds.Kind*);\n    l.add_saved(k0 as host.Saved*);"), "add_all");
    CHECK(has(r.text, "    l.add_source(\"door_lock\", \"src/parts.sosu\", 10);"), "where it is written, for the editor");
    done(&r);
}

static void test_named_and_none(void) {
    Result r = describe_one("// component \"post\"\npub struct Fence {\n    f32 seen = 0.0;\n}\n");
    CHECK(r.text && r.components == 1 && has(r.text, "return \"post\";") && has(r.text, "return 0;"),
          "a component named by its comment, without fields");
    done(&r);
    r = describe_one("// A plain struct: no component here.\npub struct Plain {\n    pub mut f32 x = 0.0;\n}\n");
    CHECK(r.text && r.components == 0 && has(r.text, "pub void add_all(host.Level* l) { }") &&
              !has(r.text, "import std.json;"),
          "no component: add_all does nothing");
    done(&r);
    r = describe_in("// component\npub struct Lone {\n    pub mut f32 x = 0.0;\n}\n", true);
    CHECK(r.text && r.components == 1 && r.diags.count == 1 && r.diags.error_count == 0 &&
              said(&r, "E0608", "no module imports `game.described`") && r.diags.items[0].severity == SEVERITY_WARNING,
          "components that no module imports: a warning, the module written all the same");
    done(&r);
    r = describe_in("// A plain struct.\npub struct Plain {\n    pub mut f32 x = 0.0;\n}\n", true);
    CHECK(r.text && r.diags.count == 0, "no component, no warning");
    done(&r);
    r = describe_one("// component\n\npub struct Apart {\n    pub mut f32 x = 0.0;\n}\n");
    CHECK(r.text && r.components == 0, "a comment apart, a blank line before the struct, is not its own");
    done(&r);
}

static void test_errors(void) {
    Result r = describe_one("// component\npub class Door {\n    pub mut f32 x = 0.0;\n}\n");
    CHECK(!r.text && said(&r, "E0601", "pub struct"), "a class is not a component");
    done(&r);
    r = describe_one("// component\nstruct Hidden {\n    pub mut f32 x = 0.0;\n}\n");
    CHECK(said(&r, "E0601", "pub struct"), "a struct that is not pub");
    done(&r);
    r = describe_one("// component \"Bad-Name\"\npub struct A {\n}\n");
    CHECK(said(&r, "E0601", "lowercase"), "a name that cannot be a table");
    done(&r);
    r = describe_one("// component\npub struct Body {\n}\n");
    CHECK(said(&r, "E0601", "engine"), "a name of engine");
    done(&r);
    r = describe_one("// component \"door\"\npub struct A {\n}\n// component \"door\"\npub struct B {\n}\n");
    CHECK(said(&r, "E0601", "two components named `door`"), "two components of one name");
    done(&r);
    r = describe_one("// component\npub struct A {\n    pub mut f64 x = 0.0;\n}\n");
    CHECK(said(&r, "E0602", "`f64` cannot be a field"), "a type it does not know");
    done(&r);
    r = describe_one("// component\npub struct A {\n    pub mut f32[] xs = empty();\n}\n");
    CHECK(said(&r, "E0602", "list"), "a list");
    done(&r);
    r = describe_one("pub enum Shape {\n    Circle(f32 r),\n}\n// component\npub struct A {\n    pub mut Shape s = Shape.Circle(1.0);\n}\n");
    CHECK(said(&r, "E0602", "variants carry nothing"), "an enum with payloads");
    done(&r);
    r = describe_one("// component\npub struct A {\n    pub mut f32 x;\n}\n");
    CHECK(said(&r, "E0607", "no default value"), "a field without a default");
    done(&r);
    r = describe_one("// component\npub struct A {\n    // lable \"Speed\"\n    pub mut f32 x = 0.0;\n}\n");
    CHECK(said(&r, "E0603", "unknown word `lable`"), "a word misspelled");
    done(&r);
    r = describe_one("// component\npub struct A {\n    pub mut f32 x = 0.0;  // range 10..0\n}\n");
    CHECK(said(&r, "E0604", "lower bound"), "a range the wrong way");
    done(&r);
    r = describe_one("// component\npub struct A {\n    pub mut i32 x = 0;  // range 0..2.5\n}\n");
    CHECK(said(&r, "E0604", "whole numbers"), "a range of a whole, not whole");
    done(&r);
    r = describe_one("// component\npub struct A {\n    pub mut f32 x = 0.0;  // label 5\n}\n");
    CHECK(said(&r, "E0604", "in quotes"), "a text without quotes");
    done(&r);
    r = describe_one("// component\npub struct A {\n    pub mut f32 x = 0.0;  // label of the speed, unit and all\n}\n");
    CHECK(r.text && r.diags.count == 0, "a comment that starts with a word, followed by prose, is prose");
    done(&r);
    r = describe_one("// component\npub struct A {\n    pub mut f32 x = 0.0;  // range: from 0 to 10, in meters\n}\n");
    CHECK(r.text && r.diags.count == 0, "a word followed by a colon is prose");
    done(&r);
    r = describe_one("// component\npub struct A {\n    pub mut f32 x = 0.0;  // label \"Speed\n}\n");
    CHECK(said(&r, "E0604", "closing quote"), "a text not closed");
    done(&r);
    r = describe_one("// component\npub struct A {\n    pub mut f32 x = 0.0;  // range 0..5 unit \"m\"\n}\n");
    CHECK(said(&r, "E0604", "separated by `;`"), "two words without `;`");
    done(&r);
    r = describe_one("// component\npub struct A {\n    pub mut bool x = false;  // range 0..1\n}\n");
    CHECK(said(&r, "E0605", "number"), "a range on a flag");
    done(&r);
    r = describe_one("// component\npub struct A {\n    pub mut f32 x = 0.0;  // color\n}\n");
    CHECK(said(&r, "E0605", "linear.Vec3"), "a color on a number");
    done(&r);
    r = describe_one("// component\npub struct A {\n    pub mut f32 x = 0.0;  // asset\n}\n");
    CHECK(said(&r, "E0605", "str"), "an asset on a number");
    done(&r);
    r = describe_one("// component\npub struct A {\n    // component\n    pub mut f32 x = 0.0;\n}\n");
    CHECK(said(&r, "E0605", "above a struct"), "component above a field");
    done(&r);
    r = describe_one("// component\npub struct A {\n    pub f32 x = 0.0;\n}\n");
    CHECK(said(&r, "E0606", "not `mut`"), "a pub field that is not mut");
    done(&r);
    r = describe_one("// component\npub struct A {\n    pub mut f32 x = 0.0;\n}\n// unit \"m\"\npub struct B {\n}\n");
    CHECK(said(&r, "E0605", "above a field"), "a word of a field above a struct");
    done(&r);
}

static void test_two_modules(void) {
    SourceFile a, b;
    source_from_string(&a, "kinds.sosu", "pub enum Size {\n    Small,\n    Large,\n}\n");
    source_from_string(&b, "tree.sosu",
                       "import game.ai.kinds;\n// component\npub struct Tree {\n    pub mut kinds.Size size = kinds.Size.Large;\n}\n");
    DescribeSource sources[2] = { { "ai.kinds", &a }, { "plants.tree", &b } };
    DiagList diags = {0};
    size_t components = 0;
    char *text = describe_package("game", sources, 2, &diags, &components);
    CHECK(text && components == 1, "an enum of another module");
    CHECK(has(text, "import game.ai.kinds;") && has(text, "import game.plants.tree;"), "both modules imported");
    CHECK(has(text, "kinds.Size kinds_size_at(usize i) {") && has(text, "return kinds.Size.Large;"),
          "the enum named from its module");
    CHECK(has(text, "l.add_source(\"tree\", \"src/plants/tree.sosu\", 3);"), "the file of a module in a folder");
    char *again = describe_package("game", sources, 2, &diags, NULL);
    CHECK(again && strcmp(text, again) == 0, "the same text each time");
    free(again);
    free(text);
    diag_free(&diags);
    source_free(&a);
    source_free(&b);
}

int main(void) {
    test_component_and_fields();
    test_named_and_none();
    test_errors();
    test_two_modules();
    return test_summary("describe tests");
}
