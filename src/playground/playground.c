/* The compiler in a web page (playground/): built with Emscripten (`make playground`), the
   library embedded at /lib. No process is started: the program is checked and its C shown,
   it is not run. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "codegen.h"
#include "fmt.h"
#include "sema.h"

#ifndef LIB_DIR
#define LIB_DIR "/lib" /* --embed-file lib@/lib */
#endif

/* What diag_print_text writes, as a string. */
static char *diagnostics_text(const DiagList *diags) {
    FILE *out = tmpfile();
    if (!out) return sosu_strdup("sosuc: no temporary file for the diagnostics\n");
    diag_print_text(diags, out);
    long size = ftell(out);
    char *text = malloc(size > 0 ? (size_t)size + 1 : 1);
    rewind(out);
    size_t read = size > 0 ? fread(text, 1, (size_t)size, out) : 0;
    text[read] = '\0';
    fclose(out);
    return text;
}

/* A result for the page: its first character says what follows, 'c' (the C of the program,
   or the formatted text) or 'e' (the diagnostics). Freed by the caller. */
static char *result(char kind, const char *text) {
    size_t length = strlen(text);
    char *out = malloc(length + 2);
    out[0] = kind;
    memcpy(out + 1, text, length + 1);
    return out;
}

/* Checks `text`, the module `test.main` that can import `core` and `std`, and generates its C
   with every run-time check (debug, spec 7.7). */
char *sosu_playground_compile(const char *text) {
    Program program;
    Sema sema = {0};
    char *source = NULL;
    bool ok = program_load_text(&program, text, LIB_DIR);
    if (ok) ok = sema_check(&sema, &program);
    if (ok) {
        CodegenOptions options = { true, true, true };
        source = codegen_program(&sema, &program, CODEGEN_PROGRAM, options);
    }
    char *out;
    if (source) {
        out = result('c', source);
    } else {
        char *diags = diagnostics_text(&program.diags);
        out = result('e', program.fatal ? program.fatal : diags);
        free(diags);
    }
    free(source);
    sema_free(&sema);
    program_free(&program);
    return out;
}

/* `sosuc fmt` on `text`. */
char *sosu_playground_format(const char *text) {
    SourceFile file;
    DiagList diags = {0};
    source_from_string(&file, "test.sosu", text);
    char *formatted = format_source(&file, &diags);
    char *out;
    if (formatted) {
        out = result('c', formatted);
    } else {
        char *messages = diagnostics_text(&diags);
        out = result('e', messages);
        free(messages);
    }
    free(formatted);
    diag_free(&diags);
    source_free(&file);
    return out;
}
