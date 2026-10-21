#include "sema_internal.h"

#include <stdlib.h>
#include <string.h>

/* Steps, each over the whole program:
   1. collect the declarations and imports of every module (Scope);
   2. resolve the parents and interfaces of classes, then break inheritance cycles;
   3. resolve the types of fields, methods, variants and functions;
   4. reject types that contain themselves by value;
   5. check `virtual` / `override` and interface implementations. */

#define NAME_ARGS(name) (int)(name).length, (name).text
#define VEC(T) struct { T *items; size_t count; size_t cap; }

static Diagnostic *error_at(Sema *s, const Unit *unit, const char *code, size_t offset, size_t length,
                            const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    Diagnostic *diag = diag_verror(&s->program->diags, code, &unit->file, offset, length, fmt, args);
    va_end(args);
    return diag;
}

static bool name_eq(Name a, Name b) {
    return a.length == b.length && memcmp(a.text, b.text, a.length) == 0;
}

static size_t line_of(const Unit *unit, size_t offset) {
    size_t line, column;
    source_position(&unit->file, offset, &line, &column);
    return line;
}

static void *arena_vec(Sema *s, void *items, size_t count, size_t size) {
    void *result = arena_copy(&s->arena, items, count * size);
    free(items);
    return result;
}

#define FINISH(s, vec) arena_vec((s), (vec).items, (vec).count, sizeof *(vec).items)

static Scope *scope_of(Sema *s, const Unit *unit) { return &s->scopes[unit->index]; }

static Symbol *scope_find(Scope *scope, Name name) {
    for (size_t i = 0; i < scope->count; i++) {
        if (name_eq(scope->items[i].name, name)) return &scope->items[i];
    }
    return NULL;
}

static const char *decl_keyword(const Decl *decl) {
    switch (decl->kind) {
    case DECL_STRUCT: return "struct";
    case DECL_CLASS: return "class";
    case DECL_INTERFACE: return "interface";
    case DECL_ENUM: return "enum";
    default: return "";
    }
}

/* ---- Suggestions ---- */

static size_t edit_distance(Name a, Name b) {
    if (a.length > 32 || b.length > 32) return (size_t)-1;
    size_t row[33];
    for (size_t j = 0; j <= b.length; j++) row[j] = j;
    for (size_t i = 1; i <= a.length; i++) {
        size_t diagonal = row[0];
        row[0] = i;
        for (size_t j = 1; j <= b.length; j++) {
            size_t above = row[j];
            size_t cost = a.text[i - 1] == b.text[j - 1] ? 0 : 1;
            size_t best = diagonal + cost;
            if (row[j] + 1 < best) best = row[j] + 1;
            if (row[j - 1] + 1 < best) best = row[j - 1] + 1;
            row[j] = best;
            diagonal = above;
        }
    }
    return row[b.length];
}

/* Closest type name to `name` in `scope` (public ones only if `only_pub`), and among the
   built-in types if `builtins`. Returns false when nothing is close enough. */
static bool suggest_type(Sema *s, Scope *scope, Name name, bool only_pub, bool builtins, Name *out) {
    size_t best = name.length < 4 ? 1 : 2;
    bool found = false;
    for (size_t i = 0; i < scope->count; i++) {
        const Symbol *sym = &scope->items[i];
        if (sym->kind != SYM_TYPE || (only_pub && !sym->is_pub)) continue;
        size_t distance = edit_distance(name, sym->name);
        if (distance <= best) {
            best = distance;
            *out = sym->name;
            found = true;
        }
    }
    for (size_t i = 0; builtins && i < s->types.builtins.count; i++) {
        Name candidate = s->types.builtins.items[i]->name;
        size_t distance = edit_distance(name, candidate);
        if (distance <= best && !(found && distance == best)) {
            best = distance;
            *out = candidate;
            found = true;
        }
    }
    return found;
}

/* ---- Step 1: declarations ---- */

static void declare(Sema *s, Unit *unit, Symbol symbol) {
    Scope *scope = scope_of(s, unit);
    Name name = symbol.name;
    if (symbol.kind != SYM_MODULE && type_builtin(&s->types, name.text, name.length)) {
        error_at(s, unit, "E0301", name.offset, name.length, "`%.*s` is a built-in type and cannot be redefined",
                 NAME_ARGS(name));
        return;
    }
    Symbol *existing = scope_find(scope, name);
    if (!existing) {
        SOSU_VEC_PUSH(scope, symbol);
        return;
    }
    Diagnostic *d;
    if (existing->kind == SYM_MODULE && symbol.kind == SYM_MODULE) {
        d = error_at(s, unit, "E0302", name.offset, name.length, "two imports are named `%.*s`", NAME_ARGS(name));
        diag_help(d, "an import is used through the last name of its path, so `%s` and `%s` would be confused",
                  existing->module->module, symbol.module->module);
    } else if (existing->kind == SYM_MODULE) {
        d = error_at(s, unit, "E0302", name.offset, name.length, "`%.*s` is already the name of an imported module",
                     NAME_ARGS(name));
        diag_help(d, "rename the declaration: `%.*s` refers to the module `%s`", NAME_ARGS(name), existing->module->module);
    } else {
        d = error_at(s, unit, "E0301", name.offset, name.length, "`%.*s` is already declared in this module",
                     NAME_ARGS(name));
        diag_help(d, "the first declaration is on line %zu", line_of(unit, existing->name.offset));
    }
}

static TypeInfo *new_type_info(Sema *s, Unit *unit, const Decl *decl, Name name) {
    TypeInfo *info = arena_alloc(&s->arena, sizeof *info);
    info->decl = decl;
    info->unit = unit;
    info->name = name;
    Type *type = arena_alloc(&s->arena, sizeof *type);
    type->tag = decl->kind == DECL_STRUCT ? T_STRUCT
              : decl->kind == DECL_CLASS ? T_CLASS
              : decl->kind == DECL_INTERFACE ? T_INTERFACE
              : T_ENUM;
    type->name = name;
    type->as.info = info;
    info->type = type;
    SOSU_VEC_PUSH(&s->type_infos, info);
    return info;
}

static void declare_fn(Sema *s, Unit *unit, const FnDecl *fn, bool is_pub, bool is_extern) {
    FnInfo *info = arena_alloc(&s->arena, sizeof *info);
    info->decl = fn;
    info->unit = unit;
    info->is_extern = is_extern;
    SOSU_VEC_PUSH(&s->fns, info);
    Symbol symbol = {0};
    symbol.kind = SYM_FN;
    symbol.name = fn->name;
    symbol.is_pub = is_pub;
    symbol.fn = info;
    declare(s, unit, symbol);
}

static void collect(Sema *s, Unit *unit) {
    for (size_t i = 0; i < unit->import_count; i++) {
        const ImportLink *link = &unit->imports[i];
        if (!link->unit) continue;
        Path path = link->decl->as.import;
        Symbol symbol = {0};
        symbol.kind = SYM_MODULE;
        symbol.name = path.items[path.count - 1];
        symbol.module = link->unit;
        declare(s, unit, symbol);
    }
    for (size_t i = 0; i < unit->ast->count; i++) {
        const Decl *decl = unit->ast->decls[i];
        switch (decl->kind) {
        case DECL_STRUCT:
        case DECL_CLASS:
        case DECL_INTERFACE:
        case DECL_ENUM: {
            Name name = decl->kind == DECL_ENUM ? decl->as.enum_.name : decl->as.type.name;
            Symbol symbol = {0};
            symbol.kind = SYM_TYPE;
            symbol.name = name;
            symbol.is_pub = decl->is_pub;
            symbol.type_info = new_type_info(s, unit, decl, name);
            declare(s, unit, symbol);
            break;
        }
        case DECL_FN:
            declare_fn(s, unit, &decl->as.fn, decl->is_pub, false);
            break;
        case DECL_EXTERN:
            for (size_t j = 0; j < decl->as.extern_.fn_count; j++) {
                declare_fn(s, unit, &decl->as.extern_.fns[j], false, true);
            }
            break;
        case DECL_IMPORT:
        case DECL_TEST:
            break;
        }
    }
}

/* ---- Step 2: types ---- */

static void report_not_a_type(Sema *s, Unit *unit, const Symbol *sym, Name name) {
    if (sym->kind == SYM_MODULE) {
        Diagnostic *d = error_at(s, unit, "E0304", name.offset, name.length, "`%.*s` is a module, not a type",
                                 NAME_ARGS(name));
        diag_help(d, "name one of its types: `%.*s.<Type>`", NAME_ARGS(name));
    } else {
        error_at(s, unit, "E0304", name.offset, name.length, "`%.*s` is a function, not a type", NAME_ARGS(name));
    }
}

/* `Type` or `module.Type`. */
static Type *resolve_named(Sema *s, Unit *unit, Path path, size_t offset, size_t length) {
    if (path.count == 0) return s->types.error;
    Scope *scope = scope_of(s, unit);
    Name first = path.items[0];

    if (path.count == 1) {
        Type *builtin = type_builtin(&s->types, first.text, first.length);
        if (builtin) return builtin;
        Symbol *sym = scope_find(scope, first);
        if (!sym && s->prelude && unit != s->prelude) { /* public names of core.prelude */
            sym = scope_find(scope_of(s, s->prelude), first);
            if (sym && !sym->is_pub) sym = NULL;
        }
        if (sym && sym->kind == SYM_TYPE) return sym->type_info->type;
        if (sym) {
            report_not_a_type(s, unit, sym, first);
            return s->types.error;
        }
        Diagnostic *d = error_at(s, unit, "E0303", first.offset, first.length, "unknown type `%.*s`", NAME_ARGS(first));
        Name suggestion;
        if (suggest_type(s, scope, first, false, true, &suggestion)) diag_help(d, "did you mean `%.*s`?", NAME_ARGS(suggestion));
        return s->types.error;
    }

    if (path.count > 2) {
        Diagnostic *d = error_at(s, unit, "E0303", offset, length, "`%.*s` is not a type name", (int)length,
                                 unit->file.text + offset);
        diag_help(d, "a type is written `Type`, or `module.Type` for a type of an imported module");
        return s->types.error;
    }
    Symbol *module = scope_find(scope, first);
    if (!module || module->kind != SYM_MODULE) {
        Diagnostic *d = error_at(s, unit, "E0303", first.offset, first.length, "unknown module `%.*s`", NAME_ARGS(first));
        diag_help(d, "import it first, e.g. `import <package>.%.*s;`", NAME_ARGS(first));
        return s->types.error;
    }
    Name second = path.items[1];
    Scope *target = scope_of(s, module->module);
    Symbol *sym = scope_find(target, second);
    if (!sym) {
        Diagnostic *d = error_at(s, unit, "E0303", second.offset, second.length, "module `%s` has no type `%.*s`",
                                 module->module->module, NAME_ARGS(second));
        Name suggestion;
        if (suggest_type(s, target, second, true, false, &suggestion)) diag_help(d, "did you mean `%.*s`?", NAME_ARGS(suggestion));
        return s->types.error;
    }
    if (sym->kind != SYM_TYPE) {
        report_not_a_type(s, unit, sym, second);
        return s->types.error;
    }
    if (!sym->is_pub && module->module != unit) {
        Diagnostic *d = error_at(s, unit, "E0305", second.offset, second.length, "`%.*s` is private to the module `%s`",
                                 NAME_ARGS(second), module->module->module);
        diag_help(d, "declare it `pub %s %.*s` to use it from other modules", decl_keyword(sym->type_info->decl),
                  NAME_ARGS(second));
    }
    return sym->type_info->type;
}

static unsigned resolve_effects(Sema *s, Unit *unit, Path effects) {
    unsigned mask = 0;
    for (size_t i = 0; i < effects.count; i++) {
        Name name = effects.items[i];
        unsigned bit = effect_from_name(name.text, name.length);
        if (!bit) {
            Diagnostic *d = error_at(s, unit, "E0320", name.offset, name.length, "unknown effect `%.*s`", NAME_ARGS(name));
            diag_help(d, "the effects are `io`, `time`, `random` and `threads`");
        }
        mask |= bit;
    }
    return mask;
}

static Type *resolve_type(Sema *s, Unit *unit, const TypeExpr *te, TypePosition position) {
    switch (te->kind) {
    case TYPE_NAME: {
        Type *type = resolve_named(s, unit, te->as.name, te->offset, te->length);
        if (type->tag == T_VOID && position != POSITION_RETURN) {
            Diagnostic *d = error_at(s, unit, "E0306", te->offset, te->length, "`void` can only be a return type");
            diag_help(d, "a function that returns nothing is declared `void f()`; for an absent value, use `T?`");
            return s->types.error;
        }
        if ((type->tag == T_CLASS || type->tag == T_INTERFACE) && position != POSITION_POINTEE) {
            Diagnostic *d = error_at(s, unit, "E0321", te->offset, te->length, "`%.*s` is %s: it is used through a pointer",
                                     NAME_ARGS(type->name), type->tag == T_CLASS ? "a class" : "an interface");
            diag_help(d, "write `%.*s*`", NAME_ARGS(type->name));
            return s->types.error;
        }
        return type;
    }
    case TYPE_POINTER:
        return type_pointer(&s->types, resolve_type(s, unit, te->as.inner, POSITION_POINTEE));
    case TYPE_OPTION:
        return type_option(&s->types, resolve_type(s, unit, te->as.inner, POSITION_VALUE));
    case TYPE_SLICE:
        return type_slice(&s->types, resolve_type(s, unit, te->as.inner, POSITION_VALUE));
    case TYPE_ARRAY: {
        Type *element = resolve_type(s, unit, te->as.array.element, POSITION_VALUE);
        if (te->as.array.size == 0) {
            error_at(s, unit, "E0308", te->offset, te->length, "an array has at least one element");
            return s->types.error;
        }
        return type_array(&s->types, element, te->as.array.size);
    }
    case TYPE_RESULT: {
        /* `void!E` is the return type of a function that returns nothing but can fail. */
        Type *value = resolve_type(s, unit, te->as.result.value,
                                   position == POSITION_RETURN ? POSITION_RETURN : POSITION_VALUE);
        Type *error = resolve_type(s, unit, te->as.result.error, POSITION_VALUE);
        if (error->tag != T_ENUM && error->tag != T_ERROR) {
            const TypeExpr *ete = te->as.result.error;
            char *text = type_to_string(error);
            Diagnostic *d = error_at(s, unit, "E0307", ete->offset, ete->length,
                                     "the error type of `T!E` must be an enum, not `%s`", text);
            diag_help(d, "declare the possible errors: `enum MyError { NotFound, Invalid }`");
            free(text);
            return s->types.error;
        }
        return type_result(&s->types, value, error);
    }
    case TYPE_FN: {
        VEC(Type *) params = {0};
        for (size_t i = 0; i < te->as.fn.param_count; i++) {
            Type *param = resolve_type(s, unit, te->as.fn.params[i], POSITION_VALUE);
            SOSU_VEC_PUSH(&params, param);
        }
        Type *ret = resolve_type(s, unit, te->as.fn.ret, POSITION_RETURN);
        unsigned effects = resolve_effects(s, unit, te->as.fn.effects);
        Type *type = type_fn(&s->types, params.items, params.count, ret, effects);
        free(params.items);
        return type;
    }
    }
    return s->types.error;
}

/* Span of a path, for diagnostics. */
static void path_span(Path path, size_t *offset, size_t *length) {
    Name first = path.items[0];
    Name last = path.items[path.count - 1];
    *offset = first.offset;
    *length = last.offset + last.length - first.offset;
}

static void resolve_bases(Sema *s, TypeInfo *info) {
    const Decl *decl = info->decl;
    VEC(TypeInfo *) interfaces = {0};
    for (size_t i = 0; i < decl->as.type.base_count; i++) {
        Path path = decl->as.type.bases[i];
        size_t offset, length;
        path_span(path, &offset, &length);
        Type *base = resolve_named(s, info->unit, path, offset, length);
        if (base->tag == T_ERROR) continue;
        if (base->tag == T_CLASS) {
            if (i != 0) {
                Diagnostic *d = error_at(s, info->unit, "E0310", offset, length,
                                         "the parent class `%.*s` must come first", NAME_ARGS(base->name));
                diag_help(d, "a class has at most one parent class, written before its interfaces");
            } else {
                info->parent = base->as.info;
            }
        } else if (base->tag == T_INTERFACE) {
            bool duplicate = false;
            for (size_t j = 0; j < interfaces.count; j++) duplicate |= interfaces.items[j] == base->as.info;
            if (duplicate) {
                error_at(s, info->unit, "E0319", offset, length, "the interface `%.*s` is listed twice", NAME_ARGS(base->name));
            } else {
                SOSU_VEC_PUSH(&interfaces, base->as.info);
            }
        } else {
            char *text = type_to_string(base);
            Diagnostic *d = error_at(s, info->unit, "E0310", offset, length, "`%s` is not a class or an interface", text);
            diag_help(d, "a class inherits from one class and implements interfaces");
            free(text);
        }
    }
    info->interface_count = interfaces.count;
    info->interfaces = FINISH(s, interfaces);
}

static void break_inheritance_cycles(Sema *s) {
    for (size_t i = 0; i < s->type_infos.count; i++) {
        TypeInfo *info = s->type_infos.items[i];
        size_t steps = 0;
        for (TypeInfo *p = info->parent; p && steps <= s->type_infos.count; p = p->parent, steps++) {
            if (p != info) continue;
            size_t offset, length;
            path_span(info->decl->as.type.bases[0], &offset, &length);
            Diagnostic *d = error_at(s, info->unit, "E0311", offset, length, "`%.*s` inherits from itself",
                                     NAME_ARGS(info->name));
            diag_help(d, "the chain of parent classes must end");
            info->parent = NULL;
            break;
        }
    }
}

/* ---- Step 3: members ---- */

/* Reports `name` if it appears among names[0..count). */
static bool check_unique(Sema *s, Unit *unit, const Name *names, size_t count, Name name, const char *what) {
    for (size_t i = 0; i < count; i++) {
        if (!name_eq(names[i], name)) continue;
        Diagnostic *d = error_at(s, unit, "E0319", name.offset, name.length, "%s `%.*s` is declared twice", what,
                                 NAME_ARGS(name));
        diag_help(d, "the first one is on line %zu", line_of(unit, names[i].offset));
        return false;
    }
    return true;
}

static Type *function_type(Sema *s, Unit *unit, const FnDecl *fn) {
    VEC(Type *) params = {0};
    VEC(Name) names = {0};
    for (size_t i = 0; i < fn->param_count; i++) {
        check_unique(s, unit, names.items, names.count, fn->params[i].name, "the parameter");
        SOSU_VEC_PUSH(&names, fn->params[i].name);
        Type *param = resolve_type(s, unit, fn->params[i].type, POSITION_VALUE);
        SOSU_VEC_PUSH(&params, param);
    }
    Type *ret = resolve_type(s, unit, fn->ret, POSITION_RETURN);
    unsigned effects = resolve_effects(s, unit, fn->effects);
    Type *type = type_fn(&s->types, params.items, params.count, ret, effects);
    free(params.items);
    free(names.items);
    return type;
}

static void resolve_fields_and_methods(Sema *s, TypeInfo *info) {
    const Decl *decl = info->decl;
    Unit *unit = info->unit;
    VEC(FieldInfo) fields = {0};
    VEC(MethodInfo) methods = {0};
    VEC(Name) names = {0};

    for (size_t i = 0; i < decl->as.type.field_count; i++) {
        const Field *field = &decl->as.type.fields[i];
        if (check_unique(s, unit, names.items, names.count, field->name, "the field")) {
            for (const TypeInfo *p = info->parent; p; p = p->parent) {
                for (size_t j = 0; j < p->decl->as.type.field_count; j++) {
                    if (!name_eq(p->decl->as.type.fields[j].name, field->name)) continue;
                    error_at(s, unit, "E0319", field->name.offset, field->name.length,
                             "the field `%.*s` is already declared in the parent class `%.*s`",
                             NAME_ARGS(field->name), NAME_ARGS(p->name));
                }
            }
        }
        SOSU_VEC_PUSH(&names, field->name);
        FieldInfo fi = {0};
        fi.name = field->name;
        fi.decl = field;
        fi.type = resolve_type(s, unit, field->type, POSITION_VALUE);
        SOSU_VEC_PUSH(&fields, fi);
    }
    for (size_t i = 0; i < decl->as.type.method_count; i++) {
        const FnDecl *fn = &decl->as.type.methods[i];
        check_unique(s, unit, names.items, names.count, fn->name, "the member");
        SOSU_VEC_PUSH(&names, fn->name);
        MethodInfo mi = {0};
        mi.decl = fn;
        mi.owner = info;
        mi.type = function_type(s, unit, fn);
        SOSU_VEC_PUSH(&methods, mi);
    }
    info->field_count = fields.count;
    info->fields = FINISH(s, fields);
    info->method_count = methods.count;
    info->methods = FINISH(s, methods);
    free(names.items);
}

static void resolve_variants(Sema *s, TypeInfo *info) {
    const Decl *decl = info->decl;
    Unit *unit = info->unit;
    VEC(VariantInfo) variants = {0};
    VEC(Name) names = {0};
    for (size_t i = 0; i < decl->as.enum_.variant_count; i++) {
        const Variant *variant = &decl->as.enum_.variants[i];
        check_unique(s, unit, names.items, names.count, variant->name, "the variant");
        SOSU_VEC_PUSH(&names, variant->name);

        VariantInfo vi = {0};
        vi.name = variant->name;
        vi.decl = variant;
        VEC(Type *) types = {0};
        VEC(Name) field_names = {0};
        for (size_t j = 0; j < variant->field_count; j++) {
            check_unique(s, unit, field_names.items, field_names.count, variant->fields[j].name, "the field");
            SOSU_VEC_PUSH(&field_names, variant->fields[j].name);
            Type *type = resolve_type(s, unit, variant->fields[j].type, POSITION_VALUE);
            SOSU_VEC_PUSH(&types, type);
        }
        free(field_names.items);
        vi.field_count = types.count;
        vi.fields = FINISH(s, types);
        SOSU_VEC_PUSH(&variants, vi);
    }
    info->variant_count = variants.count;
    info->variants = FINISH(s, variants);
    free(names.items);
}

/* ---- Step 4: types that contain themselves ---- */

static void visit_layout(Sema *s, TypeInfo *info);

static void visit_component(Sema *s, TypeInfo *owner, Type *type, const TypeExpr *te) {
    switch (type->tag) {
    case T_ARRAY:
        visit_component(s, owner, type->as.array.element, te);
        break;
    case T_OPTION:
        visit_component(s, owner, type->as.inner, te);
        break;
    case T_RESULT:
        visit_component(s, owner, type->as.result.value, te);
        visit_component(s, owner, type->as.result.error, te);
        break;
    case T_STRUCT:
    case T_ENUM: {
        TypeInfo *child = type->as.info;
        if (child->mark == 0) {
            visit_layout(s, child);
        } else if (child->mark == 1) {
            Diagnostic *d;
            if (child == owner) {
                d = error_at(s, owner->unit, "E0309", te->offset, te->length, "`%.*s` contains itself",
                             NAME_ARGS(owner->name));
            } else {
                d = error_at(s, owner->unit, "E0309", te->offset, te->length, "`%.*s` and `%.*s` contain each other",
                             NAME_ARGS(owner->name), NAME_ARGS(child->name));
            }
            diag_help(d, "its size would be infinite: store a pointer instead, e.g. `%.*s*`", NAME_ARGS(child->name));
        }
        break;
    }
    default:
        break;
    }
}

static void visit_layout(Sema *s, TypeInfo *info) {
    info->mark = 1;
    if (info->type->tag == T_STRUCT) {
        for (size_t i = 0; i < info->field_count; i++) {
            visit_component(s, info, info->fields[i].type, info->fields[i].decl->type);
        }
    } else if (info->type->tag == T_ENUM) {
        for (size_t i = 0; i < info->variant_count; i++) {
            const VariantInfo *variant = &info->variants[i];
            for (size_t j = 0; j < variant->field_count; j++) {
                visit_component(s, info, variant->fields[j], variant->decl->fields[j].type);
            }
        }
    }
    info->mark = 2;
}

/* ---- Step 5: classes ---- */

static MethodInfo *own_method(TypeInfo *info, Name name) {
    for (size_t i = 0; i < info->method_count; i++) {
        if (name_eq(info->methods[i].decl->name, name)) return &info->methods[i];
    }
    return NULL;
}

static MethodInfo *inherited_method(TypeInfo *cls, Name name) {
    for (TypeInfo *p = cls->parent; p; p = p->parent) {
        MethodInfo *method = own_method(p, name);
        if (method) return method;
    }
    return NULL;
}

static MethodInfo *interface_method(TypeInfo *cls, Name name) {
    for (TypeInfo *c = cls; c; c = c->parent) {
        for (size_t i = 0; i < c->interface_count; i++) {
            MethodInfo *method = own_method(c->interfaces[i], name);
            if (method) return method;
        }
    }
    return NULL;
}

static bool is_virtual(const MethodInfo *method) {
    return method->decl->method_kind != METHOD_PLAIN || method->owner->type->tag == T_INTERFACE;
}

static bool same_signature(const Type *a, const Type *b) {
    if (a->tag != T_FN || b->tag != T_FN) return true; /* an error was already reported */
    if (a->as.fn.param_count != b->as.fn.param_count || a->as.fn.ret != b->as.fn.ret) return false;
    for (size_t i = 0; i < a->as.fn.param_count; i++) {
        if (a->as.fn.params[i] != b->as.fn.params[i]) return false;
    }
    return true;
}

static void check_override(Sema *s, TypeInfo *cls, MethodInfo *method) {
    Name name = method->decl->name;
    MethodInfo *target = inherited_method(cls, name);
    if (!target) target = interface_method(cls, name);

    if (method->decl->method_kind != METHOD_OVERRIDE) {
        if (target) {
            Diagnostic *d = error_at(s, cls->unit, "E0314", name.offset, name.length,
                                     "`%.*s` redefines `%.*s.%.*s` without `override`", NAME_ARGS(name),
                                     NAME_ARGS(target->owner->name), NAME_ARGS(name));
            diag_help(d, "write `override` before the method to redefine it, or give it another name");
        }
        return;
    }
    if (!target) {
        Diagnostic *d = error_at(s, cls->unit, "E0313", name.offset, name.length, "`%.*s` overrides nothing", NAME_ARGS(name));
        diag_help(d, "no parent class or interface of `%.*s` declares `%.*s`: remove `override`, or fix the name",
                  NAME_ARGS(cls->name), NAME_ARGS(name));
        return;
    }
    if (!is_virtual(target)) {
        Diagnostic *d = error_at(s, cls->unit, "E0315", name.offset, name.length,
                                 "`%.*s.%.*s` is not virtual and cannot be overridden", NAME_ARGS(target->owner->name),
                                 NAME_ARGS(name));
        diag_help(d, "declare it `virtual` in `%.*s`", NAME_ARGS(target->owner->name));
        return;
    }
    method->overridden = target;
    if (!same_signature(method->type, target->type)) {
        char *expected = type_to_string(target->type);
        Diagnostic *d = error_at(s, cls->unit, "E0316", name.offset, name.length,
                                 "`%.*s` does not have the signature of `%.*s.%.*s`", NAME_ARGS(name),
                                 NAME_ARGS(target->owner->name), NAME_ARGS(name));
        diag_help(d, "expected `%s`", expected);
        free(expected);
        return;
    }
    unsigned added = method->type->as.fn.effects & ~target->type->as.fn.effects;
    if (added) {
        unsigned bit = added & (~added + 1); /* lowest added effect */
        Diagnostic *d = error_at(s, cls->unit, "E0317", name.offset, name.length,
                                 "`%.*s` adds the effect `%s` to `%.*s.%.*s`", NAME_ARGS(name), effect_name(bit),
                                 NAME_ARGS(target->owner->name), NAME_ARGS(name));
        diag_help(d, "an override has the effects of the method it overrides, or fewer (spec §7.4)");
    }
}

static void check_class(Sema *s, TypeInfo *cls) {
    /* `init` is a constructor: each class has its own, which calls `base.init(...)` (spec §5.2). */
    for (size_t i = 0; i < cls->method_count; i++) {
        Name name = cls->methods[i].decl->name;
        if (name.length == 4 && memcmp(name.text, "init", 4) == 0) continue;
        check_override(s, cls, &cls->methods[i]);
    }

    for (size_t i = 0; i < cls->interface_count; i++) {
        TypeInfo *interface = cls->interfaces[i];
        for (size_t j = 0; j < interface->method_count; j++) {
            Name name = interface->methods[j].decl->name;
            if (own_method(cls, name) || inherited_method(cls, name)) continue;
            Diagnostic *d = error_at(s, cls->unit, "E0318", cls->name.offset, cls->name.length,
                                     "`%.*s` does not implement `%.*s.%.*s`", NAME_ARGS(cls->name),
                                     NAME_ARGS(interface->name), NAME_ARGS(name));
            char *signature = type_to_string(interface->methods[j].type);
            diag_help(d, "add the method `%.*s` (`%s`) with `override`", NAME_ARGS(name), signature);
            free(signature);
        }
    }
}

/* ---- Entry point ---- */

bool sema_check(Sema *s, Program *program) {
    memset(s, 0, sizeof *s);
    s->program = program;
    types_init(&s->types);
    s->scopes = sosu_alloc(program->units.count * sizeof *s->scopes);
    memset(s->scopes, 0, program->units.count * sizeof *s->scopes);
    size_t errors_before = program->diags.error_count;

    for (size_t i = 0; i < program->units.count; i++) {
        if (strcmp(program->units.items[i]->module, "core.prelude") == 0) s->prelude = program->units.items[i];
        collect(s, program->units.items[i]);
    }

    for (size_t i = 0; i < s->type_infos.count; i++) {
        TypeInfo *info = s->type_infos.items[i];
        if (info->decl->kind == DECL_CLASS) resolve_bases(s, info);
    }
    break_inheritance_cycles(s);

    for (size_t i = 0; i < s->type_infos.count; i++) {
        TypeInfo *info = s->type_infos.items[i];
        if (info->decl->kind == DECL_ENUM) resolve_variants(s, info);
        else resolve_fields_and_methods(s, info);
    }
    for (size_t i = 0; i < s->fns.count; i++) {
        FnInfo *fn = s->fns.items[i];
        fn->type = function_type(s, fn->unit, fn->decl);
    }

    for (size_t i = 0; i < s->type_infos.count; i++) {
        if (s->type_infos.items[i]->mark == 0) visit_layout(s, s->type_infos.items[i]);
    }
    for (size_t i = 0; i < s->type_infos.count; i++) {
        TypeInfo *info = s->type_infos.items[i];
        if (info->decl->kind == DECL_CLASS) check_class(s, info);
    }
    sema_check_bodies(s);
    return program->diags.error_count == errors_before;
}

/* ---- Shared with sema_body.c ---- */

Diagnostic *sema_verror(Sema *s, const Unit *unit, const char *code, size_t offset, size_t length,
                        const char *fmt, va_list args) {
    return diag_verror(&s->program->diags, code, &unit->file, offset, length, fmt, args);
}

bool sema_name_eq(Name a, Name b) { return name_eq(a, b); }
size_t sema_edit_distance(Name a, Name b) { return edit_distance(a, b); }
size_t sema_line_of(const Unit *unit, size_t offset) { return line_of(unit, offset); }
Scope *sema_scope(Sema *s, const Unit *unit) { return scope_of(s, unit); }
Symbol *sema_scope_find(Scope *scope, Name name) { return scope_find(scope, name); }

Type *sema_resolve_type(Sema *s, Unit *unit, const TypeExpr *te, TypePosition position) {
    return resolve_type(s, unit, te, position);
}

Type *sema_resolve_named(Sema *s, Unit *unit, Path path, size_t offset, size_t length) {
    return resolve_named(s, unit, path, offset, length);
}

void sema_free(Sema *s) {
    if (s->program) {
        for (size_t i = 0; i < s->program->units.count; i++) free(s->scopes[i].items);
    }
    free(s->scopes);
    free(s->type_infos.items);
    free(s->fns.items);
    types_free(&s->types);
    arena_free(&s->arena);
    memset(s, 0, sizeof *s);
}
