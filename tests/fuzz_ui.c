/* The fuzzing of the interface files (spec §10.2): any bytes are read as a `.ui.sosu` file, whose
   nodes become Sosu functions while it is parsed, then formatted, as `sosuc fmt` does. Nothing may
   crash, read outside of its memory nor loop forever.

   With libFuzzer (clang -fsanitize=fuzzer): `make fuzz-ui`. A crash found by the CI is replayed
   with `build/fuzz_ui crash-...`. */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "fmt.h"
#include "parser.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    char *text = malloc(size + 1);
    if (!text) return 0;
    memcpy(text, data, size);
    text[size] = '\0';

    SourceFile file;
    source_from_string(&file, "fuzz.ui.sosu", text);
    DiagList diags = {0};
    TokenList tokens = {0};
    Arena arena = {0};
    lex_file(&file, &tokens, &diags);
    parse_ui_module(&file, &tokens, &arena, &diags);
    arena_free(&arena);
    token_list_free(&tokens);
    free(format_source(&file, &diags));
    diag_free(&diags);
    source_free(&file);
    free(text);
    return 0;
}
