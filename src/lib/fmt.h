#ifndef SOSU_FMT_H
#define SOSU_FMT_H

#include "diag.h"
#include "source.h"

/* The canonical form of a source file (spec §7.6): one style, no options. The file is
   parsed, then printed from its syntax tree; comments are put back where they were.
   Returns NULL if the file has errors (reported to `diags`): a file that does not parse
   is not formatted. */
char *format_source(const SourceFile *file, DiagList *diags);

#endif
