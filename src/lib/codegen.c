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
    case RESOLVED_SELF_FIELD: return sosu_format("v_self->f_%.*s", NAME_ARGS(e->as.name));
    case RESOLVED_FN: return fn_c_name(e->target);
    case RESOLVED_VARIANT: return gen_variant(f, e, e->target, e->target_index, NULL);
    case RESOLVED_BUILTIN: /* none */
        return sosu_format("((%s){ .has = false })", value_type(f->g, e->type));
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
static char *arithmetic(Fn *f, TokenKind op, const Type *type, const char *a, const char *b, size_t offset) {
    const char *helper = op_helper(op);
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
        if (e->type->tag == T_INT) {
            char *where = loc(f, e->offset);
            result = sosu_format("sosu_neg_%.*s(%s, %s)", NAME_ARGS(e->type->name), value, where);
            free(where);
        } else {
            result = sosu_format("(-%s)", value);
        }
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
    if (base->tag == T_ARRAY) {
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
        result = sosu_format("%s%sf_%.*s", value, access, NAME_ARGS(name));
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
            char *negated;
            if (e->type->tag == T_INT) {
                char *where = loc(f, e->offset);
                negated = sosu_format("sosu_neg_%.*s(%s, %s)", NAME_ARGS(e->type->name), a, where);
                free(where);
            } else {
                negated = sosu_format("(-%s)", a);
            }
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

static char *gen_call(Fn *f, Expr *e) {
    Expr *callee = e->as.call.callee;
    if (callee->resolution == RESOLVED_PRINT) {
        gen_print(f, e);
        return NULL;
    }
    if (callee->resolution == RESOLVED_BUILTIN) return gen_builtin_call(f, e, callee->target);

    size_t count = e->as.call.arg_count;
    char *function;
    char *self = NULL;
    switch (callee->resolution) {
    case RESOLVED_FN: function = fn_c_name(callee->target); break;
    case RESOLVED_SELF_METHOD:
        function = method_c_name(f->g, callee->target);
        self = sosu_strdup("v_self");
        break;
    case RESOLVED_METHOD:
        function = method_c_name(f->g, callee->target);
        self = address_of(f, callee->as.field.object);
        break;
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
    if (f->fn) {
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
    default: return sosu_strdup("0"); /* rejected earlier: base, as?, ranges */
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
    if (fn) {
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
    char *name = method_c_name(g, method);
    char *header = signature(g, name, method->type, method->decl, info);
    buf_printf(&g->protos, "%s;\n", header);
    gen_body(g, info->unit, header, method->decl, method->type->as.fn.ret, info, method->decl->body);
    free(header);
    free(name);
}

static void report(Gen *g, const Unit *unit, const char *code, Name name, const char *message, const char *help) {
    Diagnostic *d = diag_error(&g->program->diags, code, &unit->file, name.offset, name.length, "%s", message);
    if (help) diag_help(d, "%s", help);
}

/* Constructs code generation does not handle yet. */
static bool check_supported(Gen *g) {
    bool ok = true;
    for (size_t i = 0; i < g->s->type_infos.count; i++) {
        TypeInfo *info = g->s->type_infos.items[i];
        if (info->type->tag != T_CLASS && info->type->tag != T_INTERFACE) continue;
        report(g, info->unit, "E0501", info->name, "code generation does not support classes and interfaces yet",
               "they need arenas to be created (roadmap step 6); `sosuc check` already checks them");
        ok = false;
    }
    return ok;
}

static FnInfo *find_main(Gen *g) {
    Unit *entry = g->program->units.items[0];
    for (size_t i = 0; i < g->s->fns.count; i++) {
        FnInfo *fn = g->s->fns.items[i];
        if (fn->unit == entry && !fn->is_extern && name_is(fn->decl->name, "main")) return fn;
    }
    return NULL;
}

char *codegen_program(Sema *s, Program *program, CodegenMode mode) {
    Gen g = {0};
    g.s = s;
    g.program = program;
    if (!check_supported(&g)) return NULL;

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

    /* Types first named while generating code are defined now (before the code in the output). */
    for (size_t i = 0; i < g.types.count; i++) define_type(&g, g.types.items[i].type);

    Buf out = {0};
    buf_puts(&out, "/* Generated by sosuc " SOSU_VERSION ". Do not edit. */\n#include \"sosu_rt.h\"\n\n");
    if (g.fwd.items) buf_printf(&out, "%s\n", g.fwd.items);
    if (g.defs.items) buf_puts(&out, g.defs.items);
    if (g.protos.items) buf_printf(&out, "%s\n", g.protos.items);
    if (g.code.items) buf_puts(&out, g.code.items);
    buf_puts(&out, main_code.items);

    free(main_code.items);
    free(g.fwd.items);
    free(g.defs.items);
    free(g.protos.items);
    free(g.code.items);
    for (size_t i = 0; i < g.types.count; i++) free(g.types.items[i].name);
    free(g.types.items);
    return out.items;
}
