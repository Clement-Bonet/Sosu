#ifndef SOSU_PATH_H
#define SOSU_PATH_H

#include "common.h"

/* File paths use `/` everywhere (Windows accepts it). Results are newly allocated. */

/* `base/relative`, or `relative` alone when it is absolute or `base` is empty. */
char *path_join(const char *base, const char *relative);
/* Lexical normalization: `\` becomes `/`, `.` and `dir/..` are removed. */
char *path_normalize(const char *path);
/* Directory part of a path (`.` when there is none). */
char *path_dirname(const char *path);

#endif
