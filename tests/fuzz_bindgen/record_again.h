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
    Package **deps; /* aligned with manifest.deps; NULL where loading   Module *ast;
    ImportLink *imports;
    size_t import_count;
};

typedef struct {
    struct { Package **items; size_t count; size_t cap; } packages; /* packages.items[0] is the root */
    struct { Unit **items; size_t count; size_t cap; } units;       /* units.items[0] is the entry file */
    Arena arena;    /* syntax trees */
    DiagList diags;
    char *lib_dir;  /* folder of the `core` and `std` packages */
    char *fatal;    a package, its dependencies, and every module reachable by `import`
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
    Package **deps; /* aligned with manifest.deps; NULL where loading   Module *ast;
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
    char *deps_dir; /* where git depenits;       /* units.items[0] is the entry file */
    Arena arena;    /* syntax trees */
    DiagList diags;
    char *lib_dir;  /* folder of the `core` and `std` packages */
    char *fatal;    /* an error with no position in a file (missing root package), or NULL */
    char *deps_dir; /* where git dependencies are cloned: `build/deps` of the root packageogram_is_locale_file(const char *path);

#endif
