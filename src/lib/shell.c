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

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dirent.h>
#endif

#include <string.h>

char *shell_last_entry(const char *dir) {
    char *last = NULL;
#ifdef _WIN32
    char *pattern = sosu_format("%s\\*", dir);
    WIN32_FIND_DATAA found;
    HANDLE handle = FindFirstFileA(pattern, &found);
    free(pattern);
    if (handle == INVALID_HANDLE_VALUE) return NULL;
    do {
        const char *name = found.cFileName;
        if (name[0] == '.') continue;
        if (!last || strcmp(name, last) > 0) {
            free(last);
            last = sosu_strdup(name);
        }
    } while (FindNextFileA(handle, &found));
    FindClose(handle);
#else
    DIR *folder = opendir(dir);
    if (!folder) return NULL;
    struct dirent *entry;
    while ((entry = readdir(folder))) {
        if (entry->d_name[0] == '.') continue;
        if (!last || strcmp(entry->d_name, last) > 0) {
            free(last);
            last = sosu_strdup(entry->d_name);
        }
    }
    closedir(folder);
#endif
    return last;
}

bool shell_dir_exists(const char *path) {
#ifdef _WIN32
    DWORD attributes = GetFileAttributesA(path);
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
#else
    DIR *folder = opendir(path);
    if (!folder) return false;
    closedir(folder);
    return true;
#endif
}

#ifdef _WIN32
void shell_wait_second(void) {
    Sleep(1000);
}
#endif
