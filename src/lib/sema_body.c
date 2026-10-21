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
} Local;

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
    VEC(Local) locals;
} Body;

static Type *check_expr(Body *b, Expr *e, Type *expected);
static void check_stmt(Body *b, Stmt *stmt);
static void check_block(Body *b, Stmt *block);

static const char *const builtin_names[] = { "some", "none", "ok", "err", "assert", "min", "max", "abs" };

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
static bool is_numeric(const Type *t) { return t->tag == T_INT || t->tag == T_FLOAT; }

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
    Local local = { name, type, is_mut };
    SOSU_VEC_PUSH(&b->locals, local);
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
} Ref;

static Ref lookup_name(Body *b, Name name, Type *expected) {
    Ref ref = {0};
    if ((ref.local = find_local(b, name))) {
        ref.kind = REF_LOCAL;
        return ref;
    }
    if (b->self_info) {
        if ((ref.field = find_field(b->self_info, name))) {
            ref.kind = REF_FIELD;
            return ref;
        }
        if ((ref.method = find_method(b->self_info, name))) {
            ref.kind = REF_METHOD;
            return ref;
        }
    }
    if (expected && expected->tag == T_ENUM && find_variant(expected->as.info, name, &ref.variant)) {
        ref.kind = REF_VARIANT;
        ref.info = expected->as.info;
        return ref;
    }
    Symbol *sym = sema_scope_find(sema_scope(b->s, b->unit), name);
    if (sym) {
        switch (sym->kind) {
        case SYM_FN: ref.kind = REF_FN; ref.fn = sym->fn; break;
        case SYM_TYPE: ref.kind = REF_TYPE; ref.type = sym->type_info->type; break;
        case SYM_MODULE: ref.kind = REF_MODULE; ref.module = sym->module; break;
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
        if (outer.type->tag == T_ENUM && find_variant(outer.type->as.info, name, &ref.variant)) {
            ref.kind = REF_VARIANT;
            ref.info = outer.type->as.info;
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
    case REF_LOCAL: e->resolution = RESOLVED_LOCAL; break;
    case REF_FIELD: e->resolution = RESOLVED_SELF_FIELD; e->target = ref.field; break;
    case REF_METHOD: e->resolution = RESOLVED_SELF_METHOD; e->target = ref.method; break;
    case REF_FN: e->resolution = RESOLVED_FN; e->target = ref.fn; break;
    case REF_BUILTIN: e->resolution = RESOLVED_BUILTIN; e->target = ref.builtin; break;
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
    case REF_LOCAL: return ref.local->type;
    case REF_FIELD: return ref.field->type;
    case REF_FN: return ref.fn->type;
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
    unsigned missing = effects & ~b->effects;
    if (!missing) return;
    unsigned bit = missing & (~missing + 1);
    Diagnostic *d = error_expr(b, "E0340", callee, "%s needs the effect `%s`", what, effect_name(bit));
    diag_help(d, "add `uses %s` to the signature of `%.*s` (spec §7.4)", effect_name(bit), NAME_ARGS(b->fn_name));
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
    for (size_t i = 0; i < count; i++) check_expect(b, e->as.call.args[i], fn->as.fn.params[i]);

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
    for (size_t i = 0; i < count; i++) check_expect(b, e->as.call.args[i], variant->fields[i]);
    return info->type;
}

static Type *call_builtin(Body *b, Expr *e, const char *name, Type *expected) {
    Expr *callee = e->as.call.callee;
    Expr **args = e->as.call.args;
    size_t count = e->as.call.arg_count;

    if (strcmp(name, "none") == 0) {
        Diagnostic *d = error_expr(b, "E0338", callee, "`none` is a value, not a function");
        diag_help(d, "write `none` without parentheses");
        check_args_loosely(b, e);
        return expected ? expected : error_type(b);
    }
    bool is_ok = strcmp(name, "ok") == 0;
    bool void_ok = is_ok && expected && expected->tag == T_RESULT && expected->as.result.value->tag == T_VOID;
    size_t wanted = strcmp(name, "min") == 0 || strcmp(name, "max") == 0 ? 2 : void_ok ? 0 : 1;
    if (count != wanted) {
        error_expr(b, "E0336", callee, "`%s` takes %zu argument%s, but %zu %s given", name, wanted, wanted == 1 ? "" : "s",
                   count, count == 1 ? "was" : "were");
        check_args_loosely(b, e);
        return expected ? expected : error_type(b);
    }

    if (strcmp(name, "some") == 0) {
        if (expected && expected->tag == T_OPTION) {
            check_expect(b, args[0], expected->as.inner);
            return expected;
        }
        Type *type = check_expr(b, args[0], NULL);
        if (type->tag == T_VOID) {
            error_expr(b, "E0331", args[0], "this expression has no value");
            return error_type(b);
        }
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
        if (count == 1) check_expect(b, args[0], is_ok ? expected->as.result.value : expected->as.result.error);
        return expected;
    }
    if (strcmp(name, "assert") == 0) {
        check_expect(b, args[0], builtin(b, "bool"));
        return void_type(b);
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
    /* min, max */
    Type *lt, *rt;
    check_pair(b, args[0], args[1], expected, &lt, &rt);
    if (!is_error(lt) && !is_error(rt) && (!is_numeric(lt) || lt != rt)) {
        TypeNames n = {0};
        error_expr(b, "E0334", e, "`%s` needs two numbers of the same type, found `%s` and `%s`", name, tn(&n, lt),
                   tn(&n, rt));
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
    if (base->tag == T_STRUCT || base->tag == T_CLASS || base->tag == T_INTERFACE) {
        TypeInfo *info = base->as.info;
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
            return call_with(b, e, method->type, name, method->decl->is_unsafe);
        }
        FieldInfo *field = find_field(info, name);
        if (field && field->type->tag == T_FN) {
            callee->type = field->type;
            return call_with(b, e, field->type, name, false);
        }
    }
    TypeNames n = {0};
    error_at(b, "E0337", name.offset, name.length, "`%s` has no method `%.*s`", tn(&n, base), NAME_ARGS(name));
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
        callee->type = ref.fn->type;
        return call_with(b, e, ref.fn->type, name, ref.fn->decl->is_unsafe || ref.fn->is_extern);
    case REF_METHOD:
        callee->type = ref.method->type;
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

static void report_operands(Body *b, Expr *e, const char *need, Type *lt, Type *rt) {
    if (is_error(lt) || is_error(rt)) return;
    TypeNames n = {0};
    Diagnostic *d = error_expr(b, "E0334", e, "`%s` needs %s, found `%s` and `%s`",
                               token_kind_spelling(e->as.binary.op), need, tn(&n, lt), tn(&n, rt));
    if (is_numeric(lt) && is_numeric(rt)) diag_help(d, "convert one side with `as` (spec §5.3)");
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
        if (op == TOK_PERCENT && (lt->tag != T_INT || lt != rt)) {
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
        if (!is_error(lt) && !is_error(rt) && (lt != rt || !(is_numeric(lt) || lt->tag == T_CHAR))) {
            report_operands(b, e, "two numbers (or characters) of the same type", lt, rt);
        }
        return bool_type;
    case TOK_AMP_AMP: case TOK_PIPE_PIPE:
        check_expect(b, left, bool_type);
        check_expect(b, right, bool_type);
        return bool_type;
    case TOK_AMP: case TOK_PIPE: case TOK_CARET:
        check_pair(b, left, right, expected, &lt, &rt);
        if (!is_error(lt) && !is_error(rt) && (lt->tag != T_INT || lt != rt)) {
            report_operands(b, e, "two integers of the same type", lt, rt);
        }
        return lt;
    case TOK_SHL: case TOK_SHR:
        lt = check_expr(b, left, expected);
        rt = check_expr(b, right, NULL);
        if (!is_error(lt) && !is_error(rt) && (lt->tag != T_INT || rt->tag != T_INT)) {
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
        if (!is_error(type) && !(type->tag == T_FLOAT || (type->tag == T_INT && type->as.integer.is_signed))) {
            error_expr(b, "E0334", e, "`-` needs a signed integer or a float, found `%s`", tn(&n, type));
        }
        break;
    case TOK_BANG:
        type = check_expect(b, operand, builtin(b, "bool"));
        break;
    case TOK_TILDE:
        type = check_expr(b, operand, expected);
        if (!is_error(type) && type->tag != T_INT) error_expr(b, "E0334", e, "`~` needs an integer, found `%s`", tn(&n, type));
        break;
    case TOK_AMP:
        type = check_expr(b, operand, NULL);
        if (!is_place(operand)) {
            error_expr(b, "E0353", e, "cannot take the address of a temporary value");
            type = error_type(b);
        } else if (!is_error(type)) {
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
    if (base->tag == T_STR && name_is(name, "bytes")) return type_slice(&b->s->types, builtin(b, "u8"));
    TypeNames n = {0};
    error_at(b, "E0337", name.offset, name.length, "`%s` has no field `%.*s`", tn(&n, base), NAME_ARGS(name));
    release_names(&n);
    return error_type(b);
}

static Type *check_index(Body *b, Expr *e) {
    Type *object = check_expr(b, e->as.index.object, NULL);
    Type *base = object->tag == T_POINTER ? object->as.inner : object;
    Type *index = check_expr(b, e->as.index.index, builtin(b, "usize"));
    TypeNames n = {0};
    if (!is_error(index) && index->tag != T_INT) {
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

static Type *check_struct_literal(Body *b, Expr *e) {
    const TypeExpr *te = e->as.struct_lit.type;
    Type *type = sema_resolve_named(b->s, b->unit, te->as.name, te->offset, te->length);
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

/* ---- Expressions ---- */

static Type *check_expr_kind(Body *b, Expr *e, Type *expected) {
    switch (e->kind) {
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
        return value_of_ref(b, e, lookup_name(b, e->as.name, expected), expected);
    case EXPR_SELF:
    case EXPR_BASE: {
        const char *word = e->kind == EXPR_SELF ? "self" : "base";
        if (!b->self_info) {
            error_expr(b, "E0349", e, "`%s` can only be used in a method", word);
            return error_type(b);
        }
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
        if (!b->in_ensures || b->ret->tag == T_VOID) {
            Diagnostic *d = error_expr(b, "E0349", e, "`result` can only be used in `ensures`");
            diag_help(d, "it is the value returned by a function that returns one");
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
        return check_struct_literal(b, e);
    case EXPR_MATCH:
        return check_match(b, e, expected, false);
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
            if (!ref.local->is_mut) {
                Diagnostic *d = error_expr(b, "E0335", target, "`%.*s` is not mutable", NAME_ARGS(target->as.name));
                diag_help(d, "declare it with `mut` to change it");
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
    Type *tt = check_expr(b, target, NULL);
    check_assignable(b, target);
    if (op == TOK_EQ) {
        check_expect(b, value, tt);
        return;
    }
    if (op == TOK_SHL_EQ || op == TOK_SHR_EQ) {
        Type *vt = check_expr(b, value, NULL);
        if (!is_error(tt) && !is_error(vt) && (tt->tag != T_INT || vt->tag != T_INT)) {
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
    bool ok = tt == vt && (bitwise ? tt->tag == T_INT : is_numeric(tt));
    if (!ok) {
        TypeNames n = {0};
        error_expr(b, "E0334", value, "`%s` needs %s, found `%s` and `%s`", token_kind_spelling(op),
                   bitwise ? "two integers of the same type" : "two numbers of the same type", tn(&n, tt), tn(&n, vt));
        release_names(&n);
    }
}

/* ---- Statements ---- */

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
    case STMT_WHILE:
        return stmt->as.while_.cond->kind == EXPR_BOOL && stmt->as.while_.cond->as.bool_value;
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
    declare_local(b, stmt->as.var.name, type, stmt->as.var.is_mut);
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

static void check_for_in(Body *b, Stmt *stmt) {
    Type *declared = stmt->as.for_in.type ? sema_resolve_type(b->s, b->unit, stmt->as.for_in.type, POSITION_VALUE) : NULL;
    Expr *iterable = stmt->as.for_in.iterable;
    Type *element = error_type(b);
    if (iterable->kind == EXPR_RANGE) {
        Type *lt, *rt;
        check_pair(b, iterable->as.range.start, iterable->as.range.end, declared, &lt, &rt);
        iterable->type = lt;
        if (!is_error(lt) && !is_error(rt) && (lt->tag != T_INT || lt != rt)) {
            TypeNames n = {0};
            error_expr(b, "E0352", iterable, "a range needs two integers of the same type, found `%s` and `%s`", tn(&n, lt),
                       tn(&n, rt));
            release_names(&n);
        } else {
            element = lt;
        }
    } else {
        Type *type = check_expr(b, iterable, NULL);
        Type *base = type->tag == T_POINTER ? type->as.inner : type;
        if (base->tag == T_ARRAY) {
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
    declare_local(b, stmt->as.for_in.name, declared ? declared : element, stmt->as.for_in.is_mut);
    b->loop_depth++;
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
    if (value) {
        if (b->ret->tag == T_VOID) {
            check_expr(b, value, NULL);
            Diagnostic *d = error_expr(b, "E0346", value, "`%.*s` returns nothing", NAME_ARGS(b->fn_name));
            diag_help(d, "remove the value, or declare the return type");
            return;
        }
        check_expect(b, value, b->ret);
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
        Local param = { fn->params[i].name, typed ? type->as.fn.params[i] : s->types.error, fn->params[i].is_mut };
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

void sema_check_bodies(Sema *s) {
    for (size_t i = 0; i < s->fns.count; i++) {
        FnInfo *fn = s->fns.items[i];
        if (!fn->is_extern) check_function(s, fn->unit, fn->decl, NULL, fn->type);
    }
    for (size_t i = 0; i < s->type_infos.count; i++) {
        TypeInfo *info = s->type_infos.items[i];
        for (size_t j = 0; j < info->field_count; j++) {
            const FieldInfo *field = &info->fields[j];
            if (!field->decl->init) continue;
            Body b;
            init_body(&b, s, info->unit, NULL, info->name, s->types.void_, 0);
            check_expect(&b, field->decl->init, field->type);
            free(b.locals.items);
        }
        for (size_t j = 0; j < info->method_count; j++) {
            check_function(s, info->unit, info->methods[j].decl, info, info->methods[j].type);
        }
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
