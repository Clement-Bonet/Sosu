#include "sema_internal.h"

#include <stdlib.h>
#include <string.h>

/* Checks function bodies (spec §5.3, §7). Each expression is checked against an
   optional expected type, which number literals, `none`, `ok(...)`, `err(...)` and
   bare enum variants take; the resulting type is stored in Expr.type. */

#define NAME_ARGS(name) (int)(name).length, (name).text
#define VEC(T) struct { T *items; size_t count; size_t cap; }
#define ALL_EFFECTS (EFFECT_IO | EFFECT_TIME | EFFECT_RANDOM | EFFECT_THREADS)

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

static Type *check_expr(Body *b, Expr *e, Type *expected);
static void check_assignable(Body *b, Expr *target);
static void check_stmt(Body *b, Stmt *stmt);
static void check_block(Body *b, Stmt *block);
static Type *named_instance(Body *b, const Expr *named, Type *type, Type *expected);
static Type *type_argument(Body *b, Expr *arg);

static const char *const builtin_names[] = { "some", "none", "ok", "err", "assert", "min", "max", "abs", "KB", "MB", "GB",
                                             "wrapping_add", "wrapping_sub", "wrapping_mul", "hash", "size_of" };

/* ---- Helpers ---- */

static Diagnostic *error_at(Body *b, const char *code, size_t offset, size_t length, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    Diagnostic *d = sema_verror(b->s, b->unit, code, offset, length, fmt, args);
    va_end(args);
    return d;
}

static Diagnostic *error_expr(Body *b, const char *code, const Expr *e, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    Diagnostic *d = sema_verror(b->s, b->unit, code, e->offset, e->length, fmt, args);
    va_end(args);
    return d;
}

static bool name_is(Name name, const char *text) {
    size_t length = strlen(text);
    return name.length == length && memcmp(name.text, text, length) == 0;
}

static Type *builtin(Body *b, const char *name) { return type_builtin(&b->s->types, name, strlen(name)); }
static Type *error_type(Body *b) { return b->s->types.error; }
static Type *void_type(Body *b) { return b->s->types.void_; }
static bool is_error(const Type *t) { return t->tag == T_ERROR; }
/* A type parameter allows what its constraints say (spec §5.4). */
static bool has_constraint(const Type *t, uint32_t constraint) {
    return t->tag == T_PARAM && (t->as.param.constraints & constraint);
}
static bool is_numeric(const Type *t) {
    return t->tag == T_INT || t->tag == T_FLOAT || has_constraint(t, CONSTRAINT_NUMBER);
}
static bool is_integer(const Type *t) { return t->tag == T_INT || has_constraint(t, CONSTRAINT_INTEGER); }

/* Type names for one message; freed by release_names. */
typedef struct {
    char *items[4];
    int count;
} TypeNames;

static const char *tn(TypeNames *names, const Type *type) {
    char *text = type_to_string(type);
    names->items[names->count++] = text;
    return text;
}

static void release_names(TypeNames *names) {
    for (int i = 0; i < names->count; i++) free(names->items[i]);
}

static void report_mismatch(Body *b, const Expr *e, Type *expected, Type *found) {
    if (is_error(expected) || is_error(found)) return;
    TypeNames n = {0};
    const char *expected_name = tn(&n, expected);
    Diagnostic *d = error_expr(b, "E0331", e, "expected `%s`, found `%s`", expected_name, tn(&n, found));
    if (is_numeric(expected) && is_numeric(found)) {
        diag_help(d, "conversions are explicit: write `... as %s`", expected_name);
    } else if (expected->tag == T_OPTION && expected->as.inner == found) {
        diag_help(d, "wrap the value: `some(...)`");
    } else if (expected->tag == T_RESULT && expected->as.result.value == found) {
        diag_help(d, "wrap the value: `ok(...)`");
    }
    release_names(&n);
}

/* ---- Moves (spec §6) ---- */

static Local *find_local(Body *b, Name name);

/* `e` is used as a value that goes elsewhere (a variable, an argument, a field, a returned
   value...): a variable owning a resource is moved, and cannot be used afterwards. */
static void consume(Body *b, Expr *e) {
    if (!e->type || !type_owns_resource(e->type)) return;
    if (e->kind == EXPR_TERNARY) {
        consume(b, e->as.ternary.then_expr);
        consume(b, e->as.ternary.else_expr);
        return;
    }
    if (e->kind == EXPR_NAME && e->resolution == RESOLVED_LOCAL) {
        Local *local = find_local(b, e->as.name);
        if (!local || local->moved) return;
        if (b->loop_depth > local->loop_depth) {
            Diagnostic *d = error_expr(b, "E0356", e, "`%.*s` would be moved at each iteration of the loop",
                                       NAME_ARGS(local->name));
            diag_help(d, "after the first iteration, it no longer has its value: move it before the loop, or pass a pointer");
        }
        local->moved = true;
        local->moved_offset = e->offset;
        return;
    }
    if (e->kind == EXPR_NAME || e->kind == EXPR_FIELD || e->kind == EXPR_INDEX ||
        (e->kind == EXPR_UNARY && e->as.unary.op == TOK_STAR)) {
        Diagnostic *d = error_expr(b, "E0356", e, "a value that owns a resource cannot be moved out of where it is stored");
        diag_help(d, "pass a pointer to it instead");
    }
}

/* Checks `e` and reports it if its type is not `expected`. */
static Type *check_expect(Body *b, Expr *e, Type *expected) {
    Type *type = check_expr(b, e, expected);
    if (type != expected) report_mismatch(b, e, expected, type);
    return expected;
}

/* ---- Names ---- */

static Local *find_local(Body *b, Name name) {
    for (size_t i = b->locals.count; i > 0; i--) {
        if (sema_name_eq(b->locals.items[i - 1].name, name)) return &b->locals.items[i - 1];
    }
    return NULL;
}

static void declare_local(Body *b, Name name, Type *type, bool is_mut) {
    if (name_is(name, "_")) return;
    Local *existing = find_local(b, name);
    if (existing) {
        Diagnostic *d = error_at(b, "E0347", name.offset, name.length, "`%.*s` is already declared in this function",
                                 NAME_ARGS(name));
        diag_help(d, "the first declaration is on line %zu: choose another name", sema_line_of(b->unit, existing->name.offset));
    }
    Local local = { name, type, is_mut, false, 0, b->loop_depth, false };
    SOSU_VEC_PUSH(&b->locals, local);
}

/* ---- Captures of lambdas (spec §5.5) ---- */

static void add_capture(Body *b, LambdaScope *scope, Name name, Type *type, bool is_self) {
    for (size_t i = 0; i < scope->captures.count; i++) {
        const Capture *c = &scope->captures.items[i];
        if (c->is_self == is_self && (is_self || sema_name_eq(c->name, name))) return;
    }
    Capture capture = { name, type, is_self };
    SOSU_VEC_PUSH(&scope->captures, capture);
    if (!is_self && type_owns_resource(type)) {
        Diagnostic *d = error_at(b, "E0369", name.offset, name.length,
                                 "a lambda cannot capture `%.*s`, which owns a resource", NAME_ARGS(name));
        diag_help(d, "a lambda copies what it captures: capture a pointer to it instead");
    }
}

/* `local` is used: the lambdas between it and here capture it. */
static void capture_local(Body *b, const Local *local) {
    size_t index = (size_t)(local - b->locals.items);
    for (LambdaScope *scope = b->lambda; scope && index < scope->base; scope = scope->outer) {
        add_capture(b, scope, local->name, local->type, false);
    }
}

/* `self` is used, written or not: every lambda being checked captures it. */
static void capture_self(Body *b) {
    for (LambdaScope *scope = b->lambda; scope; scope = scope->outer) {
        Name self = { "self", 4, 0 };
        add_capture(b, scope, self, b->self_info ? b->self_info->type : NULL, true);
    }
}

/* True if `e` is a lambda of this function that captures variables, or a variable holding
   one: it lives as long as the function, and stays in its variables and arguments. */
static bool is_closure(Body *b, const Expr *e) {
    switch (e->kind) {
    case EXPR_LAMBDA: return e->as.lambda.capture_count > 0;
    case EXPR_TERNARY: return is_closure(b, e->as.ternary.then_expr) || is_closure(b, e->as.ternary.else_expr);
    case EXPR_NAME: {
        if (e->resolution != RESOLVED_LOCAL) return false;
        const Local *local = find_local(b, e->as.name);
        return local && local->closure;
    }
    default: return false;
    }
}

/* `e` goes where it outlives the function (`where`: "returned", "stored in a field"...). */
static void check_escape(Body *b, const Expr *e, const char *where) {
    if (!e->type || e->type->tag != T_FN || !is_closure(b, e)) return;
    Diagnostic *d = error_expr(b, "E0370", e, "a lambda that captures variables cannot be %s", where);
    diag_help(d, "it lives as long as the function that creates it: call it, or pass it as an argument (spec §5.5)");
}

/* True if the place `e` is in a variable or a parameter of the function: the variable itself,
   or a field or an element of it, reached without going through a pointer or a view. */
static bool in_local_storage(Body *b, const Expr *e) {
    switch (e->kind) {
    case EXPR_NAME:
        return find_local(b, e->as.name) != NULL;
    case EXPR_FIELD: {
        const Type *object = e->as.field.object->type;
        return object && object->tag != T_POINTER && in_local_storage(b, e->as.field.object);
    }
    case EXPR_INDEX: {
        const Type *object = e->as.index.object->type;
        return object && object->tag == T_ARRAY && in_local_storage(b, e->as.index.object);
    }
    default:
        return false;
    }
}

/* The first part of `e` that would leave a pointer into the function once it returns: `&x`,
   `&x.f`, `&a[i]`, `a[i..j]` of a local array, also inside `some`, `ok`, `err`, a literal or
   `?:`. Pointers kept in variables first are not followed: the regions after 1.0 will. */
static const Expr *dangling_part(Body *b, const Expr *e) {
    switch (e->kind) {
    case EXPR_UNARY:
        return e->as.unary.op == TOK_AMP && in_local_storage(b, e->as.unary.operand) ? e : NULL;
    case EXPR_INDEX: {
        const Type *object = e->as.index.object->type;
        bool view = e->as.index.index->kind == EXPR_RANGE;
        return view && object && object->tag == T_ARRAY && in_local_storage(b, e->as.index.object) ? e : NULL;
    }
    case EXPR_CALL: {
        const Expr *callee = e->as.call.callee;
        if (callee->kind != EXPR_NAME || find_local(b, callee->as.name)) return NULL;
        if (!name_is(callee->as.name, "some") && !name_is(callee->as.name, "ok") && !name_is(callee->as.name, "err")) {
            return NULL;
        }
        for (size_t i = 0; i < e->as.call.arg_count; i++) {
            const Expr *part = dangling_part(b, e->as.call.args[i]);
            if (part) return part;
        }
        return NULL;
    }
    case EXPR_STRUCT_LIT:
        for (size_t i = 0; i < e->as.struct_lit.field_count; i++) {
            const Expr *part = dangling_part(b, e->as.struct_lit.fields[i].value);
            if (part) return part;
        }
        return NULL;
    case EXPR_ARRAY:
        for (size_t i = 0; !e->as.array.repeat && i < e->as.array.count; i++) {
            const Expr *part = dangling_part(b, e->as.array.items[i]);
            if (part) return part;
        }
        return e->as.array.repeat ? dangling_part(b, e->as.array.repeat) : NULL;
    case EXPR_TERNARY: {
        const Expr *part = dangling_part(b, e->as.ternary.then_expr);
        return part ? part : dangling_part(b, e->as.ternary.else_expr);
    }
    default:
        return NULL;
    }
}

/* A returned value must not point into the function (spec §7.7, E0371). */
static void check_dangling(Body *b, const Expr *e) {
    const Expr *part = dangling_part(b, e);
    if (!part) return;
    bool view = part->kind == EXPR_INDEX;
    Diagnostic *d = error_expr(b, "E0371", part, "%s returned would point into this function, which ends",
                               view ? "a view of a local array" : "the address of a local variable");
    diag_help(d, view ? "return the array itself, or a view of memory from an arena (`arena.alloc_slice`)"
                      : "return the value itself, or a pointer to memory from an arena (`arena.alloc`)");
}

/* True if `local` is a copy held by the innermost lambda being checked. */
static bool is_captured(Body *b, const Local *local) {
    return b->lambda && (size_t)(local - b->locals.items) < b->lambda->base;
}

static FieldInfo *find_field(TypeInfo *info, Name name) {
    for (TypeInfo *t = info; t; t = t->parent) {
        for (size_t i = 0; i < t->field_count; i++) {
            if (sema_name_eq(t->fields[i].name, name)) return &t->fields[i];
        }
    }
    return NULL;
}

static MethodInfo *find_method(TypeInfo *info, Name name) {
    for (TypeInfo *t = info; t; t = t->parent) {
        for (size_t i = 0; i < t->method_count; i++) {
            if (sema_name_eq(t->methods[i].decl->name, name)) return &t->methods[i];
        }
    }
    return NULL;
}

static bool find_variant(const TypeInfo *info, Name name, size_t *index) {
    for (size_t i = 0; i < info->variant_count; i++) {
        if (sema_name_eq(info->variants[i].name, name)) {
            *index = i;
            return true;
        }
    }
    return false;
}

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

static Ref lookup_name(Body *b, Name name, Type *expected) {
    Ref ref = {0};
    if ((ref.local = find_local(b, name))) {
        ref.kind = REF_LOCAL;
        capture_local(b, ref.local);
        return ref;
    }
    if (b->self_info) {
        if ((ref.field = find_field(b->self_info, name))) {
            ref.kind = REF_FIELD;
            capture_self(b);
            return ref;
        }
        if ((ref.method = find_method(b->self_info, name))) {
            ref.kind = REF_METHOD;
            capture_self(b);
            return ref;
        }
    }
    if (expected && expected->tag == T_ENUM && find_variant(expected->as.info, name, &ref.variant)) {
        ref.kind = REF_VARIANT;
        ref.info = expected->as.info;
        return ref;
    }
    Symbol *sym = sema_scope_find(sema_scope(b->s, b->unit), name);
    if (!sym && b->s->prelude && b->unit != b->s->prelude) { /* public names of core.prelude */
        sym = sema_scope_find(sema_scope(b->s, b->s->prelude), name);
        if (sym && (!sym->is_pub || sym->kind == SYM_MODULE)) sym = NULL;
    }
    if (sym) {
        switch (sym->kind) {
        case SYM_FN: ref.kind = REF_FN; ref.fn = sym->fn; break;
        case SYM_TYPE: ref.kind = REF_TYPE; ref.type = sym->type_info->type; break;
        case SYM_MODULE: ref.kind = REF_MODULE; ref.module = sym->module; break;
        case SYM_CONST: ref.kind = REF_CONST; ref.constant = sym->constant; break;
        }
        return ref;
    }
    if ((ref.type = type_builtin(&b->s->types, name.text, name.length))) {
        ref.kind = REF_TYPE;
        return ref;
    }
    for (size_t i = 0; i < sizeof builtin_names / sizeof *builtin_names; i++) {
        if (name_is(name, builtin_names[i])) {
            ref.kind = REF_BUILTIN;
            ref.builtin = builtin_names[i];
            return ref;
        }
    }
    return ref;
}

static void consider(Name candidate, Name name, size_t *best, Name *out, bool *found) {
    size_t distance = sema_edit_distance(name, candidate);
    if (distance < *best) {
        *best = distance;
        *out = candidate;
        *found = true;
    }
}

static void report_unknown(Body *b, Name name) {
    Diagnostic *d = error_at(b, "E0330", name.offset, name.length, "unknown name `%.*s`", NAME_ARGS(name));
    size_t best = name.length < 4 ? 2 : 3;
    Name suggestion = name;
    bool found = false;
    for (size_t i = 0; i < b->locals.count; i++) consider(b->locals.items[i].name, name, &best, &suggestion, &found);
    for (TypeInfo *t = b->self_info; t; t = t->parent) {
        for (size_t i = 0; i < t->field_count; i++) consider(t->fields[i].name, name, &best, &suggestion, &found);
        for (size_t i = 0; i < t->method_count; i++) consider(t->methods[i].decl->name, name, &best, &suggestion, &found);
    }
    Scope *scope = sema_scope(b->s, b->unit);
    for (size_t i = 0; i < scope->count; i++) consider(scope->items[i].name, name, &best, &suggestion, &found);
    if (found) diag_help(d, "did you mean `%.*s`?", NAME_ARGS(suggestion));
}

static Ref member_of_module(Body *b, Unit *module, Name name) {
    Ref ref = {0};
    Symbol *sym = sema_scope_find(sema_scope(b->s, module), name);
    if (!sym || sym->kind == SYM_MODULE) {
        error_at(b, "E0330", name.offset, name.length, "the module `%s` has no `%.*s`", module->module, NAME_ARGS(name));
        ref.kind = REF_ERROR;
        return ref;
    }
    if (!sym->is_pub && module != b->unit) {
        Diagnostic *d = error_at(b, "E0305", name.offset, name.length, "`%.*s` is private to the module `%s`",
                                 NAME_ARGS(name), module->module);
        diag_help(d, "declare it `pub` to use it from other modules");
    }
    if (sym->kind == SYM_FN) {
        ref.kind = REF_FN;
        ref.fn = sym->fn;
    } else if (sym->kind == SYM_CONST) {
        ref.kind = REF_CONST;
        ref.constant = sym->constant;
    } else {
        ref.kind = REF_TYPE;
        ref.type = sym->type_info->type;
    }
    return ref;
}

/* What a NAME or FIELD expression designates when it is a path: a module, a type,
   a function, a variant... REF_VALUE when it is a field of a value. */
static Ref resolve_ref(Body *b, Expr *e, Type *expected) {
    Ref ref = {0};
    if (e->kind == EXPR_NAME) return lookup_name(b, e->as.name, expected);
    ref.kind = REF_VALUE;
    if (e->kind != EXPR_FIELD) return ref;
    Expr *object = e->as.field.object;
    if (object->kind != EXPR_NAME && object->kind != EXPR_FIELD) return ref;

    Ref outer = resolve_ref(b, object, NULL);
    Name name = e->as.field.name;
    switch (outer.kind) {
    case REF_MODULE:
        return member_of_module(b, outer.module, name);
    case REF_TYPE:
        outer.type = named_instance(b, object, outer.type, expected);
        if (is_error(outer.type)) {
            ref.kind = REF_ERROR;
            return ref;
        }
        if (outer.type->tag == T_ENUM && find_variant(outer.type->as.info, name, &ref.variant)) {
            ref.kind = REF_VARIANT;
            ref.info = outer.type->as.info;
            return ref;
        }
        if (outer.type->tag == T_ARENA && name_is(name, "new")) {
            ref.kind = REF_BUILTIN;
            ref.builtin = "Arena.new";
            return ref;
        }
        if (outer.type->tag == T_STR && name_is(name, "from_bytes")) {
            ref.kind = REF_BUILTIN;
            ref.builtin = "str.from_bytes";
            return ref;
        }
        if (outer.type->tag == T_ENUM) {
            error_at(b, "E0337", name.offset, name.length, "`%.*s` has no variant `%.*s`", NAME_ARGS(outer.type->name),
                     NAME_ARGS(name));
        } else {
            error_expr(b, "E0333", e, "`%.*s` is a type: `%.*s.%.*s` is not a value", NAME_ARGS(outer.type->name),
                       NAME_ARGS(outer.type->name), NAME_ARGS(name));
        }
        ref.kind = REF_ERROR;
        return ref;
    case REF_UNKNOWN:
        report_unknown(b, object->as.name);
        ref.kind = REF_ERROR;
        return ref;
    case REF_ERROR:
        return outer;
    default:
        return ref;
    }
}

static Type *variant_value(Body *b, Expr *e, const TypeInfo *info, size_t index) {
    const VariantInfo *variant = &info->variants[index];
    if (variant->field_count > 0) {
        Diagnostic *d = error_expr(b, "E0336", e, "the variant `%.*s` has %zu field%s", NAME_ARGS(variant->name),
                                   variant->field_count, variant->field_count == 1 ? "" : "s");
        diag_help(d, "give its values: `%.*s(...)`", NAME_ARGS(variant->name));
    }
    return info->type;
}

/* Records in `e` what it refers to, for code generation. */
static void record(Expr *e, Ref ref) {
    switch (ref.kind) {
    case REF_LOCAL:
        e->resolution = RESOLVED_LOCAL;
        e->target_index = ref.local->name.offset; /* where it is declared, for the language server */
        break;
    case REF_FIELD: e->resolution = RESOLVED_SELF_FIELD; e->target = ref.field; break;
    case REF_METHOD: e->resolution = RESOLVED_SELF_METHOD; e->target = ref.method; break;
    case REF_FN: e->resolution = RESOLVED_FN; e->target = ref.fn; break;
    case REF_BUILTIN: e->resolution = RESOLVED_BUILTIN; e->target = ref.builtin; break;
    case REF_CONST: e->resolution = RESOLVED_CONST; e->target = ref.constant; break;
    case REF_VARIANT:
        e->resolution = RESOLVED_VARIANT;
        e->target = ref.info;
        e->target_index = ref.variant;
        break;
    default: e->resolution = RESOLVED_NONE; break;
    }
}

/* The type of a path used as a value. */
static Type *value_of_ref(Body *b, Expr *e, Ref ref, Type *expected) {
    record(e, ref);
    switch (ref.kind) {
    case REF_LOCAL:
        if (ref.local->moved && !b->assigning) {
            Diagnostic *d = error_expr(b, "E0356", e, "`%.*s` was moved", NAME_ARGS(ref.local->name));
            diag_help(d, "it moved on line %zu: a value that owns a resource (an `Arena`...) has one owner at a "
                         "time; pass a pointer to lend it instead", sema_line_of(b->unit, ref.local->moved_offset));
        }
        return ref.local->type;
    case REF_FIELD: return ref.field->type;
    case REF_CONST: return ref.constant->type ? ref.constant->type : error_type(b);
    case REF_FN:
        if (ref.fn->param_count || ref.fn->effect_param_count) {
            Diagnostic *d = error_expr(b, "E0360", e, "`%.*s` is generic: it is called, not used as a value",
                                       NAME_ARGS(ref.fn->decl->name));
            diag_help(d, "wrap the call in a function of its own, with the types it needs (spec §5.4)");
            return error_type(b);
        }
        if (ref.fn->is_extern && ref.fn->type->tag == T_FN) { /* a C function takes C function pointers, which a value of `fn` type does not give */
            for (size_t i = 0; i < ref.fn->type->as.fn.param_count; i++) {
                if (ref.fn->type->as.fn.params[i]->tag != T_FN) continue;
                Diagnostic *d = error_expr(b, "E0370", e, "`%.*s` is a C function that takes a function: it is called, "
                                           "not used as a value", NAME_ARGS(ref.fn->decl->name));
                diag_help(d, "wrap the call in a function of Sosu (spec §5.5)");
                return error_type(b);
            }
        }
        return ref.fn->type;
    case REF_VARIANT: return variant_value(b, e, ref.info, ref.variant);
    case REF_ERROR: return error_type(b);
    case REF_METHOD: {
        Diagnostic *d = error_expr(b, "E0333", e, "`%.*s` is a method", NAME_ARGS(ref.method->decl->name));
        diag_help(d, "call it: `%.*s(...)`", NAME_ARGS(ref.method->decl->name));
        return error_type(b);
    }
    case REF_TYPE: {
        Diagnostic *d = error_expr(b, "E0333", e, "`%.*s` is a type, not a value", NAME_ARGS(ref.type->name));
        if (ref.type->tag == T_STRUCT) diag_help(d, "build a value with `%.*s{ ... }`", NAME_ARGS(ref.type->name));
        return error_type(b);
    }
    case REF_MODULE:
        error_expr(b, "E0333", e, "`%s` is a module, not a value", ref.module->module);
        return error_type(b);
    case REF_BUILTIN:
        if (strcmp(ref.builtin, "KB") == 0 || strcmp(ref.builtin, "MB") == 0 || strcmp(ref.builtin, "GB") == 0) {
            return builtin(b, "usize");
        }
        if (strcmp(ref.builtin, "none") == 0) {
            if (expected && (expected->tag == T_OPTION || is_error(expected))) return expected;
            Diagnostic *d = error_expr(b, "E0342", e, "the type of `none` is unknown here");
            diag_help(d, "declare the expected type, e.g. `i32? x = none;`");
            return error_type(b);
        }
        error_expr(b, "E0333", e, "`%s` is a built-in function: call it", ref.builtin);
        return error_type(b);
    case REF_UNKNOWN:
        report_unknown(b, e->as.name);
        return error_type(b);
    case REF_VALUE:
        break;
    }
    return error_type(b);
}

/* ---- Literals ---- */

static bool is_untyped_literal(const Expr *e) {
    if (e->kind == EXPR_INT || e->kind == EXPR_FLOAT) return true;
    return e->kind == EXPR_UNARY && e->as.unary.op == TOK_MINUS && is_untyped_literal(e->as.unary.operand);
}

static bool int_fits(const Type *type, uint64_t value, bool negative) {
    unsigned bits = type->as.integer.bits;
    if (!type->as.integer.is_signed) return !negative ? (bits == 64 || value <= (UINT64_C(1) << bits) - 1) : value == 0;
    uint64_t limit = UINT64_C(1) << (bits - 1);
    return negative ? value <= limit : value < limit;
}

static Type *check_int_literal(Body *b, Expr *e, Type *expected, bool negative) {
    if (expected && is_error(expected)) return expected;
    if (expected && expected->tag == T_FLOAT) {
        Diagnostic *d = error_expr(b, "E0331", e, "an integer literal cannot be a `%.*s`", NAME_ARGS(expected->name));
        diag_help(d, "write a float literal: `%llu.0`", (unsigned long long)e->as.int_value);
        return expected;
    }
    Type *type = expected && expected->tag == T_INT ? expected : builtin(b, "i32");
    if (!int_fits(type, e->as.int_value, negative)) {
        Diagnostic *d = error_expr(b, "E0332", e, "`%s%llu` does not fit in `%.*s`", negative ? "-" : "",
                                   (unsigned long long)e->as.int_value, NAME_ARGS(type->name));
        if (!expected || expected->tag != T_INT) diag_help(d, "give the value a larger type, e.g. `i64 x = ...;`");
    }
    return type;
}

/* Checks two operands that must have the same type: a literal takes the type of the other side. */
static void check_pair(Body *b, Expr *left, Expr *right, Type *hint, Type **lt, Type **rt) {
    if (is_untyped_literal(left) && !is_untyped_literal(right)) {
        *rt = check_expr(b, right, hint);
        *lt = check_expr(b, left, *rt);
    } else {
        *lt = check_expr(b, left, hint);
        *rt = check_expr(b, right, *lt);
    }
}

/* ---- Calls ---- */

static void report_missing_effects(Body *b, const Expr *callee, unsigned effects, const char *what) {
    if (b->lambda && b->lambda->inferring) { /* a lambda has the effects of its body */
        b->lambda->used |= effects;
        return;
    }
    unsigned missing = effects & ~b->effects;
    if (!missing) return;
    unsigned bit = missing & (~missing + 1);
    Diagnostic *d = error_expr(b, "E0340", callee, "%s needs the effect `%s`", what, effect_name(bit));
    if (b->lambda) diag_help(d, "the type of this lambda does not allow it: add `uses %s` to its type", effect_name(bit));
    else diag_help(d, "add `uses %s` to the signature of `%.*s` (spec §7.4)", effect_name(bit), NAME_ARGS(b->fn_name));
}

static void check_args_loosely(Body *b, Expr *e) {
    for (size_t i = 0; i < e->as.call.arg_count; i++) check_expr(b, e->as.call.args[i], NULL);
}

static Type *call_with(Body *b, Expr *e, Type *fn, Name name, bool is_unsafe) {
    Expr *callee = e->as.call.callee;
    if (is_error(fn)) {
        check_args_loosely(b, e);
        return fn;
    }
    size_t count = e->as.call.arg_count;
    if (count != fn->as.fn.param_count) {
        Diagnostic *d = error_expr(b, "E0336", callee, "`%.*s` takes %zu argument%s, but %zu %s given", NAME_ARGS(name),
                                   fn->as.fn.param_count, fn->as.fn.param_count == 1 ? "" : "s", count,
                                   count == 1 ? "was" : "were");
        char *signature = type_to_string(fn);
        diag_help(d, "its signature is `%s`", signature);
        free(signature);
        check_args_loosely(b, e);
        return fn->as.fn.ret;
    }
    for (size_t i = 0; i < count; i++) {
        check_expect(b, e->as.call.args[i], fn->as.fn.params[i]);
        consume(b, e->as.call.args[i]);
    }

    char what[96];
    snprintf(what, sizeof what, "calling `%.*s`", NAME_ARGS(name));
    report_missing_effects(b, callee, fn->as.fn.effects, what);
    if (is_unsafe && b->unsafe_depth == 0) {
        Diagnostic *d = error_expr(b, "E0341", callee, "`%.*s` can only be called in an `unsafe` block", NAME_ARGS(name));
        diag_help(d, "unsafe functions and C functions are called in `unsafe { ... }` (spec §7.7)");
    }
    return fn->as.fn.ret;
}

static Type *call_variant(Body *b, Expr *e, const TypeInfo *info, size_t index) {
    const VariantInfo *variant = &info->variants[index];
    size_t count = e->as.call.arg_count;
    if (!variant->decl->has_parens || count != variant->field_count) {
        error_expr(b, "E0336", e->as.call.callee, "the variant `%.*s` has %zu field%s, but %zu %s given",
                   NAME_ARGS(variant->name), variant->field_count, variant->field_count == 1 ? "" : "s", count,
                   count == 1 ? "was" : "were");
        check_args_loosely(b, e);
        return info->type;
    }
    for (size_t i = 0; i < count; i++) {
        check_expect(b, e->as.call.args[i], variant->fields[i]);
        consume(b, e->as.call.args[i]);
        check_escape(b, e->as.call.args[i], "stored in an enum");
    }
    return info->type;
}

/* A public type of core.prelude, e.g. `Utf8Error`. */
static Type *prelude_type(Body *b, const char *name, const Expr *at) {
    Name wanted = { name, strlen(name), 0 };
    Symbol *sym = b->s->prelude ? sema_scope_find(sema_scope(b->s, b->s->prelude), wanted) : NULL;
    if (sym && sym->kind == SYM_TYPE) return sym->type_info->type;
    Diagnostic *d = error_expr(b, "E0414", at, "`%s` is missing: the standard library was not found", name);
    diag_help(d, "it is declared in `core/src/prelude.sosu`; set SOSU_HOME to the folder of the Sosu installation");
    return error_type(b);
}

static Type *call_builtin(Body *b, Expr *e, const char *name, Type *expected) {
    Expr *callee = e->as.call.callee;
    Expr **args = e->as.call.args;
    size_t count = e->as.call.arg_count;

    if (strcmp(name, "none") == 0 || name[1] == 'B') { /* none, KB, MB, GB */
        Diagnostic *d = error_expr(b, "E0338", callee, "`%s` is a value, not a function", name);
        diag_help(d, "write `%s` without parentheses", name);
        check_args_loosely(b, e);
        return strcmp(name, "none") == 0 && expected ? expected : error_type(b);
    }
    if (strcmp(name, "str.from_bytes") == 0) {
        /* str.from_bytes(ui8[]) -> str!Utf8Error (spec §4.1) */
        if (count != 1) {
            error_expr(b, "E0336", callee, "`str.from_bytes` takes the bytes to check, e.g. `str.from_bytes(data)`");
            check_args_loosely(b, e);
        } else {
            check_expect(b, args[0], type_slice(&b->s->types, builtin(b, "ui8")));
        }
        Type *error = prelude_type(b, "Utf8Error", callee);
        return is_error(error) ? error : type_result(&b->s->types, builtin(b, "str"), error);
    }
    if (strcmp(name, "Arena.new") == 0) {
        if (count != 1) {
            error_expr(b, "E0336", callee, "`Arena.new` takes the size of its blocks, e.g. `Arena.new(1 * MB)`");
            check_args_loosely(b, e);
        } else {
            check_expect(b, args[0], builtin(b, "usize"));
        }
        return builtin(b, "Arena");
    }
    if (strcmp(name, "size_of") == 0) { /* size_of(T): its size in bytes, as C counts it (spec §5.6) */
        Type *type = count == 1 ? type_argument(b, args[0]) : NULL;
        if (!type) {
            Diagnostic *d = error_expr(b, "E0336", callee, "`size_of` takes a type");
            diag_help(d, "e.g. `size_of(Point)`, `size_of(T)`");
            check_args_loosely(b, e);
        } else {
            args[0]->type = type;
        }
        return builtin(b, "usize");
    }
    bool is_ok = strcmp(name, "ok") == 0;
    bool void_ok = is_ok && expected && expected->tag == T_RESULT && expected->as.result.value->tag == T_VOID;
    bool two = strcmp(name, "min") == 0 || strcmp(name, "max") == 0 || strncmp(name, "wrapping_", 9) == 0;
    size_t wanted = two ? 2 : void_ok ? 0 : 1;
    if (count != wanted) {
        error_expr(b, "E0336", callee, "`%s` takes %zu argument%s, but %zu %s given", name, wanted, wanted == 1 ? "" : "s",
                   count, count == 1 ? "was" : "were");
        check_args_loosely(b, e);
        return expected ? expected : error_type(b);
    }

    if (strcmp(name, "some") == 0) {
        if (expected && expected->tag == T_OPTION) {
            check_expect(b, args[0], expected->as.inner);
            consume(b, args[0]);
            check_escape(b, args[0], "stored in an option");
            return expected;
        }
        Type *type = check_expr(b, args[0], NULL);
        if (type->tag == T_VOID) {
            error_expr(b, "E0331", args[0], "this expression has no value");
            return error_type(b);
        }
        check_escape(b, args[0], "stored in an option");
        return type_option(&b->s->types, type);
    }
    if (is_ok || strcmp(name, "err") == 0) {
        if (!expected || expected->tag != T_RESULT) {
            if (!expected || !is_error(expected)) {
                Diagnostic *d = error_expr(b, "E0342", e, "the type of `%s(...)` is unknown here", name);
                diag_help(d, "declare the expected type, e.g. `i32!MyError r = %s(...);`", name);
            }
            check_args_loosely(b, e);
            return error_type(b);
        }
        if (count == 1) {
            check_expect(b, args[0], is_ok ? expected->as.result.value : expected->as.result.error);
            consume(b, args[0]);
            check_escape(b, args[0], "stored in a result");
        }
        return expected;
    }
    if (strcmp(name, "assert") == 0) {
        check_expect(b, args[0], builtin(b, "bool"));
        return void_type(b);
    }
    if (strcmp(name, "hash") == 0) {
        Type *type = check_expr(b, args[0], NULL);
        if (!is_error(type) && !(sema_constraints_of(type) & CONSTRAINT_HASH)) {
            TypeNames n = {0};
            Diagnostic *d = error_expr(b, "E0334", args[0], "`hash` needs a number, a `bool`, a `char`, a `str`, a pointer "
                                                             "or an enum without data, found `%s`", tn(&n, type));
            if (type->tag == T_PARAM) diag_help(d, "add the constraint `Hash`: `<%.*s: Hash>`", NAME_ARGS(type->name));
            release_names(&n);
        }
        return builtin(b, "ui64");
    }
    if (strcmp(name, "abs") == 0) {
        Type *type = check_expr(b, args[0], expected);
        bool ok = type->tag == T_FLOAT || (type->tag == T_INT && type->as.integer.is_signed) || is_error(type);
        if (!ok) {
            TypeNames n = {0};
            error_expr(b, "E0334", args[0], "`abs` needs a signed integer or a float, found `%s`", tn(&n, type));
            release_names(&n);
        }
        return type;
    }
    /* min, max, and wrapping_add, wrapping_sub, wrapping_mul (modulo 2^n, spec §5.1) */
    bool wrapping = strncmp(name, "wrapping_", 9) == 0;
    Type *lt, *rt;
    check_pair(b, args[0], args[1], expected, &lt, &rt);
    bool valid = wrapping ? lt->tag == T_INT : is_numeric(lt);
    if (!is_error(lt) && !is_error(rt) && (!valid || lt != rt)) {
        TypeNames n = {0};
        error_expr(b, "E0334", e, "`%s` needs two %s of the same type, found `%s` and `%s`", name,
                   wrapping ? "integers" : "numbers", tn(&n, lt), tn(&n, rt));
        release_names(&n);
    }
    return lt;
}

static bool is_printable(const Type *type) {
    return is_numeric(type) || type->tag == T_BOOL || type->tag == T_CHAR || type->tag == T_STR || is_error(type);
}

/* `io.print("format {}", values...)`, built into the compiler (spec §5.3). */
static Type *call_print(Body *b, Expr *e) {
    Expr **args = e->as.call.args;
    size_t count = e->as.call.arg_count;
    report_missing_effects(b, e->as.call.callee, EFFECT_IO, "`io.print`");
    if (count == 0 || args[0]->kind != EXPR_STRING) {
        Diagnostic *d = error_expr(b, "E0351", count ? args[0] : e, "the format of `io.print` must be a string literal");
        diag_help(d, "write `io.print(\"x = {}\", x)`");
        check_args_loosely(b, e);
        return void_type(b);
    }
    args[0]->type = builtin(b, "str");
    const char *text = b->unit->file.text + args[0]->offset;
    size_t placeholders = 0;
    for (size_t i = 0; i + 1 < args[0]->length; i++) {
        if (text[i] == '{' && text[i + 1] == '}') placeholders++;
    }
    if (placeholders != count - 1) {
        error_expr(b, "E0351", args[0], "the format has %zu `{}` but %zu value%s follow%s", placeholders, count - 1,
                   count - 1 == 1 ? "" : "s", count - 1 == 1 ? "s" : "");
    }
    for (size_t i = 1; i < count; i++) {
        Type *type = check_expr(b, args[i], NULL);
        if (!is_printable(type)) {
            TypeNames n = {0};
            Diagnostic *d = error_expr(b, "E0351", args[i], "a `%s` cannot be printed", tn(&n, type));
            diag_help(d, "print its parts: numbers, `bool`, `char` and `str` can be printed");
            release_names(&n);
        }
    }
    return void_type(b);
}

static bool is_print(const Expr *callee, Ref object) {
    return callee->kind == EXPR_FIELD && object.kind == REF_MODULE && strcmp(object.module->module, "std.io") == 0 &&
           name_is(callee->as.field.name, "print");
}

/* ---- Generics (spec §5.4) ---- */

#define EFFECT_UNBOUND 0xFFFFFFFFu

static bool is_nominal(const Type *t) {
    return t->tag == T_STRUCT || t->tag == T_CLASS || t->tag == T_INTERFACE || t->tag == T_ENUM;
}

/* The generic a nominal type comes from (itself when it is not an instance), and its arguments
   (its own parameters for the generic itself). */
static TypeInfo *generic_of(const Type *t) { return t->as.info->generic ? t->as.info->generic : t->as.info; }

static Type **args_of(const Type *t) { return t->as.info->generic ? t->type_args : t->as.info->params; }

/* Binds the parameters of `fn` held by `pattern` so that it becomes `actual`. False if they
   cannot match. A parameter of the function being checked (not of `fn`) is a type as any other. */
static bool unify(FnInfo *fn, Type *pattern, Type *actual, Type **bound, unsigned *effects) {
    if (is_error(pattern) || is_error(actual)) return true;
    if (pattern->tag == T_PARAM) {
        for (size_t i = 0; i < fn->param_count; i++) {
            if (fn->params[i] != pattern) continue;
            if (!bound[i]) bound[i] = actual;
            return bound[i] == actual;
        }
        return pattern == actual;
    }
    if (pattern->tag != actual->tag) return false;
    switch (pattern->tag) {
    case T_POINTER: case T_OPTION: case T_SLICE:
        return unify(fn, pattern->as.inner, actual->as.inner, bound, effects);
    case T_ARRAY:
        return pattern->as.array.size == actual->as.array.size &&
               unify(fn, pattern->as.array.element, actual->as.array.element, bound, effects);
    case T_RESULT:
        return unify(fn, pattern->as.result.value, actual->as.result.value, bound, effects) &&
               unify(fn, pattern->as.result.error, actual->as.result.error, bound, effects);
    case T_FN: {
        if (pattern->as.fn.param_count != actual->as.fn.param_count) return false;
        for (size_t i = 0; i < pattern->as.fn.param_count; i++) {
            if (!unify(fn, pattern->as.fn.params[i], actual->as.fn.params[i], bound, effects)) return false;
        }
        if (!unify(fn, pattern->as.fn.ret, actual->as.fn.ret, bound, effects)) return false;
        unsigned own = pattern->as.fn.effects & ~EFFECT_PARAMS;
        unsigned params = pattern->as.fn.effects & EFFECT_PARAMS;
        if (!params || !fn->effect_param_count) return pattern->as.fn.effects == actual->as.fn.effects;
        if (own & ~actual->as.fn.effects) return false;
        unsigned rest = actual->as.fn.effects & ~own; /* what the first effect parameter stands for */
        for (size_t k = 0; k < fn->effect_param_count; k++) {
            if (!(params & (EFFECT_PARAM << k))) continue;
            if (effects[k] == EFFECT_UNBOUND) effects[k] = rest;
            return effects[k] == rest;
        }
        return true;
    }
    case T_STRUCT: case T_CLASS: case T_INTERFACE: case T_ENUM: {
        TypeInfo *generic = generic_of(pattern);
        if (generic != generic_of(actual)) return false;
        if (generic->param_count == 0) return pattern == actual;
        Type **pa = args_of(pattern);
        Type **aa = args_of(actual);
        for (size_t i = 0; i < generic->param_count; i++) {
            if (!unify(fn, pa[i], aa[i], bound, effects)) return false;
        }
        return true;
    }
    default:
        return pattern == actual;
    }
}

/* True if every parameter of `fn` in `type` is bound. */
static bool all_bound(FnInfo *fn, const Type *type, Type **bound, const unsigned *effects) {
    switch (type->tag) {
    case T_PARAM:
        for (size_t i = 0; i < fn->param_count; i++) {
            if (fn->params[i] == type) return bound[i] != NULL;
        }
        return true;
    case T_POINTER: case T_OPTION: case T_SLICE: return all_bound(fn, type->as.inner, bound, effects);
    case T_ARRAY: return all_bound(fn, type->as.array.element, bound, effects);
    case T_RESULT:
        return all_bound(fn, type->as.result.value, bound, effects) && all_bound(fn, type->as.result.error, bound, effects);
    case T_FN:
        for (size_t k = 0; k < fn->effect_param_count; k++) {
            if ((type->as.fn.effects & (EFFECT_PARAM << k)) && effects[k] == EFFECT_UNBOUND) return false;
        }
        for (size_t i = 0; i < type->as.fn.param_count; i++) {
            if (!all_bound(fn, type->as.fn.params[i], bound, effects)) return false;
        }
        return all_bound(fn, type->as.fn.ret, bound, effects);
    case T_STRUCT: case T_CLASS: case T_INTERFACE: case T_ENUM: {
        TypeInfo *generic = generic_of(type);
        Type **args = args_of(type);
        for (size_t i = 0; i < generic->param_count; i++) {
            if (!all_bound(fn, args[i], bound, effects)) return false;
        }
        return true;
    }
    default:
        return true;
    }
}

static Type *check_lambda(Body *b, Expr *e, Type *expected, FnInfo *generic, Type **bound, unsigned *effects);

/* An argument whose type comes from the type expected for it: checked once the parameters
   it depends on are known. */
static bool is_contextual(const Expr *e) {
    if (is_untyped_literal(e) || e->kind == EXPR_ARRAY) return true;
    if (e->kind == EXPR_NAME) return name_is(e->as.name, "none");
    if (e->kind == EXPR_CALL && e->as.call.callee->kind == EXPR_NAME) {
        Name name = e->as.call.callee->as.name;
        return name_is(name, "ok") || name_is(name, "err");
    }
    return e->kind == EXPR_STRUCT_LIT && e->as.struct_lit.type->arg_count == 0;
}

static Type *bind(Body *b, FnInfo *fn, Type *type, Type **bound, const unsigned *effects) {
    return sema_subst(b->s, type, fn->params, bound, fn->param_count, effects, fn->effect_param_count);
}

/* A call of a generic function: its type arguments are written, or inferred from its
   arguments, then from the type expected for its result (spec §5.4). */
static Type *call_generic(Body *b, Expr *e, FnInfo *fn, Name name, Type *expected) {
    Expr *callee = e->as.call.callee;
    Type *type = fn->type;
    if (is_error(type)) {
        check_args_loosely(b, e);
        return type;
    }
    size_t n = fn->param_count;
    size_t m = fn->effect_param_count;
    Type **bound = arena_alloc(&b->s->arena, (n + 1) * sizeof *bound);
    unsigned *effects = arena_alloc(&b->s->arena, (m + 1) * sizeof *effects);
    for (size_t k = 0; k < m; k++) effects[k] = EFFECT_UNBOUND;
    if (callee->type_arg_count) {
        if (callee->type_arg_count != n) {
            Diagnostic *d = error_expr(b, "E0361", callee, "`%.*s` takes %zu type argument%s, but %zu %s given",
                                       NAME_ARGS(name), n, n == 1 ? "" : "s", callee->type_arg_count,
                                       callee->type_arg_count == 1 ? "was" : "were");
            diag_help(d, "give a type for each parameter, or leave them all out to infer them");
            check_args_loosely(b, e);
            return error_type(b);
        }
        for (size_t k = 0; k < n; k++) bound[k] = sema_resolve_type(b->s, b->unit, callee->type_args[k], POSITION_VALUE);
    }
    size_t count = e->as.call.arg_count;
    if (count != type->as.fn.param_count) {
        Diagnostic *d = error_expr(b, "E0336", callee, "`%.*s` takes %zu argument%s, but %zu %s given", NAME_ARGS(name),
                                   type->as.fn.param_count, type->as.fn.param_count == 1 ? "" : "s", count,
                                   count == 1 ? "was" : "were");
        char *signature = type_to_string(type);
        diag_help(d, "its signature is `%s`", signature);
        free(signature);
        check_args_loosely(b, e);
        return error_type(b);
    }
    bool *checked = calloc(count + 1, sizeof *checked);
    for (int pass = 0; pass < 2; pass++) {
        for (size_t i = 0; i < count; i++) {
            if (checked[i]) continue;
            Expr *arg = e->as.call.args[i];
            Type *param = type->as.fn.params[i];
            if (all_bound(fn, param, bound, effects)) {
                check_expect(b, arg, bind(b, fn, param, bound, effects));
                checked[i] = true;
                continue;
            }
            if (pass == 0 && (is_contextual(arg) || arg->kind == EXPR_LAMBDA)) continue;
            Type *found;
            if (arg->kind == EXPR_LAMBDA) { /* it takes the types known, and gives the others */
                found = arg->type = check_lambda(b, arg, param, fn, bound, effects);
            } else {
                found = check_expr(b, arg, NULL);
            }
            checked[i] = true;
            if (!unify(fn, param, found, bound, effects)) report_mismatch(b, arg, bind(b, fn, param, bound, effects), found);
        }
        if (pass == 0 && expected && !is_error(expected)) unify(fn, type->as.fn.ret, expected, bound, effects);
    }
    free(checked);
    bool ok = true;
    for (size_t k = 0; k < n; k++) {
        if (bound[k]) continue;
        Diagnostic *d = error_expr(b, "E0360", callee, "the type `%.*s` of `%.*s` cannot be inferred here",
                                   NAME_ARGS(fn->params[k]->name), NAME_ARGS(name));
        diag_help(d, "write the type arguments: `%.*s<...>(...)` (spec §5.4)", NAME_ARGS(name));
        bound[k] = error_type(b);
        ok = false;
    }
    for (size_t k = 0; k < m; k++) {
        if (effects[k] == EFFECT_UNBOUND) effects[k] = 0;
    }
    for (size_t k = 0; ok && k < n; k++) {
        ok &= sema_check_type_arg(b->s, b->unit, callee->offset, callee->length, bound[k], fn->params[k], name);
    }
    for (size_t i = 0; i < count; i++) consume(b, e->as.call.args[i]);
    Type *instance = bind(b, fn, type, bound, effects);
    callee->type = instance;
    callee->type_values = bound;
    callee->type_value_count = n;
    callee->effect_values = effects;
    callee->effect_value_count = m;
    if (!ok) return error_type(b);
    char what[96];
    snprintf(what, sizeof what, "calling `%.*s`", NAME_ARGS(name));
    report_missing_effects(b, callee, instance->as.fn.effects, what);
    if (fn->decl->is_unsafe && b->unsafe_depth == 0) {
        Diagnostic *d = error_expr(b, "E0341", callee, "`%.*s` can only be called in an `unsafe` block", NAME_ARGS(name));
        diag_help(d, "unsafe functions are called in `unsafe { ... }` (spec §7.7)");
    }
    return instance->as.fn.ret;
}

/* A type named in an expression (`Tree<i32>.Leaf(1)`, `arena.new(Vec<i32>, ...)`): a
   generic needs its arguments, written after it or given by the type expected. */
static Type *named_instance(Body *b, const Expr *named, Type *type, Type *expected) {
    TypeInfo *info = is_nominal(type) ? type->as.info : NULL;
    bool generic = info && info->param_count > 0 && !info->generic;
    if (!generic) {
        if (named->type_arg_count && !is_error(type)) {
            TypeNames n = {0};
            error_expr(b, "E0361", named, "`%s` is not generic: it takes no type arguments", tn(&n, type));
            release_names(&n);
            return error_type(b);
        }
        return type;
    }
    if (named->type_arg_count == 0) {
        if (expected && is_nominal(expected) && generic_of(expected) == info) return expected;
        TypeNames n = {0};
        Diagnostic *d = error_expr(b, "E0361", named, "`%.*s` is generic: give its type arguments", NAME_ARGS(info->name));
        diag_help(d, "write `%s`, with a type for each parameter", tn(&n, info->type));
        release_names(&n);
        return error_type(b);
    }
    if (named->type_arg_count != info->param_count) {
        error_expr(b, "E0361", named, "`%.*s` takes %zu type argument%s, but %zu %s given", NAME_ARGS(info->name),
                   info->param_count, info->param_count == 1 ? "" : "s", named->type_arg_count,
                   named->type_arg_count == 1 ? "was" : "were");
        return error_type(b);
    }
    Type **args = arena_alloc(&b->s->arena, info->param_count * sizeof *args);
    bool ok = true;
    for (size_t i = 0; i < info->param_count; i++) {
        const TypeExpr *te = named->type_args[i];
        args[i] = sema_resolve_type(b->s, b->unit, te, POSITION_VALUE);
        ok &= sema_check_type_arg(b->s, b->unit, te->offset, te->length, args[i], info->params[i], info->name);
    }
    return ok ? sema_instance(b->s, info, args)->type : error_type(b);
}

/* ---- Arenas (spec §6) ---- */

/* A type given as an argument: `arena.alloc(Point)`, `arena.new(shapes.Circle, ...)`.
   NULL if the expression is not a type name. */
static Type *type_argument(Body *b, Expr *arg) {
    Name names[2];
    size_t count = 0;
    const Expr *e = arg;
    if (e->kind == EXPR_FIELD) {
        names[1] = e->as.field.name;
        e = e->as.field.object;
        count = 1;
    }
    if (e->kind != EXPR_NAME) return NULL;
    names[0] = e->as.name;
    if (count == 1) count = 2;
    else count = 1;
    Path path = { names, count };
    return named_instance(b, arg, sema_resolve_named(b->s, b->unit, path, arg->offset, arg->length), NULL);
}

/* True if all-zero memory is a valid value of `type` (no pointer, which is never null). */
static bool zeroable(const Type *type) {
    switch (type->tag) {
    case T_POINTER: case T_FN: case T_ARENA: case T_INTERFACE: case T_PARAM: /* a parameter may be a pointer */
        return false;
    case T_ARRAY:
        return zeroable(type->as.array.element);
    case T_RESULT:
        return zeroable(type->as.result.error); /* zero is `err` with the first variant */
    case T_ENUM: {
        const VariantInfo *first = type->as.info->variant_count ? &type->as.info->variants[0] : NULL;
        for (size_t i = 0; first && i < first->field_count; i++) {
            if (!zeroable(first->fields[i])) return false;
        }
        return true;
    }
    case T_STRUCT:
    case T_CLASS:
        for (const TypeInfo *t = type->as.info; t; t = t->parent) {
            for (size_t i = 0; i < t->field_count; i++) {
                if (!zeroable(t->fields[i].type)) return false;
            }
        }
        return true;
    default:
        return true; /* numbers, bool, char, str, options */
    }
}

static Type *call_arena(Body *b, Expr *e, Name name) {
    Expr *callee = e->as.call.callee;
    Expr **args = e->as.call.args;
    size_t count = e->as.call.arg_count;
    callee->resolution = RESOLVED_BUILTIN;
    TypeNames n = {0};

    if (name_is(name, "free")) {
        callee->target = "arena.free";
        if (count) error_expr(b, "E0336", callee, "`free` takes no argument");
        check_args_loosely(b, e);
        return void_type(b);
    }
    bool is_alloc = name_is(name, "alloc");
    bool is_try = name_is(name, "try_alloc");
    bool is_new = name_is(name, "new");
    bool is_slice = name_is(name, "alloc_slice");
    if (!is_alloc && !is_try && !is_new && !is_slice) {
        Diagnostic *d = error_at(b, "E0337", name.offset, name.length, "`Arena` has no method `%.*s`", NAME_ARGS(name));
        diag_help(d, "the methods of an arena are `alloc`, `alloc_slice`, `try_alloc`, `new` and `free` (spec §6)");
        check_args_loosely(b, e);
        return error_type(b);
    }
    callee->target = is_alloc ? "arena.alloc" : is_try ? "arena.try_alloc" : is_new ? "arena.new" : "arena.alloc_slice";
    Type *type = count ? type_argument(b, args[0]) : NULL;
    if (!type) {
        Diagnostic *d = error_expr(b, "E0354", count ? args[0] : e, "`%.*s` takes the type to allocate first",
                                   NAME_ARGS(name));
        diag_help(d, "e.g. `arena.%.*s(Point)`", NAME_ARGS(name));
        for (size_t i = count ? 1 : 0; i < count; i++) check_expr(b, args[i], NULL);
        return error_type(b);
    }
    args[0]->type = type;
    if (is_error(type)) {
        for (size_t i = 1; i < count; i++) check_expr(b, args[i], NULL);
        return error_type(b);
    }
    Type *pointer = type_pointer(&b->s->types, type);

    if (is_new) {
        if (type->tag != T_CLASS && type->tag != T_STRUCT) {
            error_expr(b, "E0354", args[0], "`arena.new` creates a class or a struct, not a `%s`", tn(&n, type));
            release_names(&n);
            for (size_t i = 1; i < count; i++) check_expr(b, args[i], NULL);
            return error_type(b);
        }
        Name init_name = { "init", 4, 0 };
        MethodInfo *init = find_method(type->as.info, init_name);
        if (init) {
            Type *fn = init->type;
            if (count - 1 != fn->as.fn.param_count) {
                error_expr(b, "E0336", callee, "`%.*s.init` takes %zu argument%s, but %zu %s given",
                           NAME_ARGS(type->name), fn->as.fn.param_count, fn->as.fn.param_count == 1 ? "" : "s",
                           count - 1, count - 1 == 1 ? "was" : "were");
                for (size_t i = 1; i < count; i++) check_expr(b, args[i], NULL);
            } else {
                for (size_t i = 1; i < count; i++) {
                    check_expect(b, args[i], fn->as.fn.params[i - 1]);
                    consume(b, args[i]);
                }
            }
            report_missing_effects(b, callee, fn->as.fn.effects, "`init`");
        } else if (count > 1) {
            error_expr(b, "E0336", callee, "`%.*s` has no `init` method: `arena.new` takes no other argument",
                       NAME_ARGS(type->name));
            for (size_t i = 1; i < count; i++) check_expr(b, args[i], NULL);
        } else if (!zeroable(type)) {
            Diagnostic *d = error_expr(b, "E0354", args[0], "`%.*s` has no `init` method, and zeroed memory is not a valid `%.*s`",
                                       NAME_ARGS(type->name), NAME_ARGS(type->name));
            diag_help(d, "it contains a pointer, which is never null: add an `init` method that gives it a value");
        }
        return pointer;
    }

    if (is_slice) {
        if (count != 2) {
            error_expr(b, "E0336", callee, "`alloc_slice` takes the type and the number of elements, e.g. "
                                           "`arena.alloc_slice(ui8, 64)`");
            for (size_t i = 1; i < count; i++) check_expr(b, args[i], NULL);
        } else {
            check_expect(b, args[1], builtin(b, "usize"));
        }
    } else if (count != 1) {
        error_expr(b, "E0336", callee, "`%.*s` takes only the type to allocate", NAME_ARGS(name));
        for (size_t i = 1; i < count; i++) check_expr(b, args[i], NULL);
    }
    if (type->tag == T_CLASS || type->tag == T_INTERFACE) {
        Diagnostic *d = error_expr(b, "E0354", args[0], "a class is created with `arena.new`, which calls its `init`");
        diag_help(d, "write `arena.new(%.*s, ...)`", NAME_ARGS(type->name));
    } else if (!zeroable(type) && b->unsafe_depth == 0) {
        Diagnostic *d = error_expr(b, "E0354", args[0], "`%.*s` gives zeroed memory, which is not a valid `%s`",
                                   NAME_ARGS(name), tn(&n, type));
        diag_help(d, "it contains a pointer, which is never null: use a struct with `init` and `arena.new`, or `T*?`; in "
                     "`unsafe`, the memory may be given before it holds valid values (spec §5.4)");
    }
    release_names(&n);
    if (is_slice) return type_slice(&b->s->types, type);
    return is_try ? type_option(&b->s->types, pointer) : pointer;
}

static Type *call_method(Body *b, Expr *e, Type *expected) {
    Expr *callee = e->as.call.callee;
    Name name = callee->as.field.name;
    (void)expected;
    Type *object = check_expr(b, callee->as.field.object, NULL);
    Type *base = object->tag == T_POINTER ? object->as.inner : object;
    if (is_error(base)) {
        check_args_loosely(b, e);
        return error_type(b);
    }
    bool through_bound = base->tag == T_PARAM && base->as.param.bound; /* `<T: Drawable>`: `t.draw(r)` */
    if (base->tag == T_STRUCT || base->tag == T_CLASS || base->tag == T_INTERFACE || through_bound) {
        TypeInfo *info = through_bound ? base->as.param.bound : base->as.info;
        MethodInfo *method = find_method(info, name);
        if (method) {
            if (method->owner->unit != b->unit && !method->decl->is_pub && method->owner->type->tag != T_INTERFACE) {
                Diagnostic *d = error_at(b, "E0305", name.offset, name.length, "the method `%.*s` of `%.*s` is private",
                                         NAME_ARGS(name), NAME_ARGS(method->owner->name));
                diag_help(d, "declare it `pub` in `%.*s`", NAME_ARGS(method->owner->name));
            }
            callee->type = method->type;
            callee->resolution = RESOLVED_METHOD;
            callee->target = method;
            if (method->generic) return call_generic(b, e, method->generic, name, expected);
            return call_with(b, e, method->type, name, method->decl->is_unsafe);
        }
        FieldInfo *field = find_field(info, name);
        if (field && field->type->tag == T_FN) {
            callee->type = field->type;
            return call_with(b, e, field->type, name, false);
        }
    }
    if (name_is(name, "copy") && type_owns_resource(base)) {
        /* x.copy(): its arenas are new and empty (same block size); the rest is copied (spec §6) */
        callee->resolution = RESOLVED_BUILTIN;
        callee->target = "copy";
        if (e->as.call.arg_count) error_expr(b, "E0336", callee, "`copy` takes no argument");
        check_args_loosely(b, e);
        return base;
    }
    if (base->tag == T_ARENA) return call_arena(b, e, name);
    if (base->tag == T_STR && name_is(name, "to_cstr")) {
        /* s.to_cstr(arena): a NUL-terminated copy for C (spec §4.1) */
        callee->resolution = RESOLVED_BUILTIN;
        callee->target = "str.to_cstr";
        Type *arena = e->as.call.arg_count == 1 ? check_expr(b, e->as.call.args[0], NULL) : NULL;
        Type *arena_base = arena && arena->tag == T_POINTER ? arena->as.inner : arena;
        if (!arena_base || (arena_base->tag != T_ARENA && !is_error(arena_base))) {
            Diagnostic *d = error_expr(b, "E0336", e, "`to_cstr` takes the arena that holds the copy");
            diag_help(d, "write `text.to_cstr(arena)` (spec §4.1)");
            if (!arena) check_args_loosely(b, e);
        }
        return type_pointer(&b->s->types, builtin(b, "ui8"));
    }
    if (base->tag == T_STR && name_is(name, "chars")) {
        Diagnostic *d = error_expr(b, "E0352", e, "`chars()` can only be used in a `for` loop");
        diag_help(d, "write `for (char c in s.chars()) { ... }` (spec §4.1)");
        check_args_loosely(b, e);
        return error_type(b);
    }
    TypeNames n = {0};
    Diagnostic *d = error_at(b, "E0337", name.offset, name.length, "`%s` has no method `%.*s`", tn(&n, base), NAME_ARGS(name));
    if (name_is(name, "copy")) diag_help(d, "a value without an `Arena` is copied by value: use it directly (spec §6)");
    release_names(&n);
    check_args_loosely(b, e);
    return error_type(b);
}

static Type *check_call(Body *b, Expr *e, Type *expected) {
    Expr *callee = e->as.call.callee;
    if (callee->kind == EXPR_FIELD && (callee->as.field.object->kind == EXPR_NAME ||
                                       callee->as.field.object->kind == EXPR_FIELD)) {
        Ref object = resolve_ref(b, callee->as.field.object, NULL);
        if (is_print(callee, object)) {
            callee->resolution = RESOLVED_PRINT;
            return call_print(b, e);
        }
    }
    if (callee->kind != EXPR_NAME && callee->kind != EXPR_FIELD) {
        Type *type = check_expr(b, callee, NULL);
        if (type->tag != T_FN && !is_error(type)) {
            TypeNames n = {0};
            error_expr(b, "E0338", callee, "a `%s` cannot be called", tn(&n, type));
            release_names(&n);
            check_args_loosely(b, e);
            return error_type(b);
        }
        Name name = { "function", 8, callee->offset };
        return call_with(b, e, type, name, false);
    }

    Ref ref = resolve_ref(b, callee, expected);
    record(callee, ref);
    Name name = callee->kind == EXPR_NAME ? callee->as.name : callee->as.field.name;
    switch (ref.kind) {
    case REF_VALUE:
        return call_method(b, e, expected);
    case REF_FN:
        if (ref.fn->param_count || ref.fn->effect_param_count) return call_generic(b, e, ref.fn, name, expected);
        if (callee->type_arg_count) {
            error_expr(b, "E0361", callee, "`%.*s` is not generic: it takes no type arguments", NAME_ARGS(name));
        }
        callee->type = ref.fn->type;
        if (ref.fn->is_extern) {
            Type *ret = call_with(b, e, ref.fn->type, name, true);
            for (size_t i = 0; i < e->as.call.arg_count; i++) check_escape(b, e->as.call.args[i], "given to a C function");
            return ret;
        }
        return call_with(b, e, ref.fn->type, name, ref.fn->decl->is_unsafe);
    case REF_METHOD:
        callee->type = ref.method->type;
        if (ref.method->generic) return call_generic(b, e, ref.method->generic, name, expected);
        return call_with(b, e, ref.method->type, name, ref.method->decl->is_unsafe);
    case REF_LOCAL:
    case REF_FIELD: {
        Type *type = ref.kind == REF_LOCAL ? ref.local->type : ref.field->type;
        callee->type = type;
        if (type->tag == T_FN || is_error(type)) return call_with(b, e, type, name, false);
        TypeNames n = {0};
        error_expr(b, "E0338", callee, "`%.*s` is a `%s`, not a function", NAME_ARGS(name), tn(&n, type));
        release_names(&n);
        check_args_loosely(b, e);
        return error_type(b);
    }
    case REF_VARIANT:
        callee->type = ref.info->type;
        return call_variant(b, e, ref.info, ref.variant);
    case REF_BUILTIN:
        return call_builtin(b, e, ref.builtin, expected);
    case REF_TYPE: {
        Diagnostic *d = error_expr(b, "E0338", callee, "`%.*s` is a type, not a function", NAME_ARGS(ref.type->name));
        if (ref.type->tag == T_STRUCT) diag_help(d, "build a value with `%.*s{ ... }`", NAME_ARGS(ref.type->name));
        else if (ref.type->tag == T_CLASS) diag_help(d, "create it in an arena: `arena.new(%.*s, ...)`", NAME_ARGS(ref.type->name));
        else if (is_numeric(ref.type)) diag_help(d, "convert with `as`: `x as %.*s`", NAME_ARGS(ref.type->name));
        check_args_loosely(b, e);
        return error_type(b);
    }
    case REF_CONST:
        error_expr(b, "E0338", callee, "`%.*s` is a constant, not a function", NAME_ARGS(name));
        check_args_loosely(b, e);
        return error_type(b);
    case REF_MODULE:
        error_expr(b, "E0338", callee, "`%s` is a module, not a function", ref.module->module);
        check_args_loosely(b, e);
        return error_type(b);
    case REF_UNKNOWN:
        report_unknown(b, callee->as.name);
        check_args_loosely(b, e);
        return error_type(b);
    case REF_ERROR:
        check_args_loosely(b, e);
        return error_type(b);
    }
    return error_type(b);
}

/* ---- Operators and conversions ---- */

static bool inherits(const TypeInfo *sub, const TypeInfo *super) {
    for (const TypeInfo *t = sub; t; t = t->parent) {
        if (t == super) return true;
        for (size_t i = 0; i < t->interface_count; i++) {
            if (t->interfaces[i] == super) return true;
        }
    }
    return false;
}

static bool is_object_pointer(const Type *type) {
    return type->tag == T_POINTER && (type->as.inner->tag == T_CLASS || type->as.inner->tag == T_INTERFACE);
}

static Type *check_cast(Body *b, Expr *e) {
    Expr *value = e->as.cast.value;
    Type *target = sema_resolve_type(b->s, b->unit, e->as.cast.type, POSITION_VALUE);
    Type *from;
    if (is_untyped_literal(value) && is_numeric(target)) from = check_expr(b, value, target);
    else from = check_expr(b, value, NULL);
    if (is_error(from) || is_error(target)) return e->kind == EXPR_TRY_CAST ? error_type(b) : target;

    TypeNames n = {0};
    if (e->kind == EXPR_TRY_CAST) {
        bool ok = is_object_pointer(from) && target->tag == T_POINTER && target->as.inner->tag == T_CLASS &&
                  inherits(target->as.inner->as.info, from->as.inner->as.info);
        if (!ok) {
            Diagnostic *d = error_expr(b, "E0339", e, "`as?` cannot convert `%s` to `%s`", tn(&n, from), tn(&n, target));
            diag_help(d, "`as?` converts a class or interface pointer to a pointer to one of its subclasses");
        }
        release_names(&n);
        return type_option(&b->s->types, target);
    }

    bool allowed = false;
    bool needs_unsafe = false;
    if (from == target) {
        allowed = true;
    } else if (is_numeric(from) && is_numeric(target)) {
        allowed = true;
    } else if ((from->tag == T_CHAR || from->tag == T_BOOL) && target->tag == T_INT) {
        allowed = true;
    } else if (from->tag == T_INT && from->as.integer.bits == 8 && !from->as.integer.is_signed && target->tag == T_CHAR) {
        allowed = true;
    } else if (is_object_pointer(from) && is_object_pointer(target) &&
               inherits(from->as.inner->as.info, target->as.inner->as.info)) {
        allowed = true;
    } else if (from->tag == T_POINTER && target->tag == T_POINTER) {
        allowed = needs_unsafe = true;
    } else if ((from->tag == T_POINTER && target->tag == T_INT) || (from->tag == T_INT && target->tag == T_POINTER)) {
        allowed = needs_unsafe = true;
    } else if (from->tag == T_FN && target->tag == T_FN && from->as.fn.param_count == target->as.fn.param_count &&
               from->as.fn.ret == target->as.fn.ret) {
        /* the same function with other effects: what a library gives to C, which does not know them */
        allowed = needs_unsafe = true;
        for (size_t i = 0; i < from->as.fn.param_count; i++) allowed &= from->as.fn.params[i] == target->as.fn.params[i];
    }
    if (!allowed) {
        error_expr(b, "E0339", e, "cannot convert `%s` to `%s`", tn(&n, from), tn(&n, target));
    } else if (needs_unsafe && b->unsafe_depth == 0) {
        Diagnostic *d = error_expr(b, "E0341", e, "converting `%s` to `%s` is only allowed in an `unsafe` block",
                                   tn(&n, from), tn(&n, target));
        diag_help(d, "the conversion reinterprets memory (spec §7.7)");
    }
    release_names(&n);
    return target;
}

/* The constraint that allows `op` on a type parameter. */
static const char *constraint_for(TokenKind op) {
    switch (op) {
    case TOK_EQ_EQ: case TOK_BANG_EQ: return "Equal";
    case TOK_LT: case TOK_LT_EQ: case TOK_GT: case TOK_GT_EQ: return "Ordered";
    case TOK_PLUS: case TOK_MINUS: case TOK_STAR: case TOK_SLASH: return "Number";
    default: return "Integer";
    }
}

static void report_operands(Body *b, Expr *e, const char *need, Type *lt, Type *rt) {
    if (is_error(lt) || is_error(rt)) return;
    TypeNames n = {0};
    Diagnostic *d = error_expr(b, "E0334", e, "`%s` needs %s, found `%s` and `%s`",
                               token_kind_spelling(e->as.binary.op), need, tn(&n, lt), tn(&n, rt));
    if (is_numeric(lt) && is_numeric(rt)) diag_help(d, "convert one side with `as` (spec §5.3)");
    else if (lt->tag == T_PARAM || rt->tag == T_PARAM) {
        diag_help(d, "a type parameter allows what its constraints say: add one, e.g. `<%.*s: %s>` (spec §5.4)",
                  NAME_ARGS((lt->tag == T_PARAM ? lt : rt)->name), constraint_for(e->as.binary.op));
    }
    release_names(&n);
}

static bool is_comparable(const Type *type) {
    switch (type->tag) {
    case T_ENUM: /* enums without data; the others are compared with `match` */
        for (size_t i = 0; i < type->as.info->variant_count; i++) {
            if (type->as.info->variants[i].field_count > 0) return false;
        }
        return true;
    case T_INT: case T_FLOAT: case T_BOOL: case T_CHAR: case T_STR: case T_POINTER: case T_ERROR:
        return true;
    case T_PARAM:
        return type->as.param.constraints & CONSTRAINT_EQUAL;
    default:
        return false;
    }
}

static Type *check_binary(Body *b, Expr *e, Type *expected) {
    TokenKind op = e->as.binary.op;
    Expr *left = e->as.binary.left;
    Expr *right = e->as.binary.right;
    Type *lt, *rt;
    Type *bool_type = builtin(b, "bool");

    switch (op) {
    case TOK_PLUS: case TOK_MINUS: case TOK_STAR: case TOK_SLASH: case TOK_PERCENT:
        check_pair(b, left, right, expected, &lt, &rt);
        if (lt->tag == T_POINTER && rt->tag == T_INT && (op == TOK_PLUS || op == TOK_MINUS)) {
            if (b->unsafe_depth == 0) {
                Diagnostic *d = error_expr(b, "E0341", e, "pointer arithmetic is only allowed in an `unsafe` block");
                diag_help(d, "outside `unsafe`, use arrays and slices (spec §7.7)");
            }
            return lt;
        }
        if (is_error(lt) || is_error(rt)) return is_error(lt) ? rt : lt;
        if (op == TOK_PERCENT && (!is_integer(lt) || lt != rt)) {
            report_operands(b, e, "two integers of the same type", lt, rt);
        } else if (!is_numeric(lt) || lt != rt) {
            report_operands(b, e, "two numbers of the same type", lt, rt);
        }
        return lt;
    case TOK_EQ_EQ: case TOK_BANG_EQ:
        check_pair(b, left, right, NULL, &lt, &rt);
        if (!is_error(lt) && !is_error(rt) && (lt != rt || !is_comparable(lt))) {
            report_operands(b, e, "two values of the same comparable type", lt, rt);
        }
        return bool_type;
    case TOK_LT: case TOK_LT_EQ: case TOK_GT: case TOK_GT_EQ:
        check_pair(b, left, right, NULL, &lt, &rt);
        bool ordered = is_numeric(lt) || lt->tag == T_CHAR || lt->tag == T_STR || has_constraint(lt, CONSTRAINT_ORDERED);
        if (!is_error(lt) && !is_error(rt) && (lt != rt || !ordered)) {
            report_operands(b, e, "two numbers, characters or texts of the same type", lt, rt);
        }
        return bool_type;
    case TOK_AMP_AMP: case TOK_PIPE_PIPE:
        check_expect(b, left, bool_type);
        check_expect(b, right, bool_type);
        return bool_type;
    case TOK_AMP: case TOK_PIPE: case TOK_CARET:
        check_pair(b, left, right, expected, &lt, &rt);
        if (!is_error(lt) && !is_error(rt) && (!is_integer(lt) || lt != rt)) {
            report_operands(b, e, "two integers of the same type", lt, rt);
        }
        return lt;
    case TOK_SHL: case TOK_SHR:
        lt = check_expr(b, left, expected);
        rt = check_expr(b, right, NULL);
        if (!is_error(lt) && !is_error(rt) && (!is_integer(lt) || !is_integer(rt))) {
            report_operands(b, e, "integers", lt, rt);
        }
        return lt;
    default:
        return error_type(b);
    }
}

static bool is_place(const Expr *e) {
    return e->kind == EXPR_NAME || e->kind == EXPR_FIELD || e->kind == EXPR_INDEX ||
           (e->kind == EXPR_UNARY && e->as.unary.op == TOK_STAR) || e->kind == EXPR_SELF;
}

static Type *check_unary(Body *b, Expr *e, Type *expected) {
    Expr *operand = e->as.unary.operand;
    TypeNames n = {0};
    Type *type;
    switch (e->as.unary.op) {
    case TOK_MINUS:
        if (operand->kind == EXPR_INT) {
            type = check_int_literal(b, operand, expected, true);
            operand->type = type;
            return type;
        }
        type = check_expr(b, operand, expected);
        bool is_signed = type->tag == T_FLOAT ? !type->as.floating.is_unsigned
                                              : type->tag == T_INT && type->as.integer.is_signed;
        if (!is_error(type) && !is_signed) {
            Diagnostic *d = error_expr(b, "E0334", e, "`-` needs a signed integer or a signed float, found `%s`", tn(&n, type));
            if (type->tag == T_FLOAT) diag_help(d, "an unsigned float is never negative: convert it first, `x as f64`");
            else if (type->tag == T_PARAM) {
                diag_help(d, "a parameter may stand for an unsigned type: write `(0 as %.*s) - x`", NAME_ARGS(type->name));
            }
        }
        break;
    case TOK_BANG:
        type = check_expect(b, operand, builtin(b, "bool"));
        break;
    case TOK_TILDE:
        type = check_expr(b, operand, expected);
        if (!is_error(type) && !is_integer(type)) error_expr(b, "E0334", e, "`~` needs an integer, found `%s`", tn(&n, type));
        break;
    case TOK_AMP:
        type = check_expr(b, operand, NULL);
        if (!is_place(operand)) {
            error_expr(b, "E0353", e, "cannot take the address of a temporary value");
            type = error_type(b);
        } else if (type->tag == T_FN && (operand->kind == EXPR_FIELD || operand->resolution == RESOLVED_SELF_FIELD)) {
            Diagnostic *d = error_expr(b, "E0353", e, "cannot take the address of a field of function type");
            diag_help(d, "a field holds a C function pointer (spec §5.5): copy it into a variable first");
            type = error_type(b);
        } else if (!is_error(type)) {
            /* the pointer allows writing: the value must be mutable (spec §5.3) */
            b->taking_address = true;
            check_assignable(b, operand);
            b->taking_address = false;
            type = type_pointer(&b->s->types, type);
        }
        break;
    case TOK_STAR:
        type = check_expr(b, operand, NULL);
        if (type->tag == T_POINTER) {
            type = type->as.inner;
        } else if (!is_error(type)) {
            error_expr(b, "E0334", e, "`*` needs a pointer, found `%s`", tn(&n, type));
            type = error_type(b);
        }
        break;
    default:
        type = error_type(b);
        break;
    }
    release_names(&n);
    return type;
}

/* ---- Fields, indexes, literals ---- */

static Type *check_field(Body *b, Expr *e, Type *expected) {
    Ref ref = resolve_ref(b, e, expected);
    if (ref.kind != REF_VALUE) return value_of_ref(b, e, ref, expected);

    Name name = e->as.field.name;
    Type *object = check_expr(b, e->as.field.object, NULL);
    Type *base = object->tag == T_POINTER ? object->as.inner : object;
    if (is_error(base)) return base;
    if (base->tag == T_STRUCT || base->tag == T_CLASS) {
        FieldInfo *field = find_field(base->as.info, name);
        if (field) {
            if (base->as.info->unit != b->unit && !field->decl->is_pub) {
                Diagnostic *d = error_at(b, "E0305", name.offset, name.length, "the field `%.*s` of `%.*s` is private",
                                         NAME_ARGS(name), NAME_ARGS(base->name));
                diag_help(d, "declare it `pub` in `%.*s`", NAME_ARGS(base->name));
            }
            return field->type;
        }
        if (find_method(base->as.info, name)) {
            Diagnostic *d = error_at(b, "E0333", name.offset, name.length, "`%.*s` is a method", NAME_ARGS(name));
            diag_help(d, "call it: `%.*s(...)`", NAME_ARGS(name));
            return error_type(b);
        }
    }
    if ((base->tag == T_ARRAY || base->tag == T_SLICE || base->tag == T_STR) && name_is(name, "len")) {
        return builtin(b, "usize");
    }
    if (base->tag == T_STR && name_is(name, "bytes")) return type_slice(&b->s->types, builtin(b, "ui8"));
    TypeNames n = {0};
    error_at(b, "E0337", name.offset, name.length, "`%s` has no field `%.*s`", tn(&n, base), NAME_ARGS(name));
    release_names(&n);
    return error_type(b);
}

/* `x[a..b]`: a view of the elements a to b - 1 of an array or a slice. */
static Type *check_slicing(Body *b, Expr *e, Type *object) {
    Type *base = object->tag == T_POINTER ? object->as.inner : object;
    Expr *range = e->as.index.index;
    Type *usize = builtin(b, "usize");
    check_expect(b, range->as.range.start, usize);
    check_expect(b, range->as.range.end, usize);
    range->type = usize;
    if (base->tag == T_ARRAY) {
        if (object->tag != T_POINTER && is_place(e->as.index.object)) {
            /* the view allows writing, like `&x`: the array must be mutable */
            b->taking_address = true;
            check_assignable(b, e->as.index.object);
            b->taking_address = false;
        } else if (object->tag != T_POINTER) {
            error_expr(b, "E0353", e, "cannot take a view of a temporary array");
        }
        return type_slice(&b->s->types, base->as.array.element);
    }
    if (base->tag == T_SLICE) return base;
    if (!is_error(base)) {
        TypeNames n = {0};
        Diagnostic *d = error_expr(b, "E0334", e, "a `%s` cannot be sliced", tn(&n, base));
        if (base->tag == T_STR) diag_help(d, "slice its bytes: `s.bytes[a..b]` (spec §4.1)");
        release_names(&n);
    }
    return error_type(b);
}

static Type *check_index(Body *b, Expr *e) {
    Type *object = check_expr(b, e->as.index.object, NULL);
    if (e->as.index.index->kind == EXPR_RANGE) {
        if (e->as.index.index->as.range.inclusive) {
            Diagnostic *d = error_expr(b, "E0352", e->as.index.index, "a view takes a range without its end: `a..b`");
            diag_help(d, "to include the element b, write `x[a..b + 1]`");
        }
        return check_slicing(b, e, object);
    }
    Type *base = object->tag == T_POINTER ? object->as.inner : object;
    Type *index = check_expr(b, e->as.index.index, builtin(b, "usize"));
    TypeNames n = {0};
    if (!is_error(index) && !is_integer(index)) {
        error_expr(b, "E0334", e->as.index.index, "an index must be an integer, not `%s`", tn(&n, index));
    }
    Type *result = error_type(b);
    if (base->tag == T_ARRAY) {
        result = base->as.array.element;
    } else if (base->tag == T_SLICE) {
        result = base->as.inner;
    } else if (base->tag == T_STR) {
        Diagnostic *d = error_expr(b, "E0334", e, "a `str` cannot be indexed");
        diag_help(d, "use `s.bytes[i]` for a byte (spec §4.1)");
    } else if (!is_error(base)) {
        error_expr(b, "E0334", e, "a `%s` cannot be indexed", tn(&n, base));
    }
    release_names(&n);
    return result;
}

/* `[a, b, c]` and `[value; count]`: the element type comes from the expected array type,
   or else from the first element. */
static Type *check_array(Body *b, Expr *e, Type *expected) {
    Type *element = expected && expected->tag == T_ARRAY ? expected->as.array.element : NULL;
    uint64_t size;
    if (e->as.array.repeat) {
        Expr *value = e->as.array.repeat;
        element = element ? check_expect(b, value, element) : check_expr(b, value, NULL);
        if (type_owns_resource(element)) {
            error_expr(b, "E0356", value, "a value that owns a resource cannot be repeated");
        }
        size = e->as.array.repeat_count;
    } else {
        for (size_t i = 0; i < e->as.array.count; i++) {
            Expr *item = e->as.array.items[i];
            if (element) check_expect(b, item, element);
            else element = check_expr(b, item, NULL);
            consume(b, item);
            check_escape(b, item, "stored in an array");
        }
        size = e->as.array.count;
    }
    if (size == 0) {
        error_expr(b, "E0308", e, "an array has at least one element");
        return error_type(b);
    }
    if (!element || is_error(element)) return error_type(b);
    if (element->tag == T_VOID) {
        error_expr(b, "E0331", e, "the elements of this array have no value");
        return error_type(b);
    }
    if (expected && expected->tag == T_ARRAY && expected->as.array.size != size) {
        TypeNames n = {0};
        Diagnostic *d = error_expr(b, "E0355", e, "the array has %llu element%s, but `%s` has %llu", (unsigned long long)size,
                                   size == 1 ? "" : "s", tn(&n, expected), (unsigned long long)expected->as.array.size);
        diag_help(d, "give every element, or repeat one value: `[value; %llu]`", (unsigned long long)expected->as.array.size);
        release_names(&n);
        return expected;
    }
    return type_array(&b->s->types, element, size);
}

static Type *check_struct_literal(Body *b, Expr *e, Type *expected) {
    const TypeExpr *te = e->as.struct_lit.type;
    Type *type = sema_resolve_named(b->s, b->unit, te->as.name, te->offset, te->length);
    if (type->tag == T_STRUCT && type->as.info->param_count && !type->as.info->generic) {
        /* `Pair<A, B>{ ... }`, or `Pair{ ... }` where a `Pair<A, B>` is expected (spec §5.4) */
        if (te->arg_count) type = sema_resolve_type(b->s, b->unit, te, POSITION_VALUE);
        else if (expected && is_nominal(expected) && generic_of(expected) == type->as.info) type = expected;
        else type = sema_resolve_type(b->s, b->unit, te, POSITION_VALUE); /* reports the missing arguments */
    } else if (te->arg_count) {
        type = sema_resolve_type(b->s, b->unit, te, POSITION_VALUE);
    }
    size_t count = e->as.struct_lit.field_count;
    FieldInit *inits = e->as.struct_lit.fields;
    if (type->tag != T_STRUCT) {
        if (!is_error(type)) {
            Diagnostic *d = error_expr(b, "E0350", e, "`%.*s` is not a struct", NAME_ARGS(type->name));
            if (type->tag == T_CLASS) diag_help(d, "a class is created in an arena: `arena.new(%.*s, ...)`", NAME_ARGS(type->name));
        }
        for (size_t i = 0; i < count; i++) check_expr(b, inits[i].value, NULL);
        return error_type(b);
    }
    TypeInfo *info = type->as.info;
    bool *given = calloc(info->field_count + 1, sizeof *given);
    for (size_t i = 0; i < count; i++) {
        Name name = inits[i].name;
        size_t index = info->field_count;
        for (size_t j = 0; j < info->field_count; j++) {
            if (sema_name_eq(info->fields[j].name, name)) index = j;
        }
        if (index == info->field_count) {
            error_at(b, "E0350", name.offset, name.length, "`%.*s` has no field `%.*s`", NAME_ARGS(type->name), NAME_ARGS(name));
            check_expr(b, inits[i].value, NULL);
            continue;
        }
        if (given[index]) error_at(b, "E0350", name.offset, name.length, "the field `%.*s` is given twice", NAME_ARGS(name));
        given[index] = true;
        check_expect(b, inits[i].value, info->fields[index].type);
        consume(b, inits[i].value);
        check_escape(b, inits[i].value, "stored in a field");
    }
    for (size_t j = 0; j < info->field_count; j++) {
        if (given[j] || info->fields[j].decl->init) continue;
        Diagnostic *d = error_expr(b, "E0350", e, "missing field `%.*s` in `%.*s{ ... }`", NAME_ARGS(info->fields[j].name),
                                   NAME_ARGS(type->name));
        diag_help(d, "give every field a value; only fields with a default value can be left out");
        break;
    }
    free(given);
    return type;
}

/* ---- match ---- */

static bool always_returns(const Stmt *stmt);

/* The cases a `match` must cover: the variants of an enum, or some / none, ok / err. */
typedef struct {
    Name name;
    Type *fields[1];
    size_t field_count;
} Case;

static Type *check_match(Body *b, Expr *e, Type *expected, bool statement) {
    Expr *subject_expr = e->as.match.subject;
    Type *subject = check_expr(b, subject_expr, NULL);
    if (subject->tag == T_POINTER) subject = subject->as.inner;

    VEC(Case) cases = {0};
    const TypeInfo *info = NULL;
    if (subject->tag == T_ENUM) {
        info = subject->as.info;
    } else if (subject->tag == T_OPTION) {
        Case some = { { "some", 4, 0 }, { subject->as.inner }, 1 };
        Case none = { { "none", 4, 0 }, { NULL }, 0 };
        SOSU_VEC_PUSH(&cases, some);
        SOSU_VEC_PUSH(&cases, none);
    } else if (subject->tag == T_RESULT) {
        Type *value = subject->as.result.value;
        Case ok = { { "ok", 2, 0 }, { value }, value->tag == T_VOID ? 0 : 1 };
        Case err = { { "err", 3, 0 }, { subject->as.result.error }, 1 };
        SOSU_VEC_PUSH(&cases, ok);
        SOSU_VEC_PUSH(&cases, err);
    } else if (!is_error(subject)) {
        TypeNames n = {0};
        error_expr(b, "E0343", subject_expr, "`match` needs an enum, an option or a result, not `%s`", tn(&n, subject));
        release_names(&n);
    }
    size_t case_count = info ? info->variant_count : cases.count;
    bool *covered = calloc(case_count + 1, sizeof *covered);
    bool wildcard = false;
    bool known = info || cases.count > 0;
    Type *result = statement ? void_type(b) : expected;

    for (size_t a = 0; a < e->as.match.arm_count; a++) {
        MatchArm *arm = &e->as.match.arms[a];
        const Pattern *pattern = &arm->pattern;
        size_t mark = b->locals.count;
        bool all_covered = known && !wildcard;
        for (size_t i = 0; i < case_count && all_covered; i++) all_covered = covered[i];
        if (wildcard || (known && all_covered)) {
            error_at(b, "E0343", pattern->offset, pattern->length, "this arm can never match");
        }

        if (pattern->kind == PATTERN_WILDCARD) {
            wildcard = true;
        } else if (known) {
            Name name = pattern->path.items[pattern->path.count - 1];
            size_t index = case_count;
            for (size_t i = 0; i < case_count; i++) {
                Name candidate = info ? info->variants[i].name : cases.items[i].name;
                if (sema_name_eq(candidate, name)) index = i;
            }
            bool bad_path = pattern->path.count > 2 || (pattern->path.count == 2 && info &&
                                                        !sema_name_eq(pattern->path.items[0], info->name));
            if (index == case_count || bad_path) {
                TypeNames n = {0};
                Diagnostic *d = error_at(b, "E0343", pattern->offset, pattern->length, "`%s` has no case `%.*s`",
                                         tn(&n, subject), NAME_ARGS(name));
                if (!info) diag_help(d, subject->tag == T_OPTION ? "the cases are `some(x)` and `none`"
                                                                 : "the cases are `ok(x)` and `err(e)`");
                release_names(&n);
            } else {
                if (covered[index]) {
                    error_at(b, "E0343", pattern->offset, pattern->length, "`%.*s` is already matched", NAME_ARGS(name));
                }
                covered[index] = true;
                size_t fields = info ? info->variants[index].field_count : cases.items[index].field_count;
                if (pattern->binding_count != fields || (fields > 0 && !pattern->has_parens)) {
                    error_at(b, "E0343", pattern->offset, pattern->length, "`%.*s` has %zu field%s, the pattern binds %zu",
                             NAME_ARGS(name), fields, fields == 1 ? "" : "s", pattern->binding_count);
                } else {
                    for (size_t j = 0; j < fields; j++) {
                        Type *field_type = info ? info->variants[index].fields[j] : cases.items[index].fields[j];
                        declare_local(b, pattern->bindings[j], field_type, false);
                    }
                }
            }
        }

        if (arm->block) {
            check_block(b, arm->block);
            if (!statement && !always_returns(arm->block)) {
                Diagnostic *d = error_at(b, "E0343", arm->block->offset, arm->block->length,
                                         "a block arm in a `match` that gives a value must end with `return`");
                diag_help(d, "give the value as an expression: `%.*s => value`", (int)pattern->length,
                          b->unit->file.text + pattern->offset);
            }
        } else if (statement) {
            check_expr(b, arm->expr, NULL);
        } else if (!result) {
            result = check_expr(b, arm->expr, NULL);
        } else {
            check_expect(b, arm->expr, result);
        }
        b->locals.count = mark;
    }

    if (known && !wildcard) {
        char missing[160] = "";
        size_t missing_count = 0;
        for (size_t i = 0; i < case_count; i++) {
            if (covered[i]) continue;
            Name name = info ? info->variants[i].name : cases.items[i].name;
            if (missing_count < 3) {
                size_t used = strlen(missing);
                snprintf(missing + used, sizeof missing - used, "%s`%.*s`", missing_count ? ", " : "", NAME_ARGS(name));
            }
            missing_count++;
        }
        if (missing_count) {
            Diagnostic *d = error_at(b, "E0312", e->offset, 5, "non-exhaustive match: missing %s%s%s",
                                     missing_count == 1 ? "case " : "cases ", missing, missing_count > 3 ? "..." : "");
            diag_help(d, "add an arm for each missing case, or `_ => ...`");
        }
    }
    free(covered);
    free(cases.items);
    return result ? result : error_type(b);
}

/* ---- Lambdas (spec §5.5) ---- */

static void check_expr_stmt(Body *b, Expr *e);
static bool always_returns(const Stmt *stmt);

/* The type `pattern` of a parameter of `generic` once its parameters are bound, or NULL if
   some are not yet (without `generic`, `pattern` itself). */
static Type *hinted(Body *b, Type *pattern, FnInfo *generic, Type **bound, unsigned *effects) {
    if (!generic) return pattern;
    return all_bound(generic, pattern, bound, effects) ? bind(b, generic, pattern, bound, effects) : NULL;
}

/* Checks a lambda against the function type expected for it, if any. Given to a parameter
   of the generic function `generic` (whose parameters are in `bound` and `effects`), it takes
   the types that are known, and gives the others. */
static Type *check_lambda(Body *b, Expr *e, Type *expected, FnInfo *generic, Type **bound, unsigned *effects) {
    size_t count = e->as.lambda.param_count;
    Type *pattern = expected && expected->tag == T_FN ? expected : NULL;
    bool wrong_count = pattern && pattern->as.fn.param_count != count;
    if (wrong_count) {
        TypeNames n = {0};
        error_expr(b, "E0336", e, "the lambda takes %zu parameter%s, but `%s` takes %zu", count, count == 1 ? "" : "s",
                   tn(&n, pattern), pattern->as.fn.param_count);
        release_names(&n);
        pattern = NULL;
    }

    Type **params = arena_alloc(&b->s->arena, (count + 1) * sizeof *params);
    for (size_t i = 0; i < count; i++) {
        const Param *param = &e->as.lambda.params[i];
        Type *known = pattern ? hinted(b, pattern->as.fn.params[i], generic, bound, effects) : NULL;
        if (param->type) {
            params[i] = sema_resolve_type(b->s, b->unit, param->type, POSITION_VALUE);
        } else if (known) {
            params[i] = known;
        } else if (wrong_count) {
            params[i] = error_type(b);
        } else {
            Diagnostic *d = error_at(b, "E0369", param->name.offset, param->name.length,
                                     "the type of the parameter `%.*s` is unknown here", NAME_ARGS(param->name));
            diag_help(d, "write it, `(i32 %.*s) => ...`, or give the lambda a type: `fn(i32) -> i32 f = ...`",
                      NAME_ARGS(param->name));
            params[i] = error_type(b);
        }
    }
    Type *ret = pattern ? hinted(b, pattern->as.fn.ret, generic, bound, effects) : NULL;

    /* its effects: those of its type, or of its body when no type says them */
    bool inferring = !pattern;
    unsigned allowed = pattern ? pattern->as.fn.effects : 0;
    if (pattern && generic) {
        allowed &= ~EFFECT_PARAMS;
        for (size_t k = 0; k < generic->effect_param_count; k++) {
            if (!(pattern->as.fn.effects & (EFFECT_PARAM << k))) continue;
            if (effects[k] == EFFECT_UNBOUND) inferring = true;
            else allowed |= effects[k];
        }
    }

    Body saved = *b;
    LambdaScope scope = {0};
    scope.outer = b->lambda;
    scope.base = b->locals.count;
    scope.inferring = inferring;
    b->lambda = &scope;
    b->ret = ret;
    b->effects = allowed;
    b->fn_name = (Name){ "lambda", 6, e->offset };
    b->in_init = false;
    b->in_ensures = false;
    b->unsafe_depth = 0;
    b->loop_depth = 0;
    b->defer_depth = 0;
    for (size_t i = 0; i < count; i++) declare_local(b, e->as.lambda.params[i].name, params[i], false);

    if (e->as.lambda.block) {
        check_block(b, e->as.lambda.block);
        if (!b->ret) b->ret = void_type(b);
        if (b->ret->tag != T_VOID && !is_error(b->ret) && !always_returns(e->as.lambda.block)) {
            Diagnostic *d = error_at(b, "E0346", e->offset, e->length, "the lambda does not return a value on every path");
            diag_help(d, "end each path with `return ...;`");
        }
    } else {
        Expr *body = e->as.lambda.body;
        if (b->ret && b->ret->tag == T_VOID) {
            check_expr_stmt(b, body);
        } else {
            if (b->ret) check_expect(b, body, b->ret);
            else b->ret = check_expr(b, body, NULL);
            consume(b, body);
            check_escape(b, body, "returned");
        }
    }
    ret = b->ret;
    unsigned fx = inferring ? scope.used : allowed;

    b->locals.count = scope.base;
    b->lambda = saved.lambda;
    b->ret = saved.ret;
    b->effects = saved.effects;
    b->fn_name = saved.fn_name;
    b->in_init = saved.in_init;
    b->in_ensures = saved.in_ensures;
    b->unsafe_depth = saved.unsafe_depth;
    b->loop_depth = saved.loop_depth;
    b->defer_depth = saved.defer_depth;

    e->as.lambda.capture_count = scope.captures.count;
    e->as.lambda.captures = NULL;
    if (scope.captures.count) {
        e->as.lambda.captures = arena_alloc(&b->s->arena, scope.captures.count * sizeof *e->as.lambda.captures);
        memcpy(e->as.lambda.captures, scope.captures.items, scope.captures.count * sizeof *e->as.lambda.captures);
    }
    free(scope.captures.items);
    if (wrong_count) return error_type(b); /* already reported */
    return type_fn(&b->s->types, params, count, ret, fx);
}

/* ---- Expressions ---- */

static Type *check_expr_kind(Body *b, Expr *e, Type *expected) {
    switch (e->kind) {
    case EXPR_LAMBDA:
        return check_lambda(b, e, expected, NULL, NULL, NULL);
    case EXPR_ERROR:
        return error_type(b);
    case EXPR_INT:
        return check_int_literal(b, e, expected, false);
    case EXPR_FLOAT:
        return expected && expected->tag == T_FLOAT ? expected : builtin(b, "f64");
    case EXPR_STRING:
        return builtin(b, "str");
    case EXPR_CHAR:
        return builtin(b, "char");
    case EXPR_BOOL:
        return builtin(b, "bool");
    case EXPR_NAME:
        if (b->in_ensures && name_is(e->as.name, "result")) { /* contextual: the returned value */
            e->kind = EXPR_RESULT;
            return check_expr_kind(b, e, expected);
        }
        return value_of_ref(b, e, lookup_name(b, e->as.name, expected), expected);
    case EXPR_SELF:
    case EXPR_BASE: {
        const char *word = e->kind == EXPR_SELF ? "self" : "base";
        if (!b->self_info) {
            error_expr(b, "E0349", e, "`%s` can only be used in a method", word);
            return error_type(b);
        }
        capture_self(b);
        if (e->kind == EXPR_BASE) {
            if (!b->self_info->parent) {
                error_expr(b, "E0349", e, "`%.*s` has no parent class", NAME_ARGS(b->self_info->name));
                return error_type(b);
            }
            return type_pointer(&b->s->types, b->self_info->parent->type);
        }
        return type_pointer(&b->s->types, b->self_info->type);
    }
    case EXPR_RESULT:
        if (b->ret->tag == T_VOID) {
            Diagnostic *d = error_expr(b, "E0349", e, "`%.*s` returns nothing: `result` has no value here",
                                       NAME_ARGS(b->fn_name));
            diag_help(d, "in `ensures`, `result` is the value returned by the function");
            return error_type(b);
        }
        return b->ret;
    case EXPR_UNARY:
        return check_unary(b, e, expected);
    case EXPR_BINARY:
        return check_binary(b, e, expected);
    case EXPR_CAST:
    case EXPR_TRY_CAST:
        return check_cast(b, e);
    case EXPR_TERNARY: {
        check_expect(b, e->as.ternary.cond, builtin(b, "bool"));
        Type *then_type, *else_type;
        check_pair(b, e->as.ternary.then_expr, e->as.ternary.else_expr, expected, &then_type, &else_type);
        if (then_type != else_type && !is_error(then_type)) report_mismatch(b, e->as.ternary.else_expr, then_type, else_type);
        return then_type;
    }
    case EXPR_RANGE: {
        Diagnostic *d = error_expr(b, "E0352", e, "a range can only be used in `for (... in a..b)`");
        diag_help(d, "loop over it: `for (i32 i in a..b) { ... }`");
        Type *lt, *rt;
        check_pair(b, e->as.range.start, e->as.range.end, NULL, &lt, &rt);
        return error_type(b);
    }
    case EXPR_CALL:
        return check_call(b, e, expected);
    case EXPR_INDEX:
        return check_index(b, e);
    case EXPR_FIELD:
        return check_field(b, e, expected);
    case EXPR_STRUCT_LIT:
        return check_struct_literal(b, e, expected);
    case EXPR_MATCH:
        return check_match(b, e, expected, false);
    case EXPR_ARRAY:
        return check_array(b, e, expected);
    case EXPR_TRY: {
        Type *type = check_expr(b, e->as.inner, NULL);
        if (is_error(type)) return type;
        TypeNames n = {0};
        if (type->tag != T_RESULT) {
            Diagnostic *d = error_expr(b, "E0345", e, "`try` needs a `T!E` value, found `%s`", tn(&n, type));
            if (type->tag == T_OPTION) diag_help(d, "for an option, use `match` with `some(x)` and `none`");
            release_names(&n);
            return error_type(b);
        }
        if (b->ret->tag != T_RESULT || b->ret->as.result.error != type->as.result.error) {
            const char *error_name = tn(&n, type->as.result.error);
            Diagnostic *d = error_expr(b, "E0345", e, "`try` returns the error early, but `%.*s` does not return `...!%s`",
                                       NAME_ARGS(b->fn_name), error_name);
            diag_help(d, "return a result with the error `%s`, or handle it with `match`", error_name);
        }
        release_names(&n);
        return type->as.result.value;
    }
    }
    return error_type(b);
}

static Type *check_expr(Body *b, Expr *e, Type *expected) {
    Type *type = check_expr_kind(b, e, expected);
    e->type = type;
    return type;
}

/* ---- Assignments ---- */

static bool field_is_assignable(Body *b, const FieldInfo *field) {
    if (field->decl->is_mut) return true;
    return b->in_init && b->self_info && find_field(b->self_info, field->name) == field;
}

static void report_field(Body *b, const Expr *e, Name name, const char *owner, size_t owner_length) {
    Diagnostic *d = error_expr(b, "E0335", e, "the field `%.*s` is not mutable", NAME_ARGS(name));
    diag_help(d, "declare it `mut` in `%.*s`, or assign it in `init` (spec §5.2)", (int)owner_length, owner);
}

/* Reports `target` if it cannot be assigned. Its types are already checked. */
static void check_assignable(Body *b, Expr *target) {
    switch (target->kind) {
    case EXPR_NAME: {
        Ref ref = lookup_name(b, target->as.name, NULL);
        if (ref.kind == REF_LOCAL) {
            if (is_captured(b, ref.local)) {
                Diagnostic *d = error_expr(b, "E0369", target, "`%.*s` is captured by the lambda: it cannot be changed",
                                           NAME_ARGS(target->as.name));
                diag_help(d, "a lambda copies the variables it uses; to change one, capture a pointer to it (spec §5.5)");
            } else if (!ref.local->is_mut) {
                Diagnostic *d = error_expr(b, "E0335", target, "`%.*s` is not mutable", NAME_ARGS(target->as.name));
                if (b->taking_address) diag_help(d, "declare it with `mut`: through its address, it can be changed");
                else diag_help(d, "declare it with `mut` to change it");
            }
        } else if (ref.kind == REF_FIELD) {
            if (!field_is_assignable(b, ref.field)) {
                report_field(b, target, ref.field->name, b->self_info->name.text, b->self_info->name.length);
            }
        } else if (ref.kind != REF_UNKNOWN && ref.kind != REF_ERROR) {
            error_expr(b, "E0335", target, "`%.*s` cannot be assigned", NAME_ARGS(target->as.name));
        }
        return;
    }
    case EXPR_FIELD: {
        Expr *object = target->as.field.object;
        Type *type = object->type ? object->type : error_type(b);
        Type *base = type->tag == T_POINTER ? type->as.inner : type;
        if (is_error(base) || is_error(target->type)) return;
        FieldInfo *field = (base->tag == T_STRUCT || base->tag == T_CLASS) ? find_field(base->as.info, target->as.field.name)
                                                                          : NULL;
        if (!field) {
            error_expr(b, "E0335", target, "`%.*s` cannot be assigned", NAME_ARGS(target->as.field.name));
            return;
        }
        if (!field_is_assignable(b, field)) {
            report_field(b, target, field->name, base->name.text, base->name.length);
            return;
        }
        if (type->tag != T_POINTER) check_assignable(b, object); /* the value holding the field */
        return;
    }
    case EXPR_INDEX: {
        if (target->as.index.index->kind == EXPR_RANGE) {
            Diagnostic *d = error_expr(b, "E0335", target, "a view `x[a..b]` cannot be assigned");
            diag_help(d, "assign its elements, one by one");
            return;
        }
        Type *type = target->as.index.object->type;
        if (type && type->tag == T_ARRAY) check_assignable(b, target->as.index.object);
        return;
    }
    case EXPR_UNARY:
        if (target->as.unary.op == TOK_STAR) return;
        break;
    case EXPR_SELF:
    case EXPR_ERROR:
        return;
    default:
        break;
    }
    error_expr(b, "E0335", target, "this expression cannot be assigned");
}

static void check_assign(Body *b, Stmt *stmt) {
    Expr *target = stmt->as.assign.target;
    Expr *value = stmt->as.assign.value;
    TokenKind op = stmt->as.assign.op;
    b->assigning = op == TOK_EQ && target->kind == EXPR_NAME; /* a moved variable can get a new value */
    Type *tt = check_expr(b, target, NULL);
    b->assigning = false;
    check_assignable(b, target);
    if (op == TOK_EQ) {
        check_expect(b, value, tt);
        consume(b, value);
        check_escape(b, value, target->kind == EXPR_NAME && target->resolution == RESOLVED_LOCAL
                                   ? "assigned with `=`: give it to a new variable"
                                   : "stored there");
        if (target->kind == EXPR_NAME && target->resolution == RESOLVED_LOCAL) {
            Local *local = find_local(b, target->as.name);
            if (local) local->moved = false;
        }
        return;
    }
    if (op == TOK_SHL_EQ || op == TOK_SHR_EQ) {
        Type *vt = check_expr(b, value, NULL);
        if (!is_error(tt) && !is_error(vt) && (!is_integer(tt) || !is_integer(vt))) {
            TypeNames n = {0};
            error_expr(b, "E0334", value, "`%s` needs integers, found `%s` and `%s`", token_kind_spelling(op), tn(&n, tt),
                       tn(&n, vt));
            release_names(&n);
        }
        return;
    }
    Type *vt = check_expr(b, value, tt->tag == T_POINTER ? NULL : tt);
    if (is_error(tt) || is_error(vt)) return;
    bool bitwise = op == TOK_AMP_EQ || op == TOK_PIPE_EQ || op == TOK_CARET_EQ || op == TOK_PERCENT_EQ;
    bool pointer_step = tt->tag == T_POINTER && vt->tag == T_INT && (op == TOK_PLUS_EQ || op == TOK_MINUS_EQ);
    if (pointer_step) {
        if (b->unsafe_depth == 0) error_expr(b, "E0341", value, "pointer arithmetic is only allowed in an `unsafe` block");
        return;
    }
    bool ok = tt == vt && (bitwise ? is_integer(tt) : is_numeric(tt));
    if (!ok) {
        TypeNames n = {0};
        error_expr(b, "E0334", value, "`%s` needs %s, found `%s` and `%s`", token_kind_spelling(op),
                   bitwise ? "two integers of the same type" : "two numbers of the same type", tn(&n, tt), tn(&n, vt));
        release_names(&n);
    }
}

/* ---- Statements ---- */

/* True if `stmt` contains a `break` for the enclosing loop (not for a nested one). */
static bool contains_break(const Stmt *stmt) {
    if (!stmt) return false;
    switch (stmt->kind) {
    case STMT_BREAK:
        return true;
    case STMT_BLOCK:
        for (size_t i = 0; i < stmt->as.block.count; i++) {
            if (contains_break(stmt->as.block.items[i])) return true;
        }
        return false;
    case STMT_IF:
        return contains_break(stmt->as.if_.then_block) || contains_break(stmt->as.if_.else_stmt);
    case STMT_UNSAFE:
        return contains_break(stmt->as.inner);
    case STMT_EXPR: {
        const Expr *e = stmt->as.expr;
        if (e->kind != EXPR_MATCH) return false;
        for (size_t i = 0; i < e->as.match.arm_count; i++) {
            if (contains_break(e->as.match.arms[i].block)) return true;
        }
        return false;
    }
    default:
        return false;
    }
}

static bool always_returns(const Stmt *stmt) {
    if (!stmt) return false;
    switch (stmt->kind) {
    case STMT_RETURN:
        return true;
    case STMT_BLOCK:
        for (size_t i = 0; i < stmt->as.block.count; i++) {
            if (always_returns(stmt->as.block.items[i])) return true;
        }
        return false;
    case STMT_IF:
        return always_returns(stmt->as.if_.then_block) && always_returns(stmt->as.if_.else_stmt);
    case STMT_UNSAFE:
        return always_returns(stmt->as.inner);
    case STMT_WHILE: /* `while (true)` only ends with `break` */
        return stmt->as.while_.cond->kind == EXPR_BOOL && stmt->as.while_.cond->as.bool_value &&
               !contains_break(stmt->as.while_.body);
    case STMT_EXPR: {
        const Expr *e = stmt->as.expr;
        if (e->kind != EXPR_MATCH || e->as.match.arm_count == 0) return false;
        for (size_t i = 0; i < e->as.match.arm_count; i++) {
            if (!always_returns(e->as.match.arms[i].block)) return false;
        }
        return true;
    }
    default:
        return false;
    }
}

static void check_block(Body *b, Stmt *block) {
    size_t mark = b->locals.count;
    for (size_t i = 0; i < block->as.block.count; i++) check_stmt(b, block->as.block.items[i]);
    b->locals.count = mark;
}

static void check_var(Body *b, Stmt *stmt) {
    Type *type = stmt->as.var.type ? sema_resolve_type(b->s, b->unit, stmt->as.var.type, POSITION_VALUE) : NULL;
    Expr *init = stmt->as.var.init;
    if (type) {
        check_expect(b, init, type);
    } else {
        type = check_expr(b, init, NULL);
        if (type->tag == T_VOID) {
            error_expr(b, "E0331", init, "this expression has no value");
            type = error_type(b);
        }
    }
    consume(b, init);
    bool closure = type->tag == T_FN && is_closure(b, init);
    declare_local(b, stmt->as.var.name, type, stmt->as.var.is_mut);
    if (closure && !name_is(stmt->as.var.name, "_")) b->locals.items[b->locals.count - 1].closure = true;
}

static void check_expr_stmt(Body *b, Expr *e) {
    if (e->kind == EXPR_MATCH) {
        e->type = check_match(b, e, NULL, true);
        return;
    }
    Type *type = check_expr(b, e, NULL);
    bool is_call = e->kind == EXPR_CALL || (e->kind == EXPR_TRY && e->as.inner->kind == EXPR_CALL);
    if (!is_call) {
        Diagnostic *d = error_expr(b, "E0348", e, "this expression has no effect");
        diag_help(d, "an expression statement is a call or a `match`; assign the value, or remove it");
    } else if (e->kind == EXPR_CALL && type->tag == T_RESULT) {
        Diagnostic *d = error_expr(b, "E0348", e, "the result of this call must be used");
        diag_help(d, "handle the error with `try` or `match`, or store the result in a variable");
    }
}

static bool is_chars_call(const Expr *e) {
    return e->kind == EXPR_CALL && e->as.call.arg_count == 0 && e->as.call.callee->kind == EXPR_FIELD &&
           name_is(e->as.call.callee->as.field.name, "chars");
}

static void check_for_in(Body *b, Stmt *stmt) {
    Type *declared = stmt->as.for_in.type ? sema_resolve_type(b->s, b->unit, stmt->as.for_in.type, POSITION_VALUE) : NULL;
    Expr *iterable = stmt->as.for_in.iterable;
    Type *element = error_type(b);
    if (iterable->kind == EXPR_RANGE) {
        Type *lt, *rt;
        check_pair(b, iterable->as.range.start, iterable->as.range.end, declared, &lt, &rt);
        iterable->type = lt;
        if (!is_error(lt) && !is_error(rt) && (!is_integer(lt) || lt != rt)) {
            TypeNames n = {0};
            error_expr(b, "E0352", iterable, "a range needs two integers of the same type, found `%s` and `%s`", tn(&n, lt),
                       tn(&n, rt));
            release_names(&n);
        } else {
            element = lt;
        }
    } else {
        Type *type = NULL;
        if (is_chars_call(iterable)) {
            /* `for (char c in s.chars())`: the code points of a string (spec §4.1) */
            Expr *callee = iterable->as.call.callee;
            Type *text = check_expr(b, callee->as.field.object, NULL);
            if (text->tag == T_POINTER) text = text->as.inner;
            if (text->tag == T_STR || is_error(text)) {
                callee->resolution = RESOLVED_BUILTIN;
                callee->target = "chars";
                iterable->type = text;
                element = is_error(text) ? text : builtin(b, "char");
            } else {
                type = check_expr(b, iterable, NULL); /* a `chars()` method of a struct or class */
            }
        } else {
            type = check_expr(b, iterable, NULL);
        }
        Type *base = !type ? error_type(b) : type->tag == T_POINTER ? type->as.inner : type;
        if (!type) {
            /* already handled */
        } else if (base->tag == T_ARRAY) {
            element = base->as.array.element;
        } else if (base->tag == T_SLICE) {
            element = base->as.inner;
        } else if (!is_error(base)) {
            TypeNames n = {0};
            Diagnostic *d = error_expr(b, "E0352", iterable, "a `%s` cannot be iterated", tn(&n, base));
            diag_help(d, "`for ... in` loops over a range `a..b`, an array or a slice");
            release_names(&n);
        }
    }
    if (declared && declared != element && !is_error(element)) {
        TypeNames n = {0};
        const TypeExpr *te = stmt->as.for_in.type;
        error_at(b, "E0331", te->offset, te->length, "the elements are `%s`, not `%s`", tn(&n, element), tn(&n, declared));
        release_names(&n);
    }
    size_t mark = b->locals.count;
    b->loop_depth++; /* the loop variable is new at each iteration */
    declare_local(b, stmt->as.for_in.name, declared ? declared : element, stmt->as.for_in.is_mut);
    check_block(b, stmt->as.for_in.body);
    b->loop_depth--;
    b->locals.count = mark;
}

static void check_return(Body *b, Stmt *stmt) {
    if (b->defer_depth > 0) {
        error_at(b, "E0344", stmt->offset, stmt->length, "`return` is not allowed in `defer`");
        return;
    }
    Expr *value = stmt->as.expr;
    if (!b->ret) { /* a lambda whose type is not given: its first `return` says it */
        b->ret = value ? check_expr(b, value, NULL) : void_type(b);
        if (value) {
            consume(b, value);
            check_escape(b, value, "returned");
            check_dangling(b, value);
        }
        return;
    }
    if (value) {
        if (b->ret->tag == T_VOID) {
            check_expr(b, value, NULL);
            Diagnostic *d = error_expr(b, "E0346", value, "`%.*s` returns nothing", NAME_ARGS(b->fn_name));
            diag_help(d, "remove the value, or declare the return type");
            return;
        }
        check_expect(b, value, b->ret);
        consume(b, value);
        check_escape(b, value, "returned");
        check_dangling(b, value);
    } else if (b->ret->tag != T_VOID && !is_error(b->ret)) {
        TypeNames n = {0};
        Diagnostic *d = error_at(b, "E0346", stmt->offset, stmt->length, "`%.*s` must return a `%s`",
                                 NAME_ARGS(b->fn_name), tn(&n, b->ret));
        if (b->ret->tag == T_RESULT && b->ret->as.result.value->tag == T_VOID) diag_help(d, "write `return ok();`");
        release_names(&n);
    }
}

static void check_stmt(Body *b, Stmt *stmt) {
    Type *bool_type = builtin(b, "bool");
    switch (stmt->kind) {
    case STMT_BLOCK:
        check_block(b, stmt);
        break;
    case STMT_VAR:
        check_var(b, stmt);
        break;
    case STMT_ASSIGN:
        check_assign(b, stmt);
        break;
    case STMT_EXPR:
        check_expr_stmt(b, stmt->as.expr);
        break;
    case STMT_IF:
        check_expect(b, stmt->as.if_.cond, bool_type);
        check_block(b, stmt->as.if_.then_block);
        if (stmt->as.if_.else_stmt) check_stmt(b, stmt->as.if_.else_stmt);
        break;
    case STMT_WHILE:
        check_expect(b, stmt->as.while_.cond, bool_type);
        b->loop_depth++;
        check_block(b, stmt->as.while_.body);
        b->loop_depth--;
        break;
    case STMT_FOR: {
        size_t mark = b->locals.count;
        if (stmt->as.for_.init) check_stmt(b, stmt->as.for_.init);
        if (stmt->as.for_.cond) check_expect(b, stmt->as.for_.cond, bool_type);
        if (stmt->as.for_.step) check_stmt(b, stmt->as.for_.step);
        b->loop_depth++;
        check_block(b, stmt->as.for_.body);
        b->loop_depth--;
        b->locals.count = mark;
        break;
    }
    case STMT_FOR_IN:
        check_for_in(b, stmt);
        break;
    case STMT_RETURN:
        check_return(b, stmt);
        break;
    case STMT_BREAK:
    case STMT_CONTINUE: {
        const char *word = stmt->kind == STMT_BREAK ? "break" : "continue";
        if (b->defer_depth > 0) error_at(b, "E0344", stmt->offset, stmt->length, "`%s` is not allowed in `defer`", word);
        else if (b->loop_depth == 0) error_at(b, "E0344", stmt->offset, stmt->length, "`%s` outside of a loop", word);
        break;
    }
    case STMT_DEFER:
        b->defer_depth++;
        check_stmt(b, stmt->as.inner);
        b->defer_depth--;
        break;
    case STMT_UNSAFE:
        b->unsafe_depth++;
        check_block(b, stmt->as.inner);
        b->unsafe_depth--;
        break;
    }
}

/* ---- Functions ---- */

static void init_body(Body *b, Sema *s, Unit *unit, TypeInfo *self_info, Name fn_name, Type *ret, unsigned effects) {
    memset(b, 0, sizeof *b);
    b->s = s;
    b->unit = unit;
    b->self_info = self_info;
    b->fn_name = fn_name;
    b->ret = ret;
    b->effects = effects;
}

static void check_function(Sema *s, Unit *unit, const FnDecl *fn, TypeInfo *self_info, Type *type) {
    if (!fn->body) return;
    Body b;
    bool typed = type->tag == T_FN;
    init_body(&b, s, unit, self_info, fn->name, typed ? type->as.fn.ret : s->types.error, typed ? type->as.fn.effects : 0);
    b.in_init = self_info && name_is(fn->name, "init");
    for (size_t i = 0; i < fn->param_count; i++) {
        Local param = { fn->params[i].name, typed ? type->as.fn.params[i] : s->types.error, fn->params[i].is_mut,
                        false, 0, 0, false };
        SOSU_VEC_PUSH(&b.locals, param); /* duplicates were reported with the declarations (E0319) */
    }
    for (size_t i = 0; i < fn->contract_count; i++) {
        b.in_ensures = fn->contracts[i].kind == CONTRACT_ENSURES;
        check_expect(&b, fn->contracts[i].expr, builtin(&b, "bool"));
    }
    b.in_ensures = false;
    check_block(&b, fn->body);
    if (b.ret->tag != T_VOID && !is_error(b.ret) && !always_returns(fn->body)) {
        Diagnostic *d = error_at(&b, "E0346", fn->name.offset, fn->name.length,
                                 "`%.*s` does not return a value on every path", NAME_ARGS(fn->name));
        diag_help(d, "end each path with `return ...;`");
    }
    free(b.locals.items);
}

/* ---- Instances, checked again before they are generated (spec §5.4) ---- */

void sema_check_instance_fn(Sema *s, FnInfo *fn, Type **args, const unsigned *effects) {
    const TypeEnv *saved = s->env;
    s->env = sema_fn_env(s, fn, args, effects);
    Type *type = sema_subst(s, fn->type, fn->params, args, fn->param_count, effects, fn->effect_param_count);
    check_function(s, fn->unit, fn->decl, NULL, type);
    s->env = saved;
}

void sema_check_instance_method(Sema *s, TypeInfo *instance, MethodInfo *method) {
    const TypeEnv *saved = s->env;
    s->env = sema_info_env(s, instance->generic, instance->type->type_args);
    check_function(s, instance->unit, method->decl, instance, method->type);
    s->env = saved;
}

Type *sema_check_generic_method(Sema *s, TypeInfo *owner, MethodInfo *method, Type **args, const unsigned *effects) {
    const TypeEnv *saved = s->env;
    FnInfo *generic = method->generic;
    const TypeEnv *outer = owner->generic ? sema_info_env(s, owner->generic, owner->type->type_args) : NULL;
    s->env = sema_method_env(s, outer, generic, args, effects);
    Type *type = sema_subst(s, method->type, generic->params, args, generic->param_count, effects,
                            generic->effect_param_count);
    check_function(s, owner->unit, method->decl, owner, type);
    s->env = saved;
    return type;
}

void sema_check_instance_field(Sema *s, TypeInfo *instance, const FieldInfo *field) {
    const TypeEnv *saved = s->env;
    s->env = sema_info_env(s, instance->generic, instance->type->type_args);
    Body b;
    init_body(&b, s, instance->unit, NULL, instance->name, s->types.void_, 0);
    check_expect(&b, field->decl->init, field->type);
    free(b.locals.items);
    s->env = saved;
}

/* Reports what, in the value of a constant, is not computed when the program is compiled
   (spec §11.6): calls, variables, structs... Its types are already checked. */
static void check_constant_expr(Body *b, const Expr *e) {
    switch (e->kind) {
    case EXPR_ERROR: case EXPR_INT: case EXPR_FLOAT: case EXPR_STRING: case EXPR_CHAR: case EXPR_BOOL:
        return;
    case EXPR_UNARY:
        if (e->as.unary.op != TOK_STAR && e->as.unary.op != TOK_AMP) {
            check_constant_expr(b, e->as.unary.operand);
            return;
        }
        break;
    case EXPR_BINARY:
        check_constant_expr(b, e->as.binary.left);
        check_constant_expr(b, e->as.binary.right);
        return;
    case EXPR_CAST:
        check_constant_expr(b, e->as.cast.value);
        return;
    case EXPR_TERNARY:
        check_constant_expr(b, e->as.ternary.cond);
        check_constant_expr(b, e->as.ternary.then_expr);
        check_constant_expr(b, e->as.ternary.else_expr);
        return;
    case EXPR_NAME:
    case EXPR_FIELD:
        if (e->resolution == RESOLVED_CONST || e->type == NULL || is_error(e->type)) return;
        if (e->resolution == RESOLVED_BUILTIN && e->kind == EXPR_NAME &&
            (name_is(e->as.name, "KB") || name_is(e->as.name, "MB") || name_is(e->as.name, "GB"))) {
            return;
        }
        break;
    default: break;
    }
    Diagnostic *d = error_expr(b, "E0367", e, "the value of a constant is computed when the program is compiled");
    diag_help(d, "use literals, operators, `as` and other constants; a value computed at run time is returned by "
                 "a function");
}

void sema_check_bodies(Sema *s) {
    for (size_t i = 0; i < s->consts.count; i++) {
        ConstInfo *c = s->consts.items[i];
        if (!c->type || !c->decl->as.const_.value) continue;
        Body b;
        init_body(&b, s, c->unit, NULL, c->decl->as.const_.name, s->types.void_, 0);
        check_expect(&b, c->decl->as.const_.value, c->type);
        if (c->type->tag != T_ERROR) check_constant_expr(&b, c->decl->as.const_.value);
        free(b.locals.items);
    }
    for (size_t i = 0; i < s->fns.count; i++) {
        FnInfo *fn = s->fns.items[i];
        s->env = fn->param_count || fn->effect_param_count ? sema_fn_env(s, fn, fn->params, NULL) : NULL;
        if (!fn->is_extern) check_function(s, fn->unit, fn->decl, NULL, fn->type);
        s->env = NULL;
    }
    for (size_t i = 0; i < s->type_infos.count; i++) {
        TypeInfo *info = s->type_infos.items[i];
        s->env = info->param_count ? sema_info_env(s, info, info->params) : NULL;
        for (size_t j = 0; j < info->field_count; j++) {
            const FieldInfo *field = &info->fields[j];
            if (!field->decl->init) continue;
            Body b;
            init_body(&b, s, info->unit, NULL, info->name, s->types.void_, 0);
            check_expect(&b, field->decl->init, field->type);
            free(b.locals.items);
        }
        const TypeEnv *env = s->env;
        for (size_t j = 0; j < info->method_count; j++) {
            FnInfo *generic = info->methods[j].generic;
            if (generic) s->env = sema_method_env(s, env, generic, generic->params, NULL);
            check_function(s, info->unit, info->methods[j].decl, info, info->methods[j].type);
            s->env = env;
        }
        s->env = NULL;
    }
    for (size_t i = 0; i < s->program->units.count; i++) {
        Unit *unit = s->program->units.items[i];
        for (size_t j = 0; j < unit->ast->count; j++) {
            const Decl *decl = unit->ast->decls[j];
            if (decl->kind != DECL_TEST) continue;
            Body b;
            Name name = { "test", 4, decl->offset };
            init_body(&b, s, unit, NULL, name, s->types.void_, ALL_EFFECTS);
            b.is_test = true;
            check_block(&b, decl->as.test.body);
            free(b.locals.items);
        }
    }
}
