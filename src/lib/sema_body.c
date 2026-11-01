#include "sema_body.h"

#include <stdlib.h>
#include <string.h>

/* Checks function bodies (spec §5.3, §7). Each expression is checked against an
   optional expected type, which number literals, `none`, `ok(...)`, `err(...)` and
   bare enum variants take; the resulting type is stored in Expr.type. */

static void check_assignable(Body *b, Expr *target);
static void check_stmt(Body *b, Stmt *stmt);

static const char *const builtin_names[] = { "some", "none", "ok", "err", "assert", "min", "max", "abs", "KB", "MB", "GB",
                                             "wrapping_add", "wrapping_sub", "wrapping_mul", "hash", "size_of" };

/* ---- Helpers ---- */

Diagnostic *sema_error_at(Body *b, const char *code, size_t offset, size_t length, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    Diagnostic *d = sema_verror(b->s, b->unit, code, offset, length, fmt, args);
    va_end(args);
    return d;
}

Diagnostic *sema_error_expr(Body *b, const char *code, const Expr *e, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    Diagnostic *d = sema_verror(b->s, b->unit, code, e->offset, e->length, fmt, args);
    va_end(args);
    return d;
}

bool sema_name_is(Name name, const char *text) {
    size_t length = strlen(text);
    return name.length == length && memcmp(name.text, text, length) == 0;
}

Type *sema_builtin(Body *b, const char *name) { return type_builtin(&b->s->types, name, strlen(name)); }
Type *sema_error_type(Body *b) { return b->s->types.error; }
Type *sema_void_type(Body *b) { return b->s->types.void_; }
bool sema_is_error(const Type *t) { return t->tag == T_ERROR; }
/* A type parameter allows what its constraints say (spec §5.4). */
static bool has_constraint(const Type *t, uint32_t constraint) {
    return t->tag == T_PARAM && (t->as.param.constraints & constraint);
}
bool sema_is_numeric(const Type *t) {
    return t->tag == T_INT || t->tag == T_FLOAT || has_constraint(t, CONSTRAINT_NUMBER);
}
static bool is_integer(const Type *t) { return t->tag == T_INT || has_constraint(t, CONSTRAINT_INTEGER); }

const char *sema_tn(TypeNames *names, const Type *type) {
    char *text = type_to_string(type);
    names->items[names->count++] = text;
    return text;
}

void sema_release_names(TypeNames *names) {
    for (int i = 0; i < names->count; i++) free(names->items[i]);
}

void sema_report_mismatch(Body *b, const Expr *e, Type *expected, Type *found) {
    if (sema_is_error(expected) || sema_is_error(found)) return;
    TypeNames n = {0};
    const char *expected_name = sema_tn(&n, expected);
    Diagnostic *d = sema_error_expr(b, "E0331", e, "expected `%s`, found `%s`", expected_name, sema_tn(&n, found));
    if (sema_is_numeric(expected) && sema_is_numeric(found)) {
        diag_help(d, "conversions are explicit: write `... as %s`", expected_name);
    } else if (expected->tag == T_OPTION && expected->as.inner == found) {
        diag_help(d, "wrap the value: `some(...)`");
    } else if (expected->tag == T_RESULT && expected->as.result.value == found) {
        diag_help(d, "wrap the value: `ok(...)`");
    }
    sema_release_names(&n);
}

/* ---- Moves (spec §6) ---- */


/* `e` is used as a value that goes elsewhere (a variable, an argument, a field, a returned
   value...): a variable owning a resource is moved, and cannot be used afterwards. */
void sema_consume(Body *b, Expr *e) {
    if (!e->type || !type_owns_resource(e->type)) return;
    if (e->kind == EXPR_TERNARY) {
        sema_consume(b, e->as.ternary.then_expr);
        sema_consume(b, e->as.ternary.else_expr);
        return;
    }
    if (e->kind == EXPR_NAME && e->resolution == RESOLVED_LOCAL) {
        Local *local = sema_find_local(b, e->as.name);
        if (!local || local->moved) return;
        if (b->loop_depth > local->loop_depth) {
            Diagnostic *d = sema_error_expr(b, "E0356", e, "`%.*s` would be moved at each iteration of the loop",
                                       NAME_ARGS(local->name));
            diag_help(d, "after the first iteration, it no longer has its value: move it before the loop, or pass a pointer");
        }
        local->moved = true;
        local->moved_offset = e->offset;
        return;
    }
    if (e->kind == EXPR_NAME || e->kind == EXPR_FIELD || e->kind == EXPR_INDEX ||
        (e->kind == EXPR_UNARY && e->as.unary.op == TOK_STAR)) {
        Diagnostic *d = sema_error_expr(b, "E0356", e, "a value that owns a resource cannot be moved out of where it is stored");
        diag_help(d, "pass a pointer to it instead");
    }
}

/* Checks `e` and reports it if its type is not `expected`. */
Type *sema_check_expect(Body *b, Expr *e, Type *expected) {
    Type *type = sema_check_expr(b, e, expected);
    if (type != expected) sema_report_mismatch(b, e, expected, type);
    return expected;
}

/* ---- Names ---- */

Local *sema_find_local(Body *b, Name name) {
    for (size_t i = b->locals.count; i > 0; i--) {
        if (sema_name_eq(b->locals.items[i - 1].name, name)) return &b->locals.items[i - 1];
    }
    return NULL;
}

void sema_declare_local(Body *b, Name name, Type *type, bool is_mut) {
    if (sema_name_is(name, "_")) return;
    Local *existing = sema_find_local(b, name);
    if (existing) {
        Diagnostic *d = sema_error_at(b, "E0347", name.offset, name.length, "`%.*s` is already declared in this function",
                                 NAME_ARGS(name));
        diag_help(d, "the first declaration is on line %zu: choose another name", sema_line_of(b->unit, existing->name.offset));
    }
    Local local = { name, type, is_mut, false, 0, b->loop_depth, false };
    SOSU_VEC_PUSH(&b->locals, local);
}

/* True if the place `e` is in a variable or a parameter of the function: the variable itself,
   or a field or an element of it, reached without going through a pointer or a view. */
static bool in_local_storage(Body *b, const Expr *e) {
    switch (e->kind) {
    case EXPR_NAME:
        return sema_find_local(b, e->as.name) != NULL;
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
        if (callee->kind != EXPR_NAME || sema_find_local(b, callee->as.name)) return NULL;
        if (!sema_name_is(callee->as.name, "some") && !sema_name_is(callee->as.name, "ok") && !sema_name_is(callee->as.name, "err")) {
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
    Diagnostic *d = sema_error_expr(b, "E0371", part, "%s returned would point into this function, which ends",
                               view ? "a view of a local array" : "the address of a local variable");
    diag_help(d, view ? "return the array itself, or a view of memory from an arena (`arena.alloc_slice`)"
                      : "return the value itself, or a pointer to memory from an arena (`arena.alloc`)");
}

/* True if `local` is a copy held by the innermost lambda being checked. */
static bool is_captured(Body *b, const Local *local) {
    return b->lambda && (size_t)(local - b->locals.items) < b->lambda->base;
}

FieldInfo *sema_find_field(TypeInfo *info, Name name) {
    for (TypeInfo *t = info; t; t = t->parent) {
        for (size_t i = 0; i < t->field_count; i++) {
            if (sema_name_eq(t->fields[i].name, name)) return &t->fields[i];
        }
    }
    return NULL;
}

MethodInfo *sema_find_method(TypeInfo *info, Name name) {
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

static Ref lookup_name(Body *b, Name name, Type *expected) {
    Ref ref = {0};
    if ((ref.local = sema_find_local(b, name))) {
        ref.kind = REF_LOCAL;
        sema_capture_local(b, ref.local);
        return ref;
    }
    if (b->self_info) {
        if ((ref.field = sema_find_field(b->self_info, name))) {
            ref.kind = REF_FIELD;
            sema_capture_self(b);
            return ref;
        }
        if ((ref.method = sema_find_method(b->self_info, name))) {
            ref.kind = REF_METHOD;
            sema_capture_self(b);
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
        if (sema_name_is(name, builtin_names[i])) {
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

void sema_report_unknown(Body *b, Name name) {
    Diagnostic *d = sema_error_at(b, "E0330", name.offset, name.length, "unknown name `%.*s`", NAME_ARGS(name));
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
        sema_error_at(b, "E0330", name.offset, name.length, "the module `%s` has no `%.*s`", module->module, NAME_ARGS(name));
        ref.kind = REF_ERROR;
        return ref;
    }
    if (!sym->is_pub && module != b->unit) {
        Diagnostic *d = sema_error_at(b, "E0305", name.offset, name.length, "`%.*s` is private to the module `%s`",
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
Ref sema_resolve_ref(Body *b, Expr *e, Type *expected) {
    Ref ref = {0};
    if (e->kind == EXPR_NAME) return lookup_name(b, e->as.name, expected);
    ref.kind = REF_VALUE;
    if (e->kind != EXPR_FIELD) return ref;
    Expr *object = e->as.field.object;
    if (object->kind != EXPR_NAME && object->kind != EXPR_FIELD) return ref;

    Ref outer = sema_resolve_ref(b, object, NULL);
    Name name = e->as.field.name;
    switch (outer.kind) {
    case REF_MODULE:
        return member_of_module(b, outer.module, name);
    case REF_TYPE:
        outer.type = sema_named_instance(b, object, outer.type, expected);
        if (sema_is_error(outer.type)) {
            ref.kind = REF_ERROR;
            return ref;
        }
        if (outer.type->tag == T_ENUM && find_variant(outer.type->as.info, name, &ref.variant)) {
            ref.kind = REF_VARIANT;
            ref.info = outer.type->as.info;
            return ref;
        }
        if (outer.type->tag == T_ARENA && sema_name_is(name, "new")) {
            ref.kind = REF_BUILTIN;
            ref.builtin = "Arena.new";
            return ref;
        }
        if (outer.type->tag == T_STR && sema_name_is(name, "from_bytes")) {
            ref.kind = REF_BUILTIN;
            ref.builtin = "str.from_bytes";
            return ref;
        }
        if (outer.type->tag == T_ENUM) {
            sema_error_at(b, "E0337", name.offset, name.length, "`%.*s` has no variant `%.*s`", NAME_ARGS(outer.type->name),
                     NAME_ARGS(name));
        } else {
            sema_error_expr(b, "E0333", e, "`%.*s` is a type: `%.*s.%.*s` is not a value", NAME_ARGS(outer.type->name),
                       NAME_ARGS(outer.type->name), NAME_ARGS(name));
        }
        ref.kind = REF_ERROR;
        return ref;
    case REF_UNKNOWN:
        sema_report_unknown(b, object->as.name);
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
        Diagnostic *d = sema_error_expr(b, "E0336", e, "the variant `%.*s` has %zu field%s", NAME_ARGS(variant->name),
                                   variant->field_count, variant->field_count == 1 ? "" : "s");
        diag_help(d, "give its values: `%.*s(...)`", NAME_ARGS(variant->name));
    }
    return info->type;
}

/* Records in `e` what it refers to, for code generation. */
void sema_record(Expr *e, Ref ref) {
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
    sema_record(e, ref);
    switch (ref.kind) {
    case REF_LOCAL:
        if (ref.local->moved && !b->assigning) {
            Diagnostic *d = sema_error_expr(b, "E0356", e, "`%.*s` was moved", NAME_ARGS(ref.local->name));
            diag_help(d, "it moved on line %zu: a value that owns a resource (an `Arena`...) has one owner at a "
                         "time; pass a pointer to lend it instead", sema_line_of(b->unit, ref.local->moved_offset));
        }
        return ref.local->type;
    case REF_FIELD: return ref.field->type;
    case REF_CONST: return ref.constant->type ? ref.constant->type : sema_error_type(b);
    case REF_FN:
        if (ref.fn->param_count || ref.fn->effect_param_count) {
            Diagnostic *d = sema_error_expr(b, "E0360", e, "`%.*s` is generic: it is called, not used as a value",
                                       NAME_ARGS(ref.fn->decl->name));
            diag_help(d, "wrap the call in a function of its own, with the types it needs (spec §5.4)");
            return sema_error_type(b);
        }
        if (ref.fn->is_extern && ref.fn->type->tag == T_FN) { /* a C function takes C function pointers, which a value of `fn` type does not give */
            for (size_t i = 0; i < ref.fn->type->as.fn.param_count; i++) {
                if (ref.fn->type->as.fn.params[i]->tag != T_FN) continue;
                Diagnostic *d = sema_error_expr(b, "E0370", e, "`%.*s` is a C function that takes a function: it is called, "
                                           "not used as a value", NAME_ARGS(ref.fn->decl->name));
                diag_help(d, "wrap the call in a function of Sosu (spec §5.5)");
                return sema_error_type(b);
            }
        }
        return ref.fn->type;
    case REF_VARIANT: return variant_value(b, e, ref.info, ref.variant);
    case REF_ERROR: return sema_error_type(b);
    case REF_METHOD: {
        Diagnostic *d = sema_error_expr(b, "E0333", e, "`%.*s` is a method", NAME_ARGS(ref.method->decl->name));
        diag_help(d, "call it: `%.*s(...)`", NAME_ARGS(ref.method->decl->name));
        return sema_error_type(b);
    }
    case REF_TYPE: {
        Diagnostic *d = sema_error_expr(b, "E0333", e, "`%.*s` is a type, not a value", NAME_ARGS(ref.type->name));
        if (ref.type->tag == T_STRUCT) diag_help(d, "build a value with `%.*s{ ... }`", NAME_ARGS(ref.type->name));
        return sema_error_type(b);
    }
    case REF_MODULE:
        sema_error_expr(b, "E0333", e, "`%s` is a module, not a value", ref.module->module);
        return sema_error_type(b);
    case REF_BUILTIN:
        if (strcmp(ref.builtin, "KB") == 0 || strcmp(ref.builtin, "MB") == 0 || strcmp(ref.builtin, "GB") == 0) {
            return sema_builtin(b, "usize");
        }
        if (strcmp(ref.builtin, "none") == 0) {
            if (expected && (expected->tag == T_OPTION || sema_is_error(expected))) return expected;
            Diagnostic *d = sema_error_expr(b, "E0342", e, "the type of `none` is unknown here");
            diag_help(d, "declare the expected type, e.g. `i32? x = none;`");
            return sema_error_type(b);
        }
        sema_error_expr(b, "E0333", e, "`%s` is a built-in function: call it", ref.builtin);
        return sema_error_type(b);
    case REF_UNKNOWN:
        sema_report_unknown(b, e->as.name);
        return sema_error_type(b);
    case REF_VALUE:
        break;
    }
    return sema_error_type(b);
}

/* ---- Literals ---- */

bool sema_is_untyped_literal(const Expr *e) {
    if (e->kind == EXPR_INT || e->kind == EXPR_FLOAT) return true;
    return e->kind == EXPR_UNARY && e->as.unary.op == TOK_MINUS && sema_is_untyped_literal(e->as.unary.operand);
}

static bool int_fits(const Type *type, uint64_t value, bool negative) {
    unsigned bits = type->as.integer.bits;
    if (!type->as.integer.is_signed) return !negative ? (bits == 64 || value <= (UINT64_C(1) << bits) - 1) : value == 0;
    uint64_t limit = UINT64_C(1) << (bits - 1);
    return negative ? value <= limit : value < limit;
}

static Type *check_int_literal(Body *b, Expr *e, Type *expected, bool negative) {
    if (expected && sema_is_error(expected)) return expected;
    if (expected && expected->tag == T_FLOAT) {
        Diagnostic *d = sema_error_expr(b, "E0331", e, "an integer literal cannot be a `%.*s`", NAME_ARGS(expected->name));
        diag_help(d, "write a float literal: `%llu.0`", (unsigned long long)e->as.int_value);
        return expected;
    }
    Type *type = expected && expected->tag == T_INT ? expected : sema_builtin(b, "i32");
    if (!int_fits(type, e->as.int_value, negative)) {
        Diagnostic *d = sema_error_expr(b, "E0332", e, "`%s%llu` does not fit in `%.*s`", negative ? "-" : "",
                                   (unsigned long long)e->as.int_value, NAME_ARGS(type->name));
        if (!expected || expected->tag != T_INT) diag_help(d, "give the value a larger type, e.g. `i64 x = ...;`");
    }
    return type;
}

/* Checks two operands that must have the same type: a literal takes the type of the other side. */
void sema_check_pair(Body *b, Expr *left, Expr *right, Type *hint, Type **lt, Type **rt) {
    if (sema_is_untyped_literal(left) && !sema_is_untyped_literal(right)) {
        *rt = sema_check_expr(b, right, hint);
        *lt = sema_check_expr(b, left, *rt);
    } else {
        *lt = sema_check_expr(b, left, hint);
        *rt = sema_check_expr(b, right, *lt);
    }
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
    if (sema_is_untyped_literal(value) && sema_is_numeric(target)) from = sema_check_expr(b, value, target);
    else from = sema_check_expr(b, value, NULL);
    if (sema_is_error(from) || sema_is_error(target)) return e->kind == EXPR_TRY_CAST ? sema_error_type(b) : target;

    TypeNames n = {0};
    if (e->kind == EXPR_TRY_CAST) {
        bool ok = is_object_pointer(from) && target->tag == T_POINTER && target->as.inner->tag == T_CLASS &&
                  inherits(target->as.inner->as.info, from->as.inner->as.info);
        if (!ok) {
            Diagnostic *d = sema_error_expr(b, "E0339", e, "`as?` cannot convert `%s` to `%s`", sema_tn(&n, from), sema_tn(&n, target));
            diag_help(d, "`as?` converts a class or interface pointer to a pointer to one of its subclasses");
        }
        sema_release_names(&n);
        return type_option(&b->s->types, target);
    }

    bool allowed = false;
    bool needs_unsafe = false;
    if (from == target) {
        allowed = true;
    } else if (sema_is_numeric(from) && sema_is_numeric(target)) {
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
        sema_error_expr(b, "E0339", e, "cannot convert `%s` to `%s`", sema_tn(&n, from), sema_tn(&n, target));
    } else if (needs_unsafe && b->unsafe_depth == 0) {
        Diagnostic *d = sema_error_expr(b, "E0341", e, "converting `%s` to `%s` is only allowed in an `unsafe` block",
                                   sema_tn(&n, from), sema_tn(&n, target));
        diag_help(d, "the conversion reinterprets memory (spec §7.7)");
    }
    sema_release_names(&n);
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
    if (sema_is_error(lt) || sema_is_error(rt)) return;
    TypeNames n = {0};
    Diagnostic *d = sema_error_expr(b, "E0334", e, "`%s` needs %s, found `%s` and `%s`",
                               token_kind_spelling(e->as.binary.op), need, sema_tn(&n, lt), sema_tn(&n, rt));
    if (sema_is_numeric(lt) && sema_is_numeric(rt)) diag_help(d, "convert one side with `as` (spec §5.3)");
    else if (lt->tag == T_PARAM || rt->tag == T_PARAM) {
        diag_help(d, "a type parameter allows what its constraints say: add one, e.g. `<%.*s: %s>` (spec §5.4)",
                  NAME_ARGS((lt->tag == T_PARAM ? lt : rt)->name), constraint_for(e->as.binary.op));
    }
    sema_release_names(&n);
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
    Type *bool_type = sema_builtin(b, "bool");

    switch (op) {
    case TOK_PLUS: case TOK_MINUS: case TOK_STAR: case TOK_SLASH: case TOK_PERCENT:
        sema_check_pair(b, left, right, expected, &lt, &rt);
        if (lt->tag == T_POINTER && rt->tag == T_INT && (op == TOK_PLUS || op == TOK_MINUS)) {
            if (b->unsafe_depth == 0) {
                Diagnostic *d = sema_error_expr(b, "E0341", e, "pointer arithmetic is only allowed in an `unsafe` block");
                diag_help(d, "outside `unsafe`, use arrays and slices (spec §7.7)");
            }
            return lt;
        }
        if (sema_is_error(lt) || sema_is_error(rt)) return sema_is_error(lt) ? rt : lt;
        if (op == TOK_PERCENT && (!is_integer(lt) || lt != rt)) {
            report_operands(b, e, "two integers of the same type", lt, rt);
        } else if (!sema_is_numeric(lt) || lt != rt) {
            report_operands(b, e, "two numbers of the same type", lt, rt);
        }
        return lt;
    case TOK_EQ_EQ: case TOK_BANG_EQ:
        sema_check_pair(b, left, right, NULL, &lt, &rt);
        if (!sema_is_error(lt) && !sema_is_error(rt) && (lt != rt || !is_comparable(lt))) {
            report_operands(b, e, "two values of the same comparable type", lt, rt);
        }
        return bool_type;
    case TOK_LT: case TOK_LT_EQ: case TOK_GT: case TOK_GT_EQ:
        sema_check_pair(b, left, right, NULL, &lt, &rt);
        bool ordered = sema_is_numeric(lt) || lt->tag == T_CHAR || lt->tag == T_STR || has_constraint(lt, CONSTRAINT_ORDERED);
        if (!sema_is_error(lt) && !sema_is_error(rt) && (lt != rt || !ordered)) {
            report_operands(b, e, "two numbers, characters or texts of the same type", lt, rt);
        }
        return bool_type;
    case TOK_AMP_AMP: case TOK_PIPE_PIPE:
        sema_check_expect(b, left, bool_type);
        sema_check_expect(b, right, bool_type);
        return bool_type;
    case TOK_AMP: case TOK_PIPE: case TOK_CARET:
        sema_check_pair(b, left, right, expected, &lt, &rt);
        if (!sema_is_error(lt) && !sema_is_error(rt) && (!is_integer(lt) || lt != rt)) {
            report_operands(b, e, "two integers of the same type", lt, rt);
        }
        return lt;
    case TOK_SHL: case TOK_SHR:
        lt = sema_check_expr(b, left, expected);
        rt = sema_check_expr(b, right, NULL);
        if (!sema_is_error(lt) && !sema_is_error(rt) && (!is_integer(lt) || !is_integer(rt))) {
            report_operands(b, e, "integers", lt, rt);
        }
        return lt;
    default:
        return sema_error_type(b);
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
        type = sema_check_expr(b, operand, expected);
        bool is_signed = type->tag == T_FLOAT ? !type->as.floating.is_unsigned
                                              : type->tag == T_INT && type->as.integer.is_signed;
        if (!sema_is_error(type) && !is_signed) {
            Diagnostic *d = sema_error_expr(b, "E0334", e, "`-` needs a signed integer or a signed float, found `%s`", sema_tn(&n, type));
            if (type->tag == T_FLOAT) diag_help(d, "an unsigned float is never negative: convert it first, `x as f64`");
            else if (type->tag == T_PARAM) {
                diag_help(d, "a parameter may stand for an unsigned type: write `(0 as %.*s) - x`", NAME_ARGS(type->name));
            }
        }
        break;
    case TOK_BANG:
        type = sema_check_expect(b, operand, sema_builtin(b, "bool"));
        break;
    case TOK_TILDE:
        type = sema_check_expr(b, operand, expected);
        if (!sema_is_error(type) && !is_integer(type)) sema_error_expr(b, "E0334", e, "`~` needs an integer, found `%s`", sema_tn(&n, type));
        break;
    case TOK_AMP:
        type = sema_check_expr(b, operand, NULL);
        if (!is_place(operand)) {
            sema_error_expr(b, "E0353", e, "cannot take the address of a temporary value");
            type = sema_error_type(b);
        } else if (type->tag == T_FN && (operand->kind == EXPR_FIELD || operand->resolution == RESOLVED_SELF_FIELD)) {
            Diagnostic *d = sema_error_expr(b, "E0353", e, "cannot take the address of a field of function type");
            diag_help(d, "a field holds a C function pointer (spec §5.5): copy it into a variable first");
            type = sema_error_type(b);
        } else if (!sema_is_error(type)) {
            /* the pointer allows writing: the value must be mutable (spec §5.3) */
            b->taking_address = true;
            check_assignable(b, operand);
            b->taking_address = false;
            type = type_pointer(&b->s->types, type);
        }
        break;
    case TOK_STAR:
        type = sema_check_expr(b, operand, NULL);
        if (type->tag == T_POINTER) {
            type = type->as.inner;
        } else if (!sema_is_error(type)) {
            sema_error_expr(b, "E0334", e, "`*` needs a pointer, found `%s`", sema_tn(&n, type));
            type = sema_error_type(b);
        }
        break;
    default:
        type = sema_error_type(b);
        break;
    }
    sema_release_names(&n);
    return type;
}

/* ---- Fields, indexes, literals ---- */

static Type *check_field(Body *b, Expr *e, Type *expected) {
    Ref ref = sema_resolve_ref(b, e, expected);
    if (ref.kind != REF_VALUE) return value_of_ref(b, e, ref, expected);

    Name name = e->as.field.name;
    Type *object = sema_check_expr(b, e->as.field.object, NULL);
    Type *base = object->tag == T_POINTER ? object->as.inner : object;
    if (sema_is_error(base)) return base;
    if (base->tag == T_STRUCT || base->tag == T_CLASS) {
        FieldInfo *field = sema_find_field(base->as.info, name);
        if (field) {
            if (base->as.info->unit != b->unit && !field->decl->is_pub) {
                Diagnostic *d = sema_error_at(b, "E0305", name.offset, name.length, "the field `%.*s` of `%.*s` is private",
                                         NAME_ARGS(name), NAME_ARGS(base->name));
                diag_help(d, "declare it `pub` in `%.*s`", NAME_ARGS(base->name));
            }
            return field->type;
        }
        if (sema_find_method(base->as.info, name)) {
            Diagnostic *d = sema_error_at(b, "E0333", name.offset, name.length, "`%.*s` is a method", NAME_ARGS(name));
            diag_help(d, "call it: `%.*s(...)`", NAME_ARGS(name));
            return sema_error_type(b);
        }
    }
    if ((base->tag == T_ARRAY || base->tag == T_SLICE || base->tag == T_STR) && sema_name_is(name, "len")) {
        return sema_builtin(b, "usize");
    }
    if (base->tag == T_STR && sema_name_is(name, "bytes")) return type_slice(&b->s->types, sema_builtin(b, "ui8"));
    TypeNames n = {0};
    sema_error_at(b, "E0337", name.offset, name.length, "`%s` has no field `%.*s`", sema_tn(&n, base), NAME_ARGS(name));
    sema_release_names(&n);
    return sema_error_type(b);
}

/* `x[a..b]`: a view of the elements a to b - 1 of an array or a slice. */
static Type *check_slicing(Body *b, Expr *e, Type *object) {
    Type *base = object->tag == T_POINTER ? object->as.inner : object;
    Expr *range = e->as.index.index;
    Type *usize = sema_builtin(b, "usize");
    sema_check_expect(b, range->as.range.start, usize);
    sema_check_expect(b, range->as.range.end, usize);
    range->type = usize;
    if (base->tag == T_ARRAY) {
        if (object->tag != T_POINTER && is_place(e->as.index.object)) {
            /* the view allows writing, like `&x`: the array must be mutable */
            b->taking_address = true;
            check_assignable(b, e->as.index.object);
            b->taking_address = false;
        } else if (object->tag != T_POINTER) {
            sema_error_expr(b, "E0353", e, "cannot take a view of a temporary array");
        }
        return type_slice(&b->s->types, base->as.array.element);
    }
    if (base->tag == T_SLICE) return base;
    if (!sema_is_error(base)) {
        TypeNames n = {0};
        Diagnostic *d = sema_error_expr(b, "E0334", e, "a `%s` cannot be sliced", sema_tn(&n, base));
        if (base->tag == T_STR) diag_help(d, "slice its bytes: `s.bytes[a..b]` (spec §4.1)");
        sema_release_names(&n);
    }
    return sema_error_type(b);
}

static Type *check_index(Body *b, Expr *e) {
    Type *object = sema_check_expr(b, e->as.index.object, NULL);
    if (e->as.index.index->kind == EXPR_RANGE) {
        if (e->as.index.index->as.range.inclusive) {
            Diagnostic *d = sema_error_expr(b, "E0352", e->as.index.index, "a view takes a range without its end: `a..b`");
            diag_help(d, "to include the element b, write `x[a..b + 1]`");
        }
        return check_slicing(b, e, object);
    }
    Type *base = object->tag == T_POINTER ? object->as.inner : object;
    Type *index = sema_check_expr(b, e->as.index.index, sema_builtin(b, "usize"));
    TypeNames n = {0};
    if (!sema_is_error(index) && !is_integer(index)) {
        sema_error_expr(b, "E0334", e->as.index.index, "an index must be an integer, not `%s`", sema_tn(&n, index));
    }
    Type *result = sema_error_type(b);
    if (base->tag == T_ARRAY) {
        result = base->as.array.element;
    } else if (base->tag == T_SLICE) {
        result = base->as.inner;
    } else if (base->tag == T_STR) {
        Diagnostic *d = sema_error_expr(b, "E0334", e, "a `str` cannot be indexed");
        diag_help(d, "use `s.bytes[i]` for a byte (spec §4.1)");
    } else if (!sema_is_error(base)) {
        sema_error_expr(b, "E0334", e, "a `%s` cannot be indexed", sema_tn(&n, base));
    }
    sema_release_names(&n);
    return result;
}

/* `[a, b, c]` and `[value; count]`: the element type comes from the expected array type,
   or else from the first element. */
static Type *check_array(Body *b, Expr *e, Type *expected) {
    Type *element = expected && expected->tag == T_ARRAY ? expected->as.array.element : NULL;
    uint64_t size;
    if (e->as.array.repeat) {
        Expr *value = e->as.array.repeat;
        element = element ? sema_check_expect(b, value, element) : sema_check_expr(b, value, NULL);
        if (type_owns_resource(element)) {
            sema_error_expr(b, "E0356", value, "a value that owns a resource cannot be repeated");
        }
        size = e->as.array.repeat_count;
    } else {
        for (size_t i = 0; i < e->as.array.count; i++) {
            Expr *item = e->as.array.items[i];
            if (element) sema_check_expect(b, item, element);
            else element = sema_check_expr(b, item, NULL);
            sema_consume(b, item);
            sema_check_escape(b, item, "stored in an array");
        }
        size = e->as.array.count;
    }
    if (size == 0) {
        sema_error_expr(b, "E0308", e, "an array has at least one element");
        return sema_error_type(b);
    }
    if (!element || sema_is_error(element)) return sema_error_type(b);
    if (element->tag == T_VOID) {
        sema_error_expr(b, "E0331", e, "the elements of this array have no value");
        return sema_error_type(b);
    }
    if (expected && expected->tag == T_ARRAY && expected->as.array.size != size) {
        TypeNames n = {0};
        Diagnostic *d = sema_error_expr(b, "E0355", e, "the array has %llu element%s, but `%s` has %llu", (unsigned long long)size,
                                   size == 1 ? "" : "s", sema_tn(&n, expected), (unsigned long long)expected->as.array.size);
        diag_help(d, "give every element, or repeat one value: `[value; %llu]`", (unsigned long long)expected->as.array.size);
        sema_release_names(&n);
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
        else if (expected && sema_is_nominal(expected) && sema_generic_of(expected) == type->as.info) type = expected;
        else type = sema_resolve_type(b->s, b->unit, te, POSITION_VALUE); /* reports the missing arguments */
    } else if (te->arg_count) {
        type = sema_resolve_type(b->s, b->unit, te, POSITION_VALUE);
    }
    size_t count = e->as.struct_lit.field_count;
    FieldInit *inits = e->as.struct_lit.fields;
    if (type->tag != T_STRUCT) {
        if (!sema_is_error(type)) {
            Diagnostic *d = sema_error_expr(b, "E0350", e, "`%.*s` is not a struct", NAME_ARGS(type->name));
            if (type->tag == T_CLASS) diag_help(d, "a class is created in an arena: `arena.new(%.*s, ...)`", NAME_ARGS(type->name));
        }
        for (size_t i = 0; i < count; i++) sema_check_expr(b, inits[i].value, NULL);
        return sema_error_type(b);
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
            sema_error_at(b, "E0350", name.offset, name.length, "`%.*s` has no field `%.*s`", NAME_ARGS(type->name), NAME_ARGS(name));
            sema_check_expr(b, inits[i].value, NULL);
            continue;
        }
        if (given[index]) sema_error_at(b, "E0350", name.offset, name.length, "the field `%.*s` is given twice", NAME_ARGS(name));
        given[index] = true;
        sema_check_expect(b, inits[i].value, info->fields[index].type);
        sema_consume(b, inits[i].value);
        sema_check_escape(b, inits[i].value, "stored in a field");
    }
    for (size_t j = 0; j < info->field_count; j++) {
        if (given[j] || info->fields[j].decl->init) continue;
        Diagnostic *d = sema_error_expr(b, "E0350", e, "missing field `%.*s` in `%.*s{ ... }`", NAME_ARGS(info->fields[j].name),
                                   NAME_ARGS(type->name));
        diag_help(d, "give every field a value; only fields with a default value can be left out");
        break;
    }
    free(given);
    return type;
}

/* ---- match ---- */


/* The cases a `match` must cover: the variants of an enum, or some / none, ok / err. */
typedef struct {
    Name name;
    Type *fields[1];
    size_t field_count;
} Case;

static Type *check_match(Body *b, Expr *e, Type *expected, bool statement) {
    Expr *subject_expr = e->as.match.subject;
    Type *subject = sema_check_expr(b, subject_expr, NULL);
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
    } else if (!sema_is_error(subject)) {
        TypeNames n = {0};
        sema_error_expr(b, "E0343", subject_expr, "`match` needs an enum, an option or a result, not `%s`", sema_tn(&n, subject));
        sema_release_names(&n);
    }
    size_t case_count = info ? info->variant_count : cases.count;
    bool *covered = calloc(case_count + 1, sizeof *covered);
    bool wildcard = false;
    bool known = info || cases.count > 0;
    Type *result = statement ? sema_void_type(b) : expected;

    for (size_t a = 0; a < e->as.match.arm_count; a++) {
        MatchArm *arm = &e->as.match.arms[a];
        const Pattern *pattern = &arm->pattern;
        size_t mark = b->locals.count;
        bool all_covered = known && !wildcard;
        for (size_t i = 0; i < case_count && all_covered; i++) all_covered = covered[i];
        if (wildcard || (known && all_covered)) {
            sema_error_at(b, "E0343", pattern->offset, pattern->length, "this arm can never match");
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
                Diagnostic *d = sema_error_at(b, "E0343", pattern->offset, pattern->length, "`%s` has no case `%.*s`",
                                         sema_tn(&n, subject), NAME_ARGS(name));
                if (!info) diag_help(d, subject->tag == T_OPTION ? "the cases are `some(x)` and `none`"
                                                                 : "the cases are `ok(x)` and `err(e)`");
                sema_release_names(&n);
            } else {
                if (covered[index]) {
                    sema_error_at(b, "E0343", pattern->offset, pattern->length, "`%.*s` is already matched", NAME_ARGS(name));
                }
                covered[index] = true;
                size_t fields = info ? info->variants[index].field_count : cases.items[index].field_count;
                if (pattern->binding_count != fields || (fields > 0 && !pattern->has_parens)) {
                    sema_error_at(b, "E0343", pattern->offset, pattern->length, "`%.*s` has %zu field%s, the pattern binds %zu",
                             NAME_ARGS(name), fields, fields == 1 ? "" : "s", pattern->binding_count);
                } else {
                    for (size_t j = 0; j < fields; j++) {
                        Type *field_type = info ? info->variants[index].fields[j] : cases.items[index].fields[j];
                        sema_declare_local(b, pattern->bindings[j], field_type, false);
                    }
                }
            }
        }

        if (arm->block) {
            sema_check_block(b, arm->block);
            if (!statement && !sema_always_returns(arm->block)) {
                Diagnostic *d = sema_error_at(b, "E0343", arm->block->offset, arm->block->length,
                                         "a block arm in a `match` that gives a value must end with `return`");
                diag_help(d, "give the value as an expression: `%.*s => value`", (int)pattern->length,
                          b->unit->file.text + pattern->offset);
            }
        } else if (statement) {
            sema_check_expr(b, arm->expr, NULL);
        } else if (!result) {
            result = sema_check_expr(b, arm->expr, NULL);
        } else {
            sema_check_expect(b, arm->expr, result);
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
            Diagnostic *d = sema_error_at(b, "E0312", e->offset, 5, "non-exhaustive match: missing %s%s%s",
                                     missing_count == 1 ? "case " : "cases ", missing, missing_count > 3 ? "..." : "");
            diag_help(d, "add an arm for each missing case, or `_ => ...`");
        }
    }
    free(covered);
    free(cases.items);
    return result ? result : sema_error_type(b);
}

/* ---- Expressions ---- */

static Type *check_expr_kind(Body *b, Expr *e, Type *expected) {
    switch (e->kind) {
    case EXPR_LAMBDA:
        return sema_check_lambda(b, e, expected, NULL, NULL, NULL);
    case EXPR_ERROR:
        return sema_error_type(b);
    case EXPR_INT:
        return check_int_literal(b, e, expected, false);
    case EXPR_FLOAT:
        return expected && expected->tag == T_FLOAT ? expected : sema_builtin(b, "f64");
    case EXPR_STRING:
        return sema_builtin(b, "str");
    case EXPR_CHAR:
        return sema_builtin(b, "char");
    case EXPR_BOOL:
        return sema_builtin(b, "bool");
    case EXPR_NAME:
        if (b->in_ensures && sema_name_is(e->as.name, "result")) { /* contextual: the returned value */
            e->kind = EXPR_RESULT;
            return check_expr_kind(b, e, expected);
        }
        return value_of_ref(b, e, lookup_name(b, e->as.name, expected), expected);
    case EXPR_SELF:
    case EXPR_BASE: {
        const char *word = e->kind == EXPR_SELF ? "self" : "base";
        if (!b->self_info) {
            sema_error_expr(b, "E0349", e, "`%s` can only be used in a method", word);
            return sema_error_type(b);
        }
        sema_capture_self(b);
        if (e->kind == EXPR_BASE) {
            if (!b->self_info->parent) {
                sema_error_expr(b, "E0349", e, "`%.*s` has no parent class", NAME_ARGS(b->self_info->name));
                return sema_error_type(b);
            }
            return type_pointer(&b->s->types, b->self_info->parent->type);
        }
        return type_pointer(&b->s->types, b->self_info->type);
    }
    case EXPR_RESULT:
        if (b->ret->tag == T_VOID) {
            Diagnostic *d = sema_error_expr(b, "E0349", e, "`%.*s` returns nothing: `result` has no value here",
                                       NAME_ARGS(b->fn_name));
            diag_help(d, "in `ensures`, `result` is the value returned by the function");
            return sema_error_type(b);
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
        sema_check_expect(b, e->as.ternary.cond, sema_builtin(b, "bool"));
        Type *then_type, *else_type;
        sema_check_pair(b, e->as.ternary.then_expr, e->as.ternary.else_expr, expected, &then_type, &else_type);
        if (then_type != else_type && !sema_is_error(then_type)) sema_report_mismatch(b, e->as.ternary.else_expr, then_type, else_type);
        return then_type;
    }
    case EXPR_RANGE: {
        Diagnostic *d = sema_error_expr(b, "E0352", e, "a range can only be used in `for (... in a..b)`");
        diag_help(d, "loop over it: `for (i32 i in a..b) { ... }`");
        Type *lt, *rt;
        sema_check_pair(b, e->as.range.start, e->as.range.end, NULL, &lt, &rt);
        return sema_error_type(b);
    }
    case EXPR_CALL:
        return sema_check_call(b, e, expected);
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
    case EXPR_TYPE: {
        Diagnostic *d = sema_error_expr(b, "E0333", e, "a type is not a value");
        diag_help(d, "a type is given to a call that takes one, as `arena.alloc_slice(ui8*, n)`");
        return sema_error_type(b);
    }
    case EXPR_TRY: {
        Type *type = sema_check_expr(b, e->as.inner, NULL);
        if (sema_is_error(type)) return type;
        TypeNames n = {0};
        if (type->tag != T_RESULT) {
            Diagnostic *d = sema_error_expr(b, "E0345", e, "`try` needs a `T!E` value, found `%s`", sema_tn(&n, type));
            if (type->tag == T_OPTION) diag_help(d, "for an option, use `match` with `some(x)` and `none`");
            sema_release_names(&n);
            return sema_error_type(b);
        }
        if (b->ret->tag != T_RESULT || b->ret->as.result.error != type->as.result.error) {
            const char *error_name = sema_tn(&n, type->as.result.error);
            Diagnostic *d = sema_error_expr(b, "E0345", e, "`try` returns the error early, but `%.*s` does not return `...!%s`",
                                       NAME_ARGS(b->fn_name), error_name);
            diag_help(d, "return a result with the error `%s`, or handle it with `match`", error_name);
        }
        sema_release_names(&n);
        return type->as.result.value;
    }
    }
    return sema_error_type(b);
}

Type *sema_check_expr(Body *b, Expr *e, Type *expected) {
    Type *type = check_expr_kind(b, e, expected);
    e->type = type;
    return type;
}

/* ---- Assignments ---- */

static bool field_is_assignable(Body *b, const FieldInfo *field) {
    if (field->decl->is_mut) return true;
    return b->in_init && b->self_info && sema_find_field(b->self_info, field->name) == field;
}

static void report_field(Body *b, const Expr *e, Name name, const char *owner, size_t owner_length) {
    Diagnostic *d = sema_error_expr(b, "E0335", e, "the field `%.*s` is not mutable", NAME_ARGS(name));
    diag_help(d, "declare it `mut` in `%.*s`, or assign it in `init` (spec §5.2)", (int)owner_length, owner);
}

/* Reports `target` if it cannot be assigned. Its types are already checked. */
static void check_assignable(Body *b, Expr *target) {
    switch (target->kind) {
    case EXPR_NAME: {
        Ref ref = lookup_name(b, target->as.name, NULL);
        if (ref.kind == REF_LOCAL) {
            if (is_captured(b, ref.local)) {
                Diagnostic *d = sema_error_expr(b, "E0369", target, "`%.*s` is captured by the lambda: it cannot be changed",
                                           NAME_ARGS(target->as.name));
                diag_help(d, "a lambda copies the variables it uses; to change one, capture a pointer to it (spec §5.5)");
            } else if (!ref.local->is_mut) {
                Diagnostic *d = sema_error_expr(b, "E0335", target, "`%.*s` is not mutable", NAME_ARGS(target->as.name));
                if (b->taking_address) diag_help(d, "declare it with `mut`: through its address, it can be changed");
                else diag_help(d, "declare it with `mut` to change it");
            }
        } else if (ref.kind == REF_FIELD) {
            if (!field_is_assignable(b, ref.field)) {
                report_field(b, target, ref.field->name, b->self_info->name.text, b->self_info->name.length);
            }
        } else if (ref.kind != REF_UNKNOWN && ref.kind != REF_ERROR) {
            sema_error_expr(b, "E0335", target, "`%.*s` cannot be assigned", NAME_ARGS(target->as.name));
        }
        return;
    }
    case EXPR_FIELD: {
        Expr *object = target->as.field.object;
        Type *type = object->type ? object->type : sema_error_type(b);
        Type *base = type->tag == T_POINTER ? type->as.inner : type;
        if (sema_is_error(base) || sema_is_error(target->type)) return;
        FieldInfo *field = (base->tag == T_STRUCT || base->tag == T_CLASS) ? sema_find_field(base->as.info, target->as.field.name)
                                                                          : NULL;
        if (!field) {
            sema_error_expr(b, "E0335", target, "`%.*s` cannot be assigned", NAME_ARGS(target->as.field.name));
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
            Diagnostic *d = sema_error_expr(b, "E0335", target, "a view `x[a..b]` cannot be assigned");
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
    sema_error_expr(b, "E0335", target, "this expression cannot be assigned");
}

static void check_assign(Body *b, Stmt *stmt) {
    Expr *target = stmt->as.assign.target;
    Expr *value = stmt->as.assign.value;
    TokenKind op = stmt->as.assign.op;
    b->assigning = op == TOK_EQ && target->kind == EXPR_NAME; /* a moved variable can get a new value */
    Type *tt = sema_check_expr(b, target, NULL);
    b->assigning = false;
    check_assignable(b, target);
    if (op == TOK_EQ) {
        sema_check_expect(b, value, tt);
        sema_consume(b, value);
        sema_check_escape(b, value, target->kind == EXPR_NAME && target->resolution == RESOLVED_LOCAL
                                   ? "assigned with `=`: give it to a new variable"
                                   : "stored there");
        if (target->kind == EXPR_NAME && target->resolution == RESOLVED_LOCAL) {
            Local *local = sema_find_local(b, target->as.name);
            if (local) local->moved = false;
        }
        return;
    }
    if (op == TOK_SHL_EQ || op == TOK_SHR_EQ) {
        Type *vt = sema_check_expr(b, value, NULL);
        if (!sema_is_error(tt) && !sema_is_error(vt) && (!is_integer(tt) || !is_integer(vt))) {
            TypeNames n = {0};
            sema_error_expr(b, "E0334", value, "`%s` needs integers, found `%s` and `%s`", token_kind_spelling(op), sema_tn(&n, tt),
                       sema_tn(&n, vt));
            sema_release_names(&n);
        }
        return;
    }
    Type *vt = sema_check_expr(b, value, tt->tag == T_POINTER ? NULL : tt);
    if (sema_is_error(tt) || sema_is_error(vt)) return;
    bool bitwise = op == TOK_AMP_EQ || op == TOK_PIPE_EQ || op == TOK_CARET_EQ || op == TOK_PERCENT_EQ;
    bool pointer_step = tt->tag == T_POINTER && vt->tag == T_INT && (op == TOK_PLUS_EQ || op == TOK_MINUS_EQ);
    if (pointer_step) {
        if (b->unsafe_depth == 0) sema_error_expr(b, "E0341", value, "pointer arithmetic is only allowed in an `unsafe` block");
        return;
    }
    bool ok = tt == vt && (bitwise ? is_integer(tt) : sema_is_numeric(tt));
    if (!ok) {
        TypeNames n = {0};
        sema_error_expr(b, "E0334", value, "`%s` needs %s, found `%s` and `%s`", token_kind_spelling(op),
                   bitwise ? "two integers of the same type" : "two numbers of the same type", sema_tn(&n, tt), sema_tn(&n, vt));
        sema_release_names(&n);
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

bool sema_always_returns(const Stmt *stmt) {
    if (!stmt) return false;
    switch (stmt->kind) {
    case STMT_RETURN:
        return true;
    case STMT_BLOCK:
        for (size_t i = 0; i < stmt->as.block.count; i++) {
            if (sema_always_returns(stmt->as.block.items[i])) return true;
        }
        return false;
    case STMT_IF:
        return sema_always_returns(stmt->as.if_.then_block) && sema_always_returns(stmt->as.if_.else_stmt);
    case STMT_UNSAFE:
        return sema_always_returns(stmt->as.inner);
    case STMT_WHILE: /* `while (true)` only ends with `break` */
        return stmt->as.while_.cond->kind == EXPR_BOOL && stmt->as.while_.cond->as.bool_value &&
               !contains_break(stmt->as.while_.body);
    case STMT_EXPR: {
        const Expr *e = stmt->as.expr;
        if (e->kind != EXPR_MATCH || e->as.match.arm_count == 0) return false;
        for (size_t i = 0; i < e->as.match.arm_count; i++) {
            if (!sema_always_returns(e->as.match.arms[i].block)) return false;
        }
        return true;
    }
    default:
        return false;
    }
}

void sema_check_block(Body *b, Stmt *block) {
    size_t mark = b->locals.count;
    for (size_t i = 0; i < block->as.block.count; i++) check_stmt(b, block->as.block.items[i]);
    b->locals.count = mark;
}

static void check_var(Body *b, Stmt *stmt) {
    Type *type = stmt->as.var.type ? sema_resolve_type(b->s, b->unit, stmt->as.var.type, POSITION_VALUE) : NULL;
    Expr *init = stmt->as.var.init;
    if (type) {
        sema_check_expect(b, init, type);
    } else {
        type = sema_check_expr(b, init, NULL);
        if (type->tag == T_VOID) {
            sema_error_expr(b, "E0331", init, "this expression has no value");
            type = sema_error_type(b);
        }
    }
    sema_consume(b, init);
    bool closure = type->tag == T_FN && sema_is_closure(b, init);
    sema_declare_local(b, stmt->as.var.name, type, stmt->as.var.is_mut);
    if (closure && !sema_name_is(stmt->as.var.name, "_")) b->locals.items[b->locals.count - 1].closure = true;
}

void sema_check_expr_stmt(Body *b, Expr *e) {
    if (e->kind == EXPR_MATCH) {
        e->type = check_match(b, e, NULL, true);
        return;
    }
    Type *type = sema_check_expr(b, e, NULL);
    bool is_call = e->kind == EXPR_CALL || (e->kind == EXPR_TRY && e->as.inner->kind == EXPR_CALL);
    if (!is_call) {
        Diagnostic *d = sema_error_expr(b, "E0348", e, "this expression has no effect");
        diag_help(d, "an expression statement is a call or a `match`; assign the value, or remove it");
    } else if (e->kind == EXPR_CALL && type->tag == T_RESULT) {
        Diagnostic *d = sema_error_expr(b, "E0348", e, "the result of this call must be used");
        diag_help(d, "handle the error with `try` or `match`, or store the result in a variable");
    }
}

static bool is_chars_call(const Expr *e) {
    return e->kind == EXPR_CALL && e->as.call.arg_count == 0 && e->as.call.callee->kind == EXPR_FIELD &&
           sema_name_is(e->as.call.callee->as.field.name, "chars");
}

static void check_for_in(Body *b, Stmt *stmt) {
    Type *declared = stmt->as.for_in.type ? sema_resolve_type(b->s, b->unit, stmt->as.for_in.type, POSITION_VALUE) : NULL;
    Expr *iterable = stmt->as.for_in.iterable;
    Type *element = sema_error_type(b);
    if (iterable->kind == EXPR_RANGE) {
        Type *lt, *rt;
        sema_check_pair(b, iterable->as.range.start, iterable->as.range.end, declared, &lt, &rt);
        iterable->type = lt;
        if (!sema_is_error(lt) && !sema_is_error(rt) && (!is_integer(lt) || lt != rt)) {
            TypeNames n = {0};
            sema_error_expr(b, "E0352", iterable, "a range needs two integers of the same type, found `%s` and `%s`", sema_tn(&n, lt),
                       sema_tn(&n, rt));
            sema_release_names(&n);
        } else {
            element = lt;
        }
    } else {
        Type *type = NULL;
        if (is_chars_call(iterable)) {
            /* `for (char c in s.chars())`: the code points of a string (spec §4.1) */
            Expr *callee = iterable->as.call.callee;
            Type *text = sema_check_expr(b, callee->as.field.object, NULL);
            if (text->tag == T_POINTER) text = text->as.inner;
            if (text->tag == T_STR || sema_is_error(text)) {
                callee->resolution = RESOLVED_BUILTIN;
                callee->target = "chars";
                iterable->type = text;
                element = sema_is_error(text) ? text : sema_builtin(b, "char");
            } else {
                type = sema_check_expr(b, iterable, NULL); /* a `chars()` method of a struct or class */
            }
        } else {
            type = sema_check_expr(b, iterable, NULL);
        }
        Type *base = !type ? sema_error_type(b) : type->tag == T_POINTER ? type->as.inner : type;
        if (!type) {
            /* already handled */
        } else if (base->tag == T_ARRAY) {
            element = base->as.array.element;
        } else if (base->tag == T_SLICE) {
            element = base->as.inner;
        } else if (!sema_is_error(base)) {
            TypeNames n = {0};
            Diagnostic *d = sema_error_expr(b, "E0352", iterable, "a `%s` cannot be iterated", sema_tn(&n, base));
            diag_help(d, "`for ... in` loops over a range `a..b`, an array or a slice");
            sema_release_names(&n);
        }
    }
    if (declared && declared != element && !sema_is_error(element)) {
        TypeNames n = {0};
        const TypeExpr *te = stmt->as.for_in.type;
        sema_error_at(b, "E0331", te->offset, te->length, "the elements are `%s`, not `%s`", sema_tn(&n, element), sema_tn(&n, declared));
        sema_release_names(&n);
    }
    size_t mark = b->locals.count;
    b->loop_depth++; /* the loop variable is new at each iteration */
    sema_declare_local(b, stmt->as.for_in.name, declared ? declared : element, stmt->as.for_in.is_mut);
    sema_check_block(b, stmt->as.for_in.body);
    b->loop_depth--;
    b->locals.count = mark;
}

static void check_return(Body *b, Stmt *stmt) {
    if (b->defer_depth > 0) {
        sema_error_at(b, "E0344", stmt->offset, stmt->length, "`return` is not allowed in `defer`");
        return;
    }
    Expr *value = stmt->as.expr;
    if (!b->ret) { /* a lambda whose type is not given: its first `return` says it */
        b->ret = value ? sema_check_expr(b, value, NULL) : sema_void_type(b);
        if (value) {
            sema_consume(b, value);
            sema_check_escape(b, value, "returned");
            check_dangling(b, value);
        }
        return;
    }
    if (value) {
        if (b->ret->tag == T_VOID) {
            sema_check_expr(b, value, NULL);
            Diagnostic *d = sema_error_expr(b, "E0346", value, "`%.*s` returns nothing", NAME_ARGS(b->fn_name));
            diag_help(d, "remove the value, or declare the return type");
            return;
        }
        sema_check_expect(b, value, b->ret);
        sema_consume(b, value);
        sema_check_escape(b, value, "returned");
        check_dangling(b, value);
    } else if (b->ret->tag != T_VOID && !sema_is_error(b->ret)) {
        TypeNames n = {0};
        Diagnostic *d = sema_error_at(b, "E0346", stmt->offset, stmt->length, "`%.*s` must return a `%s`",
                                 NAME_ARGS(b->fn_name), sema_tn(&n, b->ret));
        if (b->ret->tag == T_RESULT && b->ret->as.result.value->tag == T_VOID) diag_help(d, "write `return ok();`");
        sema_release_names(&n);
    }
}

static void check_stmt(Body *b, Stmt *stmt) {
    Type *bool_type = sema_builtin(b, "bool");
    switch (stmt->kind) {
    case STMT_BLOCK:
        sema_check_block(b, stmt);
        break;
    case STMT_VAR:
        check_var(b, stmt);
        break;
    case STMT_ASSIGN:
        check_assign(b, stmt);
        break;
    case STMT_EXPR:
        sema_check_expr_stmt(b, stmt->as.expr);
        break;
    case STMT_IF:
        sema_check_expect(b, stmt->as.if_.cond, bool_type);
        sema_check_block(b, stmt->as.if_.then_block);
        if (stmt->as.if_.else_stmt) check_stmt(b, stmt->as.if_.else_stmt);
        break;
    case STMT_WHILE:
        sema_check_expect(b, stmt->as.while_.cond, bool_type);
        b->loop_depth++;
        sema_check_block(b, stmt->as.while_.body);
        b->loop_depth--;
        break;
    case STMT_FOR: {
        size_t mark = b->locals.count;
        if (stmt->as.for_.init) check_stmt(b, stmt->as.for_.init);
        if (stmt->as.for_.cond) sema_check_expect(b, stmt->as.for_.cond, bool_type);
        if (stmt->as.for_.step) check_stmt(b, stmt->as.for_.step);
        b->loop_depth++;
        sema_check_block(b, stmt->as.for_.body);
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
        if (b->defer_depth > 0) sema_error_at(b, "E0344", stmt->offset, stmt->length, "`%s` is not allowed in `defer`", word);
        else if (b->loop_depth == 0) sema_error_at(b, "E0344", stmt->offset, stmt->length, "`%s` outside of a loop", word);
        break;
    }
    case STMT_DEFER:
        b->defer_depth++;
        check_stmt(b, stmt->as.inner);
        b->defer_depth--;
        break;
    case STMT_UNSAFE:
        b->unsafe_depth++;
        sema_check_block(b, stmt->as.inner);
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
    b.in_init = self_info && sema_name_is(fn->name, "init");
    for (size_t i = 0; i < fn->param_count; i++) {
        Local param = { fn->params[i].name, typed ? type->as.fn.params[i] : s->types.error, fn->params[i].is_mut,
                        false, 0, 0, false };
        SOSU_VEC_PUSH(&b.locals, param); /* duplicates were reported with the declarations (E0319) */
    }
    for (size_t i = 0; i < fn->contract_count; i++) {
        b.in_ensures = fn->contracts[i].kind == CONTRACT_ENSURES;
        sema_check_expect(&b, fn->contracts[i].expr, sema_builtin(&b, "bool"));
    }
    b.in_ensures = false;
    sema_check_block(&b, fn->body);
    if (b.ret->tag != T_VOID && !sema_is_error(b.ret) && !sema_always_returns(fn->body)) {
        Diagnostic *d = sema_error_at(&b, "E0346", fn->name.offset, fn->name.length,
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
    sema_check_expect(&b, field->decl->init, field->type);
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
        if (e->resolution == RESOLVED_CONST || e->type == NULL || sema_is_error(e->type)) return;
        if (e->resolution == RESOLVED_BUILTIN && e->kind == EXPR_NAME &&
            (sema_name_is(e->as.name, "KB") || sema_name_is(e->as.name, "MB") || sema_name_is(e->as.name, "GB"))) {
            return;
        }
        break;
    default: break;
    }
    Diagnostic *d = sema_error_expr(b, "E0367", e, "the value of a constant is computed when the program is compiled");
    diag_help(d, "use literals, operators, `as` and other constants; a value computed at run time is returned by "
                 "a function");
}

void sema_check_bodies(Sema *s) {
    for (size_t i = 0; i < s->consts.count; i++) {
        ConstInfo *c = s->consts.items[i];
        if (!c->type || !c->decl->as.const_.value) continue;
        Body b;
        init_body(&b, s, c->unit, NULL, c->decl->as.const_.name, s->types.void_, 0);
        sema_check_expect(&b, c->decl->as.const_.value, c->type);
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
            sema_check_expect(&b, field->decl->init, field->type);
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
            sema_check_block(&b, decl->as.test.body);
            free(b.locals.items);
        }
    }
}
