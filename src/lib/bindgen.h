#ifndef SOSU_BINDGEN_H
#define SOSU_BINDGEN_H

#include "common.h"

/* `sosuc bindgen` (spec §11.6): Sosu declarations from C headers. The C compiler preprocesses
   the header; this reads its output, declarations of C99 without bodies of functions, and
   writes a Sosu module. */

typedef struct {
    char **items;
    size_t count;
    size_t cap;
} BindgenNames;

typedef struct {
    const char *text; /* the output of the preprocessor, with its line markers */
    size_t length;
    const char *const *own_dirs; /* the folders whose headers are bound (the header's, the -I ones) */
    size_t own_dir_count;
    const char *const *prefixes; /* keep the names starting with one of them; none: every name */
    size_t prefix_count;
    bool long_is_32;      /* Windows: `long` is 32 bits */
    const char *comment;  /* the first lines of the module: header, version, target, command */
    const char *include;  /* the header as the check file includes it */
} BindgenInput;

typedef struct {
    char *module;   /* the Sosu module */
    char *check;    /* a C file of static assertions: sizes, alignments, offsets, values */
    char *warnings; /* one line each, or "" */
    size_t constants;
    size_t structs;
    size_t functions;
    size_t skipped;
} BindgenOutput;

/* The files named by the line markers of preprocessed `text`, in order of appearance. */
void bindgen_files(const char *text, size_t length, BindgenNames *files);

/* True if `path` is under one of `dirs` (paths compared with `/`, without case on Windows). */
bool bindgen_is_own(const char *path, const char *const *dirs, size_t count);

/* Appends the names of the object-like macros `#define NAME value` of a header's `source`
   that start with one of `prefixes` (any name without prefixes). Their values are asked of
   the preprocessor afterwards, which knows which `#if` branch defined them. */
void bindgen_macro_names(const char *source, size_t length, const char *const *prefixes, size_t prefix_count,
                         BindgenNames *names);

/* The marker that asks the preprocessor for the value of a macro: `__sosu_macro__ "NAME" NAME ;`. */
#define BINDGEN_PROBE "__sosu_macro__"

bool bindgen_generate(const BindgenInput *input, BindgenOutput *output);
void bindgen_output_free(BindgenOutput *output);
void bindgen_names_free(BindgenNames *names);

#endif
