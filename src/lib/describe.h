/* `sosuc describe`: the description of the components of a package, written from its comments
   (describe.c, spec §11.4). */
#ifndef SOSU_DESCRIBE_H
#define SOSU_DESCRIBE_H

#include "diag.h"
#include "source.h"

/* The name of the module written, in `src/` of the package. */
#define DESCRIBE_MODULE "described"

typedef struct {
    const char *module;     /* its path in the package, without the package: `parts`, `ai.brain` */
    const SourceFile *file; /* its text */
} DescribeSource;

/* The text of `src/described.sosu` for the package `package` from its modules (the generated one
   left out): for each struct marked `// component`, the classes that describe and save it and
   make it a kind of the editor, and `add_all`. NULL when something is wrong, said in `diags`.
   `*components` gets the number of components (may be NULL). */
char *describe_package(const char *package, const DescribeSource *sources, size_t count, DiagList *diags,
                       size_t *components);

#endif
