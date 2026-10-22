#ifndef SOSU_PROGRAM_H
#define SOSU_PROGRAM_H

#include "git.h"
#include "manifest.h"
#include "parser.h"

/* A program is a package, its dependencies, and every module reachable by `import`
   from its entry file (spec §8.1). */

typedef struct Package Package;
typedef struct Unit Unit;

typedef enum {
    PACKAGE_UNVISITED,
    PACKAGE_LOADING, /* its dependencies are being loaded: reaching it again is a cycle */
    PACKAGE_LOADED,
} PackageState;

struct Package {
    char *name;
    char *root; /* normalized directory containing sosu.toml */
    bool builtin; /* `core` and `std`, found in the library folder */
    PackageState state;
    SourceFile manifest_file;
    Manifest manifest;
    Package **deps; /* aligned with manifest.deps; NULL where loading failed */
};

typedef struct {
    const Decl *decl; /* DECL_IMPORT */
    Unit *unit;       /* NULL if it could not be resolved (already reported) */
} ImportLink;

/* One source file: a module, named after its package and path (`app.shapes.circle`). */
struct Unit {
    size_t index; /* position in Program.units */
    char *module;
    Package *package;
    SourceFile file;
    TokenList tokens;
    Module *ast;
    ImportLink *imports;
    size_t import_count;
};

typedef struct {
    struct { Package **items; size_t count; size_t cap; } packages; /* packages.items[0] is the root */
    struct { Unit **items; size_t count; size_t cap; } units;       /* units.items[0] is the entry file */
    Arena arena;    /* syntax trees */
    DiagList diags;
    char *lib_dir;  /* folder of the `core` and `std` packages */
    char *fatal;    /* an error with no position in a file (missing root package), or NULL */
    char *deps_dir; /* where git dependencies are cloned: `build/deps` of the root package */
    LockFile lock;  /* sosu.lock of the root package, as read */
    LockFile used;  /* the commits of the git dependencies loaded */
} Program;

/* Loads the package in `dir`, its dependencies and its modules, starting from
   `src/main.sosu` (an application) or `src/lib.sosu` (a library). Git dependencies are
   cloned if needed, and `sosu.lock` is updated when their commits change.
   Returns true if no error was reported. */
bool program_load(Program *program, const char *dir, const char *lib_dir);
/* A program made of one module (`test.main`, file `test.sosu`) given as text; it can
   import `core` and `std`. Used by the tests. */
bool program_load_text(Program *program, const char *text, const char *lib_dir);
void program_free(Program *program);

#endif
