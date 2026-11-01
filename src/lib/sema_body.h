#ifndef SOSU_SEMA_BODY_H
#define SOSU_SEMA_BODY_H

/* Shared by the checks of function bodies: sema_body.c (statements, expressions, names),
   sema_call.c (calls, generics, arenas) and sema_lambda.c (lambdas and their captures). */

#include "sema_internal.h"

#define NAME_ARGS(name) (int)(name).length, (name).text
#define VEC(T) struct { T *items; size_t count; size_t cap; }
#define ALL_EFFECTS (EFFECT_IO | EFFECT_TIME | EFFECT_RANDOM | EFFECT_THREADS)
#define EFFECT_UNBOUND 0xFFFFFFFFu /* a generic effect parameter not inferred yet (sema_call.c) */

typedef struct {
    Name name;
    Type *type;
    bool is_mut;
    bool moved;          /* a value owning a resource that was moved away (spec §6) */
    size_t moved_offset; /* where */
    int loop_depth;      /* loops around its declaration */
    bool closure;        /* holds a lambda of this function that captures variables (spec §5.5) */
} Local;

/* A lambda being checked (spec §5.5): its own locals start at `base`; those below it, and
   `self`, are captured. Its effects are inferred when no type gives them. */
typedef struct LambdaScope {
    struct LambdaScope *outer;
    size_t base;
    VEC(Capture) captures;
    bool inferring;
    unsigned used;
} LambdaScope;

typedef struct {
    Sema *s;
    Unit *unit;
    TypeInfo *self_info; /* type of `self` in a method */
    Name fn_name;
    bool is_test;
    Type *ret;
    unsigned effects;
    bool in_init;
    bool in_ensures;
    int unsafe_depth;
    int loop_depth;
    int defer_depth;
    bool taking_address; /* `&x`: the checks of assignment apply */
    bool assigning;      /* the target of `=`: a moved variable may receive a new value */
    VEC(Local) locals;
    LambdaScope *lambda; /* the innermost lambda being checked, or NULL */
} Body;

/* Type names for one message; freed by release_names. */
typedef struct {
    char *items[4];
    int count;
} TypeNames;

typedef enum {
    REF_UNKNOWN,
    REF_ERROR, /* already reported */
    REF_VALUE, /* an expression to check normally (field of a value...) */
    REF_LOCAL,
    REF_FIELD,
    REF_METHOD,
    REF_FN,
    REF_TYPE,
    REF_MODULE,
    REF_VARIANT,
    REF_BUILTIN,
    REF_CONST,
} RefKind;

typedef struct {
    RefKind kind;
    Local *local;
    FieldInfo *field;
    MethodInfo *method;
    FnInfo *fn;
    Type *type;
    Unit *module;
    TypeInfo *info; /* REF_VARIANT: the enum */
    size_t variant;
    const char *builtin;
    ConstInfo *constant;
} Ref;

bool sema_all_bound(FnInfo *fn, const Type *type, Type **bound, const unsigned *effects);
bool sema_always_returns(const Stmt *stmt);
Type *sema_bind(Body *b, FnInfo *fn, Type *type, Type **bound, const unsigned *effects);
Type *sema_builtin(Body *b, const char *name);
void sema_capture_local(Body *b, const Local *local);
void sema_capture_self(Body *b);
void sema_check_block(Body *b, Stmt *block);
Type *sema_check_call(Body *b, Expr *e, Type *expected);
void sema_check_escape(Body *b, const Expr *e, const char *where);
Type *sema_check_expect(Body *b, Expr *e, Type *expected);
Type *sema_check_expr(Body *b, Expr *e, Type *expected);
void sema_check_expr_stmt(Body *b, Expr *e);
Type *sema_check_lambda(Body *b, Expr *e, Type *expected, FnInfo *generic, Type **bound, unsigned *effects);
void sema_check_pair(Body *b, Expr *left, Expr *right, Type *hint, Type **lt, Type **rt);
void sema_consume(Body *b, Expr *e);
void sema_declare_local(Body *b, Name name, Type *type, bool is_mut);
Diagnostic *sema_error_at(Body *b, const char *code, size_t offset, size_t length, const char *fmt, ...);
Diagnostic *sema_error_expr(Body *b, const char *code, const Expr *e, const char *fmt, ...);
Type *sema_error_type(Body *b);
FieldInfo *sema_find_field(TypeInfo *info, Name name);
Local *sema_find_local(Body *b, Name name);
MethodInfo *sema_find_method(TypeInfo *info, Name name);
TypeInfo *sema_generic_of(const Type *t);
bool sema_is_closure(Body *b, const Expr *e);
bool sema_is_error(const Type *t);
bool sema_is_nominal(const Type *t);
bool sema_is_numeric(const Type *t);
bool sema_is_untyped_literal(const Expr *e);
bool sema_name_is(Name name, const char *text);
Type *sema_named_instance(Body *b, const Expr *named, Type *type, Type *expected);
void sema_record(Expr *e, Ref ref);
void sema_release_names(TypeNames *names);
void sema_report_mismatch(Body *b, const Expr *e, Type *expected, Type *found);
void sema_report_unknown(Body *b, Name name);
Ref sema_resolve_ref(Body *b, Expr *e, Type *expected);
const char *sema_tn(TypeNames *names, const Type *type);
Type *sema_type_argument(Body *b, Expr *arg);
Type *sema_void_type(Body *b);

#endif
