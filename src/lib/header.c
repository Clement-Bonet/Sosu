#include "codegen.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The C header of a library (spec §9.2): its exports, and the types they use under the names
   `<package>_<Type>`. Plain C99, usable from C++. The types have the layout of those of the
   generated code, so the host and the library agree on how they are passed. */

#define NAME_ARGS(name) (int)(name).length, (name).text
#define VEC(T) struct { T *items; size_t count; size_t cap; }

typedef struct {
    char *items;
    size_t count;
    size_t cap;
} Buf;

static void put(Buf *buf, const char *text) {
    size_t length = strlen(text);
    if (buf->count + length + 1 > buf->cap) {
        buf->cap = (buf->count + length + 1) * 2;
        buf->items = sosu_realloc(buf->items, buf->cap);
    }
    memcpy(buf->items + buf->count, text, length + 1);
    buf->count += length;
}

static void putf(Buf *buf, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    char *text = sosu_vformat(fmt, args);
    va_end(args);
    put(buf, text);
    free(text);
}

typedef struct {
    const Type *type;
    char *name;
} Named;

typedef struct {
    Buf fwd;  /* typedefs of structs, opaque classes */
    Buf defs; /* struct bodies, constants, function pointer types, in the order they are needed */
    VEC(Named) names;
    VEC(const Type *) defined;
    const char *package; /* of the library: names its function types */
    size_t fn_types;
} Header;

static const char *h_name(Header *h, const Type *type);

static const char *int_type(const Type *type) {
    unsigned bits = type->as.integer.bits;
    bool is_signed = type->as.integer.is_signed;
    if (bits == 8) return is_signed ? "int8_t" : "uint8_t";
    if (bits == 16) return is_signed ? "int16_t" : "uint16_t";
    if (bits == 32) return is_signed ? "int32_t" : "uint32_t";
    return is_signed ? "int64_t" : "uint64_t";
}

static char *package_of(const TypeInfo *info) { return sosu_strdup(info->unit->package->name); }

/* A name made of the letters and digits of `text`. */
static char *identifier(const char *text) {
    char *id = sosu_strdup(text);
    for (char *c = id; *c; c++) {
        if (*c == '*') *c = 'p';
        else if (!isalnum((unsigned char)*c)) *c = '_';
    }
    return id;
}

static bool is_defined(Header *h, const Type *type) {
    for (size_t i = 0; i < h->defined.count; i++) {
        if (h->defined.items[i] == type) return true;
    }
    SOSU_VEC_PUSH(&h->defined, type);
    return false;
}

static const char *remember(Header *h, const Type *type, char *name) {
    Named named = { type, name };
    SOSU_VEC_PUSH(&h->names, named);
    return name;
}

/* A field of a struct; an array is written after the name, as C wants it. */
static void field(Header *h, Buf *out, const Type *type, Name name) {
    if (type->tag == T_ARRAY) {
        const char *element = h_name(h, type->as.array.element);
        putf(out, "    %s %.*s[%llu];\n", element, NAME_ARGS(name), (unsigned long long)type->as.array.size);
    } else {
        putf(out, "    %s %.*s;\n", h_name(h, type), NAME_ARGS(name));
    }
}

/* The C name of a type of an export, defining what it needs first. */
static const char *h_name(Header *h, const Type *type) {
    for (size_t i = 0; i < h->names.count; i++) {
        if (h->names.items[i].type == type) return h->names.items[i].name;
    }
    switch (type->tag) {
    case T_VOID: return "void";
    case T_BOOL: return "bool";
    case T_CHAR: return "uint32_t";
    case T_STR: return "sosu_str";
    case T_INT: return int_type(type);
    case T_FLOAT: return type->as.floating.bits == 32 ? "float" : "double";
    case T_ARENA: return "sosu_arena";
    case T_POINTER: return remember(h, type, sosu_format("%s*", h_name(h, type->as.inner)));
    case T_SLICE: {
        char *element = identifier(h_name(h, type->as.inner));
        const char *name = remember(h, type, sosu_format("sosu_slice_%s", element));
        free(element);
        if (!is_defined(h, type)) {
            putf(&h->defs, "#ifndef SOSU_SLICE_%s\n#define SOSU_SLICE_%s\ntypedef struct { %s *ptr; size_t len; } %s;\n#endif\n\n",
                 name + 11, name + 11, h_name(h, type->as.inner), name);
        }
        return name;
    }
    case T_CLASS: {
        char *package = package_of(type->as.info);
        const char *name = remember(h, type, sosu_format("%s_%.*s", package, NAME_ARGS(type->as.info->name)));
        free(package);
        putf(&h->fwd, "typedef struct %s %s; /* a class: only passed back to the library */\n", name, name);
        return name;
    }
    case T_STRUCT: {
        char *package = package_of(type->as.info);
        const char *name = remember(h, type, sosu_format("%s_%.*s", package, NAME_ARGS(type->as.info->name)));
        free(package);
        putf(&h->fwd, "typedef struct %s %s;\n", name, name);
        Buf body = {0};
        const TypeInfo *info = type->as.info;
        for (size_t i = 0; i < info->field_count; i++) field(h, &body, info->fields[i].type, info->fields[i].name);
        if (info->field_count == 0) put(&body, "    char unused;\n");
        putf(&h->defs, "struct %s {\n%s};\n\n", name, body.items);
        free(body.items);
        return name;
    }
    case T_ENUM: {
        char *package = package_of(type->as.info);
        const char *name = remember(h, type, sosu_format("%s_%.*s", package, NAME_ARGS(type->as.info->name)));
        free(package);
        putf(&h->defs, "typedef struct %s { uint32_t tag; } %s;\nenum {", name, name);
        const TypeInfo *info = type->as.info;
        for (size_t i = 0; i < info->variant_count; i++) {
            putf(&h->defs, "%s %s_%.*s = %zu", i ? "," : "", name, NAME_ARGS(info->variants[i].name), i);
        }
        put(&h->defs, " };\n\n");
        return name;
    }
    case T_FN: {
        Buf params = {0};
        for (size_t i = 0; i < type->as.fn.param_count; i++) {
            putf(&params, "%s%s", i ? ", " : "", h_name(h, type->as.fn.params[i]));
        }
        const char *ret = h_name(h, type->as.fn.ret);
        const char *name = remember(h, type, sosu_format("%s_fn%zu", h->package, ++h->fn_types));
        putf(&h->defs, "typedef %s (*%s)(%s);\n\n", ret, name, params.items ? params.items : "void");
        free(params.items);
        return name;
    }
    default:
        return "void";
    }
}

char *codegen_header(Sema *s, Program *program) {
    const Package *root = program->packages.items[0];
    Header h = {0};
    h.package = root->name;
    Buf fns = {0};
    for (size_t i = 0; i < s->fns.count; i++) {
        const FnInfo *fn = s->fns.items[i];
        if (!fn->decl->is_export || fn->type->tag != T_FN) continue;
        const Type *type = fn->type;
        Buf params = {0};
        for (size_t j = 0; j < fn->decl->param_count; j++) {
            putf(&params, "%s%s %.*s", j ? ", " : "", h_name(&h, type->as.fn.params[j]), NAME_ARGS(fn->decl->params[j].name));
        }
        putf(&fns, "%s %.*s(%s);\n", h_name(&h, type->as.fn.ret), NAME_ARGS(fn->decl->name), params.items ? params.items : "void");
        free(params.items);
    }
    char *guard = identifier(root->name);
    for (char *c = guard; *c; c++) *c = (char)toupper((unsigned char)*c);
    Buf out = {0};
    putf(&out,
         "/* Generated by sosuc " SOSU_VERSION ". Do not edit. The library `%s` (spec 9.2). */\n"
         "#ifndef %s_H\n#define %s_H\n\n"
         "#include <stdbool.h>\n#include <stddef.h>\n#include <stdint.h>\n\n"
         "#ifdef __cplusplus\nextern \"C\" {\n#endif\n\n"
         "#ifndef SOSU_STR_DEFINED\n#define SOSU_STR_DEFINED\n"
         "/* UTF-8, not terminated by a NUL */\ntypedef struct { const uint8_t *ptr; size_t len; } sosu_str;\n"
         "typedef struct sosu_arena sosu_arena;\n#endif\n\n",
         root->name, guard, guard);
    if (h.fwd.items) putf(&out, "%s\n", h.fwd.items);
    if (h.defs.items) put(&out, h.defs.items);
    if (fns.items) put(&out, fns.items);
    putf(&out,
         "\n/* The message of the run-time error of the last call of this thread, or NULL if it succeeded. */\n"
         "const char *%s_error(void);\n\n"
         "#ifdef __cplusplus\n}\n#endif\n\n#endif\n",
         root->name);
    free(guard);
    free(fns.items);
    free(h.fwd.items);
    free(h.defs.items);
    for (size_t i = 0; i < h.names.count; i++) free(h.names.items[i].name);
    free(h.names.items);
    free(h.defined.items);
    return out.items;
}
