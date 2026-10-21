#ifndef SOSU_TYPES_H
#define SOSU_TYPES_H

#include "arena.h"
#include "ast.h"

/* Semantic types. Structural types (pointers, options, function types...) are interned:
   two equal types are the same pointer, so types are compared with `==`. Nominal types
   (struct, class, interface, enum) have one Type each, owned by their TypeInfo (sema.h). */

typedef enum {
    T_ERROR, /* the type of something already reported as wrong: accepted everywhere */
    T_VOID,
    T_BOOL,
    T_CHAR,
    T_STR,
    T_INT,
    T_FLOAT,
    T_POINTER,
    T_OPTION,
    T_RESULT,
    T_ARRAY,
    T_SLICE,
    T_FN,
    T_STRUCT,
    T_CLASS,
    T_INTERFACE,
    T_ENUM,
} TypeTag;

/* Effects (spec §7.4), as bits of a mask. */
enum {
    EFFECT_IO = 1,
    EFFECT_TIME = 2,
    EFFECT_RANDOM = 4,
    EFFECT_THREADS = 8,
};

typedef struct TypeInfo TypeInfo;
typedef struct Type Type;

struct Type {
    TypeTag tag;
    Name name; /* built-in and nominal types */
    union {
        struct { uint8_t bits; bool is_signed; } integer; /* isize / usize: 64 bits */
        uint8_t float_bits;
        Type *inner; /* POINTER, OPTION, SLICE */
        struct { Type *element; uint64_t size; } array;
        struct { Type *value; Type *error; } result;
        struct { Type **params; size_t param_count; Type *ret; unsigned effects; } fn;
        TypeInfo *info; /* STRUCT, CLASS, INTERFACE, ENUM */
    } as;
};

typedef struct {
    Arena arena;
    struct { Type **items; size_t count; size_t cap; } interned;
    struct { Type **items; size_t count; size_t cap; } builtins;
    Type *error;
    Type *void_;
} TypeTable;

void types_init(TypeTable *table);
void types_free(TypeTable *table);

/* The built-in type with this name (`i32`, `str`...), or NULL. */
Type *type_builtin(const TypeTable *table, const char *name, size_t length);

Type *type_pointer(TypeTable *table, Type *inner);
Type *type_option(TypeTable *table, Type *inner);
Type *type_slice(TypeTable *table, Type *inner);
Type *type_array(TypeTable *table, Type *element, uint64_t size);
Type *type_result(TypeTable *table, Type *value, Type *error);
Type *type_fn(TypeTable *table, Type **params, size_t param_count, Type *ret, unsigned effects);

/* Source form of a type, e.g. "Vec2*" or "fn(i32) -> i32 uses io". New string. */
char *type_to_string(const Type *type);

/* EFFECT_* bit for an effect name, or 0. */
unsigned effect_from_name(const char *name, size_t length);
const char *effect_name(unsigned effect);

#endif
