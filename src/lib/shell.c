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
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
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

void command_add(Command *command, const char *arg) {
    SOSU_VEC_PUSH(command, sosu_strdup(arg));
}

void command_addf(Command *command, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    SOSU_VEC_PUSH(command, sosu_vformat(fmt, args));
    va_end(args);
}

void command_free(Command *command) {
    for (size_t i = 0; i < command->count; i++) free(command->items[i]);
    free(command->items);
    memset(command, 0, sizeof *command);
}

#ifdef _WIN32
/* Appends `arg` to a command line, quoted as the C library of Windows reads it back: a quote
   behind a backslash, and the backslashes before a quote doubled. An argument without a space
   nor a quote stays as it is: some programs read their line themselves (xcopy refuses "/E"). */
static void quote_arg(char **line, size_t *size, size_t *cap, const char *arg) {
    size_t length = strlen(arg);
    if (*size + 2 * length + 4 > *cap) {
        *cap = 2 * (*size + 2 * length + 4);
        *line = sosu_realloc(*line, *cap);
    }
    char *out = *line;
    if (*size) out[(*size)++] = ' ';
    if (length && !strpbrk(arg, " \t\n\v\"")) {
        memcpy(out + *size, arg, length + 1);
        *size += length;
        return;
    }
    out[(*size)++] = '"';
    for (size_t i = 0; i < length; i++) {
        if (arg[i] == '"') {
            for (size_t k = i; k > 0 && arg[k - 1] == '\\'; k--) out[(*size)++] = '\\';
            out[(*size)++] = '\\';
        }
        out[(*size)++] = arg[i];
    }
    for (size_t k = length; k > 0 && arg[k - 1] == '\\'; k--) out[(*size)++] = '\\';
    out[(*size)++] = '"';
    out[*size] = '\0';
}

static HANDLE open_output(const char *path) {
    SECURITY_ATTRIBUTES inherit = { sizeof inherit, NULL, TRUE };
    return CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, CREATE_ALWAYS,
                       FILE_ATTRIBUTE_NORMAL, NULL);
}

/* Runs a command line as it is, with the folder and the files of `command`. */
static int run_line(char *line, const Command *command, const char *program) {
    STARTUPINFOA start;
    memset(&start, 0, sizeof start);
    start.cb = sizeof start;
    HANDLE out = INVALID_HANDLE_VALUE, err = INVALID_HANDLE_VALUE;
    if (command->out || command->err) {
        start.dwFlags = STARTF_USESTDHANDLES;
        start.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        start.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
        start.hStdError = GetStdHandle(STD_ERROR_HANDLE);
        const char *unwritable = NULL;
        if (command->out) {
            start.hStdOutput = out = open_output(command->out);
            if (out == INVALID_HANDLE_VALUE) unwritable = command->out;
        }
        if (command->err && command->out && strcmp(command->err, command->out) == 0) {
            start.hStdError = out;
        } else if (command->err) {
            start.hStdError = err = open_output(command->err);
            if (err == INVALID_HANDLE_VALUE) unwritable = command->err;
        }
        if (unwritable) {
            fprintf(stderr, "sosuc: cannot write `%s`\n", unwritable);
            if (out != INVALID_HANDLE_VALUE) CloseHandle(out);
            if (err != INVALID_HANDLE_VALUE) CloseHandle(err);
            return 127;
        }
    }
    fflush(stdout);
    fflush(stderr);
    PROCESS_INFORMATION process;
    BOOL started = CreateProcessA(NULL, line, NULL, NULL, TRUE, 0, NULL, command->dir, &start, &process);
    DWORD failure = GetLastError();
    if (out != INVALID_HANDLE_VALUE) CloseHandle(out);
    if (err != INVALID_HANDLE_VALUE) CloseHandle(err);
    if (!started) {
        fprintf(stderr, "sosuc: cannot run `%s`%s\n", program,
                failure == ERROR_FILE_NOT_FOUND || failure == ERROR_PATH_NOT_FOUND ? ": it is not in the PATH" : "");
        return 127;
    }
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hProcess);
    CloseHandle(process.hThread);
    return (int)code;
}

int command_run(const Command *command) {
    char *line = NULL;
    size_t size = 0, cap = 0;
    for (size_t i = 0; i < command->count; i++) quote_arg(&line, &size, &cap, command->items[i]);
    int status = line ? run_line(line, command, command->items[0]) : 127;
    free(line);
    return status;
}

/* The first line of a file, without its end of line, or "" (new string). */
static char *first_line(const char *path) {
    char line[2048] = "";
    FILE *file = fopen(path, "r");
    if (file) {
        if (!fgets(line, sizeof line, file)) line[0] = '\0';
        fclose(file);
    }
    line[strcspn(line, "\r\n")] = '\0';
    return sosu_strdup(line);
}

/* The variables `NAME=value` of a file written by `set`, put in the environment of this process. */
static bool read_environment(const char *path) {
    FILE *file = fopen(path, "r");
    if (!file) return false;
    static char line[32768]; /* the longest a variable can be */
    bool found = false;
    while (fgets(line, sizeof line, file)) {
        line[strcspn(line, "\r\n")] = '\0';
        char *equal = strchr(line, '=');
        if (!equal || equal == line) continue;
        *equal = '\0';
        _putenv_s(line, equal + 1);
        found = found || _stricmp(line, "VCToolsInstallDir") == 0;
    }
    fclose(file);
    return found;
}

bool shell_use_msvc(const char *arch, const char *work_dir) {
    static char used[32] = ""; /* the architecture already read: "" none, "-" this machine's */
    const char *key = arch ? arch : "-";
    if (strcmp(used, key) == 0) return true;
    char found[MAX_PATH];
    if (!arch && !used[0] && SearchPathA(NULL, "cl.exe", NULL, MAX_PATH, found, NULL)) { /* a prompt of Visual Studio */
        snprintf(used, sizeof used, "%s", key);
        return true;
    }
    const char *programs = getenv("ProgramFiles(x86)");
    char *installer = sosu_format("%s\\Microsoft Visual Studio\\Installer", programs ? programs : "C:\\Program Files (x86)");
    char *vswhere = sosu_format("%s\\vswhere.exe", installer);
    char *where_log = sosu_format("%s/vswhere.txt", work_dir);
    char *set_log = sosu_format("%s/vcvars.txt", work_dir);
    bool ok = false;
    if (shell_file_exists(vswhere)) {
        Command where = { .out = where_log, .err = SHELL_NOWHERE };
        const char *args[] = { vswhere, "-latest", "-products", "*", "-requires",
                               "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property", "installationPath" };
        for (size_t i = 0; i < sizeof args / sizeof *args; i++) command_add(&where, args[i]);
        char *vs = command_run(&where) == 0 ? first_line(where_log) : sosu_strdup("");
        command_free(&where);
        /* the one line read by cmd: the folder of Visual Studio, never a path of the user */
        if (vs[0] && !strpbrk(vs, "\"%^&|<>!")) {
            const char *path = getenv("PATH");
            char *with_installer = sosu_format("%s;%s", path ? path : "", installer); /* vcvarsall runs vswhere */
            _putenv_s("PATH", with_installer);
            free(with_installer);
            const char *cmd = getenv("ComSpec");
            char *line = sosu_format("\"%s\" /d /s /c \"\"%s\\VC\\Auxiliary\\Build\\%s\" %s >nul && set\"",
                                     cmd && cmd[0] ? cmd : "cmd.exe", vs, arch ? "vcvarsall.bat" : "vcvars64.bat",
                                     arch ? arch : "");
            Command set = { .out = set_log, .err = SHELL_NOWHERE };
            ok = run_line(line, &set, "cmd") == 0 && read_environment(set_log);
            free(line);
        }
        free(vs);
    }
    remove(where_log);
    remove(set_log);
    if (ok) snprintf(used, sizeof used, "%s", key);
    else fprintf(stderr, "sosuc: no C compiler found: install Visual Studio with the C++ tools%s, or set SOSU_CC\n",
                 arch ? " of the architecture of the target (ARM64 or x64)" : "");
    free(set_log);
    free(where_log);
    free(vswhere);
    free(installer);
    return ok;
}
#else
static int open_output(const char *path, int fd) {
    int file = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (file < 0) return -1;
    dup2(file, fd);
    close(file);
    return 0;
}

int command_run(const Command *command) {
    if (command->count == 0) return 127;
    char **argv = sosu_alloc((command->count + 1) * sizeof *argv);
    for (size_t i = 0; i < command->count; i++) argv[i] = command->items[i];
    argv[command->count] = NULL;
    int report[2];
    if (pipe(report) != 0) {
        free(argv);
        return 127;
    }
    fcntl(report[1], F_SETFD, FD_CLOEXEC); /* closed by a successful exec */
    fflush(stdout);
    fflush(stderr);
    pid_t child = fork();
    if (child == 0) {
        close(report[0]);
        int failure = 0;
        if (command->dir && chdir(command->dir) != 0) failure = errno;
        if (!failure && command->out && open_output(command->out, 1) != 0) failure = errno;
        if (!failure && command->err) {
            if (command->out && strcmp(command->err, command->out) == 0) dup2(1, 2);
            else if (open_output(command->err, 2) != 0) failure = errno;
        }
        if (!failure) {
            execvp(argv[0], argv);
            failure = errno;
        }
        ssize_t written = write(report[1], &failure, sizeof failure);
        (void)written;
        _exit(127);
    }
    free(argv);
    close(report[1]);
    if (child < 0) {
        close(report[0]);
        fprintf(stderr, "sosuc: cannot run `%s`\n", command->items[0]);
        return 127;
    }
    int failure = 0;
    ssize_t reported;
    while ((reported = read(report[0], &failure, sizeof failure)) < 0 && errno == EINTR) {
    }
    close(report[0]);
    int status = 0;
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
    }
    if (reported == (ssize_t)sizeof failure) {
        fprintf(stderr, "sosuc: cannot run `%s`: %s\n", command->items[0],
                failure == ENOENT ? "it is not in the PATH" : strerror(failure));
        return 127;
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
}
#endif

void shell_set_env(const char *name, const char *value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
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
    struct stat info;
    if (lstat(path, &info) != 0) return;
    if (!S_ISDIR(info.st_mode)) { /* a file or a link: removed itself, even to a folder */
        unlink(path);
        return;
    }
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
