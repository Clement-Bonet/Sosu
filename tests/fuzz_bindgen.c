/* The fuzzing of `sosuc bindgen` (spec §11.6): any bytes are read as the output of the C
   preprocessor for a header of its own, the declarations turned into a Sosu module, and that
   module formatted, as `sosuc bindgen` does. Nothing may crash, read outside of its memory nor
   loop forever, whatever the header.

   With libFuzzer (clang -fsanitize=fuzzer): `make fuzz-bindgen`. A crash found by the CI is
   replayed with `build/fuzz_bindgen crash-...`. */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "bindgen.h"
#include "fmt.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    /* the line marker of a header in the folder `own`, so that its declarations are bound */
    static const char marker[] = "# 1 \"own/fuzz.h\"\n";
    char *text = malloc(sizeof marker + size);
    if (!text) return 0;
    memcpy(text, marker, sizeof marker - 1);
    memcpy(text + sizeof marker - 1, data, size);
    size_t length = sizeof marker - 1 + size;
    text[length] = '\0';

    const char *own[] = { "own" };
    BindgenInput input = { text, length, own, 1, NULL, 0, size % 2 == 0, "// fuzz", "fuzz.h" };
    BindgenOutput output;
    bindgen_generate(&input, &output);
    SourceFile module;
    source_from_string(&module, "fuzz.sosu", output.module);
    DiagList diags = {0};
    free(format_source(&module, &diags));
    diag_free(&diags);
    source_free(&module);
    bindgen_output_free(&output);
    free(text);
    return 0;
}
