/* The reference of the public API of a package, in Markdown (docgen.c). */
#ifndef SOSU_DOCGEN_H
#define SOSU_DOCGEN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

typedef struct {
    const char *module; /* `std.io`, `app.shapes.circle` */
    const char *text;   /* the source, in the canonical form of `sosuc fmt` */
} DocSource;

/* True if `text` declares something `pub` at the top level. */
bool docgen_has_public(const char *text);

/* Writes `intro`, a table of the modules with the first sentence of their header comment, then
   each module: its public declarations and the comments above them. */
void docgen_write(FILE *out, const char *intro, const DocSource *sources, size_t count);

#endif
