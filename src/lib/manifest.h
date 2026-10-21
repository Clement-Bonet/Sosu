#ifndef SOSU_MANIFEST_H
#define SOSU_MANIFEST_H

#include "diag.h"
#include "source.h"

/* `sosu.toml` (spec §8.1), a subset of TOML:

   [package]
   name = "myapp"
   version = "0.1.0"

   [dependencies]
   shared = { path = "../shared" }
   ui = { git = "https://git.example/sosu-ui", tag = "v0.1.0" }

   Values are strings or one-line inline tables of strings; `#` starts a comment. */

typedef struct {
    char *name; /* key in [dependencies]: the name used by `import` */
    char *path; /* the fields below are NULL when absent */
    char *git;
    char *tag;
    char *rev;
    size_t offset; /* span of the key in sosu.toml, for diagnostics */
    size_t length;
} Dependency;

typedef struct {
    char *name;
    char *version;
    size_t name_offset; /* span of the `name` value, for diagnostics */
    size_t name_length;
    struct {
        Dependency *items;
        size_t count;
        size_t cap;
    } deps;
} Manifest;

/* Parses a manifest. Problems are reported to `diags`; returns true if there were none. */
bool manifest_parse(Manifest *manifest, const SourceFile *file, DiagList *diags);
void manifest_free(Manifest *manifest);

/* True for `[A-Za-z_][A-Za-z0-9_]*`. */
bool is_identifier(const char *text);

#endif
