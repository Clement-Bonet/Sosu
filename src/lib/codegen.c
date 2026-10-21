#include "codegen.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The generated C follows the source closely, with three rules:
   - evaluation order: Sosu evaluates left to right (spec §5.1), C does not. Every call
     is stored in a temporary, in source order; the operands written before a call are
     stored first;
   - checks (spec §7.7): integer operations and indexes go through the runtime helpers;
   - `defer` statements are emitted again on every exit of their block.
   `#line` directives point the C compiler and the debugger to the .sosu lines.

   Names: locals `v_x`, fields `f_x`, types `T_...`, functions `F_<module>__<name>`. */

#define NAME_ARGS(name) (int)(name).length, (name).text
#define VEC(T) struct { T *items; size_t count; size_t cap; }

/* ---- Text buffers ---- */

typedef struct {
    char *items;
    size_t count;
    size_t cap;
} Buf;

static void buf_add(Buf *buf, const char *text, size_t length) {
    if (buf->count + length + 1 > buf->cap) {
        buf->cap = (buf->count + length + 1) * 2;
        buf->items = sosu_realloc(buf->items, buf->cap);
    }
    memcpy(buf->items + buf->count, text, length);
    buf->count += length;
    buf->items[buf->count] = '\0';
}

static void buf_puts(Buf *buf, const char *text) { buf_add(buf, text, strlen(text)); }

static void buf_printf(Buf *buf, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    char *text = sosu_vformat(fmt, args);
    va_end(args);
    buf_puts(buf, text);
    free(text);
}

/* ---- Program state ---- */

typedef struct {
    const Type *type;
    char *name;
    bool defined;
} TypeEntry;

typedef struct {
    Sema *s;
    Program *program;
    Buf fwd;    /* forward typedefs and function pointer types */
    Buf defs;   /* struct definitions */
    Buf protos; /* function prototypes */
    Buf code;   /* function definitions */
    VEC(TypeEntry) types;
    CodegenOptions options;
} Gen;

typedef struct {
    const Stmt *stmt;
    int depth;
} Deferred;

typedef struct {
    int depth;         /* scope depth of the loop body */
    int continue_label; /* -1: a C `continue` is enough */
} Loop;

typedef struct {
    Gen *g;
    Unit *unit;
    Buf *out;
    int indent;
    int temps;
    int labels;
    const FnDecl *fn; /* NULL in tests */
    Type *ret;
    TypeInfo *self_info;
    int depth;
    VEC(Deferred) defers;
    VEC(Loop) loops;
} Fn;

static char *gen_expr(Fn *f, Expr *e);
static void gen_stmt(Fn *f, const Stmt *stmt);
static void gen_block(Fn *f, const Stmt *block);
static const char *c_type(Gen *g, const Type *type);

static bool name_eq(Name a, Name b) {
    return a.length == b.length && memcmp(a.text, b.text, a.length) == 0;
}

static bool name_is(Name name, const char *text) {
    size_t length = strlen(text);
    return name.length == length && memcmp(name.text, text, length) == 0;
}

static Type *builtin(Gen *g, const char *name) { return type_builtin(&g->s->types, name, strlen(name)); }

/* ---- C names ---- */

static char *module_c_name(const Unit *unit) {
    char *name = sosu_strdup(unit->module);
    for (char *c = name; *c; c++) {
        if (*c == '.') *c = '_';
    }
    return name;
}

static char *nominal_c_name(const TypeInfo *info) {
    char *module = module_c_name(info->unit);
    char *name = sosu_format("T_%s__%.*s", module, NAME_ARGS(info->name));
    free(module);
    return name;
}

static char *fn_c_name(const FnInfo *fn) {
    if (fn->is_extern) return sosu_format("%.*s", NAME_ARGS(fn->decl->name));
    char *module = module_c_name(fn->unit);
    char *name = sosu_format("F_%s__%.*s", module, NAME_ARGS(fn->decl->name));
    free(module);
    return name;
}

static char *method_c_name(Gen *g, const MethodInfo *method) {
    return sosu_format("%s__%.*s", c_type(g, method->owner->type), NAME_ARGS(method->decl->name));
}

static const char *int_c_type(const Type *type) {
    static const char *const names[][2] = {
        { "i8", "int8_t" }, { "i16", "int16_t" }, { "i32", "int32_t" }, { "i64", "int64_t" }, { "isize", "int64_t" },
        { "u8", "uint8_t" }, { "u16", "uint16_t" }, { "u32", "uint32_t" }, { "u64", "uint64_t" }, { "usize", "uint64_t" },
    };
    for (size_t i = 0; i < sizeof names / sizeof *names; i++) {
        if (name_is(type->name, names[i][0])) return names[i][1];
    }
    return "int64_t";
}

/* Self-delimiting code of a type, used to name the generated aggregates. */
static void mangle(Gen *g, Buf *out, const Type *type) {
    switch (type->tag) {
    case T_POINTER: buf_puts(out, "P"); mangle(g, out, type->as.inner); break;
    case T_OPTION: buf_puts(out, "O"); mangle(g, out, type->as.inner); break;
    case T_SLICE: buf_puts(out, "S"); mangle(g, out, type->as.inner); break;
    case T_ARRAY:
        buf_printf(out, "A%llu_", (unsigned long long)type->as.array.size);
        mangle(g, out, type->as.array.element);
        break;
    case T_RESULT:
        buf_puts(out, "R");
        mangle(g, out, type->as.result.value);
        mangle(g, out, type->as.result.error);
        break;
    case T_FN:
        buf_printf(out, "F%zu", type->as.fn.param_count);
        for (size_t i = 0; i < type->as.fn.param_count; i++) mangle(g, out, type->as.fn.params[i]);
        mangle(g, out, type->as.fn.ret);
        break;
    case T_STRUCT:
    case T_ENUM:
    case T_CLASS:
    case T_INTERFACE: {
        const char *name = c_type(g, type);
        buf_printf(out, "N%zu%s", strlen(name), name);
        break;
    }
    default:
        buf_add(out, type->name.text, type->name.length);
        break;
    }
}

/* ---- Types ---- */

static TypeEntry *find_type(Gen *g, const Type *type) {
    for (size_t i = 0; i < g->types.count; i++) {
        if (g->types.items[i].type == type) return &g->types.items[i];
    }
    return NULL;
}

/* The C name of a struct-like type, with its forward typedef (no definition). */
static const char *struct_name(Gen *g, const Type *type) {
    TypeEntry *entry = find_type(g, type);
    if (entry) return entry->name;
    char *name;
    if (type->tag == T_STRUCT || type->tag == T_ENUM || type->tag == T_CLASS || type->tag == T_INTERFACE) {
        name = nominal_c_name(type->as.info);
    } else {
        Buf code = {0};
        buf_puts(&code, "T_");
        mangle(g, &code, type);
        name = code.items;
    }
    TypeEntry added = { type, name, false };
    SOSU_VEC_PUSH(&g->types, added);
    if (type->tag == T_FN) {
        Buf params = {0};
        for (size_t i = 0; i < type->as.fn.param_count; i++) {
            buf_printf(&params, "%s%s", i ? ", " : "", c_type(g, type->as.fn.params[i]));
        }
        buf_printf(&g->fwd, "typedef %s (*%s)(%s);\n", c_type(g, type->as.fn.ret), name,
                   params.items ? params.items : "void");
        free(params.items);
        find_type(g, type)->defined = true;
    } else {
        buf_printf(&g->fwd, "typedef struct %s %s;\n", name, name);
    }
    return find_type(g, type)->name;
}

static void define_type(Gen *g, const Type *type);

static const char *c_type(Gen *g, const Type *type) {
    switch (type->tag) {
    case T_ERROR:
    case T_VOID: return "void";
    case T_BOOL: return "bool";
    case T_CHAR: return "uint32_t";
    case T_STR: return "sosu_str";
    case T_ARENA: return "sosu_arena";
    case T_INT: return int_c_type(type);
    case T_FLOAT: return type->as.float_bits == 32 ? "float" : "double";
    case T_POINTER: {
        TypeEntry *entry = find_type(g, type);
        if (entry) return entry->name;
        char *name = sosu_format("%s*", c_type(g, type->as.inner));
        TypeEntry added = { type, name, true };
        SOSU_VEC_PUSH(&g->types, added);
        return name;
    }
    default:
        return struct_name(g, type);
    }
}

/* A type used by value: its definition (and those of its parts) comes first. */
static const char *value_type(Gen *g, const Type *type) {
    define_type(g, type);
    return c_type(g, type);
}

/* ---- Classes ----
   An object starts with its parent object (field `base`), or for a root class with a
   pointer `vt` to its class's table of virtual methods. That table starts likewise with
   its parent's table, or with the sosu_class description; then come the slots of the
   virtual methods the class introduces. An interface pointer is the object pointer; its
   methods are found through the interface tables of the class (sosu_find_interface). */

/* `virtual`, or `override` of an interface method only: the method gets a new slot. */
static bool introduces_slot(const MethodInfo *method) {
    if (method->owner->type->tag != T_CLASS) return false;
    if (method->decl->method_kind == METHOD_VIRTUAL) return true;
    return method->decl->method_kind == METHOD_OVERRIDE &&
           (!method->overridden || method->overridden->owner->type->tag != T_CLASS);
}

/* The method that introduced the slot of `method`, or NULL if it is not virtual. */
static const MethodInfo *slot_of(const MethodInfo *method) {
    if (method->owner->type->tag != T_CLASS || method->decl->method_kind == METHOD_PLAIN) return NULL;
    while (method->overridden && method->overridden->owner->type->tag == T_CLASS) method = method->overridden;
    return method;
}

static const TypeInfo *root_of(const TypeInfo *cls) {
    while (cls->parent) cls = cls->parent;
    return cls;
}

/* The method `name` of `cls`, or of its nearest parent that has one. */
static const MethodInfo *method_of(const TypeInfo *cls, Name name) {
    for (const TypeInfo *t = cls; t; t = t->parent) {
        for (size_t i = 0; i < t->method_count; i++) {
            if (name_eq(t->methods[i].decl->name, name)) return &t->methods[i];
        }
    }
    return NULL;
}

/* `base.` repeated: from an object to the part of a parent class that declares `field`. */
static char *field_path(const TypeInfo *cls, Name field) {
    Buf path = {0};
    buf_puts(&path, "");
    for (const TypeInfo *t = cls; t; t = t->parent) {
        for (size_t i = 0; i < t->field_count; i++) {
            if (name_eq(t->fields[i].name, field)) return path.items;
        }
        buf_puts(&path, "base.");
    }
    return path.items;
}

/* C parameters `<self> *self, P0 a0, ...` for a method of type `fn`. */
static char *method_params(Gen *g, const char *self_type, const Type *fn) {
    Buf params = {0};
    buf_printf(&params, "%s *self", self_type);
    for (size_t i = 0; i < fn->as.fn.param_count; i++) {
        buf_printf(&params, ", %s a%zu", value_type(g, fn->as.fn.params[i]), i);
    }
    return params.items;
}

static const char *return_type(Gen *g, const Type *fn) {
    return fn->as.fn.ret->tag == T_VOID ? "void" : value_type(g, fn->as.fn.ret);
}

/* The slots of a class table, or the methods of an interface table. */
static void table_slots(Gen *g, Buf *out, const TypeInfo *info, const char *self_type) {
    for (size_t i = 0; i < info->method_count; i++) {
        const MethodInfo *method = &info->methods[i];
        if (info->type->tag == T_CLASS && !introduces_slot(method)) continue;
        char *params = method_params(g, self_type, method->type);
        buf_printf(out, "    %s (*m_%.*s)(%s);\n", return_type(g, method->type), NAME_ARGS(method->decl->name), params);
        free(params);
    }
}

static void define_type(Gen *g, const Type *type) {
    if (type->tag <= T_FLOAT || type->tag == T_POINTER || type->tag == T_FN) {
        if (type->tag == T_FN) c_type(g, type);
        return;
    }
    const char *name = c_type(g, type);
    TypeEntry *entry = find_type(g, type);
    if (entry->defined) return;
    entry->defined = true;

    Buf body = {0};
    switch (type->tag) {
    case T_STRUCT: {
        const TypeInfo *info = type->as.info;
        for (size_t i = 0; i < info->field_count; i++) {
            buf_printf(&body, "    %s f_%.*s;\n", value_type(g, info->fields[i].type), NAME_ARGS(info->fields[i].name));
        }
        if (info->field_count == 0) buf_puts(&body, "    char unused;\n");
        break;
    }
    case T_ENUM: {
        const TypeInfo *info = type->as.info;
        Buf variants = {0};
        for (size_t i = 0; i < info->variant_count; i++) {
            const VariantInfo *variant = &info->variants[i];
            if (variant->field_count == 0) continue;
            buf_puts(&variants, "        struct {");
            for (size_t j = 0; j < variant->field_count; j++) {
                buf_printf(&variants, " %s f%zu;", value_type(g, variant->fields[j]), j);
            }
            buf_printf(&variants, " } v_%.*s;\n", NAME_ARGS(variant->name));
        }
        buf_puts(&body, "    uint32_t tag;\n");
        if (variants.items) buf_printf(&body, "    union {\n%s    } as;\n", variants.items);
        free(variants.items);
        break;
    }
    case T_OPTION:
        buf_printf(&body, "    bool has;\n    %s value;\n", value_type(g, type->as.inner));
        break;
    case T_RESULT: {
        Type *value = type->as.result.value;
        buf_puts(&body, "    bool ok;\n    union {\n");
        if (value->tag != T_VOID) buf_printf(&body, "        %s value;\n", value_type(g, value));
        buf_printf(&body, "        %s error;\n    } as;\n", value_type(g, type->as.result.error));
        break;
    }
    case T_ARRAY:
        buf_printf(&body, "    %s items[%llu];\n", value_type(g, type->as.array.element),
                   (unsigned long long)type->as.array.size);
        break;
    case T_SLICE:
        buf_printf(&body, "    %s *ptr;\n    size_t len;\n", c_type(g, type->as.inner));
        break;
    case T_CLASS: {
        const TypeInfo *info = type->as.info;
        Buf table = {0};
        if (info->parent) {
            const char *parent = value_type(g, info->parent->type);
            buf_printf(&table, "    %s__vt base;\n", parent);
            buf_printf(&body, "    %s base;\n", parent);
        } else {
            buf_puts(&table, "    sosu_class info;\n");
            buf_printf(&body, "    const %s__vt *vt;\n", name);
        }
        table_slots(g, &table, info, name);
        buf_printf(&g->fwd, "typedef struct %s__vt %s__vt;\n", name, name);
        buf_printf(&g->defs, "struct %s__vt {\n%s};\n\n", name, table.items);
        free(table.items);
        for (size_t i = 0; i < info->field_count; i++) {
            buf_printf(&body, "    %s f_%.*s;\n", value_type(g, info->fields[i].type), NAME_ARGS(info->fields[i].name));
        }
        break;
    }
    case T_INTERFACE: {
        Buf table = {0};
        table_slots(g, &table, type->as.info, name);
        buf_printf(&g->fwd, "typedef struct %s__it %s__it;\n", name, name);
        buf_printf(&g->defs, "struct %s__it {\n%s    char unused;\n};\n\n", name, table.items ? table.items : "");
        free(table.items);
        buf_puts(&body, "    char unused;\n");
        break;
    }
    default:
        break;
    }
    buf_printf(&g->defs, "struct %s {\n%s};\n\n", name, body.items ? body.items : "");
    free(body.items);
}

/* ---- Statement output ---- */

static void emit(Fn *f, const char *fmt, ...) {
    for (int i = 0; i < f->indent; i++) buf_puts(f->out, "    ");
    va_list args;
    va_start(args, fmt);
    char *text = sosu_vformat(fmt, args);
    va_end(args);
    buf_puts(f->out, text);
    buf_puts(f->out, "\n");
    free(text);
}

static void emit_line(Fn *f, size_t offset) {
    size_t line, column;
    source_position(&f->unit->file, offset, &line, &column);
    buf_printf(f->out, "#line %zu \"", line);
    for (const char *c = f->unit->file.path; *c; c++) {
        if (*c == '\\' || *c == '"') buf_puts(f->out, "\\");
        buf_add(f->out, c, 1);
    }
    buf_puts(f->out, "\"\n");
}

/* A C string literal "file:line:column" for runtime messages. */
static char *loc(Fn *f, size_t offset) {
    size_t line, column;
    source_position(&f->unit->file, offset, &line, &column);
    Buf text = {0};
    buf_puts(&text, "\"");
    for (const char *c = f->unit->file.path; *c; c++) {
        if (*c == '\\' || *c == '"') buf_puts(&text, "\\");
        buf_add(&text, c, 1);
    }
    buf_printf(&text, ":%zu:%zu\"", line, column);
    return text.items;
}

static void append_c_char(Buf *out, unsigned char c) {
    if (c >= 0x20 && c < 0x7F && c != '"' && c != '\\' && c != '?') buf_add(out, (const char *)&c, 1);
    else buf_printf(out, "\\%03o", c);
}

/* A C string literal containing `text`. */
static char *c_string(const char *text, size_t length) {
    Buf out = {0};
    buf_puts(&out, "\"");
    for (size_t i = 0; i < length; i++) append_c_char(&out, (unsigned char)text[i]);
    buf_puts(&out, "\"");
    return out.items;
}

static char *source_text(Fn *f, const Expr *e) {
    return c_string(f->unit->file.text + e->offset, e->length);
}

static char *new_temp(Fn *f) { return sosu_format("t%d", ++f->temps); }

/* Stores a value in a new temporary and returns its name. */
static char *materialize(Fn *f, const Type *type, char *value) {
    char *temp = new_temp(f);
    emit(f, "%s %s = %s;", value_type(f->g, type), temp, value);
    free(value);
    return temp;
}

/* Generates `e` into a separate buffer; *statements tells whether it emitted any. */
static char *gen_apart(Fn *f, Expr *e, Buf *into) {
    Buf *saved = f->out;
    f->out = into;
    char *value = gen_expr(f, e);
    f->out = saved;
    return value;
}

/* True if evaluating `e` can run code with effects (a call, a match, a try). */
static bool has_call(const Expr *e) {
    if (!e) return false;
    switch (e->kind) {
    case EXPR_CALL: case EXPR_MATCH: case EXPR_TRY: return true;
    case EXPR_UNARY: return has_call(e->as.unary.operand);
    case EXPR_BINARY: return has_call(e->as.binary.left) || has_call(e->as.binary.right);
    case EXPR_CAST: case EXPR_TRY_CAST: return has_call(e->as.cast.value);
    case EXPR_TERNARY:
        return has_call(e->as.ternary.cond) || has_call(e->as.ternary.then_expr) || has_call(e->as.ternary.else_expr);
    case EXPR_INDEX: return has_call(e->as.index.object) || has_call(e->as.index.index);
    case EXPR_FIELD: return has_call(e->as.field.object);
    case EXPR_STRUCT_LIT:
        for (size_t i = 0; i < e->as.struct_lit.field_count; i++) {
            if (has_call(e->as.struct_lit.fields[i].value)) return true;
        }
        return false;
    default: return false;
    }
}

/* Generates operands left to right: an operand followed by a call is stored first. */
static char **gen_operands(Fn *f, Expr **exprs, size_t count) {
    char **values = sosu_alloc((count + 1) * sizeof *values);
    for (size_t i = 0; i < count; i++) {
        values[i] = gen_expr(f, exprs[i]);
        bool later_call = false;
        for (size_t j = i + 1; j < count; j++) later_call |= has_call(exprs[j]);
        if (later_call && values[i]) values[i] = materialize(f, exprs[i]->type, values[i]);
    }
    return values;
}

static void free_values(char **values, size_t count) {
    for (size_t i = 0; i < count; i++) free(values[i]);
    free(values);
}

/* ---- Literals ---- */

static void utf8_append(Buf *out, uint32_t c) {
    unsigned char bytes[4];
    size_t n;
    if (c < 0x80) { bytes[0] = (unsigned char)c; n = 1; }
    else if (c < 0x800) { bytes[0] = (unsigned char)(0xC0 | (c >> 6)); bytes[1] = (unsigned char)(0x80 | (c & 0x3F)); n = 2; }
    else if (c < 0x10000) {
        bytes[0] = (unsigned char)(0xE0 | (c >> 12));
        bytes[1] = (unsigned char)(0x80 | ((c >> 6) & 0x3F));
        bytes[2] = (unsigned char)(0x80 | (c & 0x3F));
        n = 3;
    } else {
        bytes[0] = (unsigned char)(0xF0 | (c >> 18));
        bytes[1] = (unsigned char)(0x80 | ((c >> 12) & 0x3F));
        bytes[2] = (unsigned char)(0x80 | ((c >> 6) & 0x3F));
        bytes[3] = (unsigned char)(0x80 | (c & 0x3F));
        n = 4;
    }
    buf_add(out, (const char *)bytes, n);
}

static unsigned hex_value(char c) {
    if (c >= '0' && c <= '9') return (unsigned)(c - '0');
    if (c >= 'a' && c <= 'f') return (unsigned)(c - 'a' + 10);
    return (unsigned)(c - 'A' + 10);
}

/* The bytes of a string literal (checked by the lexer), without its quotes. */
static void decode_string(const char *text, size_t length, Buf *out) {
    for (size_t i = 0; i < length; i++) {
        if (text[i] != '\\') {
            buf_add(out, text + i, 1);
            continue;
        }
        char e = text[++i];
        switch (e) {
        case 'n': buf_puts(out, "\n"); break;
        case 't': buf_puts(out, "\t"); break;
        case 'r': buf_puts(out, "\r"); break;
        case '0': buf_add(out, "\0", 1); break;
        case 'x': {
            char byte = (char)(hex_value(text[i + 1]) * 16 + hex_value(text[i + 2]));
            buf_add(out, &byte, 1);
            i += 2;
            break;
        }
        case 'u': {
            uint32_t value = 0;
            i += 2; /* u{ */
            while (text[i] != '}') value = value * 16 + hex_value(text[i++]);
            utf8_append(out, value);
            break;
        }
        default: buf_add(out, &e, 1); break; /* \\ \' \" */
        }
    }
}

static char *gen_string(Fn *f, const Expr *e) {
    Buf bytes = {0};
    decode_string(f->unit->file.text + e->offset + 1, e->length - 2, &bytes);
    char *literal = c_string(bytes.items ? bytes.items : "", bytes.count);
    char *result = sosu_format("SOSU_STR(%s, %zu)", literal, bytes.count);
    free(literal);
    free(bytes.items);
    return result;
}

static char *gen_int(Fn *f, const Type *type, uint64_t value, bool negative) {
    const char *ctype = c_type(f->g, type);
    if (!negative) return sosu_format("((%s)%lluULL)", ctype, (unsigned long long)value);
    if (value == UINT64_C(9223372036854775808)) return sosu_format("((%s)INT64_MIN)", ctype);
    return sosu_format("((%s)-%lldLL)", ctype, (long long)value);
}

/* ---- Expressions ---- */

static bool is_place(const Expr *e) {
    return e->kind == EXPR_NAME || e->kind == EXPR_INDEX || e->kind == EXPR_SELF ||
           (e->kind == EXPR_FIELD && e->resolution == RESOLVED_NONE) ||
           (e->kind == EXPR_UNARY && e->as.unary.op == TOK_STAR);
}

/* A pointer to the value of `e`: its address if it is a place, else a temporary's. */
static char *address_of(Fn *f, Expr *e) {
    char *value = gen_expr(f, e);
    if (e->type->tag == T_POINTER) return value;
    if (!is_place(e)) value = materialize(f, e->type, value);
    char *result = sosu_format("(&%s)", value);
    free(value);
    return result;
}

static char *gen_variant(Fn *f, const Expr *e, const TypeInfo *info, size_t index, char **args) {
    const VariantInfo *variant = &info->variants[index];
    const char *type = value_type(f->g, info->type);
    if (variant->field_count == 0) return sosu_format("((%s){ .tag = %zu })", type, index);
    Buf fields = {0};
    for (size_t i = 0; i < variant->field_count; i++) buf_printf(&fields, "%s%s", i ? ", " : "", args[i]);
    char *result = sosu_format("((%s){ .tag = %zu, .as.v_%.*s = { %s } })", type, index, NAME_ARGS(variant->name),
                               fields.items);
    free(fields.items);
    (void)e;
    return result;
}

static char *gen_name(Fn *f, Expr *e) {
    switch (e->resolution) {
    case RESOLVED_LOCAL: return sosu_format("v_%.*s", NAME_ARGS(e->as.name));
    case RESOLVED_SELF_FIELD: {
        char *path = f->self_info->type->tag == T_CLASS ? field_path(f->self_info, e->as.name) : sosu_strdup("");
        char *result = sosu_format("v_self->%sf_%.*s", path, NAME_ARGS(e->as.name));
        free(path);
        return result;
    }
    case RESOLVED_FN: return fn_c_name(e->target);
    case RESOLVED_VARIANT: return gen_variant(f, e, e->target, e->target_index, NULL);
    case RESOLVED_BUILTIN: {
        const char *name = e->target;
        if (strcmp(name, "KB") == 0) return sosu_strdup("((uint64_t)1024ULL)");
        if (strcmp(name, "MB") == 0) return sosu_strdup("((uint64_t)1048576ULL)");
        if (strcmp(name, "GB") == 0) return sosu_strdup("((uint64_t)1073741824ULL)");
        return sosu_format("((%s){ .has = false })", value_type(f->g, e->type)); /* none */
    }
    default: return sosu_strdup("0");
    }
}

static const char *op_helper(TokenKind op) {
    switch (op) {
    case TOK_PLUS: case TOK_PLUS_EQ: return "add";
    case TOK_MINUS: case TOK_MINUS_EQ: return "sub";
    case TOK_STAR: case TOK_STAR_EQ: return "mul";
    case TOK_SLASH: case TOK_SLASH_EQ: return "div";
    case TOK_PERCENT: case TOK_PERCENT_EQ: return "rem";
    case TOK_SHL: case TOK_SHL_EQ: return "shl";
    case TOK_SHR: case TOK_SHR_EQ: return "shr";
    default: return NULL;
    }
}

static const char *c_operator(TokenKind op) {
    switch (op) {
    case TOK_PLUS: case TOK_PLUS_EQ: return "+";
    case TOK_MINUS: case TOK_MINUS_EQ: return "-";
    case TOK_STAR: case TOK_STAR_EQ: return "*";
    case TOK_SLASH: case TOK_SLASH_EQ: return "/";
    case TOK_PERCENT: case TOK_PERCENT_EQ: return "%";
    case TOK_AMP: case TOK_AMP_EQ: return "&";
    case TOK_PIPE: case TOK_PIPE_EQ: return "|";
    case TOK_CARET: case TOK_CARET_EQ: return "^";
    default: return token_kind_spelling(op);
    }
}

/* `a op b` for an arithmetic or bitwise operator (also used by `+=`...). */
/* Unsigned type in which modulo arithmetic on `type` is computed without undefined
   behavior (8 and 16 bits would be promoted to `int`). */
static const char *wrapping_type(const Type *type) {
    return type->as.integer.bits <= 32 ? "uint32_t" : "uint64_t";
}

static bool is_wrapping(TokenKind op) {
    return op == TOK_PLUS || op == TOK_PLUS_EQ || op == TOK_MINUS || op == TOK_MINUS_EQ || op == TOK_STAR ||
           op == TOK_STAR_EQ;
}

static char *arithmetic(Fn *f, TokenKind op, const Type *type, const char *a, const char *b, size_t offset) {
    const char *helper = op_helper(op);
    if (type->tag == T_INT && is_wrapping(op) && !f->g->options.overflow_checks) {
        const char *wide = wrapping_type(type);
        return sosu_format("((%s)((%s)%s %s (%s)%s))", c_type(f->g, type), wide, a, c_operator(op), wide, b);
    }
    if (type->tag == T_INT && helper) {
        char *where = loc(f, offset);
        char *result;
        if (op == TOK_SHL || op == TOK_SHR || op == TOK_SHL_EQ || op == TOK_SHR_EQ) {
            result = sosu_format("sosu_%s_%.*s(%s, (int64_t)(%s), %s)", helper, NAME_ARGS(type->name), a, b, where);
        } else {
            result = sosu_format("sosu_%s_%.*s(%s, %s, %s)", helper, NAME_ARGS(type->name), a, b, where);
        }
        free(where);
        return result;
    }
    if (type->tag == T_INT) return sosu_format("((%s)(%s %s %s))", c_type(f->g, type), a, c_operator(op), b);
    return sosu_format("(%s %s %s)", a, c_operator(op), b);
}

static char *negate(Fn *f, const Type *type, const char *value, size_t offset) {
    if (type->tag != T_INT) return sosu_format("(-%s)", value);
    if (!f->g->options.overflow_checks) {
        return sosu_format("((%s)(0u - (%s)%s))", c_type(f->g, type), wrapping_type(type), value);
    }
    char *where = loc(f, offset);
    char *result = sosu_format("sosu_neg_%.*s(%s, %s)", NAME_ARGS(type->name), value, where);
    free(where);
    return result;
}

/* `&&`, `||` and `?:` evaluate their right side only when needed: if it emits
   statements, they go into an `if`. */
static char *gen_logical(Fn *f, Expr *e) {
    bool is_and = e->as.binary.op == TOK_AMP_AMP;
    char *left = gen_expr(f, e->as.binary.left);
    Buf right_code = {0};
    int indent = f->indent;
    f->indent++;
    char *right = gen_apart(f, e->as.binary.right, &right_code);
    f->indent = indent;
    char *result;
    if (!right_code.items) {
        result = sosu_format("(%s %s %s)", left, is_and ? "&&" : "||", right);
    } else {
        result = new_temp(f);
        emit(f, "bool %s = %s;", result, left);
        emit(f, "if (%s%s) {", is_and ? "" : "!", result);
        buf_puts(f->out, right_code.items);
        emit(f, "    %s = %s;", result, right);
        emit(f, "}");
    }
    free(left);
    free(right);
    free(right_code.items);
    return result;
}

static char *gen_ternary(Fn *f, Expr *e) {
    char *cond = gen_expr(f, e->as.ternary.cond);
    Buf then_code = {0}, else_code = {0};
    int indent = f->indent;
    f->indent++;
    char *then_value = gen_apart(f, e->as.ternary.then_expr, &then_code);
    char *else_value = gen_apart(f, e->as.ternary.else_expr, &else_code);
    f->indent = indent;
    char *result;
    if (!then_code.items && !else_code.items) {
        result = sosu_format("(%s ? %s : %s)", cond, then_value, else_value);
    } else {
        result = new_temp(f);
        emit(f, "%s %s;", value_type(f->g, e->type), result);
        emit(f, "if (%s) {", cond);
        if (then_code.items) buf_puts(f->out, then_code.items);
        emit(f, "    %s = %s;", result, then_value);
        emit(f, "} else {");
        if (else_code.items) buf_puts(f->out, else_code.items);
        emit(f, "    %s = %s;", result, else_value);
        emit(f, "}");
    }
    free(cond);
    free(then_value);
    free(else_value);
    free(then_code.items);
    free(else_code.items);
    return result;
}

static char *gen_binary(Fn *f, Expr *e) {
    TokenKind op = e->as.binary.op;
    if (op == TOK_AMP_AMP || op == TOK_PIPE_PIPE) return gen_logical(f, e);
    Expr *operands[2] = { e->as.binary.left, e->as.binary.right };
    char **values = gen_operands(f, operands, 2);
    const Type *lt = operands[0]->type;
    char *result;
    switch (op) {
    case TOK_EQ_EQ:
    case TOK_BANG_EQ: {
        const char *negate = op == TOK_BANG_EQ ? "!" : "";
        if (lt->tag == T_STR) result = sosu_format("%ssosu_str_eq(%s, %s)", negate, values[0], values[1]);
        else if (lt->tag == T_ENUM) result = sosu_format("%s(%s.tag == %s.tag)", negate, values[0], values[1]);
        else result = sosu_format("(%s %s %s)", values[0], op == TOK_EQ_EQ ? "==" : "!=", values[1]);
        break;
    }
    case TOK_LT: case TOK_LT_EQ: case TOK_GT: case TOK_GT_EQ:
        result = sosu_format("(%s %s %s)", values[0], token_kind_spelling(op), values[1]);
        break;
    default:
        if (lt->tag == T_POINTER) result = sosu_format("(%s %s %s)", values[0], c_operator(op), values[1]);
        else result = arithmetic(f, op, e->type, values[0], values[1], e->offset);
        break;
    }
    free_values(values, 2);
    return result;
}

static char *gen_unary(Fn *f, Expr *e) {
    Expr *operand = e->as.unary.operand;
    if (e->as.unary.op == TOK_MINUS && operand->kind == EXPR_INT) return gen_int(f, e->type, operand->as.int_value, true);
    if (e->as.unary.op == TOK_AMP) {
        char *value = gen_expr(f, operand);
        char *result = sosu_format("(&%s)", value);
        free(value);
        return result;
    }
    char *value = gen_expr(f, operand);
    char *result;
    switch (e->as.unary.op) {
    case TOK_MINUS:
        result = negate(f, e->type, value, e->offset);
        break;
    case TOK_BANG: result = sosu_format("(!%s)", value); break;
    case TOK_TILDE: result = sosu_format("((%s)~%s)", c_type(f->g, e->type), value); break;
    case TOK_STAR: result = sosu_format("(*%s)", value); break;
    default: result = sosu_strdup(value); break;
    }
    free(value);
    return result;
}

static char *gen_cast(Fn *f, Expr *e) {
    const Type *from = e->as.cast.value->type;
    const Type *to = e->type;
    char *value = gen_expr(f, e->as.cast.value);
    char *result;
    if (from == to) {
        return value;
    } else if (from->tag == T_FLOAT && to->tag == T_INT) {
        char *where = loc(f, e->offset);
        result = sosu_format("sosu_f2i_%.*s((double)%s, %s)", NAME_ARGS(to->name), value, where);
        free(where);
    } else if ((from->tag == T_POINTER && to->tag == T_INT) || (from->tag == T_INT && to->tag == T_POINTER)) {
        result = sosu_format("((%s)(uintptr_t)%s)", c_type(f->g, to), value);
    } else {
        result = sosu_format("((%s)%s)", c_type(f->g, to), value);
    }
    free(value);
    return result;
}

/* `x as? T*`: the class of the object decides, at run time. */
static char *gen_try_cast(Fn *f, Expr *e) {
    const Type *target = e->type->as.inner->as.inner;
    char *object = materialize(f, e->as.cast.value->type, gen_expr(f, e->as.cast.value));
    const char *option = value_type(f->g, e->type);
    const char *cls = value_type(f->g, target);
    char *result = new_temp(f);
    emit(f, "%s %s = { false };", option, result);
    emit(f, "if (sosu_instance_of(SOSU_CLASS_OF(%s), (const sosu_class *)&%s__vtable)) %s = (%s){ true, (%s *)%s };",
         object, cls, result, option, cls, object);
    free(object);
    return result;
}

static char *gen_index(Fn *f, Expr *e) {
    Expr *operands[2] = { e->as.index.object, e->as.index.index };
    char **values = gen_operands(f, operands, 2);
    const Type *type = operands[0]->type;
    bool through_pointer = type->tag == T_POINTER;
    const Type *base = through_pointer ? type->as.inner : type;
    const Type *index_type = operands[1]->type;
    const char *helper = index_type->as.integer.is_signed ? "sosu_index" : "sosu_uindex";
    const char *cast = index_type->as.integer.is_signed ? "int64_t" : "uint64_t";
    char *where = loc(f, e->offset);
    char *result;
    const char *access = through_pointer ? "->" : ".";
    if (!f->g->options.bounds_checks) {
        result = sosu_format("%s%s%s[(size_t)%s]", values[0], access, base->tag == T_ARRAY ? "items" : "ptr", values[1]);
    } else if (base->tag == T_ARRAY) {
        result = sosu_format("%s%sitems[%s((%s)%s, %llu, %s)]", values[0], access, helper, cast, values[1],
                             (unsigned long long)base->as.array.size, where);
    } else {
        char *slice = values[0];
        if (!through_pointer && operands[0]->kind != EXPR_NAME) slice = values[0] = materialize(f, base, values[0]);
        result = sosu_format("%s%sptr[%s((%s)%s, %s%slen, %s)]", slice, access, helper, cast, values[1], slice, access,
                             where);
    }
    free(where);
    free_values(values, 2);
    return result;
}

static char *gen_field(Fn *f, Expr *e) {
    if (e->resolution == RESOLVED_FN) return fn_c_name(e->target);
    if (e->resolution == RESOLVED_VARIANT) return gen_variant(f, e, e->target, e->target_index, NULL);
    Expr *object = e->as.field.object;
    Name name = e->as.field.name;
    char *value = gen_expr(f, object);
    const Type *type = object->type;
    bool through_pointer = type->tag == T_POINTER;
    const Type *base = through_pointer ? type->as.inner : type;
    const char *access = through_pointer ? "->" : ".";
    char *result;
    if (base->tag == T_ARRAY && name_is(name, "len")) {
        result = sosu_format("((uint64_t)%llu)", (unsigned long long)base->as.array.size);
    } else if (name_is(name, "len") && (base->tag == T_SLICE || base->tag == T_STR)) {
        result = sosu_format("((uint64_t)%s%slen)", value, access);
    } else if (base->tag == T_STR && name_is(name, "bytes")) {
        char *text = through_pointer ? sosu_format("(*%s)", value) : sosu_strdup(value);
        if (!is_place(object)) {
            char *temp = materialize(f, base, text);
            text = temp;
        }
        result = sosu_format("((%s){ (uint8_t *)%s.ptr, %s.len })", value_type(f->g, e->type), text, text);
        free(text);
    } else {
        char *path = base->tag == T_CLASS ? field_path(base->as.info, name) : sosu_strdup("");
        result = sosu_format("%s%s%sf_%.*s", value, access, path, NAME_ARGS(name));
        free(path);
    }
    free(value);
    return result;
}

static char *gen_struct_literal(Fn *f, Expr *e) {
    const TypeInfo *info = e->type->as.info;
    size_t count = e->as.struct_lit.field_count;
    Expr **exprs = sosu_alloc((count + 1) * sizeof *exprs);
    for (size_t i = 0; i < count; i++) exprs[i] = e->as.struct_lit.fields[i].value;
    char **values = gen_operands(f, exprs, count);

    Buf fields = {0};
    for (size_t j = 0; j < info->field_count; j++) {
        Name name = info->fields[j].name;
        char *value = NULL;
        for (size_t i = 0; i < count; i++) {
            if (name_eq(e->as.struct_lit.fields[i].name, name)) value = values[i];
        }
        char *fallback = value ? NULL : gen_expr(f, info->fields[j].decl->init);
        buf_printf(&fields, "%s.f_%.*s = %s", j ? ", " : "", NAME_ARGS(name), value ? value : fallback);
        free(fallback);
    }
    char *result = sosu_format("((%s){ %s })", value_type(f->g, e->type), fields.items ? fields.items : "0");
    free(fields.items);
    free_values(values, count);
    free(exprs);
    return result;
}

/* ---- Calls ---- */

static void gen_print(Fn *f, Expr *e) {
    Expr **args = e->as.call.args;
    size_t count = e->as.call.arg_count;
    char **values = gen_operands(f, args + 1, count - 1);
    Buf format = {0};
    decode_string(f->unit->file.text + args[0]->offset + 1, args[0]->length - 2, &format);
    size_t start = 0;
    size_t next = 0;
    for (size_t i = 0; i <= format.count; i++) {
        bool placeholder = i + 1 < format.count && format.items[i] == '{' && format.items[i + 1] == '}';
        if (!placeholder && i < format.count) continue;
        if (i > start) {
            char *text = c_string(format.items + start, i - start);
            emit(f, "sosu_print_str(SOSU_STR(%s, %zu));", text, i - start);
            free(text);
        }
        if (placeholder) {
            const Type *type = args[next + 1]->type;
            const char *value = values[next++];
            switch (type->tag) {
            case T_INT:
                emit(f, type->as.integer.is_signed ? "sosu_print_i64((int64_t)%s);" : "sosu_print_u64((uint64_t)%s);", value);
                break;
            case T_FLOAT: emit(f, "sosu_print_f64((double)%s);", value); break;
            case T_BOOL: emit(f, "sosu_print_bool(%s);", value); break;
            case T_CHAR: emit(f, "sosu_print_char(%s);", value); break;
            default: emit(f, "sosu_print_str(%s);", value); break;
            }
            i++;
        }
        start = i + 1;
    }
    emit(f, "sosu_print_end();");
    free(format.items);
    free_values(values, count - 1);
}

static char *gen_builtin_call(Fn *f, Expr *e, const char *name) {
    Expr **args = e->as.call.args;
    size_t count = e->as.call.arg_count;
    char **values = gen_operands(f, args, count);
    const char *type = e->type->tag == T_VOID ? "void" : value_type(f->g, e->type);
    char *result = NULL;
    if (strcmp(name, "some") == 0) {
        result = sosu_format("((%s){ true, %s })", type, values[0]);
    } else if (strcmp(name, "ok") == 0) {
        result = count ? sosu_format("((%s){ .ok = true, .as.value = %s })", type, values[0])
                       : sosu_format("((%s){ .ok = true })", type);
    } else if (strcmp(name, "err") == 0) {
        result = sosu_format("((%s){ .ok = false, .as.error = %s })", type, values[0]);
    } else if (strcmp(name, "assert") == 0) {
        char *where = loc(f, e->offset);
        char *text = source_text(f, args[0]);
        emit(f, "sosu_check(%s, %s, \"assertion failed: \" %s);", values[0], where, text);
        free(where);
        free(text);
    } else {
        /* min, max, abs: operands are read twice, so they are stored first */
        char *a = materialize(f, args[0]->type, sosu_strdup(values[0]));
        if (strcmp(name, "abs") == 0) {
            char *negated = negate(f, e->type, a, e->offset);
            result = sosu_format("(%s < 0 ? %s : %s)", a, negated, a);
            free(negated);
        } else {
            char *b = materialize(f, args[1]->type, sosu_strdup(values[1]));
            result = sosu_format("(%s %s %s ? %s : %s)", a, strcmp(name, "min") == 0 ? "<" : ">", b, a, b);
            free(b);
        }
        free(a);
    }
    free_values(values, count);
    return result;
}

/* Arena.new(size), arena.alloc(T), arena.try_alloc(T), arena.new(T, ...), arena.free() */
static char *gen_arena_call(Fn *f, Expr *e, const char *name) {
    Expr **args = e->as.call.args;
    size_t count = e->as.call.arg_count;
    char *where = loc(f, e->offset);
    char *result = NULL;
    if (strcmp(name, "Arena.new") == 0) {
        char *size = gen_expr(f, args[0]);
        result = materialize(f, e->type, sosu_format("sosu_arena_new(%s, %s)", size, where));
        free(size);
        free(where);
        return result;
    }
    char *arena = address_of(f, e->as.call.callee->as.field.object);
    if (strcmp(name, "arena.free") == 0) {
        emit(f, "sosu_arena_free(%s);", arena);
        free(arena);
        free(where);
        return NULL;
    }
    const Type *type = args[0]->type;
    const char *ctype = value_type(f->g, type);
    if (strcmp(name, "arena.try_alloc") == 0) {
        char *pointer = new_temp(f);
        emit(f, "%s *%s = (%s *)sosu_arena_try_alloc(%s, sizeof(%s));", ctype, pointer, ctype, arena, ctype);
        result = materialize(f, e->type, sosu_format("((%s){ %s != NULL, %s })", value_type(f->g, e->type), pointer, pointer));
        free(pointer);
    } else {
        char **values = gen_operands(f, args + 1, count - 1);
        result = materialize(f, e->type, sosu_format("((%s *)sosu_arena_alloc(%s, sizeof(%s), %s))", ctype, arena, ctype, where));
        if (strcmp(name, "arena.new") == 0) {
            if (type->tag == T_CLASS) {
                const char *root = c_type(f->g, root_of(type->as.info)->type);
                emit(f, "((%s *)%s)->vt = (const %s__vt *)&%s__vtable;", root, result, root, ctype);
            }
            Name init_name = { "init", 4, 0 };
            const MethodInfo *init = method_of(type->as.info, init_name);
            if (init) {
                Buf call_args = {0};
                for (size_t i = 0; i + 1 < count; i++) buf_printf(&call_args, ", %s", values[i]);
                char *function = method_c_name(f->g, init);
                emit(f, "%s((%s *)%s%s);", function, c_type(f->g, init->owner->type), result,
                     call_args.items ? call_args.items : "");
                free(function);
                free(call_args.items);
            }
        }
        free_values(values, count - 1);
    }
    free(arena);
    free(where);
    return result;
}

/* The function and `self` of a call to a method of a class or an interface: a slot of
   the class table for a virtual method (except through `base`), else a direct call. */
static void class_call(Fn *f, const MethodInfo *method, const char *object, bool through_base, char **function,
                       char **self) {
    const TypeInfo *owner = method->owner;
    Name name = method->decl->name;
    if (owner->type->tag == T_INTERFACE) {
        *function = method_c_name(f->g, method); /* the dispatcher */
        *self = sosu_format("((%s *)%s)", c_type(f->g, owner->type), object);
        return;
    }
    const MethodInfo *slot = through_base ? NULL : slot_of(method);
    if (slot) {
        const char *introducer = c_type(f->g, slot->owner->type);
        const char *root = c_type(f->g, root_of(slot->owner)->type);
        *function = sosu_format("((const %s__vt *)((%s *)%s)->vt)->m_%.*s", introducer, root, object, NAME_ARGS(name));
        *self = sosu_format("((%s *)%s)", introducer, object);
    } else {
        *function = method_c_name(f->g, method);
        *self = sosu_format("((%s *)%s)", c_type(f->g, owner->type), object);
    }
}

static bool is_object(const Type *type) {
    return type->tag == T_CLASS || type->tag == T_INTERFACE;
}

static char *gen_call(Fn *f, Expr *e) {
    Expr *callee = e->as.call.callee;
    if (callee->resolution == RESOLVED_PRINT) {
        gen_print(f, e);
        return NULL;
    }
    if (callee->resolution == RESOLVED_BUILTIN) {
        const char *name = callee->target;
        if (strncmp(name, "arena.", 6) == 0 || strcmp(name, "Arena.new") == 0) return gen_arena_call(f, e, name);
        return gen_builtin_call(f, e, name);
    }

    size_t count = e->as.call.arg_count;
    char *function;
    char *self = NULL;
    switch (callee->resolution) {
    case RESOLVED_FN: function = fn_c_name(callee->target); break;
    case RESOLVED_SELF_METHOD:
        if (f->self_info->type->tag == T_CLASS) {
            class_call(f, callee->target, "v_self", false, &function, &self);
        } else {
            function = method_c_name(f->g, callee->target);
            self = sosu_strdup("v_self");
        }
        break;
    case RESOLVED_METHOD: {
        Expr *object = callee->as.field.object;
        const Type *type = object->type;
        if (type->tag == T_POINTER && is_object(type->as.inner)) {
            char *value = gen_expr(f, object);
            if (object->kind != EXPR_NAME && object->kind != EXPR_SELF && object->kind != EXPR_BASE) {
                value = materialize(f, type, value); /* read twice by a virtual call */
            }
            class_call(f, callee->target, value, object->kind == EXPR_BASE, &function, &self);
            free(value);
        } else {
            function = method_c_name(f->g, callee->target);
            self = address_of(f, object);
        }
        break;
    }
    case RESOLVED_VARIANT: {
        char **values = gen_operands(f, e->as.call.args, count);
        char *result = gen_variant(f, e, callee->target, callee->target_index, values);
        free_values(values, count);
        return result;
    }
    default:
        function = gen_expr(f, callee); /* a function value */
        if (has_call(callee) || count > 0) function = materialize(f, callee->type, function);
        break;
    }

    char **values = gen_operands(f, e->as.call.args, count);
    Buf args = {0};
    if (self) buf_puts(&args, self);
    for (size_t i = 0; i < count; i++) buf_printf(&args, "%s%s", self || i ? ", " : "", values[i]);
    char *call = sosu_format("%s(%s)", function, args.items ? args.items : "");
    free(args.items);
    free_values(values, count);
    free(function);
    free(self);

    if (e->type->tag == T_VOID) {
        emit(f, "%s;", call);
        free(call);
        return NULL;
    }
    return materialize(f, e->type, call);
}

/* ---- match ---- */

/* The condition of an arm and the C expressions of the values it binds. */
static char *arm_condition(Fn *f, const char *subject, const Type *type, const Pattern *pattern, size_t *index) {
    Name name = pattern->path.items[pattern->path.count - 1];
    if (type->tag == T_OPTION) {
        *index = name_is(name, "some") ? 0 : 1;
        return sosu_format(*index == 0 ? "%s.has" : "!%s.has", subject);
    }
    if (type->tag == T_RESULT) {
        *index = name_is(name, "ok") ? 0 : 1;
        return sosu_format(*index == 0 ? "%s.ok" : "!%s.ok", subject);
    }
    const TypeInfo *info = type->as.info;
    for (size_t i = 0; i < info->variant_count; i++) {
        if (name_eq(info->variants[i].name, name)) *index = i;
    }
    (void)f;
    return sosu_format("%s.tag == %zu", subject, *index);
}

static char *binding_value(const char *subject, const Type *type, size_t index, size_t field) {
    if (type->tag == T_OPTION) return sosu_format("%s.value", subject);
    if (type->tag == T_RESULT) return sosu_format(index == 0 ? "%s.as.value" : "%s.as.error", subject);
    const VariantInfo *variant = &type->as.info->variants[index];
    return sosu_format("%s.as.v_%.*s.f%zu", subject, NAME_ARGS(variant->name), field);
}

static const Type *binding_type(const Type *type, size_t index, size_t field) {
    if (type->tag == T_OPTION) return type->as.inner;
    if (type->tag == T_RESULT) return index == 0 ? type->as.result.value : type->as.result.error;
    return type->as.info->variants[index].fields[field];
}

static void gen_expr_stmt(Fn *f, Expr *e);

static char *gen_match(Fn *f, Expr *e, bool statement) {
    Expr *subject_expr = e->as.match.subject;
    const Type *type = subject_expr->type;
    char *value = gen_expr(f, subject_expr);
    if (type->tag == T_POINTER) {
        char *deref = sosu_format("(*%s)", value);
        free(value);
        value = deref;
        type = type->as.inner;
    }
    char *subject = materialize(f, type, value);
    char *result = NULL;
    if (!statement) {
        result = new_temp(f);
        emit(f, "%s %s;", value_type(f->g, e->type), result);
    }
    for (size_t a = 0; a < e->as.match.arm_count; a++) {
        const MatchArm *arm = &e->as.match.arms[a];
        const Pattern *pattern = &arm->pattern;
        size_t index = 0;
        if (pattern->kind == PATTERN_WILDCARD) {
            emit(f, a ? "} else {" : "{");
        } else {
            char *condition = arm_condition(f, subject, type, pattern, &index);
            emit(f, a ? "} else if (%s) {" : "if (%s) {", condition);
            free(condition);
        }
        f->indent++;
        for (size_t j = 0; j < pattern->binding_count; j++) {
            if (name_is(pattern->bindings[j], "_")) continue;
            char *bound = binding_value(subject, type, index, j);
            emit(f, "%s v_%.*s = %s;", value_type(f->g, binding_type(type, index, j)), NAME_ARGS(pattern->bindings[j]),
                 bound);
            free(bound);
        }
        if (arm->block) {
            gen_block(f, arm->block);
        } else if (statement) {
            gen_expr_stmt(f, arm->expr);
        } else {
            char *arm_value = gen_expr(f, arm->expr);
            emit(f, "%s = %s;", result, arm_value);
            free(arm_value);
        }
        f->indent--;
    }
    if (e->as.match.arm_count > 0) {
        char *where = loc(f, e->offset);
        emit(f, "} else {");
        emit(f, "    sosu_fail(%s, \"no arm matched\");", where);
        emit(f, "}");
        free(where);
    }
    free(subject);
    return result;
}

/* ---- Returns and defer ---- */

static void gen_defers(Fn *f, int above_depth) {
    for (size_t i = f->defers.count; i > 0; i--) {
        const Deferred *d = &f->defers.items[i - 1];
        if (d->depth > above_depth) gen_stmt(f, d->stmt);
    }
}

/* Checks `ensures` (with `result` bound to the returned value), runs every `defer`, returns. */
static void gen_return(Fn *f, char *value, size_t offset) {
    if (f->fn && f->g->options.contracts) {
        for (size_t i = 0; i < f->fn->contract_count; i++) {
            const Contract *contract = &f->fn->contracts[i];
            if (contract->kind != CONTRACT_ENSURES) continue;
            emit(f, "{");
            f->indent++;
            if (value) emit(f, "%s v_result = %s;", value_type(f->g, f->ret), value);
            char *check = gen_expr(f, contract->expr);
            char *where = loc(f, contract->expr->offset);
            char *text = source_text(f, contract->expr);
            emit(f, "sosu_check(%s, %s, \"ensures failed: \" %s);", check, where, text);
            free(check);
            free(where);
            free(text);
            f->indent--;
            emit(f, "}");
        }
    }
    gen_defers(f, -1);
    (void)offset;
    if (value) emit(f, "return %s;", value);
    else emit(f, "return;");
}

static char *gen_try(Fn *f, Expr *e) {
    const Type *type = e->as.inner->type;
    char *value = gen_expr(f, e->as.inner);
    char *result = materialize(f, type, value);
    emit(f, "if (!%s.ok) {", result);
    f->indent++;
    char *error = sosu_format("((%s){ .ok = false, .as.error = %s.as.error })", value_type(f->g, f->ret), result);
    char *returned = materialize(f, f->ret, error);
    gen_return(f, returned, e->offset);
    free(returned);
    f->indent--;
    emit(f, "}");
    char *unwrapped = type->as.result.value->tag == T_VOID ? NULL : sosu_format("%s.as.value", result);
    free(result);
    return unwrapped;
}

static char *gen_expr(Fn *f, Expr *e) {
    switch (e->kind) {
    case EXPR_INT: return gen_int(f, e->type, e->as.int_value, false);
    case EXPR_FLOAT:
        return sosu_format("((%s)%.*s)", c_type(f->g, e->type), (int)e->length, f->unit->file.text + e->offset);
    case EXPR_STRING: return gen_string(f, e);
    case EXPR_CHAR: return sosu_format("((uint32_t)%u)", (unsigned)e->as.char_value);
    case EXPR_BOOL: return sosu_strdup(e->as.bool_value ? "true" : "false");
    case EXPR_NAME: return gen_name(f, e);
    case EXPR_SELF: return sosu_strdup("v_self");
    case EXPR_BASE: return sosu_format("((%s)v_self)", c_type(f->g, e->type));
    case EXPR_RESULT: return sosu_strdup("v_result");
    case EXPR_UNARY: return gen_unary(f, e);
    case EXPR_BINARY: return gen_binary(f, e);
    case EXPR_CAST: return gen_cast(f, e);
    case EXPR_TERNARY: return gen_ternary(f, e);
    case EXPR_CALL: return gen_call(f, e);
    case EXPR_INDEX: return gen_index(f, e);
    case EXPR_FIELD: return gen_field(f, e);
    case EXPR_STRUCT_LIT: return gen_struct_literal(f, e);
    case EXPR_MATCH: return gen_match(f, e, false);
    case EXPR_TRY: return gen_try(f, e);
    case EXPR_TRY_CAST: return gen_try_cast(f, e);
    default: return sosu_strdup("0"); /* rejected earlier: ranges outside `for` */
    }
}

/* ---- Statements ---- */

static void gen_expr_stmt(Fn *f, Expr *e) {
    if (e->kind == EXPR_MATCH) {
        free(gen_match(f, e, true));
        return;
    }
    char *value = gen_expr(f, e);
    if (value && e->kind != EXPR_CALL) emit(f, "(void)%s;", value);
    free(value);
}

static void gen_assign(Fn *f, const Stmt *stmt) {
    Expr *target = stmt->as.assign.target;
    Expr *value_expr = stmt->as.assign.value;
    TokenKind op = stmt->as.assign.op;
    char *place = gen_expr(f, target);
    bool simple = target->kind == EXPR_NAME;
    if (!simple && (has_call(value_expr) || op != TOK_EQ)) {
        char *pointer = new_temp(f);
        emit(f, "%s *%s = &%s;", value_type(f->g, target->type), pointer, place);
        free(place);
        place = sosu_format("(*%s)", pointer);
        free(pointer);
    }
    char *value = gen_expr(f, value_expr);
    if (op == TOK_EQ) {
        emit(f, "%s = %s;", place, value);
    } else if (target->type->tag == T_POINTER) {
        emit(f, "%s = %s %s %s;", place, place, op == TOK_PLUS_EQ ? "+" : "-", value);
    } else {
        char *combined = arithmetic(f, op, target->type, place, value, stmt->offset);
        emit(f, "%s = %s;", place, combined);
        free(combined);
    }
    free(place);
    free(value);
}

static void push_loop(Fn *f, int continue_label) {
    Loop loop = { f->depth + 1, continue_label };
    SOSU_VEC_PUSH(&f->loops, loop);
}

static void gen_for_in(Fn *f, const Stmt *stmt) {
    Expr *iterable = stmt->as.for_in.iterable;
    Name name = stmt->as.for_in.name;
    emit(f, "{");
    f->indent++;
    if (iterable->kind == EXPR_RANGE) {
        const Type *type = iterable->type;
        const char *ctype = value_type(f->g, type);
        Expr *bounds[2] = { iterable->as.range.start, iterable->as.range.end };
        char **values = gen_operands(f, bounds, 2);
        char *start = materialize(f, type, sosu_strdup(values[0]));
        char *end = materialize(f, type, sosu_strdup(values[1]));
        free_values(values, 2);
        if (!iterable->as.range.inclusive) {
            push_loop(f, -1);
            emit(f, "for (%s v_%.*s = %s; v_%.*s < %s; v_%.*s++) {", ctype, NAME_ARGS(name), start, NAME_ARGS(name), end,
                 NAME_ARGS(name));
            gen_block(f, stmt->as.for_in.body);
            emit(f, "}");
        } else {
            /* `a..=b` up to the largest value: stop after `b` rather than going past it */
            int label = ++f->labels;
            push_loop(f, label);
            emit(f, "if (%s <= %s) for (%s v_%.*s = %s;; v_%.*s++) {", start, end, ctype, NAME_ARGS(name), start,
                 NAME_ARGS(name));
            gen_block(f, stmt->as.for_in.body);
            emit(f, "    next%d:;", label);
            emit(f, "    if (v_%.*s == %s) break;", NAME_ARGS(name), end);
            emit(f, "}");
        }
        free(start);
        free(end);
    } else if (iterable->kind == EXPR_CALL && iterable->as.call.callee->resolution == RESOLVED_BUILTIN) {
        /* s.chars() */
        Expr *object = iterable->as.call.callee->as.field.object;
        char *value = gen_expr(f, object);
        if (object->type->tag == T_POINTER) {
            char *deref = sosu_format("(*%s)", value);
            free(value);
            value = deref;
        }
        char *text = materialize(f, builtin(f->g, "str"), value);
        char *pos = new_temp(f);
        emit(f, "size_t %s = 0;", pos);
        push_loop(f, -1);
        emit(f, "while (%s < %s.len) {", pos, text);
        emit(f, "    uint32_t v_%.*s = sosu_next_char(%s, &%s);", NAME_ARGS(name), text, pos);
        gen_block(f, stmt->as.for_in.body);
        emit(f, "}");
        free(text);
        free(pos);
    } else {
        const Type *type = iterable->type;
        char *value = gen_expr(f, iterable);
        bool through_pointer = type->tag == T_POINTER;
        const Type *base = through_pointer ? type->as.inner : type;
        char *items;
        char *length;
        if (base->tag == T_ARRAY) {
            /* loop over the array in place, through a pointer */
            char *pointer;
            if (through_pointer) {
                pointer = sosu_strdup(value);
            } else if (is_place(iterable)) {
                pointer = sosu_format("(&%s)", value);
            } else {
                char *temp = materialize(f, base, sosu_strdup(value));
                pointer = sosu_format("(&%s)", temp);
                free(temp);
            }
            char *stored = materialize(f, type_pointer(&f->g->s->types, (Type *)base), pointer);
            items = sosu_format("%s->items", stored);
            length = sosu_format("%llu", (unsigned long long)base->as.array.size);
            free(stored);
        } else {
            char *slice = materialize(f, base, through_pointer ? sosu_format("(*%s)", value) : sosu_strdup(value));
            items = sosu_format("%s.ptr", slice);
            length = sosu_format("%s.len", slice);
            free(slice);
        }
        free(value);
        char *counter = new_temp(f);
        push_loop(f, -1);
        emit(f, "for (size_t %s = 0; %s < %s; %s++) {", counter, counter, length, counter);
        emit(f, "    %s v_%.*s = %s[%s];", value_type(f->g, base->tag == T_ARRAY ? base->as.array.element : base->as.inner),
             NAME_ARGS(name), items, counter);
        gen_block(f, stmt->as.for_in.body);
        emit(f, "}");
        free(counter);
        free(items);
        free(length);
    }
    f->loops.count--;
    f->indent--;
    emit(f, "}");
}

static void gen_stmt(Fn *f, const Stmt *stmt) {
    emit_line(f, stmt->offset);
    switch (stmt->kind) {
    case STMT_BLOCK:
        emit(f, "{");
        gen_block(f, stmt);
        emit(f, "}");
        break;
    case STMT_VAR: {
        char *value = gen_expr(f, stmt->as.var.init);
        emit(f, "%s v_%.*s = %s;", value_type(f->g, stmt->as.var.init->type), NAME_ARGS(stmt->as.var.name), value);
        free(value);
        break;
    }
    case STMT_ASSIGN:
        gen_assign(f, stmt);
        break;
    case STMT_EXPR:
        gen_expr_stmt(f, stmt->as.expr);
        break;
    case STMT_IF: {
        char *cond = gen_expr(f, stmt->as.if_.cond);
        emit(f, "if (%s) {", cond);
        free(cond);
        gen_block(f, stmt->as.if_.then_block);
        const Stmt *else_stmt = stmt->as.if_.else_stmt;
        if (else_stmt) {
            emit(f, "} else {");
            if (else_stmt->kind == STMT_BLOCK) {
                gen_block(f, else_stmt);
            } else { /* else if: its condition may need statements, so it is nested */
                f->indent++;
                gen_stmt(f, else_stmt);
                f->indent--;
            }
        }
        emit(f, "}");
        break;
    }
    case STMT_WHILE: {
        emit(f, "for (;;) {");
        f->indent++;
        char *cond = gen_expr(f, stmt->as.while_.cond);
        emit(f, "if (!%s) break;", cond);
        free(cond);
        f->indent--;
        push_loop(f, -1);
        gen_block(f, stmt->as.while_.body);
        f->loops.count--;
        emit(f, "}");
        break;
    }
    case STMT_FOR: {
        int label = ++f->labels;
        emit(f, "{");
        f->indent++;
        f->depth++;
        if (stmt->as.for_.init) gen_stmt(f, stmt->as.for_.init);
        emit(f, "for (;;) {");
        f->indent++;
        if (stmt->as.for_.cond) {
            char *cond = gen_expr(f, stmt->as.for_.cond);
            emit(f, "if (!%s) break;", cond);
            free(cond);
        }
        f->indent--;
        push_loop(f, label);
        gen_block(f, stmt->as.for_.body);
        f->loops.count--;
        f->indent++;
        emit(f, "next%d:;", label);
        if (stmt->as.for_.step) gen_stmt(f, stmt->as.for_.step);
        f->indent--;
        emit(f, "}");
        f->depth--;
        f->indent--;
        emit(f, "}");
        break;
    }
    case STMT_FOR_IN:
        gen_for_in(f, stmt);
        break;
    case STMT_RETURN: {
        char *value = stmt->as.expr ? gen_expr(f, stmt->as.expr) : NULL;
        if (value) value = materialize(f, f->ret, value);
        gen_return(f, value, stmt->offset);
        free(value);
        break;
    }
    case STMT_BREAK:
    case STMT_CONTINUE: {
        Loop loop = f->loops.items[f->loops.count - 1];
        gen_defers(f, loop.depth - 1);
        if (stmt->kind == STMT_BREAK) emit(f, "break;");
        else if (loop.continue_label >= 0) emit(f, "goto next%d;", loop.continue_label);
        else emit(f, "continue;");
        break;
    }
    case STMT_DEFER: {
        Deferred deferred = { stmt->as.inner, f->depth };
        SOSU_VEC_PUSH(&f->defers, deferred);
        break;
    }
    case STMT_UNSAFE:
        emit(f, "{");
        gen_block(f, stmt->as.inner);
        emit(f, "}");
        break;
    }
}

/* The statements of a block, at one more level of indentation and scope; its `defer`
   statements run at its end. */
static void gen_block(Fn *f, const Stmt *block) {
    f->indent++;
    f->depth++;
    size_t defers = f->defers.count;
    for (size_t i = 0; i < block->as.block.count; i++) gen_stmt(f, block->as.block.items[i]);
    gen_defers(f, f->depth - 1);
    f->defers.count = defers;
    f->depth--;
    f->indent--;
}

/* ---- Functions ---- */

static char *signature(Gen *g, const char *name, const Type *type, const FnDecl *fn, const TypeInfo *self_info) {
    Buf params = {0};
    if (self_info) buf_printf(&params, "%s *v_self", value_type(g, self_info->type));
    for (size_t i = 0; i < fn->param_count; i++) {
        buf_printf(&params, "%s%s v_%.*s", params.items ? ", " : "", value_type(g, type->as.fn.params[i]),
                   NAME_ARGS(fn->params[i].name));
    }
    Type *ret = type->as.fn.ret;
    char *result = sosu_format("static %s %s(%s)", ret->tag == T_VOID ? "void" : value_type(g, ret), name,
                               params.items ? params.items : "void");
    free(params.items);
    return result;
}

static void gen_body(Gen *g, Unit *unit, const char *header, const FnDecl *fn, Type *ret, TypeInfo *self_info,
                     const Stmt *body) {
    Fn f = {0};
    Buf code = {0};
    f.g = g;
    f.unit = unit;
    f.out = &code;
    f.fn = fn;
    f.ret = ret;
    f.self_info = self_info;
    buf_printf(&code, "%s {\n", header);
    if (fn && g->options.contracts) {
        for (size_t i = 0; i < fn->contract_count; i++) {
            const Contract *contract = &fn->contracts[i];
            if (contract->kind != CONTRACT_REQUIRES) continue;
            f.indent = 1;
            emit_line(&f, contract->expr->offset);
            char *check = gen_expr(&f, contract->expr);
            char *where = loc(&f, contract->expr->offset);
            char *text = source_text(&f, contract->expr);
            emit(&f, "sosu_check(%s, %s, \"requires failed: \" %s);", check, where, text);
            free(check);
            free(where);
            free(text);
        }
    }
    f.indent = 0;
    gen_block(&f, body);
    if (ret->tag == T_VOID) {
        f.indent = 1;
        gen_return(&f, NULL, body->offset + body->length);
    }
    buf_puts(&code, "}\n\n");
    buf_puts(&g->code, code.items);
    free(code.items);
    free(f.defers.items);
    free(f.loops.items);
}

static void gen_function(Gen *g, FnInfo *fn) {
    char *name = fn_c_name(fn);
    char *header = signature(g, name, fn->type, fn->decl, NULL);
    buf_printf(&g->protos, "%s;\n", header);
    gen_body(g, fn->unit, header, fn->decl, fn->type->as.fn.ret, NULL, fn->decl->body);
    free(header);
    free(name);
}

static void gen_method(Gen *g, TypeInfo *info, MethodInfo *method) {
    if (!method->decl->body) return; /* interface signatures: see gen_tables */
    char *name = method_c_name(g, method);
    char *header = signature(g, name, method->type, method->decl, info);
    buf_printf(&g->protos, "%s;\n", header);
    gen_body(g, info->unit, header, method->decl, method->type->as.fn.ret, info, method->decl->body);
    free(header);
    free(name);
}

/* ---- Class and interface tables ---- */

/* A function with the signature of a slot (`self` typed as `self_type`) that calls `impl`:
   directly for a class table, through the class table for an interface table (a subclass
   may override `impl`). */
static void gen_thunk(Gen *g, Buf *out, const char *name, const char *self_type, const MethodInfo *impl, bool dispatch) {
    char *params = method_params(g, self_type, impl->type);
    const MethodInfo *slot = dispatch ? slot_of(impl) : NULL;
    const TypeInfo *owner = slot ? slot->owner : impl->owner;
    Buf args = {0};
    buf_printf(&args, "(%s *)self", c_type(g, owner->type));
    for (size_t i = 0; i < impl->type->as.fn.param_count; i++) buf_printf(&args, ", a%zu", i);
    char *target = slot ? sosu_format("((const %s__vt *)((%s *)self)->vt)->m_%.*s", c_type(g, owner->type),
                                      c_type(g, root_of(owner)->type), NAME_ARGS(impl->decl->name))
                        : method_c_name(g, impl);
    bool returns = impl->type->as.fn.ret->tag != T_VOID;
    buf_printf(out, "static %s %s(%s) {\n    %s%s(%s);\n}\n\n", return_type(g, impl->type), name, params,
               returns ? "return " : "", target, args.items);
    free(target);
    free(args.items);
    free(params);
}

static char *slot_function(Gen *g, const TypeInfo *cls, const TypeInfo *level, const MethodInfo *slot) {
    const MethodInfo *impl = method_of(cls, slot->decl->name);
    if (impl->owner == level) return method_c_name(g, impl);
    return sosu_format("%s__in_%s__%.*s", c_type(g, cls->type), c_type(g, level->type), NAME_ARGS(slot->decl->name));
}

/* The initializer of the table of `cls`, for its part `level` (`cls` or a parent). */
static void table_init(Gen *g, Buf *out, const TypeInfo *cls, const TypeInfo *level) {
    const char *name = c_type(g, cls->type);
    buf_puts(out, "{ ");
    if (level->parent) {
        table_init(g, out, cls, level->parent);
    } else {
        char *parent = cls->parent ? sosu_format("(const sosu_class *)&%s__vtable", c_type(g, cls->parent->type))
                                   : sosu_strdup("NULL");
        char *interfaces = cls->interface_count ? sosu_format("%s__interfaces", name) : sosu_strdup("NULL");
        buf_printf(out, "{ %s, \"%.*s\", %s, %zu }", parent, NAME_ARGS(cls->name), interfaces, cls->interface_count);
        free(parent);
        free(interfaces);
    }
    for (size_t i = 0; i < level->method_count; i++) {
        if (!introduces_slot(&level->methods[i])) continue;
        char *function = slot_function(g, cls, level, &level->methods[i]);
        buf_printf(out, ", %s", function);
        free(function);
    }
    buf_puts(out, " }");
}

static size_t class_depth(const TypeInfo *cls) {
    size_t depth = 0;
    for (const TypeInfo *t = cls->parent; t; t = t->parent) depth++;
    return depth;
}

static void gen_tables(Gen *g, Buf *out) {
    /* interfaces: an identity, and a dispatcher per method */
    for (size_t i = 0; i < g->s->type_infos.count; i++) {
        const TypeInfo *info = g->s->type_infos.items[i];
        if (info->type->tag != T_INTERFACE) continue;
        const char *name = value_type(g, info->type);
        buf_printf(out, "static const char %s__id = 0;\n\n", name);
        for (size_t j = 0; j < info->method_count; j++) {
            const MethodInfo *method = &info->methods[j];
            char *params = method_params(g, name, method->type);
            Buf args = {0};
            buf_puts(&args, "self");
            for (size_t k = 0; k < method->type->as.fn.param_count; k++) buf_printf(&args, ", a%zu", k);
            char *function = method_c_name(g, method);
            buf_printf(out,
                       "static %s %s(%s) {\n"
                       "    const %s__it *table = (const %s__it *)sosu_find_interface(SOSU_CLASS_OF(self), &%s__id);\n"
                       "    %stable->m_%.*s(%s);\n}\n\n",
                       return_type(g, method->type), function, params, name, name, name,
                       method->type->as.fn.ret->tag == T_VOID ? "" : "return ", NAME_ARGS(method->decl->name), args.items);
            free(function);
            free(args.items);
            free(params);
        }
    }

    /* classes, parents first: interface tables, thunks, then the class table */
    size_t max_depth = 0;
    for (size_t i = 0; i < g->s->type_infos.count; i++) {
        const TypeInfo *info = g->s->type_infos.items[i];
        if (info->type->tag == T_CLASS && class_depth(info) > max_depth) max_depth = class_depth(info);
    }
    for (size_t depth = 0; depth <= max_depth; depth++) {
        for (size_t i = 0; i < g->s->type_infos.count; i++) {
            const TypeInfo *cls = g->s->type_infos.items[i];
            if (cls->type->tag != T_CLASS || class_depth(cls) != depth) continue;
            const char *name = value_type(g, cls->type);

            Buf entries = {0};
            for (size_t j = 0; j < cls->interface_count; j++) {
                const TypeInfo *interface = cls->interfaces[j];
                const char *iname = value_type(g, interface->type);
                Buf slots = {0};
                for (size_t k = 0; k < interface->method_count; k++) {
                    Name method_name = interface->methods[k].decl->name;
                    char *thunk = sosu_format("%s__as_%s__%.*s", name, iname, NAME_ARGS(method_name));
                    gen_thunk(g, out, thunk, iname, method_of(cls, method_name), true);
                    buf_printf(&slots, "%s, ", thunk);
                    free(thunk);
                }
                buf_printf(out, "static const %s__it %s__as_%s = { %s0 };\n\n", iname, name, iname,
                           slots.items ? slots.items : "");
                buf_printf(&entries, "%s{ &%s__id, &%s__as_%s }", j ? ", " : "", iname, name, iname);
                free(slots.items);
            }
            if (entries.items) buf_printf(out, "static const sosu_iface %s__interfaces[] = { %s };\n\n", name, entries.items);
            free(entries.items);

            for (const TypeInfo *level = cls; level; level = level->parent) {
                for (size_t k = 0; k < level->method_count; k++) {
                    const MethodInfo *slot = &level->methods[k];
                    if (!introduces_slot(slot)) continue;
                    const MethodInfo *impl = method_of(cls, slot->decl->name);
                    if (impl->owner == level) continue;
                    char *thunk = slot_function(g, cls, level, slot);
                    gen_thunk(g, out, thunk, c_type(g, level->type), impl, false);
                    free(thunk);
                }
            }
            Buf init = {0};
            table_init(g, &init, cls, cls);
            buf_printf(out, "static const %s__vt %s__vtable = %s;\n\n", name, name, init.items);
            free(init.items);
        }
    }
}

static FnInfo *find_main(Gen *g) {
    Unit *entry = g->program->units.items[0];
    for (size_t i = 0; i < g->s->fns.count; i++) {
        FnInfo *fn = g->s->fns.items[i];
        if (fn->unit == entry && !fn->is_extern && name_is(fn->decl->name, "main")) return fn;
    }
    return NULL;
}

char *codegen_program(Sema *s, Program *program, CodegenMode mode, CodegenOptions options) {
    Gen g = {0};
    g.s = s;
    g.program = program;
    g.options = options;


    FnInfo *main_fn = NULL;
    Unit *entry = program->units.items[0];
    if (mode == CODEGEN_PROGRAM) {
        main_fn = find_main(&g);
        Type *ret = main_fn ? main_fn->type->as.fn.ret : NULL;
        bool valid = main_fn && main_fn->type->as.fn.param_count == 0 && (ret->tag == T_VOID || ret == builtin(&g, "i32"));
        if (!valid) {
            Name name = main_fn ? main_fn->decl->name : (Name){ "", 0, 0 };
            Diagnostic *d = diag_error(&program->diags, "E0502", &entry->file, name.offset, name.length,
                                       main_fn ? "`main` must be declared `i32 main()` or `void main()`"
                                               : "the entry module has no `main` function");
            diag_help(d, main_fn ? "a program starts with `i32 main()`; its value is the exit code"
                                 : "add `i32 main() { ... }` to `src/main.sosu`, or run the tests with `sosuc test`");
            return NULL;
        }
    }

    for (size_t i = 0; i < s->type_infos.count; i++) define_type(&g, s->type_infos.items[i]->type);
    for (size_t i = 0; i < s->fns.count; i++) {
        FnInfo *fn = s->fns.items[i];
        if (fn->is_extern) {
            Buf params = {0};
            for (size_t j = 0; j < fn->decl->param_count; j++) {
                buf_printf(&params, "%s%s", j ? ", " : "", value_type(&g, fn->type->as.fn.params[j]));
            }
            Type *ret = fn->type->as.fn.ret;
            buf_printf(&g.protos, "%s %.*s(%s);\n", ret->tag == T_VOID ? "void" : value_type(&g, ret),
                       NAME_ARGS(fn->decl->name), params.items ? params.items : "void");
            free(params.items);
        } else {
            gen_function(&g, fn);
        }
    }
    for (size_t i = 0; i < s->type_infos.count; i++) {
        TypeInfo *info = s->type_infos.items[i];
        for (size_t j = 0; j < info->method_count; j++) gen_method(&g, info, &info->methods[j]);
    }

    Buf main_code = {0};
    if (mode == CODEGEN_PROGRAM) {
        char *name = fn_c_name(main_fn);
        if (main_fn->type->as.fn.ret->tag == T_VOID) buf_printf(&main_code, "int main(void) {\n    %s();\n    return 0;\n}\n", name);
        else buf_printf(&main_code, "int main(void) {\n    return (int)%s();\n}\n", name);
        free(name);
    } else {
        Buf runs = {0};
        int count = 0;
        for (size_t i = 0; i < program->units.count; i++) {
            Unit *unit = program->units.items[i];
            if (unit->package != entry->package) continue;
            for (size_t j = 0; j < unit->ast->count; j++) {
                const Decl *decl = unit->ast->decls[j];
                if (decl->kind != DECL_TEST) continue;
                char *header = sosu_format("static void test_%d(void)", count);
                gen_body(&g, unit, header, NULL, s->types.void_, NULL, decl->as.test.body);
                Buf name = {0};
                Name text = decl->as.test.name;
                decode_string(text.text + 1, text.length - 2, &name);
                char *literal = c_string(name.items ? name.items : "", name.count);
                buf_printf(&runs, "    failed += !sosu_run_test(test_%d, %s);\n", count, literal);
                free(literal);
                free(name.items);
                free(header);
                count++;
            }
        }
        buf_printf(&main_code, "int main(void) {\n    int failed = 0;\n%s    return sosu_test_summary(%d, failed);\n}\n",
                   runs.items ? runs.items : "", count);
        free(runs.items);
    }

    Buf tables = {0};
    gen_tables(&g, &tables);

    /* Types first named while generating code are defined now (before the code in the output). */
    for (size_t i = 0; i < g.types.count; i++) define_type(&g, g.types.items[i].type);

    Buf out = {0};
    buf_puts(&out, "/* Generated by sosuc " SOSU_VERSION ". Do not edit. */\n#include \"sosu_rt.h\"\n\n");
    if (g.fwd.items) buf_printf(&out, "%s\n", g.fwd.items);
    if (g.defs.items) buf_puts(&out, g.defs.items);
    if (g.protos.items) buf_printf(&out, "%s\n", g.protos.items);
    if (tables.items) buf_puts(&out, tables.items);
    if (g.code.items) buf_puts(&out, g.code.items);
    buf_puts(&out, main_code.items);

    free(main_code.items);
    free(g.fwd.items);
    free(g.defs.items);
    free(g.protos.items);
    free(g.code.items);
    free(tables.items);
    for (size_t i = 0; i < g.types.count; i++) free(g.types.items[i].name);
    free(g.types.items);
    return out.items;
}
