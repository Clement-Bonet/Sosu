#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L /* mkdir, rmdir, unlink with -std=c99 */
#endif

#include "shell.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

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

/* Folders are made and removed by the system itself, not by a command: a path never goes
   through a shell. */
static void make_one_dir(const char *path) {
#ifdef _WIN32
    CreateDirectoryA(path, NULL);
#else
    mkdir(path, 0777);
#endif
}

void shell_make_dir(const char *path) {
    char *native = shell_path(path);
    /* each folder above it first: `a`, `a/b`, then `a/b/c` (not the root, nor a drive `C:`) */
    for (char *c = native + 1; *c; c++) {
        if ((*c == '/' || *c == '\\') && c[-1] != ':' && c[-1] != '/' && c[-1] != '\\') {
            char separator = *c;
            *c = '\0';
            make_one_dir(native);
            *c = separator;
        }
    }
    make_one_dir(native);
    free(native);
}

/* Removes `path` and what it holds, if it exists. A link is removed itself, never followed. */
static void remove_tree(const char *path) {
#ifdef _WIN32
    DWORD attributes = GetFileAttributesA(path);
    if (attributes == INVALID_FILE_ATTRIBUTES) return;
    if (attributes & FILE_ATTRIBUTE_READONLY) SetFileAttributesA(path, attributes & ~(DWORD)FILE_ATTRIBUTE_READONLY);
    if (!(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
        DeleteFileA(path);
        return;
    }
    if (!(attributes & FILE_ATTRIBUTE_REPARSE_POINT)) { /* a junction: removed below, not entered */
        char *pattern = sosu_format("%s\\*", path);
        WIN32_FIND_DATAA found;
        HANDLE handle = FindFirstFileA(pattern, &found);
        free(pattern);
        if (handle != INVALID_HANDLE_VALUE) {
            do {
                const char *name = found.cFileName;
                if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;
                char *child = sosu_format("%s\\%s", path, name);
                remove_tree(child);
                free(child);
            } while (FindNextFileA(handle, &found));
            FindClose(handle);
        }
    }
    RemoveDirectoryA(path);
#else
    if (unlink(path) == 0) return; /* a file or a link */
    DIR *folder = opendir(path);
    if (!folder) return;
    struct dirent *entry;
    while ((entry = readdir(folder))) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        char *child = sosu_format("%s/%s", path, entry->d_name);
        remove_tree(child);
        free(child);
    }
    closedir(folder);
    rmdir(path);
#endif
}

void shell_remove_dir(const char *path) {
    char *native = shell_path(path);
    remove_tree(native);
    free(native);
}

bool shell_file_exists(const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    fclose(file);
    return true;
}

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
