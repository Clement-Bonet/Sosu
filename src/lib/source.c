#include "source.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    size_t *items;
    size_t count;
    size_t cap;
} OffsetVec;

static bool has_bom(const SourceFile *file) {
    return file->length >= 3 && memcmp(file->text, "\xEF\xBB\xBF", 3) == 0;
}

static void init(SourceFile *file, const char *path, char *text, size_t length) {
    OffsetVec lines = {0};
    SOSU_VEC_PUSH(&lines, 0);
    for (size_t i = 0; i < length; i++) {
        if (text[i] == '\n') SOSU_VEC_PUSH(&lines, i + 1);
    }
    text[length] = '\0';
    file->path = sosu_strdup(path);
    file->text = text;
    file->length = length;
    file->line_starts = lines.items;
    file->line_count = lines.count;
}

static SourceOverlay overlay;
static void *overlay_data;

void source_set_overlay(SourceOverlay function, void *data) {
    overlay = function;
    overlay_data = data;
}

bool source_load(SourceFile *file, const char *path) {
    size_t open_length = 0;
    const char *open = overlay ? overlay(path, &open_length, overlay_data) : NULL;
    if (open) {
        char *copy = sosu_alloc(open_length + 1);
        memcpy(copy, open, open_length);
        init(file, path, copy, open_length);
        return true;
    }

    FILE *f = fopen(path, "rb");
    if (!f) return false;

    size_t cap = 4096;
    size_t length = 0;
    char *text = sosu_alloc(cap);
    for (;;) {
        if (length + 1 >= cap) {
            cap *= 2;
            text = sosu_realloc(text, cap);
        }
        size_t n = fread(text + length, 1, cap - length - 1, f);
        if (n == 0) break;
        length += n;
    }
    bool failed = ferror(f) != 0;
    fclose(f);
    if (failed) {
        free(text);
        return false;
    }
    init(file, path, text, length);
    return true;
}

void source_from_string(SourceFile *file, const char *path, const char *text) {
    size_t length = strlen(text);
    char *copy = sosu_alloc(length + 1);
    memcpy(copy, text, length);
    init(file, path, copy, length);
}

void source_free(SourceFile *file) {
    free(file->path);
    free(file->text);
    free(file->line_starts);
    memset(file, 0, sizeof *file);
}

void source_position(const SourceFile *file, size_t offset, size_t *line, size_t *column) {
    size_t lo = 0;
    size_t hi = file->line_count;
    while (hi - lo > 1) {
        size_t mid = lo + (hi - lo) / 2;
        if (file->line_starts[mid] <= offset) lo = mid;
        else hi = mid;
    }
    size_t start = file->line_starts[lo];
    if (lo == 0 && has_bom(file) && offset >= 3) start = 3;

    size_t col = 1;
    for (size_t i = start; i < offset && i < file->length; i++) {
        if (((unsigned char)file->text[i] & 0xC0) != 0x80) col++;
    }
    *line = lo + 1;
    *column = col;
}

const char *source_line(const SourceFile *file, size_t line, size_t *length) {
    size_t start = file->line_starts[line - 1];
    size_t end = line < file->line_count ? file->line_starts[line] : file->length;
    if (line == 1 && has_bom(file)) start = 3;
    while (end > start && (file->text[end - 1] == '\n' || file->text[end - 1] == '\r')) end--;
    *length = end - start;
    return file->text + start;
}
