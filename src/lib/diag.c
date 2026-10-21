#include "diag.h"

#include <stdlib.h>
#include <string.h>

Diagnostic *diag_verror(DiagList *list, const char *code, const SourceFile *file,
                        size_t offset, size_t length, const char *fmt, va_list args) {
    Diagnostic diag = {0};
    diag.severity = SEVERITY_ERROR;
    diag.code = code;
    diag.file = file;
    diag.offset = offset;
    diag.length = length;
    diag.message = sosu_vformat(fmt, args);

    SOSU_VEC_PUSH(list, diag);
    list->error_count++;
    return &list->items[list->count - 1];
}

Diagnostic *diag_error(DiagList *list, const char *code, const SourceFile *file,
                       size_t offset, size_t length, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    Diagnostic *diag = diag_verror(list, code, file, offset, length, fmt, args);
    va_end(args);
    return diag;
}

void diag_help(Diagnostic *diag, const char *fmt, ...) {
    if (!diag) return;
    va_list args;
    va_start(args, fmt);
    free(diag->help);
    diag->help = sosu_vformat(fmt, args);
    va_end(args);
}

static const char *severity_name(Severity severity) {
    return severity == SEVERITY_ERROR ? "error" : "warning";
}

static bool is_continuation_byte(char c) {
    return ((unsigned char)c & 0xC0) == 0x80;
}

/*
error[E0103]: unterminated string literal
  --> main.sosu:3:5
   |
 3 |     "abc
   |     ^
   = help: ...
*/
static void print_text(const Diagnostic *d, FILE *out) {
    size_t line, column;
    source_position(d->file, d->offset, &line, &column);
    size_t line_length;
    const char *line_text = source_line(d->file, line, &line_length);
    size_t line_start = (size_t)(line_text - d->file->text);
    int width = snprintf(NULL, 0, "%zu", line);

    fprintf(out, "%s[%s]: %s\n", severity_name(d->severity), d->code, d->message);
    fprintf(out, "%*s--> %s:%zu:%zu\n", width, "", d->file->path, line, column);
    fprintf(out, "%*s |\n", width, "");
    fprintf(out, "%zu | %.*s\n", line, (int)line_length, line_text);
    fprintf(out, "%*s | ", width, "");

    /* Pad with the same tabs as the source line so the carets line up. */
    size_t start = d->offset - line_start;
    if (start > line_length) start = line_length;
    for (size_t i = 0; i < start; i++) {
        if (line_text[i] == '\t') fputc('\t', out);
        else if (!is_continuation_byte(line_text[i])) fputc(' ', out);
    }
    size_t end = d->offset - line_start + d->length;
    if (end > line_length) end = line_length;
    size_t carets = 0;
    for (size_t i = start; i < end; i++) {
        if (!is_continuation_byte(line_text[i])) carets++;
    }
    if (carets == 0) carets = 1;
    for (size_t i = 0; i < carets; i++) fputc('^', out);
    fputc('\n', out);

    if (d->help) fprintf(out, "%*s = help: %s\n", width, "", d->help);
}

void diag_print_text(const DiagList *list, FILE *out) {
    for (size_t i = 0; i < list->count; i++) {
        print_text(&list->items[i], out);
        fputc('\n', out);
    }
}

static void print_json_string(const char *text, FILE *out) {
    fputc('"', out);
    for (const char *p = text; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '"' || c == '\\') fprintf(out, "\\%c", c);
        else if (c == '\n') fputs("\\n", out);
        else if (c == '\t') fputs("\\t", out);
        else if (c < 0x20 || c == 0x7F) fprintf(out, "\\u%04x", c);
        else fputc(c, out);
    }
    fputc('"', out);
}

void diag_print_json(const DiagList *list, FILE *out) {
    fputc('[', out);
    for (size_t i = 0; i < list->count; i++) {
        const Diagnostic *d = &list->items[i];
        size_t line, column, end_line, end_column;
        source_position(d->file, d->offset, &line, &column);
        source_position(d->file, d->offset + d->length, &end_line, &end_column);

        fputs(i ? ",\n {" : "\n {", out);
        fprintf(out, "\"severity\":\"%s\",\"code\":\"%s\",\"message\":", severity_name(d->severity), d->code);
        print_json_string(d->message, out);
        fputs(",\"file\":", out);
        print_json_string(d->file->path, out);
        fprintf(out, ",\"line\":%zu,\"column\":%zu,\"end_line\":%zu,\"end_column\":%zu,\"help\":",
                line, column, end_line, end_column);
        if (d->help) print_json_string(d->help, out);
        else fputs("null", out);
        fputc('}', out);
    }
    fputs(list->count ? "\n]\n" : "]\n", out);
}

void diag_free(DiagList *list) {
    for (size_t i = 0; i < list->count; i++) {
        free(list->items[i].message);
        free(list->items[i].help);
    }
    free(list->items);
    memset(list, 0, sizeof *list);
}
