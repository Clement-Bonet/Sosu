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

/* A file keeps its first diagnostics only: past them, an error is counted but not kept, so that a
   file with an error every few characters costs no more than one with a few (review R2). */
#define DIAG_MAX_PER_FILE 100

typedef struct {
    Diagnostic *items;
    size_t count;
    size_t cap;
    size_t error_count; /* also the errors not kept */
    size_t dropped;     /* diagnostics not kept, past DIAG_MAX_PER_FILE in their file */
} DiagList;

/* Reports an error. The returned pointer stays valid until the next report; it is NULL when the
   file already has DIAG_MAX_PER_FILE diagnostics. */
Diagnostic *diag_error(DiagList *list, const char *code, const SourceFile *file,
                       size_t offset, size_t length, const char *fmt, ...);
Diagnostic *diag_verror(DiagList *list, const char *code, const SourceFile *file,
                        size_t offset, size_t length, const char *fmt, va_list args);
/* Reports a warning: it does not stop the build (error_count is unchanged). */
Diagnostic *diag_vwarning(DiagList *list, const char *code, const SourceFile *file,
                          size_t offset, size_t length, const char *fmt, va_list args);
/* Adds a suggested fix; does nothing when `diag` is NULL (silenced error). */
void diag_help(Diagnostic *diag, const char *fmt, ...);

void diag_print_text(const DiagList *list, FILE *out);
void diag_print_json(const DiagList *list, FILE *out);
void diag_free(DiagList *list);

#endif
