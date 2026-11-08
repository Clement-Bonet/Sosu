/* The fuzzing of the language server (`sosuc lsp`): any bytes are cut at each NUL byte into the
   bodies of messages, given in order to a new server, as an editor would send them. Nothing may
   crash, read outside of its memory nor loop forever, whatever the editor sends.

   With libFuzzer (clang -fsanitize=fuzzer): `make fuzz-lsp`, run from the repository root, where
   the standard library is in lib/. A crash found by the CI is replayed with
   `build/fuzz_lsp crash-...`. */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "lsp.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    char *text = malloc(size + 1);
    if (!text) return 0;
    memcpy(text, data, size);
    text[size] = '\0';

    LspServer *server = lsp_new("lib");
    size_t start = 0;
    bool running = true;
    for (size_t i = 0; running && i <= size; i++) {
        if (i < size && text[i] != '\0') continue;
        LspMessages out = {0};
        running = lsp_handle(server, text + start, i - start, &out);
        lsp_messages_free(&out);
        start = i + 1;
    }
    lsp_free(server);
    free(text);
    return 0;
}
