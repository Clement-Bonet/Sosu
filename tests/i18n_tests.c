#include <stdlib.h>
#include <string.h>

#include "i18n.h"
#include "program.h"
#include "test.h"

/* The template of the texts of `ui` (a .ui.sosu file) and `code` (a .sosu file). */
static char *extract(const char *ui, const char *code, size_t *count) {
    I18nTemplate *t = i18n_template_new();
    SourceFile ui_file;
    source_from_string(&ui_file, "screen.ui.sosu", ui);
    i18n_template_add(t, &ui_file, "src/screen.ui.sosu");
    SourceFile code_file;
    source_from_string(&code_file, "main.sosu", code);
    i18n_template_add(t, &code_file, "src/main.sosu");
    char *text = i18n_template_text(t);
    *count = i18n_template_count(t);
    i18n_template_free(t);
    source_free(&ui_file);
    source_free(&code_file);
    return text;
}

static bool has(const char *text, const char *part) {
    return strstr(text, part) != NULL;
}

static void test_extract(void) {
    size_t count = 0;
    char *pot = extract("window W(S* s) {\n"
                        "    title: \"Files\"\n"
                        "    Text(\"Hello, {name}\", name: s.name)\n"
                        "    Text(\"{count} file\", plural: \"{count} files\", count: s.n)\n"
                        "    Button(\"Open\", context: \"verb\")\n"
                        "    Text(s.data)\n"
                        "    TextField(bind: s.q, placeholder: \"Search \\\"all\\\"\")\n"
                        "    Button(\"Save\")\n"
                        "    Menu(\"Save\") { MenuItem(\"Quit\", shortcut: \"Ctrl+Q\") }\n"
                        "    Option(\"\\u{e9}t\\u{e9}\", value: Summer)\n"
                        "    Slider(bind: s.v, tooltip: \"Louder\")\n"
                        "}\n",
                        "void f(widgets.Frame* f, str x) {\n"
                        "    f.label(f.tr(\"Ready\"));\n"
                        "    f.label(f.tr_in(\"state\", \"Open\"));\n"
                        "    f.label(f.tr_plural(\"{count} line\", \"{count} lines\", 3));\n"
                        "    f.label(f.tr(x));\n"
                        "}\n",
                        &count);
    CHECK(count == 12, "12 texts, Save once: %zu", count);
    CHECK(has(pot, "#: src/screen.ui.sosu:11\nmsgid \"Louder\"\n"), "a tooltip");
    CHECK(has(pot, "\"Plural-Forms: nplurals=2; plural=(n != 1);\\n\""), "a header");
    CHECK(has(pot, "#: src/screen.ui.sosu:2\nmsgid \"Files\"\nmsgstr \"\"\n"), "the title of a window");
    CHECK(has(pot, "msgid \"Hello, {name}\"\nmsgstr \"\"\n"), "a value in braces");
    CHECK(has(pot, "#: src/screen.ui.sosu:4\nmsgid \"{count} file\"\nmsgid_plural \"{count} files\"\n"
                   "msgstr[0] \"\"\nmsgstr[1] \"\"\n"),
          "a plural");
    CHECK(has(pot, "msgctxt \"verb\"\nmsgid \"Open\"\n"), "a context");
    CHECK(has(pot, "msgctxt \"state\"\nmsgid \"Open\"\n"), "the other context, from code");
    CHECK(has(pot, "msgid \"Search \\\"all\\\"\"\n"), "a placeholder, escaped");
    CHECK(has(pot, "#: src/screen.ui.sosu:8 src/screen.ui.sosu:9\nmsgid \"Save\"\n"), "one text, two references");
    CHECK(has(pot, "msgid \"\xC3\xA9t\xC3\xA9\"\n"), "escapes of Sosu decoded");
    CHECK(has(pot, "#: src/main.sosu:2\nmsgid \"Ready\"\n"), "tr in code");
    CHECK(has(pot, "msgid \"{count} line\"\nmsgid_plural \"{count} lines\"\n"), "tr_plural in code");
    CHECK(!has(pot, "Ctrl+Q") && !has(pot, "msgid \"x\""), "not shortcuts nor expressions");
    free(pot);
}

/* Catalogs are named `<name>.locale.<language>.sosu`: gettext, not Sosu. */
static void test_locale_files(void) {
    CHECK(program_is_locale_file("locales/messages.locale.fr-ch.sosu"), "a language and a country");
    CHECK(program_is_locale_file("locales\\messages.locale.default.sosu"), "the template, a Windows path");
    CHECK(!program_is_locale_file("src/locale.sosu"), "a module named locale");
    CHECK(!program_is_locale_file("src/screen.ui.sosu"), "an interface");
    CHECK(!program_is_locale_file("my.locale.d/main.sosu"), "the name of a folder");
}

int main(void) {
    test_extract();
    test_locale_files();
    return test_summary("i18n tests");
}
