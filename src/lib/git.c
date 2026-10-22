#include "git.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "path.h"
#include "shell.h"

/* ---- sosu.lock ---- */

static void entry_free(LockEntry *entry) {
    free(entry->name);
    free(entry->url);
    free(entry->kind);
    free(entry->ref);
    free(entry->commit);
}

static bool is_commit(const char *text) {
    size_t length = strlen(text);
    if (length < 40) return false;
    for (size_t i = 0; i < length; i++) {
        if (!isxdigit((unsigned char)text[i])) return false;
    }
    return true;
}

void lock_read(LockFile *lock, const char *path) {
    memset(lock, 0, sizeof *lock);
    FILE *file = fopen(path, "r");
    if (!file) return;
    char line[4096];
    while (fgets(line, sizeof line, file)) {
        if (line[0] == '#') continue;
        char *words[6];
        size_t count = 0;
        for (char *word = strtok(line, " \t\r\n"); word && count < 6; word = strtok(NULL, " \t\r\n")) words[count++] = word;
        if (count != 5 || (strcmp(words[2], "tag") != 0 && strcmp(words[2], "rev") != 0) || !is_commit(words[4])) continue;
        lock_set(lock, words[0], words[1], words[2], words[3], words[4]);
    }
    fclose(file);
}

static int compare_entries(const void *a, const void *b) {
    return strcmp(((const LockEntry *)a)->name, ((const LockEntry *)b)->name);
}

bool lock_write(const LockFile *lock, const char *path) {
    LockEntry *sorted = sosu_alloc((lock->count ? lock->count : 1) * sizeof *sorted);
    if (lock->count) memcpy(sorted, lock->items, lock->count * sizeof *sorted);
    qsort(sorted, lock->count, sizeof *sorted, compare_entries);
    FILE *file = fopen(path, "wb");
    bool ok = file != NULL;
    if (file) {
        ok = fputs("# sosu.lock: the commit of each git dependency, written by sosuc. Keep it in version control.\n",
                   file) >= 0;
        for (size_t i = 0; i < lock->count; i++) {
            const LockEntry *e = &sorted[i];
            ok = ok && fprintf(file, "%s %s %s %s %s\n", e->name, e->url, e->kind, e->ref, e->commit) > 0;
        }
        ok = fclose(file) == 0 && ok;
    }
    free(sorted);
    return ok;
}

const LockEntry *lock_find(const LockFile *lock, const char *name, const char *url, const char *kind, const char *ref) {
    for (size_t i = 0; i < lock->count; i++) {
        const LockEntry *e = &lock->items[i];
        if (strcmp(e->name, name) == 0) {
            bool same = strcmp(e->url, url) == 0 && strcmp(e->kind, kind) == 0 && strcmp(e->ref, ref) == 0;
            return same ? e : NULL;
        }
    }
    return NULL;
}

void lock_set(LockFile *lock, const char *name, const char *url, const char *kind, const char *ref, const char *commit) {
    LockEntry entry = { sosu_strdup(name), sosu_strdup(url), sosu_strdup(kind), sosu_strdup(ref), sosu_strdup(commit) };
    for (size_t i = 0; i < lock->count; i++) {
        if (strcmp(lock->items[i].name, name) == 0) {
            entry_free(&lock->items[i]);
            lock->items[i] = entry;
            return;
        }
    }
    SOSU_VEC_PUSH(lock, entry);
}

bool lock_equal(const LockFile *a, const LockFile *b) {
    if (a->count != b->count) return false;
    for (size_t i = 0; i < a->count; i++) {
        const LockEntry *e = &a->items[i];
        const LockEntry *found = lock_find(b, e->name, e->url, e->kind, e->ref);
        if (!found || strcmp(found->commit, e->commit) != 0) return false;
    }
    return true;
}

void lock_free(LockFile *lock) {
    for (size_t i = 0; i < lock->count; i++) entry_free(&lock->items[i]);
    free(lock->items);
    memset(lock, 0, sizeof *lock);
}

/* ---- Fetching ---- */

/* Characters that mean nothing to `cmd` or `sh` inside double quotes. */
static bool is_safe(const char *text, const char *extra) {
    if (!text[0] || text[0] == '-') return false;
    for (const char *c = text; *c; c++) {
        if (!isalnum((unsigned char)*c) && !strchr(extra, *c)) return false;
    }
    return true;
}

const char *git_check_source(const char *url, const char *ref) {
    if (!is_safe(url, "-._~:/@+=,\\")) return "its `git` URL has a character that is not allowed";
    if (!is_safe(ref, "-._/+") || strstr(ref, "..")) return "its `tag` or `rev` has a character that is not allowed";
    return NULL;
}

bool git_is_local(const char *url) {
    if (strstr(url, "://")) return false;
    const char *colon = strchr(url, ':');
    return !colon || colon - url == 1; /* none, or a drive letter */
}

/* The first line of a file, without its end of line, or "" (new string). */
static char *first_line(const char *path) {
    char line[1024] = "";
    FILE *file = fopen(path, "r");
    if (file) {
        while (fgets(line, sizeof line, file) && (line[0] == '\n' || line[0] == '\r')) { }
        fclose(file);
    }
    line[strcspn(line, "\r\n")] = '\0';
    return sosu_strdup(line);
}

/* Runs a git command whose output goes to `log`; on failure, sets *error from it. */
static bool run_git(const char *command, const char *log, const char *what, char **error) {
    char *native_log = shell_path(log);
    char *full = sosu_format("git %s > \"%s\" 2>&1", command, native_log);
    int status = shell_run(full);
    free(full);
    free(native_log);
    if (status == 0) return true;
    char *message = first_line(log);
    *error = message[0] ? sosu_format("%s: %s", what, message)
                        : sosu_format("%s (is `git` installed and in the PATH?)", what);
    free(message);
    return false;
}

static char *checkout_dir(const char *deps_dir, const char *name, const char *commit) {
    char *leaf = sosu_format("%s-%.12s", name, commit);
    char *dir = path_join(deps_dir, leaf);
    free(leaf);
    return dir;
}

static bool holds_package(const char *dir) {
    char *manifest = path_join(dir, "sosu.toml");
    bool found = shell_file_exists(manifest);
    free(manifest);
    return found;
}

char *git_fetch(const char *deps_dir, const char *name, const char *url, const char *kind, const char *ref,
                const char *commit, char **commit_out, char **error) {
    *commit_out = NULL;
    *error = NULL;
    if (commit) {
        char *dir = checkout_dir(deps_dir, name, commit);
        if (holds_package(dir)) {
            *commit_out = sosu_strdup(commit);
            return dir;
        }
        free(dir);
    }

    shell_make_dir(deps_dir);
    char *leaf = sosu_format("%s-fetch", name);
    char *work = path_join(deps_dir, leaf);
    char *log = sosu_format("%s.log", work);
    free(leaf);
    shell_remove_dir(work);

    char *target = commit ? sosu_strdup(commit) : strcmp(kind, "tag") == 0 ? sosu_format("refs/tags/%s", ref)
                                                                         : sosu_strdup(ref);
    char *clone = sosu_format("clone --quiet --no-checkout \"%s\" \"%s\"", url, work);
    char *checkout = sosu_format("-C \"%s\" -c advice.detachedHead=false checkout --quiet \"%s\"", work, target);
    char *rev_parse = sosu_format("-C \"%s\" rev-parse HEAD", work);
    char *what_clone = sosu_format("cannot clone `%s`", url);
    char *what_checkout = sosu_format("cannot check out %s `%s`", commit ? "the commit" : kind, commit ? commit : ref);
    char *dir = NULL;
    if (run_git(clone, log, what_clone, error) && run_git(checkout, log, what_checkout, error) &&
        run_git(rev_parse, log, "cannot read the commit", error)) {
        char *found = first_line(log);
        if (is_commit(found)) {
            dir = checkout_dir(deps_dir, name, found);
            if (holds_package(dir)) {
                shell_remove_dir(work);
            } else {
                shell_remove_dir(dir);
                if (rename(work, dir) != 0) {
                    *error = sosu_format("cannot move the clone to `%s`", dir);
                    free(dir);
                    dir = NULL;
                }
            }
            if (dir) *commit_out = sosu_strdup(found);
        } else {
            *error = sosu_format("cannot read the commit: `%s`", found);
        }
        free(found);
    }
    if (!dir) shell_remove_dir(work);
    remove(log);
    free(target);
    free(clone);
    free(checkout);
    free(rev_parse);
    free(what_clone);
    free(what_checkout);
    free(work);
    free(log);
    return dir;
}
