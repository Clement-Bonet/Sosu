// What DiskTiles asks of the system and `std.fs` does not say: the size of an entry without
// following its link. A link (symbolic link, junction) is reported as `other` and never entered,
// so that a link to a parent folder cannot make the scan loop, and a size is not counted twice.
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L /* lstat: -std=c99 hides it (glibc) */
#endif
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>

// kind: 0 a file, 1 a folder, 2 anything else (a link, a device...); 0 returned if it is known.
int32_t dt_entry(const uint8_t *path, int64_t *size, int32_t *kind) {
    static __declspec(thread) wchar_t wide[32800];
    wchar_t *w = wide + 4;
    int n = MultiByteToWideChar(CP_UTF8, 0, (const char *)path, -1, w, 32760);
    if (n <= 0) return 1;
    for (int i = 0; i < n; i++) {
        if (w[i] == L'/') w[i] = L'\\';
    }
    wchar_t *use = w;
    if (n > 240 && w[1] == L':') {  // a long path: the prefix lifts the limit of 260
        memcpy(wide, L"\\\\?\\", 4 * sizeof(wchar_t));
        use = wide;
    }
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!GetFileAttributesExW(use, GetFileExInfoStandard, &data)) return 1;
    *size = 0;
    if (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
        *kind = 2;
    } else if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
        *kind = 1;
    } else {
        *kind = 0;
        *size = (int64_t)(((uint64_t)data.nFileSizeHigh << 32) | data.nFileSizeLow);
    }
    return 0;
}

// Shows an entry in the file manager (the folder that holds it, the entry selected).
int32_t dt_reveal(const uint8_t *path) {
    static __declspec(thread) wchar_t cmd[32800];
    static __declspec(thread) wchar_t wide[32700];
    int n = MultiByteToWideChar(CP_UTF8, 0, (const char *)path, -1, wide, 32700);
    if (n <= 0) return 1;
    for (int i = 0; i < n; i++) {
        if (wide[i] == L'/') wide[i] = L'\\';
    }
    if (_snwprintf(cmd, 32800, L"explorer.exe /select,\"%ls\"", wide) < 0) return 1;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) return 1;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 0;
}
#else
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <stdlib.h>

int32_t dt_reveal(const uint8_t *path) {
    char folder[4096];
    strncpy(folder, (const char *)path, sizeof folder - 1);
    folder[sizeof folder - 1] = 0;
    pid_t pid = fork();
    if (pid < 0) return 1;
    if (pid == 0) {
#ifdef __APPLE__
        execlp("open", "open", "-R", (const char *)path, (char *)NULL);
#else
        char *slash = strrchr(folder, '/');
        if (slash && slash != folder) *slash = 0;
        execlp("xdg-open", "xdg-open", folder, (char *)NULL);
#endif
        _exit(1);
    }
    return 0;
}

int32_t dt_entry(const uint8_t *path, int64_t *size, int32_t *kind) {
    struct stat info;
    if (lstat((const char *)path, &info) != 0) return 1;
    *size = 0;
    if (S_ISREG(info.st_mode)) {
        *kind = 0;
        *size = (int64_t)info.st_size;
    } else if (S_ISDIR(info.st_mode)) {
        *kind = 1;
    } else {
        *kind = 2;
    }
    return 0;
}
#endif
