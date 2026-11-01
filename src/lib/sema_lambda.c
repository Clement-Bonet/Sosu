/* Checks of lambdas (spec §5.5) and of the variables they capture. Part of the checks of
   function bodies (sema_body.h). */

#include "sema_body.h"

#include <stdlib.h>
#include <string.h>

/* ---- Captures of lambdas (spec §5.5) ---- */

static void add_capture(Body *b, LambdaScope *scope, Name name, Type *type, bool is_self) {
    for (size_t i = 0; i < scope->captures.count; i++) {
        const Capture *c = &scope->captures.items[i];
        if (c->is_self == is_self && (is_self || sema_name_eq(c->name, name))) return;
    }
    Capture capture = { name, type, is_self };
    SOSU_VEC_PUSH(&scope->captures, capture);
    if (!is_self && type_owns_resource(type)) {
        Diagnostic *d = sema_error_at(b, "E0369", name.offset, name.length,
                                 "a lambda cannot capture `%.*s`, which owns a resource", NAME_ARGS(name));
        diag_help(d, "a lambda copies what it captures: capture a pointer to it instead");
    }
}

/* `local` is used: the lambdas between it and here capture it. */
void sema_capture_local(Body *b, const Local *local) {
    size_t index = (size_t)(local - b->locals.items);
    for (LambdaScope *scope = b->lambda; scope && index < scope->base; scope = scope->outer) {
        add_capture(b, scope, local->name, local->type, false);
    }
}

/* `self` is used, written or not: every lambda being checked captures it. */
void sema_capture_self(Body *b) {
    for (LambdaScope *scope = b->lambda; scope; scope = scope->outer) {
        Name self = { "self", 4, 0 };
        add_capture(b, scope, self, b->self_info ? b->self_info->type : NULL, true);
    }
}

/* True if `e` is a lambda of this function that captures variables, or a variable holding
   one: it lives as long as the function, and stays in its variables and arguments. */
bool sema_is_closure(Body *b, const Expr *e) {
    switch (e->kind) {
    case EXPR_LAMBDA: return e->as.lambda.capture_count > 0;
    case EXPR_TERNARY: return sema_is_closure(b, e->as.ternary.then_expr) || sema_is_closure(b, e->as.ternary.else_expr);
    case EXPR_NAME: {
        if (e->resolution != RESOLVED_LOCAL) return false;
        const Local *local = sema_find_local(b, e->as.name);
        return local && local->closure;
    }
    default: return false;
    }
}

/* `e` goes where it outlives the function (`where`: "returned", "stored in a field"...). */
void sema_check_escape(Body *b, const Expr *e, const char *where) {
    if (!e->type || e->type->tag != T_FN || !sema_is_closure(b, e)) return;
    Diagnostic *d = sema_error_expr(b, "E0370", e, "a lambda that captures variables cannot be %s", where);
    diag_help(d, "it lives as long as the function that creates it: call it, or pass it as an argument (spec §5.5)");
}

/* ---- Lambdas (spec §5.5) ---- */


/* The type `pattern` of a parameter of `generic` once its parameters are bound, or NULL if
   some are not yet (without `generic`, `pattern` itself). */
static Type *hinted(Body *b, Type *pattern, FnInfo *generic, Type **bound, unsigned *effects) {
    if (!generic) return pattern;
    return sema_all_bound(generic, pattern, bound, effects) ? sema_bind(b, generic, pattern, bound, effects) : NULL;
}

/* Checks a lambda against the function type expected for it, if any. Given to a parameter
   of the generic function `generic` (whose parameters are in `bound` and `effects`), it takes
   the types that are known, and gives the others. */
Type *sema_check_lambda(Body *b, Expr *e, Type *expected, FnInfo *generic, Type **bound, unsigned *effects) {
    size_t count = e->as.lambda.param_count;
    Type *pattern = expected && expected->tag == T_FN ? expected : NULL;
    bool wrong_count = pattern && pattern->as.fn.param_count != count;
    if (wrong_count) {
        TypeNames n = {0};
        sema_error_expr(b, "E0336", e, "the lambda takes %zu parameter%s, but `%s` takes %zu", count, count == 1 ? "" : "s",
                   sema_tn(&n, pattern), pattern->as.fn.param_count);
        sema_release_names(&n);
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
            params[i] = sema_error_type(b);
        } else {
            Diagnostic *d = sema_error_at(b, "E0369", param->name.offset, param->name.length,
                                     "the type of the parameter `%.*s` is unknown here", NAME_ARGS(param->name));
            diag_help(d, "write it, `(i32 %.*s) => ...`, or give the lambda a type: `fn(i32) -> i32 f = ...`",
                      NAME_ARGS(param->name));
            params[i] = sema_error_type(b);
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
    for (size_t i = 0; i < count; i++) sema_declare_local(b, e->as.lambda.params[i].name, params[i], false);

    if (e->as.lambda.block) {
        sema_check_block(b, e->as.lambda.block);
        if (!b->ret) b->ret = sema_void_type(b);
        if (b->ret->tag != T_VOID && !sema_is_error(b->ret) && !sema_always_returns(e->as.lambda.block)) {
            Diagnostic *d = sema_error_at(b, "E0346", e->offset, e->length, "the lambda does not return a value on every path");
            diag_help(d, "end each path with `return ...;`");
        }
    } else {
        Expr *body = e->as.lambda.body;
        if (b->ret && b->ret->tag == T_VOID) {
            sema_check_expr_stmt(b, body);
        } else {
            if (b->ret) sema_check_expect(b, body, b->ret);
            else b->ret = sema_check_expr(b, body, NULL);
            sema_consume(b, body);
            sema_check_escape(b, body, "returned");
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
    if (wrong_count) return sema_error_type(b); /* already reported */
    return type_fn(&b->s->types, params, count, ret, fx);
}
