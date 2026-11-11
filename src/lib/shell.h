#ifndef SOSU_SHELL_H
#define SOSU_SHELL_H

#include "common.h"

/* Other programs (`cc`, `git`) and the files of the system. A program is run with its arguments,
   never through a shell (`cmd`, `sh`): nothing in a path or a value of sosu.toml is read as a
   command. */

/* A path in the form the system expects (`\` on Windows). New string. */
char *shell_path(const char *path);

/* A program and its arguments, found in the PATH like a shell would. */
typedef struct {
    char **items; /* the program, then its arguments: owned */
    size_t count, cap;
    const char *dir; /* the folder it runs in; NULL: this one */
    const char *out; /* the file its output goes to; NULL: this program's */
    const char *err; /* the file of its errors, which may be `out`; NULL: this program's */
} Command;

/* The file that keeps nothing, for `out` or `err`. */
#ifdef _WIN32
#define SHELL_NOWHERE "NUL"
#else
#define SHELL_NOWHERE "/dev/null"
#endif

/* Appends an argument (copied). */
void command_add(Command *command, const char *arg);
/* Appends an argument written by printf. */
void command_addf(Command *command, const char *fmt, ...);
/* Runs it and waits for it; returns its exit code, or 127 after saying why it did not start. */
int command_run(const Command *command);
void command_free(Command *command);
#ifdef _WIN32
/* Makes the tools of MSVC (`cl`, `lib`, `rc`) those of the commands run after it, for `arch`
   (an argument of vcvarsall.bat, `x64_arm64`...) or for this machine (NULL): the environment of
   vcvarsall.bat, found with vswhere, is read once into this process. `work_dir` holds what they
   write. False after saying that there is no MSVC. */
bool shell_use_msvc(const char *arch, const char *work_dir);
#endif
/* Sets a variable of the environment, which the commands run after it inherit. */
void shell_set_env(const char *name, const char *value);
/* Creates a folder and its parents, if they do not exist. */
void shell_make_dir(const char *path);
/* Deletes a folder and everything in it, if it exists. */
void shell_remove_dir(const char *path);
/* True if the file exists and can be read. */
bool shell_file_exists(const char *path);
/* True if the folder exists. */
bool shell_dir_exists(const char *path);
#ifdef _WIN32
/* Waits about a second: a file just written may stay open a moment (antivirus, indexing). */
void shell_wait_second(void);
#endif
/* The entry of a folder that comes last in byte order (the newest of folders named by version,
   `27.1.12297006`), or NULL if it is empty or does not exist. New string. */
char *shell_last_entry(const char *dir);

/* The names of the files of `dir` that end in `suffix` (not its folders), sorted; `*count` of
   them. Each name and the array are new; NULL with 0 if there are none. */
char **shell_files_ending(const char *dir, const char *suffix, size_t *count);

/* The files of `dir` and of its subfolders that end in `suffix`, as paths relative to `dir` with
   `/` (`a/b.cpp`), sorted; folders whose name starts with `.` are skipped. NULL with 0 if none. */
char **shell_files_below(const char *dir, const char *suffix, size_t *count);

/* The time of the last change of the file at `path` (seconds, or finer, from some origin of the
   system) and its size; false if it is not there. */
bool shell_file_stamp(const char *path, long long *time, long long *size);

/* The newest time of the last change of the files under `dir`, its subfolders included, but
   those of `skip` (paths written as `dir` and its files are, with `/` or `\`); 0 if there are
   none. */
long long shell_newest_time(const char *dir, char *const *skip, size_t skip_count);

#endif
