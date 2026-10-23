#ifndef SOSU_SOURCE_H
#define SOSU_SOURCE_H

#include "common.h"

typedef struct {
    char *path;
    char *text;          /* file contents, followed by a NUL byte */
    size_t length;       /* in bytes, without the NUL byte */
    size_t *line_starts; /* byte offset of each line */
    size_t line_count;
} SourceFile;

/* Text that replaces the contents of a file on disk, or NULL to read the file: the documents open
   in an editor, for the language server. */
typedef const char *(*SourceOverlay)(const char *path, size_t *length, void *data);
/* Sets the overlay that source_load asks first; NULL removes it. */
void source_set_overlay(SourceOverlay overlay, void *data);

/* Reads a file, or takes its text from the overlay. Returns false on I/O error (errno is set). */
bool source_load(SourceFile *file, const char *path);
/* Builds a source file from text in memory (used by tests and tools). */
void source_from_string(SourceFile *file, const char *path, const char *text);
void source_free(SourceFile *file);

/* 1-based line and column of a byte offset; columns count code points. */
void source_position(const SourceFile *file, size_t offset, size_t *line, size_t *column);
/* Text of a 1-based line, without its line terminator. */
const char *source_line(const SourceFile *file, size_t line, size_t *length);

#endif
