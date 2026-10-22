#include "shell.h"

#include <stdio.h>
#include <stdlib.h>

char *shell_path(const char *path) {
    char *result = sosu_strdup(path);
#ifdef _WIN32
    for (char *c = result; *c; c++) {
        if (*c == '/') *c = '\\';
    }
#endif
    return result;
}

int shell_run(const char *command) {
    fflush(stdout);
    fflush(stderr);
    int status = system(command);
#ifndef _WIN32
    if (status != -1) status = (status >> 8) & 0xFF; /* exit code from the wait status */
#endif
    return status;
}

void shell_make_dir(const char *path) {
    char *native = shell_path(path);
#ifdef _WIN32
    char *command = sosu_format("if not exist \"%s\" mkdir \"%s\"", native, native);
#else
    char *command = sosu_format("mkdir -p \"%s\"", native);
#endif
    shell_run(command);
    free(command);
    free(native);
}

void shell_remove_dir(const char *path) {
    char *native = shell_path(path);
#ifdef _WIN32
    char *command = sosu_format("if exist \"%s\" rmdir /s /q \"%s\"", native, native);
#else
    char *command = sosu_format("rm -rf \"%s\"", native);
#endif
    shell_run(command);
    free(command);
    free(native);
}

bool shell_file_exists(const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    fclose(file);
    return true;
}
