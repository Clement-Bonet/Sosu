/* Checks of calls (spec §5.3): functions, methods, built-in names, generics (spec §5.4) and
   arenas (spec §6). Part of the checks of function bodies (sema_body.h). */

#include "sema_body.h"

#include <stdlib.h>
#include <string.h>

/* ---- Calls ---- */

static void report_missing_effects(Body *b, const Expr *callee, unsigned effects, const char *what) {
    if (b->lambda && b->lambda->inferring) { /* a lambda has the effects of its body */
        b->lambda->used |= effects;
        return;
    }
    unsigned missing = effects & ~b->effects;
    if (!missing) return;
    unsigned bit = missing & (~missing + 1);
    Diagnostic *d = sema_error_expr(b, "E0340", callee, "%s needs the effect `%s`", what, effect_name(bit));
    if (b->lambda) diag_help(d, "the type of this lambda does not allow it: add `uses %s` to its type", effect_name(bit));
    else diag_help(d, "add `uses %s` to the signature of `%.*s` (spec §7.4)", effect_name(bit), NAME_ARGS(b->fn_name));
}

static void check_args_loosely(Body *b, Expr *e) {
    for (size_t i = 0; i < e->as.call.arg_count; i++) sema_check_expr(b, e->as.call.args[i], NULL);
}

static Type *call_with(Body *b, Expr *e, Type *fn, Name name, bool is_unsafe) {
    Expr *callee = e->as.call.callee;
    if (sema_is_error(fn)) {
        check_args_loosely(b, e);
        return fn;
    }
    size_t count = e->as.call.arg_count;
    if (count != fn->as.fn.param_count) {
        Diagnostic *d = sema_error_expr(b, "E0336", callee, "`%.*s` takes %zu argument%s, but %zu %s given", NAME_ARGS(name),
                                   fn->as.fn.param_count, fn->as.fn.param_count == 1 ? "" : "s", count,
                                   count == 1 ? "was" : "were");
        char *signature = type_to_string(fn);
        diag_help(d, "its signature is `%s`", signature);
        free(signature);
        check_args_loosely(b, e);
        return fn->as.fn.ret;
    }
    for (size_t i = 0; i < count; i++) {
        sema_check_expect(b, e->as.call.args[i], fn->as.fn.params[i]);
        sema_consume(b, e->as.call.args[i]);
    }

    char what[96];
    snprintf(what, sizeof what, "calling `%.*s`", NAME_ARGS(name));
    report_missing_effects(b, callee, fn->as.fn.effects, what);
    if (is_unsafe && b->unsafe_depth == 0) {
        Diagnostic *d = sema_error_expr(b, "E0341", callee, "`%.*s` can only be called in an `unsafe` block", NAME_ARGS(name));
        diag_help(d, "unsafe functions and C functions are called in `unsafe { ... }` (spec §7.7)");
    }
    return fn->as.fn.ret;
}

static Type *call_variant(Body *b, Expr *e, const TypeInfo *info, size_t index) {
    const VariantInfo *variant = &info->variants[index];
    size_t count = e->as.call.arg_count;
    if (!variant->decl->has_parens || count != variant->field_count) {
        sema_error_expr(b, "E0336", e->as.call.callee, "the variant `%.*s` has %zu field%s, but %zu %s given",
                   NAME_ARGS(variant->name), variant->field_count, variant->field_count == 1 ? "" : "s", count,
                   count == 1 ? "was" : "were");
        check_args_loosely(b, e);
        return info->type;
    }
    for (size_t i = 0; i < count; i++) {
        sema_check_expect(b, e->as.call.args[i], variant->fields[i]);
        sema_consume(b, e->as.call.args[i]);
        sema_check_escape(b, e->as.call.args[i], "stored in an enum");
    }
    return info->type;
}

/* A public type of core.prelude, e.g. `Utf8Error`. */
static Type *prelude_type(Body *b, const char *name, const Expr *at) {
    Name wanted = { name, strlen(name), 0 };
    Symbol *sym = b->s->prelude ? sema_scope_find(sema_scope(b->s, b->s->prelude), wanted) : NULL;
    if (sym && sym->kind == SYM_TYPE) return sym->type_info->type;
    Diagnostic *d = sema_error_expr(b, "E0414", at, "`%s` is missing: the standard library was not found", name);
    diag_help(d, "it is declared in `core/src/prelude.sosu`; set SOSU_HOME to the folder of the Sosu installation");
    return sema_error_type(b);
}

static Type *call_builtin(Body *b, Expr *e, const char *name, Type *expected) {
    Expr *callee = e->as.call.callee;
    Expr **args = e->as.call.args;
    size_t count = e->as.call.arg_count;

    if (strcmp(name, "none") == 0 || name[1] == 'B') { /* none, KB, MB, GB */
        Diagnostic *d = sema_error_expr(b, "E0338", callee, "`%s` is a value, not a function", name);
        diag_help(d, "write `%s` without parentheses", name);
        check_args_loosely(b, e);
        return strcmp(name, "none") == 0 && expected ? expected : sema_error_type(b);
    }
    if (strcmp(name, "str.from_bytes") == 0) {
        /* str.from_bytes(ui8[]) -> str!Utf8Error (spec §4.1) */
        if (count != 1) {
            sema_error_expr(b, "E0336", callee, "`str.from_bytes` takes the bytes to check, e.g. `str.from_bytes(data)`");
            check_args_loosely(b, e);
        } else {
            sema_check_expect(b, args[0], type_slice(&b->s->types, sema_builtin(b, "ui8")));
        }
        Type *error = prelude_type(b, "Utf8Error", callee);
        return sema_is_error(error) ? error : type_result(&b->s->types, sema_builtin(b, "str"), error);
    }
    if (strcmp(name, "Arena.new") == 0) {
        if (count != 1) {
            sema_error_expr(b, "E0336", callee, "`Arena.new` takes the size of its blocks, e.g. `Arena.new(1 * MB)`");
            check_args_loosely(b, e);
        } else {
            sema_check_expect(b, args[0], sema_builtin(b, "usize"));
        }
        return sema_builtin(b, "Arena");
    }
    if (strcmp(name, "size_of") == 0) { /* size_of(T): its size in bytes, as C counts it (spec §5.6) */
        Type *type = count == 1 ? sema_type_argument(b, args[0]) : NULL;
        if (!type) {
            Diagnostic *d = sema_error_expr(b, "E0336", callee, "`size_of` takes a type");
            diag_help(d, "e.g. `size_of(Point)`, `size_of(T)`");
            check_args_loosely(b, e);
        } else {
            args[0]->type = type;
        }
        return sema_builtin(b, "usize");
    }
    bool is_ok = strcmp(name, "ok") == 0;
    bool void_ok = is_ok && expected && expected->tag == T_RESULT && expected->as.result.value->tag == T_VOID;
    bool two = strcmp(name, "min") == 0 || strcmp(name, "max") == 0 || strncmp(name, "wrapping_", 9) == 0;
    size_t wanted = two ? 2 : void_ok ? 0 : 1;
    if (count != wanted) {
        sema_error_expr(b, "E0336", callee, "`%s` takes %zu argument%s, but %zu %s given", name, wanted, wanted == 1 ? "" : "s",
                   count, count == 1 ? "was" : "were");
        check_args_loosely(b, e);
        return expected ? expected : sema_error_type(b);
    }

    if (strcmp(name, "some") == 0) {
        if (expected && expected->tag == T_OPTION) {
            sema_check_expect(b, args[0], expected->as.inner);
            sema_consume(b, args[0]);
            sema_check_escape(b, args[0], "stored in an option");
            return expected;
        }
        Type *type = sema_check_expr(b, args[0], NULL);
        if (type->tag == T_VOID) {
            sema_error_expr(b, "E0331", args[0], "this expression has no value");
            return sema_error_type(b);
        }
        sema_check_escape(b, args[0], "stored in an option");
        return type_option(&b->s->types, type);
    }
    if (is_ok || strcmp(name, "err") == 0) {
        if (!expected || expected->tag != T_RESULT) {
            if (!expected || !sema_is_error(expected)) {
                Diagnostic *d = sema_error_expr(b, "E0342", e, "the type of `%s(...)` is unknown here", name);
                diag_help(d, "declare the expected type, e.g. `i32!MyError r = %s(...);`", name);
            }
            check_args_loosely(b, e);
            return sema_error_type(b);
        }
        if (count == 1) {
            sema_check_expect(b, args[0], is_ok ? expected->as.result.value : expected->as.result.error);
            sema_consume(b, args[0]);
            sema_check_escape(b, args[0], "stored in a result");
        }
        return expected;
    }
    if (strcmp(name, "assert") == 0) {
        sema_check_expect(b, args[0], sema_builtin(b, "bool"));
        return sema_void_type(b);
    }
    if (strcmp(name, "hash") == 0) {
        Type *type = sema_check_expr(b, args[0], NULL);
        if (!sema_is_error(type) && !(sema_constraints_of(type) & CONSTRAINT_HASH)) {
            TypeNames n = {0};
            Diagnostic *d = sema_error_expr(b, "E0334", args[0], "`hash` needs a number, a `bool`, a `char`, a `str`, a pointer "
                                                             "or an enum without data, found `%s`", sema_tn(&n, type));
            if (type->tag == T_PARAM) diag_help(d, "add the constraint `Hash`: `<%.*s: Hash>`", NAME_ARGS(type->name));
            sema_release_names(&n);
        }
        return sema_builtin(b, "ui64");
    }
    if (strcmp(name, "abs") == 0) {
        Type *type = sema_check_expr(b, args[0], expected);
        bool ok = type->tag == T_FLOAT || (type->tag == T_INT && type->as.integer.is_signed) || sema_is_error(type);
        if (!ok) {
            TypeNames n = {0};
            sema_error_expr(b, "E0334", args[0], "`abs` needs a signed integer or a float, found `%s`", sema_tn(&n, type));
            sema_release_names(&n);
        }
        return type;
    }
    /* min, max, and wrapping_add, wrapping_sub, wrapping_mul (modulo 2^n, spec §5.1) */
    bool wrapping = strncmp(name, "wrapping_", 9) == 0;
    Type *lt, *rt;
    sema_check_pair(b, args[0], args[1], expected, &lt, &rt);
    bool valid = wrapping ? lt->tag == T_INT : sema_is_numeric(lt);
    if (!sema_is_error(lt) && !sema_is_error(rt) && (!valid || lt != rt)) {
        TypeNames n = {0};
        sema_error_expr(b, "E0334", e, "`%s` needs two %s of the same type, found `%s` and `%s`", name,
                   wrapping ? "integers" : "numbers", sema_tn(&n, lt), sema_tn(&n, rt));
        sema_release_names(&n);
    }
    return lt;
}

static bool is_printable(const Type *type) {
    return sema_is_numeric(type) || type->tag == T_BOOL || type->tag == T_CHAR || type->tag == T_STR || sema_is_error(type);
}

/* `io.print("format {}", values...)`, built into the compiler (spec §5.3). */
static Type *call_print(Body *b, Expr *e) {
    Expr **args = e->as.call.args;
    size_t count = e->as.call.arg_count;
    report_missing_effects(b, e->as.call.callee, EFFECT_IO, "`io.print`");
    if (count == 0 || args[0]->kind != EXPR_STRING) {
        Diagnostic *d = sema_error_expr(b, "E0351", count ? args[0] : e, "the format of `io.print` must be a string literal");
        diag_help(d, "write `io.print(\"x = {}\", x)`");
        check_args_loosely(b, e);
        return sema_void_type(b);
    }
    args[0]->type = sema_builtin(b, "str");
    const char *text = b->unit->file.text + args[0]->offset;
    size_t placeholders = 0;
    for (size_t i = 0; i + 1 < args[0]->length; i++) {
        if (text[i] == '{' && text[i + 1] == '}') placeholders++;
    }
    if (placeholders != count - 1) {
        sema_error_expr(b, "E0351", args[0], "the format has %zu `{}` but %zu value%s follow%s", placeholders, count - 1,
                   count - 1 == 1 ? "" : "s", count - 1 == 1 ? "s" : "");
    }
    for (size_t i = 1; i < count; i++) {
        Type *type = sema_check_expr(b, args[i], NULL);
        if (!is_printable(type)) {
            TypeNames n = {0};
            Diagnostic *d = sema_error_expr(b, "E0351", args[i], "a `%s` cannot be printed", sema_tn(&n, type));
            diag_help(d, "print its parts: numbers, `bool`, `char` and `str` can be printed");
            sema_release_names(&n);
        }
    }
    return sema_void_type(b);
}

static bool is_print(const Expr *callee, Ref object) {
    return callee->kind == EXPR_FIELD && object.kind == REF_MODULE && strcmp(object.module->module, "std.io") == 0 &&
           sema_name_is(callee->as.field.name, "print");
}

/* ---- Generics (spec §5.4) ---- */

bool sema_is_nominal(const Type *t) {
    return t->tag == T_STRUCT || t->tag == T_CLASS || t->tag == T_INTERFACE || t->tag == T_ENUM;
}

/* The generic a nominal type comes from (itself when it is not an instance), and its arguments
   (its own parameters for the generic itself). */
TypeInfo *sema_generic_of(const Type *t) { return t->as.info->generic ? t->as.info->generic : t->as.info; }

static Type **args_of(const Type *t) { return t->as.info->generic ? t->type_args : t->as.info->params; }

/* Binds the parameters of `fn` held by `pattern` so that it becomes `actual`. False if they
   cannot match. A parameter of the function being checked (not of `fn`) is a type as any other. */
static bool unify(FnInfo *fn, Type *pattern, Type *actual, Type **bound, unsigned *effects) {
    if (sema_is_error(pattern) || sema_is_error(actual)) return true;
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
        TypeInfo *generic = sema_generic_of(pattern);
        if (generic != sema_generic_of(actual)) return false;
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
bool sema_all_bound(FnInfo *fn, const Type *type, Type **bound, const unsigned *effects) {
    switch (type->tag) {
    case T_PARAM:
        for (size_t i = 0; i < fn->param_count; i++) {
            if (fn->params[i] == type) return bound[i] != NULL;
        }
        return true;
    case T_POINTER: case T_OPTION: case T_SLICE: return sema_all_bound(fn, type->as.inner, bound, effects);
    case T_ARRAY: return sema_all_bound(fn, type->as.array.element, bound, effects);
    case T_RESULT:
        return sema_all_bound(fn, type->as.result.value, bound, effects) && sema_all_bound(fn, type->as.result.error, bound, effects);
    case T_FN:
        for (size_t k = 0; k < fn->effect_param_count; k++) {
            if ((type->as.fn.effects & (EFFECT_PARAM << k)) && effects[k] == EFFECT_UNBOUND) return false;
        }
        for (size_t i = 0; i < type->as.fn.param_count; i++) {
            if (!sema_all_bound(fn, type->as.fn.params[i], bound, effects)) return false;
        }
        return sema_all_bound(fn, type->as.fn.ret, bound, effects);
    case T_STRUCT: case T_CLASS: case T_INTERFACE: case T_ENUM: {
        TypeInfo *generic = sema_generic_of(type);
        Type **args = args_of(type);
        for (size_t i = 0; i < generic->param_count; i++) {
            if (!sema_all_bound(fn, args[i], bound, effects)) return false;
        }
        return true;
    }
    default:
        return true;
    }
}


/* An argument whose type comes from the type expected for it: checked once the parameters
   it depends on are known. */
static bool is_contextual(const Expr *e) {
    if (sema_is_untyped_literal(e) || e->kind == EXPR_ARRAY) return true;
    if (e->kind == EXPR_NAME) return sema_name_is(e->as.name, "none");
    if (e->kind == EXPR_CALL && e->as.call.callee->kind == EXPR_NAME) {
        Name name = e->as.call.callee->as.name;
        return sema_name_is(name, "ok") || sema_name_is(name, "err");
    }
    return e->kind == EXPR_STRUCT_LIT && e->as.struct_lit.type->arg_count == 0;
}

Type *sema_bind(Body *b, FnInfo *fn, Type *type, Type **bound, const unsigned *effects) {
    return sema_subst(b->s, type, fn->params, bound, fn->param_count, effects, fn->effect_param_count);
}

/* A call of a generic function: its type arguments are written, or inferred from its
   arguments, then from the type expected for its result (spec §5.4). */
static Type *call_generic(Body *b, Expr *e, FnInfo *fn, Name name, Type *expected) {
    Expr *callee = e->as.call.callee;
    Type *type = fn->type;
    if (sema_is_error(type)) {
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
            Diagnostic *d = sema_error_expr(b, "E0361", callee, "`%.*s` takes %zu type argument%s, but %zu %s given",
                                       NAME_ARGS(name), n, n == 1 ? "" : "s", callee->type_arg_count,
                                       callee->type_arg_count == 1 ? "was" : "were");
            diag_help(d, "give a type for each parameter, or leave them all out to infer them");
            check_args_loosely(b, e);
            return sema_error_type(b);
        }
        for (size_t k = 0; k < n; k++) bound[k] = sema_resolve_type(b->s, b->unit, callee->type_args[k], POSITION_VALUE);
    }
    size_t count = e->as.call.arg_count;
    if (count != type->as.fn.param_count) {
        Diagnostic *d = sema_error_expr(b, "E0336", callee, "`%.*s` takes %zu argument%s, but %zu %s given", NAME_ARGS(name),
                                   type->as.fn.param_count, type->as.fn.param_count == 1 ? "" : "s", count,
                                   count == 1 ? "was" : "were");
        char *signature = type_to_string(type);
        diag_help(d, "its signature is `%s`", signature);
        free(signature);
        check_args_loosely(b, e);
        return sema_error_type(b);
    }
    bool *checked = calloc(count + 1, sizeof *checked);
    for (int pass = 0; pass < 2; pass++) {
        for (size_t i = 0; i < count; i++) {
            if (checked[i]) continue;
            Expr *arg = e->as.call.args[i];
            Type *param = type->as.fn.params[i];
            if (sema_all_bound(fn, param, bound, effects)) {
                sema_check_expect(b, arg, sema_bind(b, fn, param, bound, effects));
                checked[i] = true;
                continue;
            }
            if (pass == 0 && (is_contextual(arg) || arg->kind == EXPR_LAMBDA)) continue;
            Type *found;
            if (arg->kind == EXPR_LAMBDA) { /* it takes the types known, and gives the others */
                found = arg->type = sema_check_lambda(b, arg, param, fn, bound, effects);
            } else {
                found = sema_check_expr(b, arg, NULL);
            }
            checked[i] = true;
            if (!unify(fn, param, found, bound, effects)) sema_report_mismatch(b, arg, sema_bind(b, fn, param, bound, effects), found);
        }
        if (pass == 0 && expected && !sema_is_error(expected)) unify(fn, type->as.fn.ret, expected, bound, effects);
    }
    free(checked);
    bool ok = true;
    for (size_t k = 0; k < n; k++) {
        if (bound[k]) continue;
        Diagnostic *d = sema_error_expr(b, "E0360", callee, "the type `%.*s` of `%.*s` cannot be inferred here",
                                   NAME_ARGS(fn->params[k]->name), NAME_ARGS(name));
        diag_help(d, "write the type arguments: `%.*s<...>(...)` (spec §5.4)", NAME_ARGS(name));
        bound[k] = sema_error_type(b);
        ok = false;
    }
    for (size_t k = 0; k < m; k++) {
        if (effects[k] == EFFECT_UNBOUND) effects[k] = 0;
    }
    for (size_t k = 0; ok && k < n; k++) {
        ok &= sema_check_type_arg(b->s, b->unit, callee->offset, callee->length, bound[k], fn->params[k], name);
    }
    for (size_t i = 0; i < count; i++) sema_consume(b, e->as.call.args[i]);
    Type *instance = sema_bind(b, fn, type, bound, effects);
    callee->type = instance;
    callee->type_values = bound;
    callee->type_value_count = n;
    callee->effect_values = effects;
    callee->effect_value_count = m;
    if (!ok) return sema_error_type(b);
    char what[96];
    snprintf(what, sizeof what, "calling `%.*s`", NAME_ARGS(name));
    report_missing_effects(b, callee, instance->as.fn.effects, what);
    if (fn->decl->is_unsafe && b->unsafe_depth == 0) {
        Diagnostic *d = sema_error_expr(b, "E0341", callee, "`%.*s` can only be called in an `unsafe` block", NAME_ARGS(name));
        diag_help(d, "unsafe functions are called in `unsafe { ... }` (spec §7.7)");
    }
    return instance->as.fn.ret;
}

/* A type named in an expression (`Tree<i32>.Leaf(1)`, `arena.new(Vec<i32>, ...)`): a
   generic needs its arguments, written after it or given by the type expected. */
Type *sema_named_instance(Body *b, const Expr *named, Type *type, Type *expected) {
    TypeInfo *info = sema_is_nominal(type) ? type->as.info : NULL;
    bool generic = info && info->param_count > 0 && !info->generic;
    if (!generic) {
        if (named->type_arg_count && !sema_is_error(type)) {
            TypeNames n = {0};
            sema_error_expr(b, "E0361", named, "`%s` is not generic: it takes no type arguments", sema_tn(&n, type));
            sema_release_names(&n);
            return sema_error_type(b);
        }
        return type;
    }
    if (named->type_arg_count == 0) {
        if (expected && sema_is_nominal(expected) && sema_generic_of(expected) == info) return expected;
        TypeNames n = {0};
        Diagnostic *d = sema_error_expr(b, "E0361", named, "`%.*s` is generic: give its type arguments", NAME_ARGS(info->name));
        diag_help(d, "write `%s`, with a type for each parameter", sema_tn(&n, info->type));
        sema_release_names(&n);
        return sema_error_type(b);
    }
    if (named->type_arg_count != info->param_count) {
        sema_error_expr(b, "E0361", named, "`%.*s` takes %zu type argument%s, but %zu %s given", NAME_ARGS(info->name),
                   info->param_count, info->param_count == 1 ? "" : "s", named->type_arg_count,
                   named->type_arg_count == 1 ? "was" : "were");
        return sema_error_type(b);
    }
    Type **args = arena_alloc(&b->s->arena, info->param_count * sizeof *args);
    bool ok = true;
    for (size_t i = 0; i < info->param_count; i++) {
        const TypeExpr *te = named->type_args[i];
        args[i] = sema_resolve_type(b->s, b->unit, te, POSITION_VALUE);
        ok &= sema_check_type_arg(b->s, b->unit, te->offset, te->length, args[i], info->params[i], info->name);
    }
    return ok ? sema_instance(b->s, info, args)->type : sema_error_type(b);
}

/* ---- Arenas (spec §6) ---- */

/* A type given as an argument: `arena.alloc(Point)`, `arena.new(shapes.Circle, ...)`.
   NULL if the expression is not a type name. */
Type *sema_type_argument(Body *b, Expr *arg) {
    if (arg->kind == EXPR_TYPE) return sema_resolve_type(b->s, b->unit, arg->as.type_expr, POSITION_VALUE);
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
    return sema_named_instance(b, arg, sema_resolve_named(b->s, b->unit, path, arg->offset, arg->length), NULL);
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

    if (sema_name_is(name, "free")) {
        callee->target = "arena.free";
        if (count) sema_error_expr(b, "E0336", callee, "`free` takes no argument");
        check_args_loosely(b, e);
        return sema_void_type(b);
    }
    bool is_alloc = sema_name_is(name, "alloc");
    bool is_try = sema_name_is(name, "try_alloc");
    bool is_new = sema_name_is(name, "new");
    bool is_slice = sema_name_is(name, "alloc_slice");
    if (!is_alloc && !is_try && !is_new && !is_slice) {
        Diagnostic *d = sema_error_at(b, "E0337", name.offset, name.length, "`Arena` has no method `%.*s`", NAME_ARGS(name));
        diag_help(d, "the methods of an arena are `alloc`, `alloc_slice`, `try_alloc`, `new` and `free` (spec §6)");
        check_args_loosely(b, e);
        return sema_error_type(b);
    }
    callee->target = is_alloc ? "arena.alloc" : is_try ? "arena.try_alloc" : is_new ? "arena.new" : "arena.alloc_slice";
    Type *type = count ? sema_type_argument(b, args[0]) : NULL;
    if (!type) {
        Diagnostic *d = sema_error_expr(b, "E0354", count ? args[0] : e, "`%.*s` takes the type to allocate first",
                                   NAME_ARGS(name));
        diag_help(d, "e.g. `arena.%.*s(Point)`", NAME_ARGS(name));
        for (size_t i = count ? 1 : 0; i < count; i++) sema_check_expr(b, args[i], NULL);
        return sema_error_type(b);
    }
    args[0]->type = type;
    if (sema_is_error(type)) {
        for (size_t i = 1; i < count; i++) sema_check_expr(b, args[i], NULL);
        return sema_error_type(b);
    }
    Type *pointer = type_pointer(&b->s->types, type);

    if (is_new) {
        if (type->tag != T_CLASS && type->tag != T_STRUCT) {
            sema_error_expr(b, "E0354", args[0], "`arena.new` creates a class or a struct, not a `%s`", sema_tn(&n, type));
            sema_release_names(&n);
            for (size_t i = 1; i < count; i++) sema_check_expr(b, args[i], NULL);
            return sema_error_type(b);
        }
        Name init_name = { "init", 4, 0 };
        MethodInfo *init = sema_find_method(type->as.info, init_name);
        if (init) {
            Type *fn = init->type;
            if (count - 1 != fn->as.fn.param_count) {
                sema_error_expr(b, "E0336", callee, "`%.*s.init` takes %zu argument%s, but %zu %s given",
                           NAME_ARGS(type->name), fn->as.fn.param_count, fn->as.fn.param_count == 1 ? "" : "s",
                           count - 1, count - 1 == 1 ? "was" : "were");
                for (size_t i = 1; i < count; i++) sema_check_expr(b, args[i], NULL);
            } else {
                for (size_t i = 1; i < count; i++) {
                    sema_check_expect(b, args[i], fn->as.fn.params[i - 1]);
                    sema_consume(b, args[i]);
                }
            }
            report_missing_effects(b, callee, fn->as.fn.effects, "`init`");
        } else if (count > 1) {
            sema_error_expr(b, "E0336", callee, "`%.*s` has no `init` method: `arena.new` takes no other argument",
                       NAME_ARGS(type->name));
            for (size_t i = 1; i < count; i++) sema_check_expr(b, args[i], NULL);
        } else if (!zeroable(type)) {
            Diagnostic *d = sema_error_expr(b, "E0354", args[0], "`%.*s` has no `init` method, and zeroed memory is not a valid `%.*s`",
                                       NAME_ARGS(type->name), NAME_ARGS(type->name));
            diag_help(d, "it contains a pointer, which is never null: add an `init` method that gives it a value");
        }
        return pointer;
    }

    if (is_slice) {
        if (count != 2) {
            sema_error_expr(b, "E0336", callee, "`alloc_slice` takes the type and the number of elements, e.g. "
                                           "`arena.alloc_slice(ui8, 64)`");
            for (size_t i = 1; i < count; i++) sema_check_expr(b, args[i], NULL);
        } else {
            sema_check_expect(b, args[1], sema_builtin(b, "usize"));
        }
    } else if (count != 1) {
        sema_error_expr(b, "E0336", callee, "`%.*s` takes only the type to allocate", NAME_ARGS(name));
        for (size_t i = 1; i < count; i++) sema_check_expr(b, args[i], NULL);
    }
    if (type->tag == T_CLASS || type->tag == T_INTERFACE) {
        Diagnostic *d = sema_error_expr(b, "E0354", args[0], "a class is created with `arena.new`, which calls its `init`");
        diag_help(d, "write `arena.new(%.*s, ...)`", NAME_ARGS(type->name));
    } else if (!zeroable(type) && b->unsafe_depth == 0) {
        Diagnostic *d = sema_error_expr(b, "E0354", args[0], "`%.*s` gives zeroed memory, which is not a valid `%s`",
                                   NAME_ARGS(name), sema_tn(&n, type));
        diag_help(d, "it contains a pointer, which is never null: use a struct with `init` and `arena.new`, or `T*?`; in "
                     "`unsafe`, the memory may be given before it holds valid values (spec §5.4)");
    }
    sema_release_names(&n);
    if (is_slice) return type_slice(&b->s->types, type);
    return is_try ? type_option(&b->s->types, pointer) : pointer;
}

static Type *call_method(Body *b, Expr *e, Type *expected) {
    Expr *callee = e->as.call.callee;
    Name name = callee->as.field.name;
    (void)expected;
    Type *object = sema_check_expr(b, callee->as.field.object, NULL);
    Type *base = object->tag == T_POINTER ? object->as.inner : object;
    if (sema_is_error(base)) {
        check_args_loosely(b, e);
        return sema_error_type(b);
    }
    bool through_bound = base->tag == T_PARAM && base->as.param.bound; /* `<T: Drawable>`: `t.draw(r)` */
    if (base->tag == T_STRUCT || base->tag == T_CLASS || base->tag == T_INTERFACE || through_bound) {
        TypeInfo *info = through_bound ? base->as.param.bound : base->as.info;
        MethodInfo *method = sema_find_method(info, name);
        if (method) {
            if (method->owner->unit != b->unit && !method->decl->is_pub && method->owner->type->tag != T_INTERFACE) {
                Diagnostic *d = sema_error_at(b, "E0305", name.offset, name.length, "the method `%.*s` of `%.*s` is private",
                                         NAME_ARGS(name), NAME_ARGS(method->owner->name));
                diag_help(d, "declare it `pub` in `%.*s`", NAME_ARGS(method->owner->name));
            }
            callee->type = method->type;
            callee->resolution = RESOLVED_METHOD;
            callee->target = method;
            if (method->generic) return call_generic(b, e, method->generic, name, expected);
            return call_with(b, e, method->type, name, method->decl->is_unsafe);
        }
        FieldInfo *field = sema_find_field(info, name);
        if (field && field->type->tag == T_FN) {
            callee->type = field->type;
            return call_with(b, e, field->type, name, false);
        }
    }
    if (sema_name_is(name, "copy") && type_owns_resource(base)) {
        /* x.copy(): its arenas are new and empty (same block size); the rest is copied (spec §6) */
        callee->resolution = RESOLVED_BUILTIN;
        callee->target = "copy";
        if (e->as.call.arg_count) sema_error_expr(b, "E0336", callee, "`copy` takes no argument");
        check_args_loosely(b, e);
        return base;
    }
    if (base->tag == T_ARENA) return call_arena(b, e, name);
    if (base->tag == T_STR && sema_name_is(name, "to_cstr")) {
        /* s.to_cstr(arena): a NUL-terminated copy for C (spec §4.1) */
        callee->resolution = RESOLVED_BUILTIN;
        callee->target = "str.to_cstr";
        Type *arena = e->as.call.arg_count == 1 ? sema_check_expr(b, e->as.call.args[0], NULL) : NULL;
        Type *arena_base = arena && arena->tag == T_POINTER ? arena->as.inner : arena;
        if (!arena_base || (arena_base->tag != T_ARENA && !sema_is_error(arena_base))) {
            Diagnostic *d = sema_error_expr(b, "E0336", e, "`to_cstr` takes the arena that holds the copy");
            diag_help(d, "write `text.to_cstr(arena)` (spec §4.1)");
            if (!arena) check_args_loosely(b, e);
        }
        return type_pointer(&b->s->types, sema_builtin(b, "ui8"));
    }
    if (base->tag == T_STR && sema_name_is(name, "chars")) {
        Diagnostic *d = sema_error_expr(b, "E0352", e, "`chars()` can only be used in a `for` loop");
        diag_help(d, "write `for (char c in s.chars()) { ... }` (spec §4.1)");
        check_args_loosely(b, e);
        return sema_error_type(b);
    }
    TypeNames n = {0};
    Diagnostic *d = sema_error_at(b, "E0337", name.offset, name.length, "`%s` has no method `%.*s`", sema_tn(&n, base), NAME_ARGS(name));
    if (sema_name_is(name, "copy")) diag_help(d, "a value without an `Arena` is copied by value: use it directly (spec §6)");
    sema_release_names(&n);
    check_args_loosely(b, e);
    return sema_error_type(b);
}

Type *sema_check_call(Body *b, Expr *e, Type *expected) {
    Expr *callee = e->as.call.callee;
    if (callee->kind == EXPR_FIELD && (callee->as.field.object->kind == EXPR_NAME ||
                                       callee->as.field.object->kind == EXPR_FIELD)) {
        Ref object = sema_resolve_ref(b, callee->as.field.object, NULL);
        if (is_print(callee, object)) {
            callee->resolution = RESOLVED_PRINT;
            return call_print(b, e);
        }
    }
    if (callee->kind != EXPR_NAME && callee->kind != EXPR_FIELD) {
        Type *type = sema_check_expr(b, callee, NULL);
        if (type->tag != T_FN && !sema_is_error(type)) {
            TypeNames n = {0};
            sema_error_expr(b, "E0338", callee, "a `%s` cannot be called", sema_tn(&n, type));
            sema_release_names(&n);
            check_args_loosely(b, e);
            return sema_error_type(b);
        }
        Name name = { "function", 8, callee->offset };
        return call_with(b, e, type, name, false);
    }

    Ref ref = sema_resolve_ref(b, callee, expected);
    sema_record(callee, ref);
    Name name = callee->kind == EXPR_NAME ? callee->as.name : callee->as.field.name;
    switch (ref.kind) {
    case REF_VALUE:
        return call_method(b, e, expected);
    case REF_FN:
        if (ref.fn->param_count || ref.fn->effect_param_count) return call_generic(b, e, ref.fn, name, expected);
        if (callee->type_arg_count) {
            sema_error_expr(b, "E0361", callee, "`%.*s` is not generic: it takes no type arguments", NAME_ARGS(name));
        }
        callee->type = ref.fn->type;
        if (ref.fn->is_extern) {
            Type *ret = call_with(b, e, ref.fn->type, name, true);
            for (size_t i = 0; i < e->as.call.arg_count; i++) sema_check_escape(b, e->as.call.args[i], "given to a C function");
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
        if (type->tag == T_FN || sema_is_error(type)) return call_with(b, e, type, name, false);
        TypeNames n = {0};
        sema_error_expr(b, "E0338", callee, "`%.*s` is a `%s`, not a function", NAME_ARGS(name), sema_tn(&n, type));
        sema_release_names(&n);
        check_args_loosely(b, e);
        return sema_error_type(b);
    }
    case REF_VARIANT:
        callee->type = ref.info->type;
        return call_variant(b, e, ref.info, ref.variant);
    case REF_BUILTIN:
        return call_builtin(b, e, ref.builtin, expected);
    case REF_TYPE: {
        Diagnostic *d = sema_error_expr(b, "E0338", callee, "`%.*s` is a type, not a function", NAME_ARGS(ref.type->name));
        if (ref.type->tag == T_STRUCT) diag_help(d, "build a value with `%.*s{ ... }`", NAME_ARGS(ref.type->name));
        else if (ref.type->tag == T_CLASS) diag_help(d, "create it in an arena: `arena.new(%.*s, ...)`", NAME_ARGS(ref.type->name));
        else if (sema_is_numeric(ref.type)) diag_help(d, "convert with `as`: `x as %.*s`", NAME_ARGS(ref.type->name));
        check_args_loosely(b, e);
        return sema_error_type(b);
    }
    case REF_CONST:
        sema_error_expr(b, "E0338", callee, "`%.*s` is a constant, not a function", NAME_ARGS(name));
        check_args_loosely(b, e);
        return sema_error_type(b);
    case REF_MODULE:
        sema_error_expr(b, "E0338", callee, "`%s` is a module, not a function", ref.module->module);
        check_args_loosely(b, e);
        return sema_error_type(b);
    case REF_UNKNOWN:
        sema_report_unknown(b, callee->as.name);
        check_args_loosely(b, e);
        return sema_error_type(b);
    case REF_ERROR:
        check_args_loosely(b, e);
        return sema_error_type(b);
    }
    return sema_error_type(b);
}
