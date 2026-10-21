#ifndef SOSU_SEMA_H
#define SOSU_SEMA_H

#include "program.h"
#include "types.h"

/* Semantic analysis (spec §5 to §7). sema.c checks declarations: names and imports,
   types, classes (inheritance, `virtual` / `override`, interfaces) and visibility.
   sema_body.c checks the bodies of functions, methods and tests, and records the type
   of every expression in Expr.type. */

typedef struct MethodInfo MethodInfo;

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
};

/* A function, or a function of an extern block. */
typedef struct {
    const FnDecl *decl;
    Unit *unit;
    Type *type;
    bool is_extern;
} FnInfo;

typedef enum {
    SYM_TYPE,
    SYM_FN,
    SYM_MODULE, /* an import, named after the last name of its path */
} SymbolKind;

typedef struct {
    SymbolKind kind;
    Name name;
    bool is_pub;
    TypeInfo *type_info;
    FnInfo *fn;
    Unit *module;
} Symbol;

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
} Sema;

/* Checks a loaded program; errors go to program->diags. Returns true if there were none. */
bool sema_check(Sema *sema, Program *program);
void sema_free(Sema *sema);

#endif
