#ifndef SOSU_GIT_H
#define SOSU_GIT_H

#include "common.h"

/* Git dependencies (spec §8.1). They are cloned with the `git` command into the build
   folder of the root package, one folder per commit: `build/deps/<name>-<commit>`.

   `sosu.lock`, next to the root `sosu.toml`, keeps the commit of every git dependency of
   the program, one per line:

       # comment
       <name> <url> tag|rev <tag or rev> <commit>

   While a dependency keeps its URL and its `tag` or `rev`, the commit of the lock is used,
   even if the tag has moved since. */

typedef struct {
    char *name;
    char *url;    /* as written in sosu.toml */
    char *kind;   /* "tag" or "rev" */
    char *ref;    /* the tag, or the rev */
    char *commit; /* full hash */
} LockEntry;

typedef struct {
    LockEntry *items;
    size_t count;
    size_t cap;
} LockFile;

/* Reads a lock file; an absent file gives an empty lock, and malformed lines are skipped. */
void lock_read(LockFile *lock, const char *path);
/* Writes the entries, sorted by name. Returns false if the file cannot be written. */
bool lock_write(const LockFile *lock, const char *path);
/* The entry of a dependency if it matches `url`, `kind` and `ref`, else NULL. */
const LockEntry *lock_find(const LockFile *lock, const char *name, const char *url, const char *kind, const char *ref);
/* Adds an entry, or replaces the one of the same name. */
void lock_set(LockFile *lock, const char *name, const char *url, const char *kind, const char *ref, const char *commit);
/* True if both locks have the same entries, in any order. */
bool lock_equal(const LockFile *a, const LockFile *b);
void lock_free(LockFile *lock);

/* NULL if `url` and `ref` can be passed to the shell, else what is wrong. */
const char *git_check_source(const char *url, const char *ref);
/* True for a URL that names a folder (not `https://...` nor `user@host:path`). */
bool git_is_local(const char *url);

/* Clones `url` into `deps_dir/<name>-<commit>` and checks out `commit` if it is not NULL,
   else the tag or rev. Nothing is fetched when that folder already holds a package.
   Returns the folder and sets *commit_out; on failure, returns NULL and sets *error. */
char *git_fetch(const char *deps_dir, const char *name, const char *url, const char *kind, const char *ref,
                const char *commit, char **commit_out, char **error);

#endif
