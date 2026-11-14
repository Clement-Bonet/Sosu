#include "sema_body.h"

#include <stdlib.h>
#include <string.h>

/* Checks function bodies (spec §5.3, §7). Each expression is checked against an
   optional expected type, which number literals, `none`, `ok(...)`, `err(...)` and
   bare enum variants take; the resulting type is stored in Expr.type. */

static void check_assignable(Body *b, Expr *target);
static void check_stmt(Body *b, Stmt *stmt);

static const char *const builtin_names[] = { "some", "none", "ok", "err", "assert", "min", "max", "abs", "KB", "MB", "GB",
                                             "wrapping_add", "wrapping_sub", "wrapping_mul", "hash", "size_of", "offset_of" };

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

Diagnostic *sema_warning_expr(Body *b, const char *code, const Expr *e, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    Diagnostic *d = sema_vwarning(b->s, b->unit, code, e->offset, e->length, fmt, args);
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
/* True if a function of type `from` can be given where `to` is expected: they differ only by
   `keeps`, and it keeps no parameter that the callers do not know of (spec §6.1). */
static bool fn_fits(Body *b, const Type *from, const Type *to) {
    if (from->tag != T_FN || to->tag != T_FN || (from->as.fn.keeps & ~to->as.fn.keeps)) return false;
    return type_fn(&b->s->types, from->as.fn.params, from->as.fn.param_count, from->as.fn.ret, from->as.fn.effects,
                   to->as.fn.keeps) == to;
}

Type *sema_check_expect(Body *b, Expr *e, Type *expected) {
    Type *type = sema_check_expr(b, e, expected);
    if (type != expected && !fn_fits(b, type, expected)) sema_report_mismatch(b, e, expected, type);
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
    Local local = {0};
    local.name = name;
    local.type = type;
    local.is_mut = is_mut;
    local.loop_depth = b->loop_depth;
    SOSU_VEC_PUSH(&b->locals, local);
}

/* The variable or parameter of the function that holds the place `e`: the variable itself, or a
   field or an element of it, reached without going through a pointer or a view; else NULL. */
static Local *storage_local(Body *b, const Expr *e) {
    switch (e->kind) {
    case EXPR_NAME:
        return sema_find_local(b, e->as.name);
    case EXPR_FIELD: {
        const Type *object = e->as.field.object->type;
        return object && object->tag != T_POINTER ? storage_local(b, e->as.field.object) : NULL;
    }
    case EXPR_INDEX: {
        const Type *object = e->as.index.object->type;
        return object && object->tag == T_ARRAY ? storage_local(b, e->as.index.object) : NULL;
    }
    default:
        return NULL;
    }
}

static bool in_local_storage(Body *b, const Expr *e) {
    return storage_local(b, e) != NULL;
}

/* The first part of `e` that would leave a pointer into the function once it returns: `&x`,
   `&x.f`, `&a[i]`, `a[i..j]` of a local array, or a variable that was given one (even later
   given another value: the check does not follow the order of the code), also inside `some`,
   `ok`, `err`, a literal or `?:`. A pointer stored in memory that lasts longer is not followed:
   the regions after 1.0 will. */
static const Expr *dangling_part(Body *b, const Expr *e) {
    switch (e->kind) {
    case EXPR_NAME: {
        const Local *local = sema_find_local(b, e->as.name);
        return local && local->points_inside ? e : NULL;
    }
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
    if (part->kind == EXPR_NAME) {
        Diagnostic *d = sema_error_expr(b, "E0371", part, "`%.*s` returned would point into this function, which ends",
                                        NAME_ARGS(part->as.name));
        diag_help(d, "it was given the address of a local variable or a view of a local array: return the value "
                     "itself, or memory from an arena (`arena.alloc`, `arena.alloc_slice`)");
        return;
    }
    bool view = part->kind == EXPR_INDEX;
    Diagnostic *d = sema_error_expr(b, "E0371", part, "%s returned would point into this function, which ends",
                               view ? "a view of a local array" : "the address of a local variable");
    diag_help(d, view ? "return the array itself, or a view of memory from an arena (`arena.alloc_slice`)"
                      : "return the value itself, or a pointer to memory from an arena (`arena.alloc`)");
}

/* ---- Regions, stage 1 (spec §6.1): a pointer into a local arena, used after its `free()` ---- */

/* The local `Arena` that `object` names, if any: `a` in `a.free()`, not a pointer to an arena. */
static Local *local_arena(Body *b, const Expr *object) {
    if (object->kind != EXPR_NAME || object->resolution != RESOLVED_LOCAL || !object->type ||
        object->type->tag != T_ARENA) {
        return NULL;
    }
    return sema_find_local(b, object->as.name);
}

/* The `Arena*` parameter that `object` names, if any: `a` in `a.alloc(T)` for a parameter `Arena* a`. */
static Local *param_arena(Body *b, const Expr *object) {
    if (object->kind != EXPR_NAME || object->resolution != RESOLVED_LOCAL || !object->type ||
        object->type->tag != T_POINTER || object->type->as.inner->tag != T_ARENA) {
        return NULL;
    }
    Local *local = sema_find_local(b, object->as.name);
    return local && local->is_param ? local : NULL;
}

/* The origin "the caller": memory that outlives the call (`self`, what is reached through it or
   through a parameter). Never the offset of a declaration. A parameter is an origin of its own:
   the value the caller gave, which only a `keeps` parameter may store (rule 4). */
#define ORIGIN_CALLER SIZE_MAX
/* What is reached through the parameter `i` (< 64): the caller's, but it says which argument. */
#define ORIGIN_VIA(i) (SIZE_MAX - 1 - (size_t)(i))

static const FnDecl *called(const Expr *e, const Expr **receiver, bool *self_receiver, size_t *first);
static void add_origins(Body *b, Local *to, const Expr *e);
static ReturnSummary *summary_of(Sema *s, const FnDecl *decl, bool create);
static void summarize_path(Body *b, ReturnSummary *summary, ArenaPath **list, size_t root, const Name *names, int count);
static void add_path(Sema *s, ReturnSummary *summary, ArenaPath **list, int param, const Name *names, int count);

/* An arena in a field: the place `i` of the function. */
#define ORIGIN_PLACE(i) (SIZE_MAX - 65 - (size_t)(i))

static bool is_via(size_t origin) { return origin != ORIGIN_CALLER && origin >= SIZE_MAX - 64; }

static bool is_place_origin(size_t origin) { return origin > SIZE_MAX / 2 && origin < SIZE_MAX - 64; }

typedef enum {
    ORIGIN_OTHER,
    ORIGIN_IS_CALLER,
    ORIGIN_LOCAL_ARENA, /* a local `Arena`, also one whose block has ended */
    ORIGIN_PARAM_ARENA, /* an `Arena*` parameter */
    ORIGIN_PARAM_VALUE, /* another parameter: the value it was given */
} OriginKind;

static bool is_arena_pointer(const Type *type) {
    return type->tag == T_POINTER && type->as.inner->tag == T_ARENA;
}

/* What `origin` is; `*local` is its variable (the arena, or the parameter), when it is still declared. */
static OriginKind origin_kind(Body *b, size_t origin, const Local **local) {
    *local = NULL;
    if (origin == ORIGIN_CALLER || is_via(origin)) return ORIGIN_IS_CALLER;
    if (is_place_origin(origin)) return ORIGIN_OTHER; /* only the free rule looks at it */
    for (size_t i = 0; i < b->locals.count; i++) {
        if (b->locals.items[i].name.offset == origin) *local = &b->locals.items[i];
    }
    if (!*local || (*local)->type->tag == T_ARENA) return ORIGIN_LOCAL_ARENA;
    if (!(*local)->is_param) return ORIGIN_OTHER;
    return is_arena_pointer((*local)->type) ? ORIGIN_PARAM_ARENA : ORIGIN_PARAM_VALUE;
}

static void add_to(Body *b, OriginSet *set, size_t arena) {
    for (int i = 0; i < set->count; i++) {
        if (set->items[i] == arena) return;
    }
    if (set->count == set->cap) {
        int cap = set->cap ? set->cap * 2 : 4;
        size_t *items = arena_alloc(&b->s->arena, (size_t)cap * sizeof *items);
        if (set->count) memcpy(items, set->items, (size_t)set->count * sizeof *items);
        set->items = items;
        set->cap = cap;
    }
    set->items[set->count++] = arena;
}

static void add_origin(Body *b, Local *to, size_t arena) { add_to(b, &to->origins, arena); }

/* The local just declared is a parameter of a lambda: an origin of its own, as for a function. */
void sema_lambda_param(Body *b, bool is_keeps) {
    Local *param = &b->locals.items[b->locals.count - 1];
    param->is_param = true;
    param->is_keeps = is_keeps;
    if (param->type->tag != T_ARENA && !sema_is_send(param->type)) add_origin(b, param, param->name.offset);
}

/* The last step of the path `e`: `f` in `x.f`, or in `f` for a field of `self`. */
static bool path_step(const Expr *e, Name *name, const Expr **rest) {
    if (e && e->kind == EXPR_FIELD) {
        *name = e->as.field.name;
        *rest = e->as.field.object;
        return true;
    }
    if (e && e->kind == EXPR_NAME && e->resolution == RESOLVED_SELF_FIELD) {
        *name = e->as.name;
        *rest = NULL;
        return true;
    }
    return false;
}

/* The path of `e`, as a variable or `self` and the fields from it: `s.a.b`, `self.a`, `&s.a`.
   A variable given a path when it was declared stands for that path. */
static bool expr_path(Body *b, const Expr *e, size_t *root, const Name **names, int *count) {
    while (e->kind == EXPR_UNARY && (e->as.unary.op == TOK_AMP || e->as.unary.op == TOK_STAR)) e = e->as.unary.operand;
    const Expr *start = e;
    Name name;
    int steps = 0;
    while (path_step(start, &name, &start)) steps++;
    const Name *prefix = NULL;
    int before = 0;
    *root = SIZE_MAX;
    if (start && start->kind != EXPR_SELF) {
        if (start->kind != EXPR_NAME || start->resolution != RESOLVED_LOCAL) return false;
        const Local *local = sema_find_local(b, start->as.name);
        if (!local) return false;
        *root = local->name.offset;
        if (local->aliased) {
            *root = local->alias_root;
            prefix = local->alias_names;
            before = local->alias_count;
        }
    }
    *count = before + steps;
    Name *path = *count ? arena_alloc(&b->s->arena, (size_t)*count * sizeof *path) : NULL;
    if (before) memcpy(path, prefix, (size_t)before * sizeof *path);
    start = e;
    for (int i = *count - 1; i >= before; i--) path_step(start, &path[i], &start);
    *names = path;
    return true;
}

static ArenaPlace *find_place(Body *b, size_t root, const Name *names, int count) {
    for (ArenaPlace *place = b->places; place; place = place->next) {
        bool same = place->root == root && place->count == count;
        for (int i = 0; same && i < count; i++) same = sema_name_eq(place->names[i], names[i]);
        if (same) return place;
    }
    ArenaPlace *place = arena_alloc(&b->s->arena, sizeof *place);
    place->root = root;
    place->names = names;
    place->count = count;
    place->index = b->place_count++;
    place->next = b->places;
    b->places = place;
    return place;
}

/* The place of the arena in a field that `e` names, or NULL: `self.temp`, `doc.pages.arena`. */
static ArenaPlace *place_of(Body *b, const Expr *e) {
    if (!e->type || e->type->tag != T_ARENA) return NULL;
    size_t root;
    const Name *names;
    int count;
    if (!expr_path(b, e, &root, &names, &count) || count == 0) return NULL;
    return find_place(b, root, names, count);
}

static size_t place_origin(Body *b, const Expr *e) {
    const ArenaPlace *place = place_of(b, e);
    return place ? ORIGIN_PLACE(place->index) : 0;
}

/* Which parameter the variable declared at `root` is: its index, -1 for `self`, or -2. */
static int root_param(const Body *b, size_t root) {
    if (root == SIZE_MAX) return -1;
    for (size_t i = 0; i < b->locals.count && b->locals.items[i].is_param; i++) {
        if (b->locals.items[i].name.offset == root) return (int)i;
    }
    return -2;
}

static bool same_names(const Name *x, const Name *y, int count) {
    for (int i = 0; i < count; i++) {
        if (!sema_name_eq(x[i], y[i])) return false;
    }
    return true;
}

/* The class or struct reached by the first `count` names from `root`, or NULL. */
static TypeInfo *holder_info(Body *b, size_t root, const Name *names, int count) {
    const Type *type = NULL;
    TypeInfo *info = root == SIZE_MAX ? b->self_info : NULL;
    for (size_t i = 0; root != SIZE_MAX && i < b->locals.count; i++) {
        if (b->locals.items[i].name.offset == root) type = b->locals.items[i].type;
    }
    for (int i = 0;; i++) {
        while (type && type->tag == T_POINTER) type = type->as.inner;
        if (type) info = type->tag == T_STRUCT || type->tag == T_CLASS ? type->as.info : NULL;
        if (!info || i == count) return info;
        FieldInfo *field = sema_find_field(info, names[i]);
        if (!field) return NULL;
        type = field->type;
    }
}

/* The fields of a value that hold memory of its arena `root`, `names`, freed at `offset`, are dead. */
static void kill_fields(Body *b, size_t root, const Name *names, int count, size_t offset) {
    for (const TypeInfo *t = holder_info(b, root, names, count - 1); t; t = t->parent) {
        for (size_t i = 0; i < t->field_count; i++) {
            for (const ArenaLink *link = t->fields[i].arenas; link; link = link->next) {
                if (!sema_name_eq(link->arena, names[count - 1])) continue;
                DeadField *dead = arena_alloc(&b->s->arena, sizeof *dead);
                Name *path = arena_alloc(&b->s->arena, (size_t)count * sizeof *path);
                memcpy(path, names, (size_t)(count - 1) * sizeof *path);
                path[count - 1] = t->fields[i].name;
                dead->root = root;
                dead->names = path;
                dead->count = count;
                dead->arena = names[count - 1];
                dead->offset = offset;
                dead->next = b->dead;
                b->dead = dead;
                break;
            }
        }
    }
}

/* `e` is read: a field of a freed arena cannot be, until it is given a value (spec §6.1, rule 1). */
void sema_check_dead(Body *b, const Expr *e) {
    if (!b->dead || e == b->assign_target || b->lambda || b->unsafe_depth > 0) return;
    size_t root;
    const Name *names;
    int count;
    if (!expr_path(b, e, &root, &names, &count)) return;
    for (const DeadField *dead = b->dead; dead; dead = dead->next) {
        if (dead->root != root || dead->count != count || !same_names(dead->names, names, count)) continue;
        Diagnostic *d = dead->arena.length
            ? sema_error_expr(b, "E0372", e, "`%.*s` holds memory of the arena `%.*s`, freed on line %zu",
                              NAME_ARGS(names[count - 1]), NAME_ARGS(dead->arena), sema_line_of(b->unit, dead->offset))
            : sema_error_expr(b, "E0372", e, "`%.*s` holds memory of an arena freed by the call on line %zu",
                              NAME_ARGS(names[count - 1]), sema_line_of(b->unit, dead->offset));
        diag_help(d, "give it a value first, in memory of a new arena (spec §6.1)");
        return;
    }
}

/* The field at this path gets a value: it is no longer dead; if it is an arena, the fields of that
   arena must get one too. */
static void dead_written(Body *b, size_t root, const Name *names, int count) {
    Name last = names[count - 1];
    for (DeadField **dead = &b->dead; *dead;) {
        bool same = (*dead)->root == root && (*dead)->count == count && same_names((*dead)->names, names, count - 1);
        if (same && sema_name_eq((*dead)->names[count - 1], last)) {
            *dead = (*dead)->next;
            continue;
        }
        if (same && sema_name_eq((*dead)->arena, last)) (*dead)->renewed = true;
        dead = &(*dead)->next;
    }
}

/* Adds this path to what the function being summarized writes. A field written is no reason to
   make the summaries of its callers again: that would check nearly every function twice. */
static void summarize_write(Body *b, size_t root, const Name *names, int count) {
    if (!b->s->summarizing || !b->fn_decl || b->lambda) return;
    ReturnSummary *summary = summary_of(b->s, b->fn_decl, true);
    bool grew = b->s->summary_grew;
    int pass = summary->grew_pass;
    summarize_path(b, summary, &summary->writes, root, names, count);
    b->s->summary_grew = grew;
    summary->grew_pass = pass;
}

/* `target` = `value`, into a field; in the pass that makes the summaries, a field given memory of
   an arena of the same value belongs to that arena. A field that is an array is not followed: its
   elements come and go on their own (a flag says which are in use). */
static void field_written(Body *b, const Expr *target, const Expr *value) {
    size_t root;
    const Name *names;
    int count;
    Name last;
    const Expr *rest;
    if (!path_step(target, &last, &rest) || !expr_path(b, target, &root, &names, &count) || count == 0) return;
    dead_written(b, root, names, count);
    summarize_write(b, root, names, count);
    if (!b->s->summarizing || !value->type || sema_is_send(value->type)) return;
    TypeInfo *info = holder_info(b, root, names, count - 1);
    FieldInfo *field = info ? sema_find_field(info, last) : NULL;
    if (!field || field->type->tag == T_ARRAY) return;
    Local held = {0};
    add_origins(b, &held, value);
    for (const ArenaPlace *place = b->places; place; place = place->next) {
        bool here = false;
        for (int i = 0; i < held.origins.count; i++) here = here || held.origins.items[i] == ORIGIN_PLACE(place->index);
        if (!here || place->root != root || place->count != count || !same_names(place->names, names, count - 1)) continue;
        Name arena = place->names[count - 1];
        bool known = false;
        for (const ArenaLink *link = field->arenas; link; link = link->next) known = known || sema_name_eq(link->arena, arena);
        if (known) continue;
        ArenaLink *link = arena_alloc(&b->s->arena, sizeof *link);
        link->arena = arena;
        link->next = field->arenas;
        field->arenas = link;
    }
}

/* Adds to the summary list `list` the arena at this path, if it starts from a parameter. */
static void summarize_path(Body *b, ReturnSummary *summary, ArenaPath **list, size_t root, const Name *names, int count) {
    int param = root_param(b, root);
    if (param == -2 || count > 8) return; /* a recursive call would make the path grow */
    add_path(b->s, summary, list, param, names, count);
}

static void add_path(Sema *s, ReturnSummary *summary, ArenaPath **list, int param, const Name *names, int count) {
    for (const ArenaPath *path = *list; path; path = path->next) {
        bool same = path->root == param && path->count == count;
        for (int i = 0; same && i < count; i++) same = sema_name_eq(path->names[i], names[i]);
        if (same) return;
    }
    ArenaPath *path = arena_alloc(&s->arena, sizeof *path);
    path->root = param;
    path->names = names;
    path->count = count;
    path->next = *list;
    *list = path;
    summary->grew_pass = s->summary_pass;
    s->summary_grew = true;
}

/* The arena that `path`, of the function that `e` calls, is at the call: its place in this function. */
static bool path_at_call(Body *b, const Expr *e, const ArenaPath *path, size_t *root, const Name **names, int *count) {
    const Expr *receiver;
    bool self_receiver;
    size_t first;
    called(e, &receiver, &self_receiver, &first);
    const Name *base = NULL;
    int steps = 0;
    if (path->root == -1 && self_receiver) {
        *root = SIZE_MAX;
    } else {
        const Expr *from = path->root == -1 ? (first ? NULL : receiver)
                         : first + (size_t)path->root < e->as.call.arg_count ? e->as.call.args[first + (size_t)path->root] : NULL;
        if (!from || !expr_path(b, from, root, &base, &steps)) return false;
    }
    *count = steps + path->count;
    Name *all = *count ? arena_alloc(&b->s->arena, (size_t)*count * sizeof *all) : NULL;
    if (steps) memcpy(all, base, (size_t)steps * sizeof *all);
    if (path->count) memcpy(all + steps, path->names, (size_t)path->count * sizeof *all);
    *names = all;
    return true;
}

/* The local arena `origin` left this function, for memory that outlasts it. */
static void arena_left(Body *b, size_t origin) {
    for (size_t i = 0; i < b->locals.count; i++) {
        if (b->locals.items[i].name.offset == origin) b->locals.items[i].moved_out = true;
    }
}

/* Gives `to` the local arenas that `e` holds by value, moved into it: the arena `a` itself, a
   variable that holds some, also inside a literal, `some`, `ok`, `err` or `?:`; and those given to
   an `a.new(T, ...)`, which the object keeps (`a.new(Counter, a)`: an object in its own arena). */
static void add_owned(Body *b, Local *to, const Expr *e) {
    if (e->kind == EXPR_CALL && e->as.call.callee->kind == EXPR_FIELD && e->as.call.callee->resolution == RESOLVED_BUILTIN &&
        e->as.call.callee->target && strcmp((const char *)e->as.call.callee->target, "arena.new") == 0) {
        for (size_t i = 1; i < e->as.call.arg_count; i++) add_owned(b, to, e->as.call.args[i]);
        for (int i = 0; i < to->owned.count; i++) arena_left(b, to->owned.items[i]); /* into an arena */
        return;
    }
    if (!e->type || !type_owns_resource(e->type)) return;
    switch (e->kind) {
    case EXPR_NAME: {
        const Local *local = e->resolution == RESOLVED_LOCAL ? sema_find_local(b, e->as.name) : NULL;
        if (!local || local == to) return;
        if (local->type->tag == T_ARENA) add_to(b, &to->owned, local->name.offset);
        for (int i = 0; i < local->owned.count; i++) add_to(b, &to->owned, local->owned.items[i]);
        return;
    }
    case EXPR_CALL: {
        const Expr *callee = e->as.call.callee;
        if (callee->kind != EXPR_NAME || sema_find_local(b, callee->as.name)) return;
        if (!sema_name_is(callee->as.name, "some") && !sema_name_is(callee->as.name, "ok") && !sema_name_is(callee->as.name, "err")) {
            return;
        }
        for (size_t i = 0; i < e->as.call.arg_count; i++) add_owned(b, to, e->as.call.args[i]);
        return;
    }
    case EXPR_STRUCT_LIT:
        for (size_t i = 0; i < e->as.struct_lit.field_count; i++) add_owned(b, to, e->as.struct_lit.fields[i].value);
        return;
    case EXPR_ARRAY:
        for (size_t i = 0; !e->as.array.repeat && i < e->as.array.count; i++) add_owned(b, to, e->as.array.items[i]);
        return;
    case EXPR_TERNARY:
        add_owned(b, to, e->as.ternary.then_expr);
        add_owned(b, to, e->as.ternary.else_expr);
        return;
    default:
        return;
    }
}

/* Gives `to` the local arenas whose memory `e` may hold: `a.alloc(...)` and the other
   allocations of `a`, what is reached through it (`p.next`, `v[i]`, `*p`), a variable that was
   given some, also inside `some`, `ok`, `err`, a literal or `?:`. A value of a type without
   pointers holds none; neither does what a function returns (stage 4). */

/* The summary of `decl`, made (empty, not complete) when `create`; NULL if there is none. */
static ReturnSummary *summary_of(Sema *s, const FnDecl *decl, bool create) {
    size_t *index = ptrmap_get(&s->summary_index, decl);
    if (index) return &s->summaries.items[*index];
    if (!create) return NULL;
    ReturnSummary empty = {0};
    SOSU_VEC_PUSH(&s->summaries, empty);
    ptrmap_put(&s->summary_index, decl, s->summaries.count - 1);
    return &s->summaries.items[s->summaries.count - 1];
}

/* The declaration called by `e` when its body is Sosu: a function (not C) or a method. */
static const FnDecl *called_decl(const Expr *e) {
    const Expr *callee = e->as.call.callee;
    if (!callee->target) return NULL;
    switch (callee->resolution) {
    case RESOLVED_FN: return ((const FnInfo *)callee->target)->is_extern ? NULL : ((const FnInfo *)callee->target)->decl;
    case RESOLVED_METHOD:
    case RESOLVED_SELF_METHOD: return ((const MethodInfo *)callee->target)->decl;
    default: return NULL;
    }
}

/* Gives `to` the origins of the memory that the place `e` is in: `&e` and `e[a..b]` are in that
   memory, they read nothing from it. So `&p.f` and `v[a..b]` have the origins of `p` and of `v`,
   a parameter staying that parameter, where the value of `p.f` or `v[i]` is the caller's. */
static void add_place(Body *b, Local *to, const Expr *e) {
    size_t place = place_origin(b, e);
    if (place) add_origin(b, to, place); /* `&self.temp` */
    const Expr *object = NULL;
    if (e->kind == EXPR_FIELD) object = e->as.field.object;
    if (e->kind == EXPR_INDEX) object = e->as.index.object;
    if (e->kind == EXPR_UNARY && e->as.unary.op == TOK_STAR) {
        add_origins(b, to, e->as.unary.operand);
    } else if (!object) {
        add_origins(b, to, e);
    } else if (object->type && (object->type->tag == T_POINTER || object->type->tag == T_SLICE)) {
        add_origins(b, to, object);
    } else {
        add_place(b, to, object);
    }
}

/* Gives `to` the origins of what is reached through the pointer or view `e`: those of `e`, a
   parameter becoming the caller (what is read in the memory of the caller is the caller's). */
static void add_through(Body *b, Local *to, const Expr *e) {
    Local held = {0};
    add_origins(b, &held, e);
    for (int i = 0; i < held.origins.count; i++) {
        const Local *local;
        if (origin_kind(b, held.origins.items[i], &local) != ORIGIN_PARAM_VALUE) {
            add_origin(b, to, held.origins.items[i]);
            continue;
        }
        size_t index = (size_t)(local - b->locals.items); /* the parameters come first */
        add_origin(b, to, index < 64 ? ORIGIN_VIA(index) : ORIGIN_CALLER);
    }
}

static void add_origins(Body *b, Local *to, const Expr *e) {
    if (!e->type || sema_is_send(e->type)) return;
    switch (e->kind) {
    case EXPR_SELF: add_origin(b, to, ORIGIN_CALLER); return;
    case EXPR_NAME: {
        if (e->resolution == RESOLVED_SELF_FIELD) {
            add_origin(b, to, ORIGIN_CALLER);
            return;
        }
        const Local *local = e->resolution == RESOLVED_LOCAL ? sema_find_local(b, e->as.name) : NULL;
        for (int i = 0; local && local != to && i < local->origins.count; i++) add_origin(b, to, local->origins.items[i]);
        return;
    }
    case EXPR_FIELD: {
        const Expr *object = e->as.field.object;
        if (object->type && object->type->tag == T_POINTER) add_through(b, to, object);
        else add_origins(b, to, object);
        return;
    }
    case EXPR_INDEX: {
        const Expr *object = e->as.index.object;
        if (e->as.index.index->kind == EXPR_RANGE) add_place(b, to, e);
        else if (object->type && (object->type->tag == T_POINTER || object->type->tag == T_SLICE)) add_through(b, to, object);
        else add_origins(b, to, object);
        return;
    }
    case EXPR_UNARY: {
        const Expr *operand = e->as.unary.operand;
        if (e->as.unary.op == TOK_STAR) {
            add_through(b, to, operand);
            return;
        }
        const Local *arena = e->as.unary.op == TOK_AMP ? local_arena(b, operand) : NULL;
        if (arena) add_origin(b, to, arena->name.offset); /* `&a`: what is allocated through it is of `a` */
        else if (e->as.unary.op == TOK_AMP) add_place(b, to, operand);
        else add_origins(b, to, operand);
        return;
    }
    case EXPR_TRY: add_origins(b, to, e->as.inner); return;
    case EXPR_CALL: {
        const Expr *callee = e->as.call.callee;
        if (callee->kind == EXPR_FIELD && callee->resolution == RESOLVED_BUILTIN && callee->target &&
            strncmp((const char *)callee->target, "arena.", 6) == 0) {
            const Expr *object = callee->as.field.object;
            const Local *arena = local_arena(b, object);
            if (!arena) arena = param_arena(b, object);
            if (arena) {
                add_origin(b, to, arena->name.offset);
                return;
            }
            size_t place = place_origin(b, object);
            if (place) add_origin(b, to, place);
            add_origins(b, to, object); /* an `Arena*` given another way: where it points */
            return;
        }
        if (callee->resolution == RESOLVED_FN && callee->target && ((const FnInfo *)callee->target)->is_extern) return;
        /* what a function returns comes from the arguments its summary names; without one (a
           value of function type, a body not checked yet), from any of them, the receiver of a
           method included; `some`, `ok` and `err` hold theirs */
        const FnDecl *decl = called_decl(e);
        const ReturnSummary *summary = decl ? summary_of(b->s, decl, false) : NULL;
        bool complete = summary && summary->complete;
        if (!complete || summary->receiver) {
            if (callee->resolution == RESOLVED_METHOD && callee->kind == EXPR_FIELD) {
                const Expr *object = callee->as.field.object;
                if (object->type && object->type->tag == T_POINTER) add_through(b, to, object);
                else add_origins(b, to, object);
            } else if (callee->resolution == RESOLVED_SELF_METHOD) {
                add_origin(b, to, ORIGIN_CALLER);
            }
        }
        for (size_t i = 0; i < e->as.call.arg_count; i++) {
            const Expr *arg = e->as.call.args[i];
            uint64_t bit = i < 64 ? (uint64_t)1 << i : 0;
            if (!complete || i >= 64 || (summary->values & bit)) add_origins(b, to, arg); /* a summary names 64 */
            if (complete && (summary->through & bit)) add_through(b, to, arg);
        }
        for (const ArenaPath *path = summary ? summary->returns : NULL; path; path = path->next) {
            size_t root;
            const Name *names;
            int count;
            if (path_at_call(b, e, path, &root, &names, &count) && count) {
                add_origin(b, to, ORIGIN_PLACE(find_place(b, root, names, count)->index));
            }
        }
        return;
    }
    case EXPR_STRUCT_LIT:
        for (size_t i = 0; i < e->as.struct_lit.field_count; i++) add_origins(b, to, e->as.struct_lit.fields[i].value);
        return;
    case EXPR_ARRAY:
        for (size_t i = 0; !e->as.array.repeat && i < e->as.array.count; i++) add_origins(b, to, e->as.array.items[i]);
        if (e->as.array.repeat) add_origins(b, to, e->as.array.repeat);
        return;
    case EXPR_TERNARY:
        add_origins(b, to, e->as.ternary.then_expr);
        add_origins(b, to, e->as.ternary.else_expr);
        return;
    default:
        return;
    }
}

/* The memory of the arena `origin`, named `name`, is gone: the variables below `end` that hold
   some can no longer be used. */
static void free_origin(Body *b, size_t origin, Name name, size_t end, size_t offset) {
    for (size_t i = 0; i < end; i++) {
        Local *local = &b->locals.items[i];
        for (int j = 0; j < local->origins.count; j++) {
            if (local->origins.items[j] != origin) continue;
            local->dangling = true;
            local->freed_arena = name;
            local->freed_offset = offset;
        }
    }
}

/* The arena `name` was freed, and with it what was moved into it: `set`. */
static void free_moved(Body *b, const OriginSet *set, Name name, size_t end, size_t offset) {
    for (int i = 0; i < set->count; i++) free_origin(b, set->items[i], name, end, offset);
}

/* The arena at this path was freed: the local arenas moved into the value it starts from too. */
static void free_held(Body *b, const ArenaPlace *place, size_t end, size_t offset) {
    Name name = place->names[place->count - 1];
    free_moved(b, &place->owned, name, end, offset);
    for (size_t i = 0; i < b->locals.count; i++) {
        if (b->locals.items[i].name.offset == place->root) free_moved(b, &b->locals.items[i].owned, name, end, offset);
    }
}

/* `target` = `value`, where `target` is not memory of the function: the local arenas in `value`
   leave it; an arena in a field goes on with them. */
static void arena_moved(Body *b, const Expr *target, const Expr *value) {
    if (!value->type || !type_owns_resource(value->type) || storage_local(b, target)) return;
    Local moved = {0};
    add_owned(b, &moved, value);
    ArenaPlace *place = place_of(b, target);
    for (int i = 0; i < moved.owned.count; i++) {
        arena_left(b, moved.owned.items[i]);
        if (place) add_to(b, &place->owned, moved.owned.items[i]);
    }
}

/* In the pass that makes the summaries: what `value`, returned, holds of the parameters. */
static void record_return(Body *b, const Expr *value) {
    if (!b->s->summarizing || !b->fn_decl || b->lambda) return;
    ReturnSummary *summary = summary_of(b->s, b->fn_decl, true);
    Local held = {0};
    add_origins(b, &held, value);
    for (int i = 0; i < held.origins.count; i++) {
        size_t origin = held.origins.items[i];
        if (is_place_origin(origin)) {
            for (const ArenaPlace *place = b->places; place; place = place->next) {
                if (ORIGIN_PLACE(place->index) == origin) summarize_path(b, summary, &summary->returns, place->root, place->names, place->count);
            }
        } else if (origin == ORIGIN_CALLER) {
            summary->receiver = true;
        } else if (is_via(origin)) {
            summary->through |= (uint64_t)1 << (SIZE_MAX - 1 - origin);
        } else {
            const Local *local;
            OriginKind kind = origin_kind(b, origin, &local);
            size_t index = local ? (size_t)(local - b->locals.items) : 64;
            if ((kind == ORIGIN_PARAM_VALUE || kind == ORIGIN_PARAM_ARENA) && index < 64) summary->values |= (uint64_t)1 << index;
        }
    }
}

/* True if `local` was declared in the innermost lambda being checked, or if there is none: what a
   lambda returns may be memory of its function, which it does not outlive (E0370). */
static bool in_this_body(const Body *b, const Local *local) {
    return !b->lambda || (size_t)(local - b->locals.items) >= b->lambda->base;
}

/* True if a `defer` already met frees the arena `origin`, a parameter or an arena in a field. */
static bool freed_by_defer(const Body *b, size_t origin, const Local *arena) {
    if (arena && arena->is_param) return arena->free_deferred;
    for (const ArenaPlace *place = b->places; place; place = place->next) {
        if (ORIGIN_PLACE(place->index) == origin) return place->free_deferred;
    }
    return false;
}

/* Regions, stage 2 (spec §6.1, rule 2, E0373): a returned value does not hold
   memory of a local arena, which ends with the function, unless that arena is returned with it
   (an object that owns its arena). Not followed in `unsafe`. */
static void check_returned_arena(Body *b, const Expr *value) {
    if (b->unsafe_depth > 0) return;
    Local held = {0};
    add_origins(b, &held, value);
    add_owned(b, &held, value);
    for (int i = 0; i < held.origins.count; i++) {
        bool owned = false;
        for (int j = 0; j < held.owned.count; j++) owned = owned || held.owned.items[j] == held.origins.items[i];
        const Local *arena;
        OriginKind kind = origin_kind(b, held.origins.items[i], &arena);
        if (freed_by_defer(b, held.origins.items[i], arena)) {
            Diagnostic *d = sema_error_expr(b, "E0373", value, "the returned value holds memory of an arena that a `defer` of this function frees");
            diag_help(d, "free the arena in the caller, or allocate what is returned in another arena (spec §6.1)");
            return;
        }
        if (owned || kind != ORIGIN_LOCAL_ARENA || (arena && !in_this_body(b, arena))) continue;
        Diagnostic *d = arena ? sema_error_expr(b, "E0373", value, "the returned value holds memory of `%.*s`, an arena of this function",
                                                  NAME_ARGS(arena->name))
                              : sema_error_expr(b, "E0373", value, "the returned value holds memory of an arena of this function");
        diag_help(d, "allocate it in an arena received as a parameter (`Arena*`), or return the arena with it, in the "
                     "same struct (spec §6.1)");
        return;
    }
}

/* Gives `to` the origins of the memory that `target` writes, and returns true; false when it is a
   variable of the function, or a field or an element of one: any pointer may go there. */
static bool add_destination(Body *b, Local *to, const Expr *target) {
    switch (target->kind) {
    case EXPR_SELF:
        add_origin(b, to, ORIGIN_CALLER);
        return true;
    case EXPR_NAME:
        if (target->resolution != RESOLVED_SELF_FIELD) return false;
        add_origin(b, to, ORIGIN_CALLER);
        return true;
    case EXPR_FIELD: {
        const Expr *object = target->as.field.object;
        if (object->type && object->type->tag == T_POINTER) {
            add_origins(b, to, object);
            return true;
        }
        return add_destination(b, to, object);
    }
    case EXPR_INDEX: {
        const Expr *object = target->as.index.object;
        if (target->as.index.index->kind == EXPR_RANGE) return false; /* not assignable (E0335) */
        if (object->type && (object->type->tag == T_SLICE || object->type->tag == T_POINTER)) {
            add_origins(b, to, object);
            return true;
        }
        return add_destination(b, to, object);
    }
    case EXPR_UNARY:
        if (target->as.unary.op != TOK_STAR) return false;
        add_origins(b, to, target->as.unary.operand);
        return true;
    default:
        return false;
    }
}

/* The parameter not marked `keeps` that `origin` was read through, if `dest` is memory that outlives
   the call and not reached through that parameter: what a parameter reaches is stored elsewhere
   only with `keeps` (spec §6.1). */
static const Local *read_unkept(Body *b, size_t origin, size_t dest) {
    if (!is_via(origin) || origin == dest) return NULL;
    const Local *param = &b->locals.items[SIZE_MAX - 1 - origin];
    const Local *into;
    OriginKind kind = origin_kind(b, dest, &into);
    if (param->is_keeps || kind == ORIGIN_OTHER || kind == ORIGIN_LOCAL_ARENA || into == param) return NULL;
    return param;
}

/* True if `origin`, read through `self`, goes to memory `dest` that is not reached through `self`,
   in a method that does not say `keeps self`. */
static bool self_unkept(Body *b, size_t origin, size_t dest) {
    if (origin != ORIGIN_CALLER || dest == ORIGIN_CALLER || !b->self_info || !b->fn_decl || b->fn_decl->keeps_self) return false;
    const Local *into;
    OriginKind kind = origin_kind(b, dest, &into);
    return kind == ORIGIN_IS_CALLER || kind == ORIGIN_PARAM_ARENA || kind == ORIGIN_PARAM_VALUE;
}

/* Regions, stage 3 (spec §6.1, rule 3, E0374): a pointer is stored only where
   it lasts no longer than what it points to. Into memory of the caller or of an arena, never
   memory of the function; into memory of the caller or of another arena, never memory of a
   local arena (unless that arena went into such memory); and not from one
   `Arena*` parameter into another. Not followed in `unsafe`. */
static void check_store(Body *b, const Expr *target, const Expr *value) {
    if (b->unsafe_depth > 0 || !value->type || sema_is_send(value->type)) return;
    Local dest = {0};
    if (!add_destination(b, &dest, target)) return;
    Local held = {0};
    add_origins(b, &held, value);
    bool inside = dangling_part(b, value) != NULL;
    const char *help = "store it in memory that lasts no longer than what it points to: a variable of this function, "
                       "or memory of the same arena (spec §6.1)";
    for (int i = 0; i < dest.origins.count; i++) {
        const Local *to_arena;
        OriginKind to = origin_kind(b, dest.origins.items[i], &to_arena);
        if (to == ORIGIN_OTHER) continue;
        if (to == ORIGIN_PARAM_VALUE) to = ORIGIN_IS_CALLER; /* memory the caller gave */
        if (inside) {
            Diagnostic *d = sema_error_expr(b, "E0374", value, "a pointer into this function is stored in memory that outlives it");
            diag_help(d, "%s", help);
            return;
        }
        for (int j = 0; j < held.origins.count; j++) {
            const Local *from_arena;
            OriginKind from = origin_kind(b, held.origins.items[j], &from_arena);
            bool same = held.origins.items[j] == dest.origins.items[i];
            if (from == ORIGIN_LOCAL_ARENA && !same && !(from_arena && from_arena->moved_out)) {
                Diagnostic *d = from_arena
                    ? sema_error_expr(b, "E0374", value, "memory of `%.*s`, an arena of this function, is stored where it may outlive it",
                                        NAME_ARGS(from_arena->name))
                    : sema_error_expr(b, "E0374", value, "memory of an arena of this function is stored where it may outlive it");
                diag_help(d, "%s", help);
                return;
            }
            if (from == ORIGIN_PARAM_VALUE && !from_arena->is_keeps && to != ORIGIN_LOCAL_ARENA) {
                Diagnostic *d = sema_error_expr(b, "E0376", value, "the parameter `%.*s` is stored beyond the call, but it is not marked `keeps`",
                                                  NAME_ARGS(from_arena->name));
                diag_help(d, "write `keeps` before its type, so that callers give it memory that lives as long (spec §6.1)");
                return;
            }
            if (from == ORIGIN_PARAM_ARENA && to == ORIGIN_IS_CALLER && !from_arena->is_keeps) {
                Diagnostic *d = sema_error_expr(b, "E0376", value, "the memory of the parameter `%.*s` is stored beyond the call, but it is not marked `keeps`",
                                                  NAME_ARGS(from_arena->name));
                diag_help(d, "write `keeps` before its type, so that callers know the arena must outlive what they give (spec §6.1)");
                return;
            }
            if (self_unkept(b, held.origins.items[j], dest.origins.items[i])) {
                Diagnostic *d = sema_error_expr(b, "E0376", value, "what `self` reaches is stored beyond the call, but the method does not say `keeps self`");
                diag_help(d, "write `keeps self` after its parameters, so that callers give it what lives as long (spec §6.1)");
                return;
            }
            const Local *read = read_unkept(b, held.origins.items[j], dest.origins.items[i]);
            if (read) {
                Diagnostic *d = sema_error_expr(b, "E0376", value, "what the parameter `%.*s` reaches is stored beyond the call, but it is not marked `keeps`",
                                                  NAME_ARGS(read->name));
                diag_help(d, "write `keeps` before its type, so that callers give it memory that lives as long (spec §6.1)");
                return;
            }
            if (from == ORIGIN_PARAM_ARENA && to == ORIGIN_PARAM_ARENA && !same) {
                Diagnostic *d = sema_error_expr(b, "E0374", value, "memory of the arena `%.*s` is stored in memory of the arena `%.*s`",
                                                  NAME_ARGS(from_arena->name), NAME_ARGS(to_arena->name));
                diag_help(d, "two arenas received as parameters may not live as long as each other: use one arena (spec §6.1)");
                return;
            }
        }
    }
}

/* The receiver of the call `e`, and its parameters (those of `init` for `a.new(T, ...)`, whose
   receiver is the arena); `*first` is the first argument of the parameters. */
static const FnDecl *called(const Expr *e, const Expr **receiver, bool *self_receiver, size_t *first) {
    const Expr *callee = e->as.call.callee;
    *receiver = NULL;
    *self_receiver = false;
    *first = 0;
    switch (callee->resolution) {
    case RESOLVED_FN: return callee->target ? ((const FnInfo *)callee->target)->decl : NULL;
    case RESOLVED_SELF_METHOD:
        *self_receiver = true;
        return callee->target ? ((const MethodInfo *)callee->target)->decl : NULL;
    case RESOLVED_METHOD:
        if (callee->kind == EXPR_FIELD) *receiver = callee->as.field.object;
        return callee->target ? ((const MethodInfo *)callee->target)->decl : NULL;
    case RESOLVED_BUILTIN: {
        if (!callee->target || strcmp((const char *)callee->target, "arena.new") != 0 || e->as.call.arg_count == 0) return NULL;
        const Type *type = e->as.call.args[0]->type;
        if (!type || (type->tag != T_CLASS && type->tag != T_STRUCT)) return NULL;
        Name init_name = { "init", 4, 0 };
        const MethodInfo *init = sema_find_method(type->as.info, init_name);
        if (callee->kind == EXPR_FIELD) *receiver = callee->as.field.object;
        *first = 1;
        return init ? init->decl : NULL;
    }
    default:
        return NULL;
    }
}

/* True if a pointer can be stored into what a value of `type` reaches: not a `str`, nor a view or a
   pointer of values without pointers (`ui8[]`, `i32*`). */
static bool may_receive_pointer(const Type *type) {
    switch (type->tag) {
    case T_STR: return false;
    case T_POINTER:
    case T_SLICE: return !sema_is_send(type->as.inner);
    case T_OPTION: return may_receive_pointer(type->as.inner);
    default: return !sema_is_send(type);
    }
}

/* Gives `to` the memory that the callee may store into: the receiver (an arena for `a.new`). */
static void add_receiver(Body *b, Local *to, const Expr *receiver, bool self_receiver) {
    if (self_receiver) add_origin(b, to, ORIGIN_CALLER);
    if (!receiver || !receiver->type) return;
    const Local *arena = local_arena(b, receiver);
    if (!arena) arena = param_arena(b, receiver);
    if (arena) add_origin(b, to, arena->name.offset);
    else if (receiver->type->tag == T_POINTER) add_through(b, to, receiver);
    /* a value of the function: what it stores lasts no longer than the function */
}

/* A value marked `keeps`, of origins `kept`, may be stored in `others`: reports what ends sooner. */
static bool kept_too_short(Body *b, const Expr *where, Name param_name, const Local *kept, bool inside, const Local *others,
                           const Local *direct, size_t own_arena) {
    for (int k = 0; k < others->origins.count; k++) {
        const Local *into;
        OriginKind to = origin_kind(b, others->origins.items[k], &into);
        if (to == ORIGIN_OTHER) continue;
        /* the caller, an `Arena*` or another parameter; a local arena ends with the function too,
           and where the callee stores the value among the arguments is not known */
        bool outlives_call = to != ORIGIN_LOCAL_ARENA;
        if (inside && outlives_call) {
            Diagnostic *d = sema_error_expr(b, "E0375", where, "a pointer into this function is given to the `keeps` parameter `%.*s`, which may store it where it outlives the function",
                                              NAME_ARGS(param_name));
            diag_help(d, "give it memory of an arena that lives as long as the other arguments (spec §6.1)");
            return true;
        }
        for (int m = 0; m < kept->origins.count; m++) {
            const Local *from;
            OriginKind kind = origin_kind(b, kept->origins.items[m], &from);
            if (kept->origins.items[m] == own_arena) continue;
            if (kind == ORIGIN_LOCAL_ARENA && kept->origins.items[m] != others->origins.items[k] && !(from && from->moved_out)) {
                Diagnostic *d = from
                    ? sema_error_expr(b, "E0375", where, "memory of `%.*s`, an arena of this function, is given to the `keeps` parameter `%.*s`, which may store it where it outlives the arena",
                                        NAME_ARGS(from->name), NAME_ARGS(param_name))
                    : sema_error_expr(b, "E0375", where, "memory of an arena of this function is given to the `keeps` parameter `%.*s`, which may store it where it outlives the arena",
                                        NAME_ARGS(param_name));
                diag_help(d, "give it memory of the arena of the other arguments (spec §6.1)");
                return true;
            }
            if (kind == ORIGIN_PARAM_ARENA && !from->is_keeps && outlives_call && kept->origins.items[m] != others->origins.items[k]) {
                Diagnostic *d = sema_error_expr(b, "E0376", where, "the memory of the parameter `%.*s` is given to the `keeps` parameter `%.*s`, but it is not marked `keeps` itself",
                                                  NAME_ARGS(from->name), NAME_ARGS(param_name));
                diag_help(d, "write `keeps` before its type, so that callers know the arena must outlive what they give (spec §6.1)");
                return true;
            }
            const Local *read = read_unkept(b, kept->origins.items[m], others->origins.items[k]);
            bool reached = false; /* what `self` reaches, stored in a local given by `&x`, stays here */
            for (int d = 0; d < direct->origins.count; d++) reached = reached || direct->origins.items[d] == others->origins.items[k];
            if (reached && self_unkept(b, kept->origins.items[m], others->origins.items[k])) {
                Diagnostic *d = sema_error_expr(b, "E0376", where, "what `self` reaches is given to the `keeps` parameter `%.*s`, but the method does not say `keeps self`",
                                                  NAME_ARGS(param_name));
                diag_help(d, "write `keeps self` after its parameters (spec §6.1)");
                return true;
            }
            if (read) {
                Diagnostic *d = sema_error_expr(b, "E0376", where, "what the parameter `%.*s` reaches is given to the `keeps` parameter `%.*s`, but it is not marked `keeps` itself",
                                                  NAME_ARGS(read->name), NAME_ARGS(param_name));
                diag_help(d, "write `keeps` before its type, so that callers give it memory that lives as long (spec §6.1)");
                return true;
            }
            if (kind == ORIGIN_PARAM_VALUE && !from->is_keeps && outlives_call && kept->origins.items[m] != others->origins.items[k]) {
                Diagnostic *d = sema_error_expr(b, "E0376", where, "the parameter `%.*s` is given to the `keeps` parameter `%.*s`, but it is not marked `keeps` itself",
                                                  NAME_ARGS(from->name), NAME_ARGS(param_name));
                diag_help(d, "write `keeps` before its type, so that callers give it memory that lives as long (spec §6.1)");
                return true;
            }
        }
    }
    return false;
}

/* Regions, stage 4 (spec §6.1, rule 4, E0375 and E0376): a `keeps` parameter
   may be stored in the memory of the other arguments (or of the receiver), so it is given no
   memory that ends sooner: of the function, of a local arena (unless the others are of that
   arena), nor a parameter not marked `keeps` itself. */
void sema_check_call_keeps(Body *b, const Expr *e) {
    if (b->unsafe_depth > 0 || e->kind != EXPR_CALL) return;
    const Expr *receiver;
    bool self_receiver;
    size_t first;
    const FnDecl *decl = called(e, &receiver, &self_receiver, &first);
    /* through a value of function type, the type says it; where it stores is unknown: the caller's */
    const Type *fn = decl ? NULL : e->as.call.callee->type;
    if (!decl && (!fn || fn->tag != T_FN || !fn->as.fn.keeps)) return;
    size_t param_count = decl ? decl->param_count : fn->as.fn.param_count;
    for (size_t i = 0; i < param_count && first + i < e->as.call.arg_count; i++) {
        if (decl ? !decl->params[i].is_keeps : i >= 64 || !(fn->as.fn.keeps >> i & 1)) continue;
        static const char digits[] = "0123456789";
        Name param_name = decl ? decl->params[i].name : (Name){ &digits[i % 10], 1, 0 }; /* its place */
        const Expr *arg = e->as.call.args[first + i];
        if (!arg->type || sema_is_send(arg->type)) continue;
        Local kept = {0};
        add_origins(b, &kept, arg);
        bool inside = dangling_part(b, arg) != NULL;
        Local others = {0}; /* the memory it may be stored into */
        Local direct = {0}; /* the same, without what is held by a local given as `&x` */
        add_receiver(b, &others, receiver, self_receiver);
        add_receiver(b, &direct, receiver, self_receiver);
        /* `a.new(T, a, ...)`: `init` keeps its arena in the new object, which is in that arena */
        size_t own_arena = first && others.origins.count ? others.origins.items[0] : 0;
        if (!decl) add_origin(b, &others, ORIGIN_CALLER);
        if (!decl) add_origin(b, &direct, ORIGIN_CALLER);
        for (size_t j = first; j < e->as.call.arg_count; j++) {
            const Expr *other = e->as.call.args[j];
            if (j == first + i || !other->type || !may_receive_pointer(other->type)) continue;
            add_origins(b, &others, other);
            if (other->kind != EXPR_UNARY || other->as.unary.op != TOK_AMP || !storage_local(b, other->as.unary.operand)) add_origins(b, &direct, other);
        }
        if (kept_too_short(b, arg, param_name, &kept, inside, &others, &direct, own_arena)) return;
    }
    /* `keeps self`: the receiver may be stored in the arguments */
    if (decl && decl->keeps_self && !first) {
        Local kept = {0};
        if (self_receiver) add_origin(b, &kept, ORIGIN_CALLER);
        else if (receiver) add_origins(b, &kept, receiver);
        Local others = {0};
        for (size_t j = 0; j < e->as.call.arg_count; j++) {
            const Expr *other = e->as.call.args[j];
            if (other->type && may_receive_pointer(other->type)) add_origins(b, &others, other);
        }
        kept_too_short(b, e, (Name){ "self", 4, 0 }, &kept, receiver && dangling_part(b, receiver) != NULL, &others, &others, 0);
    }
}

/* Regions, stage 5 (spec §6.1, rule 5): `arena.closure(lambda)` copies what the lambda captures
   into `arena`, as rule 3 stores a pointer there: no pointer into the function, nor memory of a
   local arena other than `arena`, nor a parameter not marked `keeps` when the arena outlives the
   call. */
void sema_check_closure_captures(Body *b, const Expr *arena, const Expr *lambda) {
    if (b->unsafe_depth > 0) return;
    const Local *into = local_arena(b, arena);
    if (!into) into = param_arena(b, arena);
    const Local *ignored;
    OriginKind to = into ? origin_kind(b, into->name.offset, &ignored) : ORIGIN_IS_CALLER; /* an arena of a field outlives the call */
    for (size_t i = 0; i < lambda->as.lambda.capture_count; i++) {
        const Capture *c = &lambda->as.lambda.captures[i];
        if (c->is_self || !c->type || sema_is_send(c->type)) continue;
        const Local *local = sema_find_local(b, c->name);
        if (!local) continue;
        if (local->points_inside) {
            Diagnostic *d = sema_error_expr(b, "E0374", lambda, "the closure captures `%.*s`, a pointer into this function, and is kept in an arena",
                                              NAME_ARGS(c->name));
            diag_help(d, "capture memory of the arena the closure goes into (spec §6.1)");
            return;
        }
        for (int k = 0; k < local->origins.count; k++) {
            const Local *from;
            OriginKind kind = origin_kind(b, local->origins.items[k], &from);
            if (kind == ORIGIN_LOCAL_ARENA && (!into || local->origins.items[k] != into->name.offset) && !(from && from->moved_out)) {
                Diagnostic *d = sema_error_expr(b, "E0374", lambda, "the closure captures `%.*s`, which holds memory of another arena than the one it is kept in",
                                                  NAME_ARGS(c->name));
                diag_help(d, "capture memory of the arena the closure goes into (spec §6.1)");
                return;
            }
            if (kind == ORIGIN_PARAM_VALUE && !from->is_keeps && to != ORIGIN_LOCAL_ARENA) {
                Diagnostic *d = sema_error_expr(b, "E0376", lambda, "the closure captures the parameter `%.*s`, but it is not marked `keeps`",
                                                  NAME_ARGS(from->name));
                diag_help(d, "write `keeps` before its type, so that callers give it memory that lives as long (spec §6.1)");
                return;
            }
        }
    }
}

/* `e`, a use of `local`, after the `free()` of an arena it holds memory of (stage 1, E0372). */
void sema_check_freed(Body *b, const Expr *e, const Local *local) {
    if (!local->dangling || b->unsafe_depth > 0) return;
    Diagnostic *d = sema_error_expr(b, "E0372", e, "`%.*s` holds memory of the arena `%.*s`, freed on line %zu",
                                    NAME_ARGS(local->name), NAME_ARGS(local->freed_arena), sema_line_of(b->unit, local->freed_offset));
    diag_help(d, "use it before the `free()`, or free the arena later (spec §6.1)");
}

/* `object.free()`, at `offset`: in a `defer`, it runs when the block of the arena ends. A free in
   a lambda is followed for an arena of the lambda only, since the lambda may never be called. */
void sema_arena_freed(Body *b, Expr *object, size_t offset) {
    Local *arena = local_arena(b, object);
    if (!arena) arena = param_arena(b, object);
    if (b->lambda && (!arena || !in_this_body(b, arena))) return;
    ArenaPlace *place = arena ? NULL : place_of(b, object);
    if (!arena && !place) return;
    if (b->s->summarizing && b->fn_decl && !b->lambda) {
        ReturnSummary *summary = summary_of(b->s, b->fn_decl, true);
        if (arena) summarize_path(b, summary, &summary->frees, arena->name.offset, NULL, 0);
        else if (place) summarize_path(b, summary, &summary->frees, place->root, place->names, place->count);
    }
    if (arena) {
        if (b->defer_depth > 0) {
            arena->free_deferred = true;
        } else {
            free_origin(b, arena->name.offset, arena->name, b->locals.count, offset);
            free_moved(b, &arena->owned, arena->name, b->locals.count, offset);
        }
        return;
    }
    if (b->defer_depth > 0) {
        place->free_deferred = true;
        place->defer_block = b->block_depth;
        return;
    }
    free_origin(b, ORIGIN_PLACE(place->index), place->names[place->count - 1], b->locals.count, offset);
    free_held(b, place, b->locals.count, offset);
    kill_fields(b, place->root, place->names, place->count, offset);
}

/* In the pass that makes the summaries: the function being checked calls `decl`. */
static void note_call(Body *b, const FnDecl *decl) {
    Sema *s = b->s;
    summary_of(s, decl, true);
    size_t callee = *ptrmap_get(&s->summary_index, decl);
    ReturnSummary *summary = summary_of(s, b->fn_decl, true);
    for (size_t i = 0; i < summary->call_count; i++) {
        if (summary->calls[i] == callee) return;
    }
    if (summary->call_count == summary->call_cap) {
        size_t cap = summary->call_cap ? summary->call_cap * 2 : 8;
        size_t *calls = arena_alloc(&s->arena, cap * sizeof *calls);
        if (summary->call_count) memcpy(calls, summary->calls, summary->call_count * sizeof *calls);
        summary->calls = calls;
        summary->call_cap = cap;
    }
    summary->calls[summary->call_count++] = callee;
}

/* True if the summary of `fn` must be made in this pass: the first one, or what it calls has grown
   since it was last made. */
static bool summary_is_stale(Sema *s, const FnDecl *fn) {
    if (s->summary_pass == 1) return true;
    const ReturnSummary *summary = summary_of(s, fn, true);
    for (size_t i = 0; i < summary->call_count; i++) {
        if (s->summaries.items[summary->calls[i]].grew_pass >= s->summary_pass - 1) return true;
    }
    return false;
}

/* The arenas that the function called by `e` frees are gone (spec §6.1, rule 1). */
void sema_call_frees(Body *b, const Expr *e) {
    if (e->kind != EXPR_CALL) return;
    const FnDecl *decl = called_decl(e);
    if (decl && b->s->summarizing && b->fn_decl) note_call(b, decl);
    const ReturnSummary *summary = decl ? summary_of(b->s, decl, false) : NULL;
    for (const ArenaPath *path = summary ? summary->frees : NULL; path; path = path->next) {
        size_t root;
        const Name *names;
        int count;
        if (!path_at_call(b, e, path, &root, &names, &count)) continue;
        if (b->lambda) { /* the call runs if the lambda does: only what ends with it is followed */
            const Local *local;
            if (count || origin_kind(b, root, &local) != ORIGIN_LOCAL_ARENA || !local || !in_this_body(b, local)) continue;
        }
        if (b->s->summarizing && b->fn_decl && !b->lambda) {
            ReturnSummary *own = summary_of(b->s, b->fn_decl, true);
            summarize_path(b, own, &own->frees, root, names, count);
        }
        ArenaPlace *place = count ? find_place(b, root, names, count) : NULL;
        if (b->defer_depth > 0) {
            if (place) {
                place->free_deferred = true;
                place->defer_block = b->block_depth;
            }
            continue;
        }
        const Local *arena;
        if (place) {
            free_origin(b, ORIGIN_PLACE(place->index), names[count - 1], b->locals.count, e->offset);
            free_held(b, place, b->locals.count, e->offset);
        } else if (origin_kind(b, root, &arena) != ORIGIN_OTHER && arena) {
            free_origin(b, root, arena->name, b->locals.count, e->offset);
            free_moved(b, &arena->owned, arena->name, b->locals.count, e->offset);
        }
    }
    for (const ArenaPath *path = summary && b->defer_depth == 0 && !b->lambda ? summary->kills : NULL; path; path = path->next) {
        size_t root;
        const Name *names;
        int count;
        if (!path_at_call(b, e, path, &root, &names, &count) || count == 0) continue;
        DeadField *dead = arena_alloc(&b->s->arena, sizeof *dead);
        dead->root = root;
        dead->names = names;
        dead->count = count;
        dead->offset = e->offset;
        dead->next = b->dead;
        b->dead = dead;
    }
    for (const ArenaPath *path = summary && !b->lambda ? summary->writes : NULL; path; path = path->next) {
        size_t root;
        const Name *names;
        int count;
        if (!path_at_call(b, e, path, &root, &names, &count) || count == 0) continue;
        dead_written(b, root, names, count);
        summarize_write(b, root, names, count);
    }
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
        if (!b->assigning) sema_check_freed(b, e, ref.local);
        return ref.local->type;
    case REF_FIELD:
        sema_check_dead(b, e);
        return ref.field->type;
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
    if (!allowed && from->tag == T_FN && target->tag == T_FN && from->as.fn.param_count == 0 &&
        from->as.fn.ret->tag == T_VOID) {
        /* `fn() -> void`, a function of unknown type (C's `void (*)(void)`), as a shared library gives
           one by its name (platform.library): to the type the program knows it has */
        allowed = needs_unsafe = true;
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
            sema_check_dead(b, e);
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
    bool *given = sosu_calloc(info->field_count + 1, sizeof *given);
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
    bool *covered = sosu_calloc(case_count + 1, sizeof *covered);
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
    case EXPR_CALL: {
        Type *type = sema_check_call(b, e, expected);
        sema_check_call_keeps(b, e);
        sema_call_frees(b, e);
        return type;
    }
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
    b->assign_target = op == TOK_EQ ? target : NULL;
    Type *tt = sema_check_expr(b, target, NULL);
    b->assigning = false;
    b->assign_target = NULL;
    check_assignable(b, target);
    if (op == TOK_EQ) {
        sema_check_expect(b, value, tt);
        sema_consume(b, value);
        sema_check_escape(b, value, target->kind == EXPR_NAME && target->resolution == RESOLVED_LOCAL
                                   ? "assigned with `=`: give it to a new variable"
                                   : "stored there");
        if (target->kind == EXPR_NAME && target->resolution == RESOLVED_LOCAL) {
            Local *local = sema_find_local(b, target->as.name);
            if (local) local->moved = local->moved_out = local->dangling = false;
        }
        /* `p = &x;` or `h.p = &x;`: never cleared, since the order of the branches is not followed */
        Local *holder = storage_local(b, target);
        if (holder && dangling_part(b, value)) holder->points_inside = true;
        if (holder) {
            add_origins(b, holder, value);
            add_owned(b, holder, value);
        }
        arena_moved(b, target, value);
        field_written(b, target, value);
        check_store(b, target, value);
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
    b->block_depth++;
    for (size_t i = 0; i < block->as.block.count; i++) check_stmt(b, block->as.block.items[i]);
    for (ArenaPlace *place = b->places; place; place = place->next) {
        if (!place->free_deferred || b->defer_depth > 0 || place->defer_block < b->block_depth) continue;
        place->free_deferred = false;
        free_origin(b, ORIGIN_PLACE(place->index), place->names[place->count - 1], mark, block->offset + block->length - 1);
        free_held(b, place, mark, block->offset + block->length - 1);
    }
    b->block_depth--;
    for (size_t i = mark; i < b->locals.count; i++) { /* the `defer`s run: `defer a.free();` */
        if (b->locals.items[i].free_deferred) {
            free_origin(b, b->locals.items[i].name.offset, b->locals.items[i].name, mark, block->offset + block->length - 1);
            free_moved(b, &b->locals.items[i].owned, b->locals.items[i].name, mark, block->offset + block->length - 1);
        }
    }
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
    bool points_inside = dangling_part(b, init) != NULL;
    sema_declare_local(b, stmt->as.var.name, type, stmt->as.var.is_mut);
    if (!sema_name_is(stmt->as.var.name, "_")) {
        b->locals.items[b->locals.count - 1].closure = closure;
        b->locals.items[b->locals.count - 1].points_inside = points_inside;
        Local *local = &b->locals.items[b->locals.count - 1];
        add_origins(b, local, init);
        add_owned(b, local, init);
        if (!local->is_mut && type->tag == T_POINTER && init->kind != EXPR_CALL) {
            local->aliased = expr_path(b, init, &local->alias_root, &local->alias_names, &local->alias_count);
        }
    }
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
            check_returned_arena(b, value);
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
        check_returned_arena(b, value);
        record_return(b, value);
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

/* A call through a parent or an interface knows the method it names: its summary also says what
   the methods that redefine it do. */
static void merge_overrides(Sema *s) {
    for (size_t i = 0; i < s->type_infos.count; i++) {
        const TypeInfo *info = s->type_infos.items[i];
        for (size_t j = 0; j < info->method_count; j++) {
            const MethodInfo *method = &info->methods[j];
            if (!method->overridden) continue;
            ReturnSummary *to = summary_of(s, method->overridden->decl, true);
            const ReturnSummary *from = summary_of(s, method->decl, false);
            if (!from) continue;
            for (const ArenaPath *path = from->frees; path; path = path->next) add_path(s, to, &to->frees, path->root, path->names, path->count);
            for (const ArenaPath *path = from->returns; path; path = path->next) add_path(s, to, &to->returns, path->root, path->names, path->count);
            for (const ArenaPath *path = from->kills; path; path = path->next) add_path(s, to, &to->kills, path->root, path->names, path->count);
            if ((from->values & ~to->values) || (from->through & ~to->through) || (from->receiver && !to->receiver)) {
                to->values |= from->values;
                to->through |= from->through;
                to->receiver = to->receiver || from->receiver;
                to->grew_pass = s->summary_pass;
                s->summary_grew = true;
            }
        }
    }
}

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
    if (!fn->body || (s->summarizing && !summary_is_stale(s, fn))) return;
    Body b;
    bool typed = type->tag == T_FN;
    init_body(&b, s, unit, self_info, fn->name, typed ? type->as.fn.ret : s->types.error, typed ? type->as.fn.effects : 0);
    b.in_init = self_info && sema_name_is(fn->name, "init");
    b.fn_decl = fn;
    if (fn->keeps_self && !self_info) {
        Diagnostic *d = sema_error_at(&b, "E0379", fn->name.offset, fn->name.length, "`%.*s` is not a method: it has no `self` to keep", NAME_ARGS(fn->name));
        diag_help(d, "`keeps self` is for a method (spec §6.1)");
    }
    if (s->summarizing) summary_of(s, fn, true);
    for (size_t i = 0; i < fn->param_count; i++) {
        Local param = {0};
        param.name = fn->params[i].name;
        param.type = typed ? type->as.fn.params[i] : s->types.error;
        param.is_mut = fn->params[i].is_mut;
        param.is_param = true;
        param.is_keeps = fn->params[i].is_keeps;
        if (param.type->tag != T_ARENA && !sema_is_send(param.type)) add_origin(&b, &param, param.name.offset);
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
    for (const DeadField *dead = b.dead; dead; dead = dead->next) {
        if (s->summarizing) {
            ReturnSummary *summary = summary_of(s, fn, true);
            summarize_path(&b, summary, &summary->kills, dead->root, dead->names, dead->count);
        } else if (dead->renewed) {
            Diagnostic *d = sema_error_at(&b, "E0378", dead->offset, dead->arena.length,
                                            "`%.*s` holds memory of the arena `%.*s`, which gets a new value here, but `%.*s` does not",
                                            NAME_ARGS(dead->names[dead->count - 1]), NAME_ARGS(dead->arena), NAME_ARGS(dead->names[dead->count - 1]));
            diag_help(d, "give `%.*s` a value in the new arena in this function (spec §6.1)", NAME_ARGS(dead->names[dead->count - 1]));
        }
    }
    if (s->summarizing) summary_of(s, fn, true)->complete = true;
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
    /* first, silently, what each function returns of its parameters (stage 4 of the regions) */
    bool quiet = s->quiet;
    s->quiet = true;
    s->summarizing = true;
    s->summary_pass = 0;
    do { /* what a function frees or returns of an arena reaches its callers: until nothing is added */
        s->summary_pass++;
        s->summary_grew = false;
        for (size_t i = 0; i < s->fns.count; i++) {
            FnInfo *fn = s->fns.items[i];
            s->env = fn->param_count || fn->effect_param_count ? sema_fn_env(s, fn, fn->params, NULL) : NULL;
            if (!fn->is_extern) check_function(s, fn->unit, fn->decl, NULL, fn->type);
            s->env = NULL;
        }
        for (size_t i = 0; i < s->type_infos.count; i++) {
            TypeInfo *info = s->type_infos.items[i];
            s->env = info->param_count ? sema_info_env(s, info, info->params) : NULL;
            const TypeEnv *env = s->env;
            for (size_t j = 0; j < info->method_count; j++) {
                FnInfo *generic = info->methods[j].generic;
                if (generic) s->env = sema_method_env(s, env, generic, generic->params, NULL);
                check_function(s, info->unit, info->methods[j].decl, info, info->methods[j].type);
                s->env = env;
            }
            s->env = NULL;
        }
        merge_overrides(s);
    } while (s->summary_grew);
    s->summarizing = false;
    s->quiet = quiet;

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
