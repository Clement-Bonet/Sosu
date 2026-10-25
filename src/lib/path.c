#include "path.h"

#include <stdio.h>
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

static bool file_exists(const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    fclose(file);
    return true;
}

char *path_of_program(const char *argv0) {
    for (const char *c = argv0; *c; c++) {
        if (is_separator(*c)) return sosu_strdup(argv0);
    }
    const char *search = getenv("PATH");
#ifdef _WIN32
    const char list = ';';
    bool has_extension = strchr(argv0, '.') != NULL;
#else
    const char list = ':';
    bool has_extension = true;
#endif
    for (const char *start = search; start && *start;) {
        const char *end = strchr(start, list);
        size_t length = end ? (size_t)(end - start) : strlen(start);
        if (length > 0) {
            char *dir = sosu_format("%.*s", (int)length, start);
            char *candidate = path_join(dir, argv0);
            char *with_exe = has_extension ? NULL : sosu_format("%s.exe", candidate);
            free(dir);
            if (with_exe && file_exists(with_exe)) {
                free(candidate);
                return with_exe;
            }
            free(with_exe);
            if (file_exists(candidate)) return candidate;
            free(candidate);
        }
        start = end ? end + 1 : NULL;
    }
    return sosu_strdup(argv0);
}
