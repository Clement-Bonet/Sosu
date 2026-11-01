/* sqlite: SQLite 3.54.1 (vendor/sqlite3.c, its amalgamation, public domain, see
   THIRD_PARTY_NOTICES.md) behind small functions of plain C types, for the declarations of
   src/sqlite.sosu. Texts come with their length in bytes, never ended by a NUL. */
/* Before any #include: glibc reads it at the first one. */
#if !defined(_WIN32) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE /* -std=c99 hides the POSIX functions SQLite calls (pread, fsync...) */
#endif
#include <stdint.h>
#include <string.h>
/* A connection belongs to one thread (spec §5.6): SQLite's mutexes per connection are not
   needed, only those of its shared state. */
#define SQLITE_THREADSAFE 2
#define SQLITE_DQS 0 /* "x" is a column, never a text */
#define SQLITE_DEFAULT_MEMSTATUS 0
#define SQLITE_DEFAULT_FOREIGN_KEYS 1
#define SQLITE_LIKE_DOESNT_MATCH_BLOBS
#define SQLITE_OMIT_DEPRECATED
#define SQLITE_OMIT_LOAD_EXTENSION /* no code loaded from a file; no -ldl */
#define SQLITE_OMIT_SHARED_CACHE
#include "vendor/sqlite3.c"

/* The connection to the file at `path` (NUL-ended UTF-8), created if it is not there, or NULL
   with `*code` set. Locks held by another connection are waited for, up to 5 seconds. */
void *sosu_sqlite_open(const uint8_t *path, int32_t read_only, int32_t *code) {
    sqlite3 *db = NULL;
    int flags = read_only ? SQLITE_OPEN_READONLY : SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE;
    int rc = sqlite3_open_v2((const char *)path, &db, flags | SQLITE_OPEN_URI, NULL);
    if (rc != SQLITE_OK) {
        sqlite3_close_v2(db);
        *code = rc;
        return NULL;
    }
    sqlite3_busy_timeout(db, 5000);
    *code = 0;
    return db;
}

/* Closes the connection, after the statements still open on it. */
void sosu_sqlite_close(void *db) {
    sqlite3_stmt *stmt;
    while ((stmt = sqlite3_next_stmt((sqlite3 *)db, NULL)) != NULL) sqlite3_finalize(stmt);
    sqlite3_close_v2((sqlite3 *)db);
}

/* The statements of `sql` (NUL-ended), one after the other. */
int32_t sosu_sqlite_exec(void *db, const uint8_t *sql) {
    return sqlite3_exec((sqlite3 *)db, (const char *)sql, NULL, NULL, NULL);
}

static int is_blank(const char *s, const char *end) {
    for (; s < end; s++) {
        if (*s != ' ' && *s != '\t' && *s != '\r' && *s != '\n' && *s != ';') return 0;
    }
    return 1;
}

/* The statement of `size` bytes at `sql`, or NULL with `*code` set: SQLITE_ERROR also for an
   empty text, or one with more than one statement. */
void *sosu_sqlite_prepare(void *db, const uint8_t *sql, int32_t size, int32_t *code) {
    sqlite3_stmt *stmt = NULL;
    const char *tail = NULL;
    const char *text = (const char *)sql;
    int rc = sqlite3_prepare_v2((sqlite3 *)db, text, (int)size, &stmt, &tail);
    if (rc == SQLITE_OK && (!stmt || !is_blank(tail, text + size))) {
        sqlite3_finalize(stmt);
        stmt = NULL;
        rc = SQLITE_ERROR;
    }
    *code = rc;
    return stmt;
}

int32_t sosu_sqlite_parameters(void *stmt) {
    return sqlite3_bind_parameter_count((sqlite3_stmt *)stmt);
}

int32_t sosu_sqlite_bind_null(void *stmt, int32_t i) {
    return sqlite3_bind_null((sqlite3_stmt *)stmt, i);
}

int32_t sosu_sqlite_bind_integer(void *stmt, int32_t i, int64_t value) {
    return sqlite3_bind_int64((sqlite3_stmt *)stmt, i, value);
}

int32_t sosu_sqlite_bind_float(void *stmt, int32_t i, double value) {
    return sqlite3_bind_double((sqlite3_stmt *)stmt, i, value);
}

/* A text or bytes of `size` bytes, copied by SQLite; empty ones are not NULL. */
int32_t sosu_sqlite_bind_text(void *stmt, int32_t i, const uint8_t *data, int64_t size) {
    return sqlite3_bind_text64((sqlite3_stmt *)stmt, i, size ? (const char *)data : "", (sqlite3_uint64)size,
                               SQLITE_TRANSIENT, SQLITE_UTF8);
}

int32_t sosu_sqlite_bind_bytes(void *stmt, int32_t i, const uint8_t *data, int64_t size) {
    if (size == 0) return sqlite3_bind_zeroblob((sqlite3_stmt *)stmt, i, 0);
    return sqlite3_bind_blob64((sqlite3_stmt *)stmt, i, data, (sqlite3_uint64)size, SQLITE_TRANSIENT);
}

/* Runs `stmt` to its end; gives the rows it changed (0 for a statement that changes none, which
   `sqlite3_changes` would not say), or -1 with `*code` set. */
int64_t sosu_sqlite_run(void *db, void *stmt, int32_t *code) {
    sqlite3_int64 before = sqlite3_total_changes64((sqlite3 *)db);
    int rc;
    while ((rc = sqlite3_step((sqlite3_stmt *)stmt)) == SQLITE_ROW) {
    }
    sqlite3_reset((sqlite3_stmt *)stmt);
    if (rc != SQLITE_DONE) {
        *code = rc;
        return -1;
    }
    *code = 0;
    return sqlite3_total_changes64((sqlite3 *)db) == before ? 0 : sqlite3_changes64((sqlite3 *)db);
}

int32_t sosu_sqlite_step(void *stmt) {
    return sqlite3_step((sqlite3_stmt *)stmt);
}

/* Ready to run again, without parameters. */
void sosu_sqlite_reset(void *stmt) {
    sqlite3_reset((sqlite3_stmt *)stmt);
    sqlite3_clear_bindings((sqlite3_stmt *)stmt);
}

void sosu_sqlite_finalize(void *stmt) {
    sqlite3_finalize((sqlite3_stmt *)stmt);
}

int32_t sosu_sqlite_columns(void *stmt) {
    return sqlite3_column_count((sqlite3_stmt *)stmt);
}

/* 1 integer, 2 float, 3 text, 4 bytes, 5 null (SQLite's own numbers). */
int32_t sosu_sqlite_column_type(void *stmt, int32_t i) {
    return sqlite3_column_type((sqlite3_stmt *)stmt, i);
}

int64_t sosu_sqlite_column_integer(void *stmt, int32_t i) {
    return sqlite3_column_int64((sqlite3_stmt *)stmt, i);
}

double sosu_sqlite_column_float(void *stmt, int32_t i) {
    return sqlite3_column_double((sqlite3_stmt *)stmt, i);
}

/* The size of a text (`text` not 0) or of bytes, then those bytes copied to `out`. */
int64_t sosu_sqlite_column_size(void *stmt, int32_t i, int32_t text) {
    if (text) sqlite3_column_text((sqlite3_stmt *)stmt, i);
    else sqlite3_column_blob((sqlite3_stmt *)stmt, i);
    return sqlite3_column_bytes((sqlite3_stmt *)stmt, i);
}

void sosu_sqlite_column_copy(void *stmt, int32_t i, int32_t text, uint8_t *out) {
    const void *data = text ? (const void *)sqlite3_column_text((sqlite3_stmt *)stmt, i)
                            : sqlite3_column_blob((sqlite3_stmt *)stmt, i);
    int size = sqlite3_column_bytes((sqlite3_stmt *)stmt, i);
    if (data && size > 0) memcpy(out, data, (size_t)size);
}

/* The name of a column, NUL-ended (NULL when there is no such column). */
const uint8_t *sosu_sqlite_column_name(void *stmt, int32_t i) {
    return (const uint8_t *)sqlite3_column_name((sqlite3_stmt *)stmt, i);
}

int64_t sosu_sqlite_last_insert_id(void *db) {
    return sqlite3_last_insert_rowid((sqlite3 *)db);
}

/* 0 inside a transaction. */
int32_t sosu_sqlite_autocommit(void *db) {
    return sqlite3_get_autocommit((sqlite3 *)db);
}

/* What SQLite said of the last error, NUL-ended. */
const uint8_t *sosu_sqlite_error(void *db) {
    return (const uint8_t *)sqlite3_errmsg((sqlite3 *)db);
}

/* The size of a NUL-ended text, and the text copied to `out`. */
int64_t sosu_sqlite_length(const uint8_t *s) {
    return s ? (int64_t)strlen((const char *)s) : 0;
}

void sosu_sqlite_copy(const uint8_t *s, uint8_t *out, int64_t size) {
    if (s && size > 0) memcpy(out, s, (size_t)size);
}

const uint8_t *sosu_sqlite_version(void) {
    return (const uint8_t *)sqlite3_libversion();
}
