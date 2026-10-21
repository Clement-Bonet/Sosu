#ifndef SOSU_DIAG_H
#define SOSU_DIAG_H

#include <stdio.h>

#include "source.h"

typedef enum {
    SEVERITY_ERROR,
    SEVERITY_WARNING,
} Severity;

typedef struct {
    Severity severity;
    const char *code; /* stable code, e.g. "E0103" (see docs/errors.md) */
    char *message;
    char *help;       /* suggested fix, or NULL */
    const SourceFile *file;
    size_t offset;    /* byte span in the file */
    size_t length;
} Diagnostic;

typedef struct {
    Diagnostic *items;
    size_t count;
    size_t cap;
    size_t error_count;
} DiagList;

/* Reports an error. The returned pointer stays valid until the next report. */
Diagnostic *diag_error(DiagList *list, const char *code, const SourceFile *file,
                       size_t offset, size_t length, const char *fmt, ...);
Diagnostic *diag_verror(DiagList *list, const char *code, const SourceFile *file,
                        size_t offset, size_t length, const char *fmt, va_list args);
/* Adds a suggested fix; does nothing when `diag` is NULL (silenced error). */
void diag_help(Diagnostic *diag, const char *fmt, ...);

void diag_print_text(const DiagList *list, FILE *out);
void diag_print_json(const DiagList *list, FILE *out);
void diag_free(DiagList *list);

#endif
