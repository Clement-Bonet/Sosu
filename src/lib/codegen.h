#ifndef SOSU_CODEGEN_H
#define SOSU_CODEGEN_H

#include "sema.h"

/* C99 code generation (spec §11.3, step 1). The generated file includes the runtime
   header `sosu_rt.h` (lib/runtime) and must be compiled with `sosu_rt.c`. */

typedef enum {
    CODEGEN_PROGRAM, /* `main` of the entry module */
    CODEGEN_TESTS,   /* runs the `test` blocks of the root package */
    CODEGEN_LIBRARY, /* no `main`: the `export "C"` functions, each under a guard (spec §9.2) */
} CodegenMode;

/* Run-time checks (spec §7.7). Debug: all on. Release: bounds only, unless `--checks=off`. */
typedef struct {
    bool overflow_checks; /* off: integer arithmetic is modulo 2^n */
    bool bounds_checks;   /* array and slice indexes */
    bool contracts;       /* `requires` and `ensures` */
} CodegenOptions;

/* Generates the C source of a checked program. Returns NULL after reporting to
   program->diags what code generation does not support yet. */
char *codegen_program(Sema *sema, Program *program, CodegenMode mode, CodegenOptions options);

/* The C header of a library built with `sosuc build --lib` (spec §9.2): its exports, the types
   they use, and `<package>_error()`. */
char *codegen_header(Sema *sema, Program *program);

#endif
