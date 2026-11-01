/* The fuzzing of the compiler (work 5 of spec §13.1): a text, any bytes, is loaded as a module,
   checked, and turned into C, as `sosuc build` does; the formatter reads it too. Nothing may
   crash, read outside of its memory nor loop forever, whatever the text.

   With libFuzzer (clang -fsanitize=fuzzer, or MSVC /fsanitize=fuzzer): `make fuzz`, run from
   the repository root, where the standard library is in lib/. Without it (SOSU_FUZZ_MAIN), the
   files given are run once each: a crash found by the CI is replayed with `build/fuzz_replay
   crash-...`. */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "codegen.h"
#include "fmt.h"
#include "sema.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    char *text = malloc(size + 1);
    if (!text) return 0;
    memcpy(text, data, size);
    text[size] = '\0'; /* a NUL ends the text, as in a file read by the compiler */

    Program program;
    if (program_load_text(&program, text, "lib")) {
        Sema sema;
        if (sema_check(&sema, &program)) {
            CodegenOptions checks = { true, true, true };
            free(codegen_program(&sema, &program, CODEGEN_PROGRAM, checks));
        }
        sema_free(&sema);
    }
    program_free(&program);

    SourceFile file;
    source_from_string(&file, "fuzz.sosu", text);
    DiagList diags = {0};
    free(format_source(&file, &diags));
    diag_free(&diags);
    source_free(&file);
    free(text);
    return 0;
}

#ifdef SOSU_FUZZ_MAIN
int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        FILE *file = fopen(argv[i], "rb");
        if (!file) {
            fprintf(stderr, "cannot read %s\n", argv[i]);
            return 2;
        }
        size_t cap = 4096, size = 0, got;
        uint8_t *data = malloc(cap);
        while (data && (got = fread(data + size, 1, cap - size, file)) > 0) {
            size += got;
            if (size == cap) data = realloc(data, cap *= 2);
        }
        fclose(file);
        if (!data) return 3;
        clock_t start = clock();
        LLVMFuzzerTestOneInput(data, size);
        free(data);
        printf("%s: ok, %.3f s\n", argv[i], (double)(clock() - start) / CLOCKS_PER_SEC);
    }
    return 0;
}
#endif
