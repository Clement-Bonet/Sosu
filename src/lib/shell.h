#ifndef SOSU_SHELL_H
#define SOSU_SHELL_H

#include "common.h"

/* Commands run through the system shell (`cmd` on Windows, `sh` elsewhere): the compiler
   needs only the libc, and calls `cc` or `git` like a user would. */

/* A path in the form the system shell expects (`\` on Windows). New string. */
char *shell_path(const char *path);
/* Runs a command; returns its exit code. */
int shell_run(const char *command);
/* Creates a folder and its parents, if they do not exist. */
void shell_make_dir(const char *path);
/* Deletes a folder and everything in it, if it exists. */
void shell_remove_dir(const char *path);
/* True if the file exists and can be read. */
bool shell_file_exists(const char *path);
/* The entry of a folder that comes last in byte order (the newest of folders named by version,
   `27.1.12297006`), or NULL if it is empty or does not exist. New string. */
char *shell_last_entry(const char *dir);

#endif
