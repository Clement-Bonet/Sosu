#ifndef SOSU_SEMA_H
#define SOSU_SEMA_H

#include "program.h"
#include "types.h"

/* Semantic analysis (spec §5 to §7). sema.c checks declarations: names and imports,
   types, classes (inheritance, `virtual` / `override`, interfaces) and visibility.
   sema_body.c checks the bodies of functions, methods and tests, and records the type
   of every expression in Expr.type. */

typedef struct MethodInfo MethodInfo;
typedef struct FnInfo FnInfo;

typedef struct {
    Name name;
    Type *type;
    const Field *decl;
} FieldInfo;

struct MethodInfo {
    const FnDecl *decl;
    Type *type; /* T_FN, without `self` */
    TypeInfo *owner;
    MethodInfo *overridden; /* the parent or interface method it overrides, or NULL */
    FnInfo *generic; /* a method with type parameters of its own (`map<U>`): them, as a generic function */
};

typedef struct {
    Name name;
    Type **fields;
    size_t field_count;
    const Variant *decl;
} VariantInfo;

/* A struct, class, interface or enum declaration. */
struct TypeInfo {
    const Decl *decl;
    Unit *unit;
    Name name;
    Type *type;
    TypeInfo *parent; /* class */
    TypeInfo **interfaces;
    size_t interface_count;
    FieldInfo *fields;
    size_t field_count;
    MethodInfo *methods;
    size_t method_count;
    VariantInfo *variants;
    size_t variant_count;
    int mark; /* traversal state */
    /* Generics (spec §5.4): the parameters of a generic declaration (T_PARAM types), or for one
       of its instances, the generic and its arguments (in `type->type_args`). An instance is
       `open` when its arguments contain parameters: it is checked, never generated. */
    Type **params;
    size_t param_count;
    TypeInfo *generic;
    struct { TypeInfo **items; size_t count; size_t cap; } instances;
    bool open;
};

/* A function, or a function of an extern block. */
struct FnInfo {
    const FnDecl *decl;
    Unit *unit;
    Type *type;
    bool is_extern;
    Type **params; /* a generic function: its type parameters, then its effect parameters */
    size_t param_count;
    Name *effect_params;
    size_t effect_param_count;
};

/* A constant of a module (spec §11.6): a number, `bool`, `char` or `str`. An integer constant is
   evaluated by semantic analysis (for array sizes, and so that an overflow is a compile-time
   error); the others are generated where they are used. */
typedef struct {
    const Decl *decl;
    Unit *unit;
    Type *type;
    int mark; /* 0: not visited, 1: being visited, 2: done (cycles) */
    bool cyclic;
    bool evaluated; /* an integer whose value is known */
    uint64_t value;
} ConstInfo;

typedef enum {
    SYM_TYPE,
    SYM_FN,
    SYM_MODULE, /* an import, named after the last name of its path */
    SYM_CONST,
} SymbolKind;

typedef struct {
    SymbolKind kind;
    Name name;
    bool is_pub;
    TypeInfo *type_info;
    FnInfo *fn;
    Unit *module;
    ConstInfo *constant;
} Symbol;

/* What the names of type and effect parameters stand for while a generic declaration is
   resolved or checked: its own parameters, or the arguments of one of its instances. */
typedef struct {
    Name *names;
    Type **types;
    size_t count;
    Name *effect_names;
    unsigned *effects; /* the mask each effect name stands for */
    size_t effect_count;
} TypeEnv;

/* The names declared or imported by a module. */
typedef struct {
    Symbol *items;
    size_t count;
    size_t cap;
} Scope;

typedef struct {
    Program *program;
    TypeTable types;
    Arena arena;
    Scope *scopes; /* one per unit, aligned with program->units */
    Unit *prelude; /* core.prelude: its public names are used without import, or NULL */
    struct { TypeInfo **items; size_t count; size_t cap; } type_infos;
    struct { FnInfo **items; size_t count; size_t cap; } fns;
    struct { ConstInfo **items; size_t count; size_t cap; } consts;
    struct { TypeInfo **items; size_t count; size_t cap; } instances; /* of generic types, in creation order */
    /* the type parameters of the methods that have their own, resolved once for every instance */
    struct { FnInfo **items; size_t count; size_t cap; } method_params;
    const TypeEnv *env; /* NULL outside generic declarations */
    bool quiet;         /* resolving the members of an instance: its errors were reported for the generic */
} Sema;

/* Checks a loaded program; errors go to program->diags. Returns true if there were none. */
bool sema_check(Sema *sema, Program *program);
void sema_free(Sema *sema);

/* True for the types that own a resource: `Arena`, and the values containing one. They are
   moved, not copied, and duplicated with `.copy()` (spec §6). */
bool type_owns_resource(const Type *type);

/* Generics (spec §5.4), also used by code generation. The instance of `generic` for `args`
   (the generic itself for its own parameters), created on first use. */
TypeInfo *sema_instance(Sema *sema, TypeInfo *generic, Type **args);
/* `type` with the parameters `from[i]` replaced by `to[i]`, and the effect parameter bits
   by `effects[i]` (NULL: none). */
Type *sema_subst(Sema *sema, Type *type, Type **from, Type **to, size_t count, const unsigned *effects,
                 size_t effect_count);
/* Checks again the body of an instance, with its arguments, before it is generated: the
   types of its expressions become those of the instance. */
void sema_check_instance_fn(Sema *sema, FnInfo *fn, Type **args, const unsigned *effects);
void sema_check_instance_method(Sema *sema, TypeInfo *instance, MethodInfo *method);
/* A method with type parameters of its own (`map<U>`), for `args` and `effects`; returns its type there. */
Type *sema_check_generic_method(Sema *sema, TypeInfo *owner, MethodInfo *method, Type **args, const unsigned *effects);
void sema_check_instance_field(Sema *sema, TypeInfo *instance, const FieldInfo *field);
/* True if `type` contains a type parameter. */
bool type_is_open(const Type *type);

#endif
