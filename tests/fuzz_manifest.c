/* The fuzzing of `sosu.toml` (spec §8.1), which comes with every git dependency: any bytes are
   parsed as a manifest, and the source of each git dependency checked, as `sosuc` does before
   fetching it. Nothing may crash, read outside of its memory nor loop forever.

   With libFuzzer (clang -fsanitize=fuzzer): `make fuzz-manifest`. A crash found by the CI is
   replayed with `build/fuzz_manifest crash-...`. */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "git.h"
#include "manifest.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    char *text = malloc(size + 1);
    if (!text) return 0;
    memcpy(text, data, size);
    text[size] = '\0';

    SourceFile file;
    source_from_string(&file, "sosu.toml", text);
    Manifest manifest;
    memset(&manifest, 0, sizeof manifest);
    DiagList diags = {0};
    if (manifest_parse(&manifest, &file, &diags)) {
        for (size_t i = 0; i < manifest.deps.count; i++) {
            const Dependency *dep = &manifest.deps.items[i];
            const char *ref = dep->tag ? dep->tag : dep->rev;
            if (dep->git && ref) git_check_source(dep->git, ref); /* as fetch_git of program.c */
        }
    }
    manifest_free(&manifest);
    diag_free(&diags);
    source_free(&file);
    free(text);
    return 0;
}
