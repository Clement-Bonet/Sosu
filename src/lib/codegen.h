#ifndef SOSU_CODEGEN_H
#define SOSU_CODEGEN_H

#include "sema.h"

/* C99 code generation (spec §11.3, step 1). The generated file includes the runtime
   header `sosu_rt.h` (lib/runtime) and must be compiled with `sosu_rt.c`. */

typedef enum {
    CODEGEN_PROGRAM, /* `main` of the entry module */
    CODEGEN_TESTS,   /* runs the `test` blocks of the root package */
} CodegenMode;

/* Generates the C source of a checked program. Returns NULL after reporting to
   program->diags what code generation does not support yet. */
char *codegen_program(Sema *sema, Program *program, CodegenMode mode);

#endif
