#include "path.h"

#include <stdlib.h>
#include <string.h>

static bool is_separator(char c) { return c == '/' || c == '\\'; }

static bool is_drive(const char *segment) {
    return strlen(segment) == 2 && segment[1] == ':';
}

static bool is_absolute(const char *path) {
    return is_separator(path[0]) || (path[0] && path[1] == ':');
}

char *path_join(const char *base, const char *relative) {
    if (!base[0] || is_absolute(relative)) return sosu_strdup(relative);
    size_t length = strlen(base);
    bool separator = is_separator(base[length - 1]);
    return sosu_format(separator ? "%s%s" : "%s/%s", base, relative);
}

char *path_normalize(const char *path) {
    char *copy = sosu_strdup(path);
    size_t length = strlen(copy);
    bool absolute = is_separator(copy[0]);
    const char **segments = sosu_alloc((length / 2 + 2) * sizeof *segments);
    size_t count = 0;

    char *segment = copy;
    for (size_t i = 0; i <= length; i++) {
        if (i < length && !is_separator(copy[i])) continue;
        copy[i] = '\0';
        if (segment[0] == '\0' || strcmp(segment, ".") == 0) {
            /* skip */
        } else if (strcmp(segment, "..") == 0) {
            if (count > 0 && strcmp(segments[count - 1], "..") != 0 && !is_drive(segments[count - 1])) count--;
            else if (!absolute && (count == 0 || !is_drive(segments[0]))) segments[count++] = segment;
        } else {
            segments[count++] = segment;
        }
        segment = copy + i + 1;
    }

    size_t size = 2;
    for (size_t i = 0; i < count; i++) size += strlen(segments[i]) + 1;
    char *result = sosu_alloc(size);
    char *out = result;
    if (absolute) *out++ = '/';
    for (size_t i = 0; i < count; i++) {
        if (i) *out++ = '/';
        size_t n = strlen(segments[i]);
        memcpy(out, segments[i], n);
        out += n;
    }
    if (out == result) *out++ = '.';
    *out = '\0';

    free(segments);
    free(copy);
    return result;
}

char *path_dirname(const char *path) {
    size_t length = strlen(path);
    while (length > 0 && !is_separator(path[length - 1])) length--;
    if (length == 0) return sosu_strdup(".");
    if (length > 1) length--; /* drop the separator, except for the root `/` */
    char *result = sosu_alloc(length + 1);
    memcpy(result, path, length);
    result[length] = '\0';
    return result;
}
