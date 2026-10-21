#ifndef SOSU_MANIFEST_H
#define SOSU_MANIFEST_H

#include "diag.h"
#include "source.h"

/* `sosu.toml` (spec §8.1), a subset of TOML:

   [package]
   name = "myapp"
   version = "0.1.0"

   [dependencies]
   shared = { path = "../shared" }
   ui = { git = "https://git.example/sosu-ui", tag = "v0.1.0" }

   [link]                         # C libraries, for every target
   libs = ["SDL3"]

   [link.windows]                 # added on one target: windows, linux, macos, android, ios
   paths = ["vendor/SDL3/lib/x64"]
   copy = ["vendor/SDL3/lib/x64/SDL3.dll"]

   Values are strings, one-line arrays of strings, or one-line inline tables of strings;
   `#` starts a comment. */

/* Target names of [link.<target>]; LINK_ALL is [link]. */
enum {
    LINK_ALL,
    LINK_WINDOWS,
    LINK_LINUX,
    LINK_MACOS,
    LINK_ANDROID,
    LINK_IOS,
    LINK_SECTION_COUNT,
};

extern const char *const link_section_names[LINK_SECTION_COUNT];

typedef struct {
    char **items;
    size_t count;
    size_t cap;
} StringList;

/* Paths are relative to the folder of the package. */
typedef struct {
    StringList libs;       /* library names: `SDL3` gives SDL3.lib or -lSDL3 */
    StringList paths;      /* folders where the linker looks for them */
    StringList frameworks; /* macOS and iOS frameworks */
    StringList copy;       /* files placed next to the program (DLLs...) */
} LinkSection;

typedef struct {
    char *name; /* key in [dependencies]: the name used by `import` */
    char *path; /* the fields below are NULL when absent */
    char *git;
    char *tag;
    char *rev;
    size_t offset; /* span of the key in sosu.toml, for diagnostics */
    size_t length;
} Dependency;

typedef struct {
    char *name;
    char *version;
    size_t name_offset; /* span of the `name` value, for diagnostics */
    size_t name_length;
    struct {
        Dependency *items;
        size_t count;
        size_t cap;
    } deps;
    LinkSection link[LINK_SECTION_COUNT];
} Manifest;

/* Parses a manifest. Problems are reported to `diags`; returns true if there were none. */
bool manifest_parse(Manifest *manifest, const SourceFile *file, DiagList *diags);
void manifest_free(Manifest *manifest);

/* True for `[A-Za-z_][A-Za-z0-9_]*`. */
bool is_identifier(const char *text);

#endif
