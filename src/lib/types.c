#include "types.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static Type *new_type(TypeTable *table, TypeTag tag) {
    Type *type = arena_alloc(&table->arena, sizeof *type);
    type->tag = tag;
    return type;
}

static Type *add_builtin(TypeTable *table, TypeTag tag, const char *name) {
    Type *type = new_type(table, tag);
    type->name.text = name;
    type->name.length = strlen(name);
    SOSU_VEC_PUSH(&table->builtins, type);
    return type;
}

static void add_integer(TypeTable *table, const char *name, uint8_t bits, bool is_signed) {
    Type *type = add_builtin(table, T_INT, name);
    type->as.integer.bits = bits;
    type->as.integer.is_signed = is_signed;
}

void types_init(TypeTable *table) {
    memset(table, 0, sizeof *table);
    table->error = new_type(table, T_ERROR);
    table->error->name.text = "<error>";
    table->error->name.length = 7;
    table->void_ = add_builtin(table, T_VOID, "void");
    add_builtin(table, T_BOOL, "bool");
    add_builtin(table, T_CHAR, "char");
    add_builtin(table, T_STR, "str");
    add_builtin(table, T_ARENA, "Arena");
    add_integer(table, "i8", 8, true);
    add_integer(table, "i16", 16, true);
    add_integer(table, "i32", 32, true);
    add_integer(table, "i64", 64, true);
    add_integer(table, "isize", 64, true);
    add_integer(table, "ui8", 8, false);
    add_integer(table, "ui16", 16, false);
    add_integer(table, "ui32", 32, false);
    add_integer(table, "ui64", 64, false);
    add_integer(table, "usize", 64, false);
    add_builtin(table, T_FLOAT, "f32")->as.floating.bits = 32;
    add_builtin(table, T_FLOAT, "f64")->as.floating.bits = 64;
    Type *uf32 = add_builtin(table, T_FLOAT, "uf32");
    uf32->as.floating.bits = 32;
    uf32->as.floating.is_unsigned = true;
    Type *uf64 = add_builtin(table, T_FLOAT, "uf64");
    uf64->as.floating.bits = 64;
    uf64->as.floating.is_unsigned = true;
}

void types_free(TypeTable *table) {
    free(table->interned.slots);
    free(table->builtins.items);
    arena_free(&table->arena);
    memset(table, 0, sizeof *table);
}

Type *type_builtin(const TypeTable *table, const char *name, size_t length) {
    for (size_t i = 0; i < table->builtins.count; i++) {
        Type *type = table->builtins.items[i];
        if (type->name.length == length && memcmp(type->name.text, name, length) == 0) return type;
    }
    return NULL;
}

static bool same_structure(const Type *a, const Type *b) {
    if (a->tag != b->tag) return false;
    switch (a->tag) {
    case T_POINTER:
    case T_OPTION:
    case T_SLICE:
        return a->as.inner == b->as.inner;
    case T_ARRAY:
        return a->as.array.element == b->as.array.element && a->as.array.size == b->as.array.size;
    case T_RESULT:
        return a->as.result.value == b->as.result.value && a->as.result.error == b->as.result.error;
    case T_FN:
        if (a->as.fn.param_count != b->as.fn.param_count || a->as.fn.ret != b->as.fn.ret ||
            a->as.fn.effects != b->as.fn.effects) {
            return false;
        }
        for (size_t i = 0; i < a->as.fn.param_count; i++) {
            if (a->as.fn.params[i] != b->as.fn.params[i]) return false;
        }
        return true;
    default:
        return false;
    }
}

static uint64_t mix(uint64_t h, uint64_t value) {
    h ^= value + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    return h;
}

/* A hash of what `same_structure` compares. */
static uint64_t structure_hash(const Type *type) {
    uint64_t h = mix(0, type->tag);
    switch (type->tag) {
    case T_POINTER: case T_OPTION: case T_SLICE: return mix(h, (uintptr_t)type->as.inner);
    case T_ARRAY: return mix(mix(h, (uintptr_t)type->as.array.element), type->as.array.size);
    case T_RESULT: return mix(mix(h, (uintptr_t)type->as.result.value), (uintptr_t)type->as.result.error);
    case T_FN:
        h = mix(mix(h, (uintptr_t)type->as.fn.ret), type->as.fn.effects);
        for (size_t i = 0; i < type->as.fn.param_count; i++) h = mix(h, (uintptr_t)type->as.fn.params[i]);
        return h;
    default:
        return h;
    }
}

/* The slot of `probe` in the table: the type of the same structure, or an empty slot. */
static Type **interned_slot(const TypeTable *table, const Type *probe) {
    size_t mask = table->interned.cap - 1;
    for (size_t slot = (size_t)structure_hash(probe) & mask;; slot = (slot + 1) & mask) {
        Type **found = &table->interned.slots[slot];
        if (!*found || same_structure(*found, probe)) return found;
    }
}

/* Returns the interned copy of `probe`. */
static Type *intern(TypeTable *table, const Type *probe) {
    if (2 * (table->interned.count + 1) > table->interned.cap) { /* half full at most */
        Type **old = table->interned.slots;
        size_t old_cap = table->interned.cap;
        table->interned.cap = old_cap ? 2 * old_cap : 256;
        table->interned.slots = sosu_alloc(table->interned.cap * sizeof *old);
        memset(table->interned.slots, 0, table->interned.cap * sizeof *old);
        for (size_t i = 0; i < old_cap; i++) {
            if (old[i]) *interned_slot(table, old[i]) = old[i];
        }
        free(old);
    }
    Type **slot = interned_slot(table, probe);
    if (*slot) return *slot;
    Type *type = arena_copy(&table->arena, probe, sizeof *probe);
    if (probe->tag == T_FN) {
        type->as.fn.params = arena_copy(&table->arena, probe->as.fn.params,
                                        probe->as.fn.param_count * sizeof *probe->as.fn.params);
    }
    *slot = type;
    table->interned.count++;
    return type;
}

static Type *wrap(TypeTable *table, TypeTag tag, Type *inner) {
    if (inner->tag == T_ERROR) return inner;
    Type probe = {0};
    probe.tag = tag;
    probe.as.inner = inner;
    return intern(table, &probe);
}

Type *type_pointer(TypeTable *table, Type *inner) { return wrap(table, T_POINTER, inner); }
Type *type_option(TypeTable *table, Type *inner) { return wrap(table, T_OPTION, inner); }
Type *type_slice(TypeTable *table, Type *inner) { return wrap(table, T_SLICE, inner); }

Type *type_array(TypeTable *table, Type *element, uint64_t size) {
    if (element->tag == T_ERROR) return element;
    Type probe = {0};
    probe.tag = T_ARRAY;
    probe.as.array.element = element;
    probe.as.array.size = size;
    return intern(table, &probe);
}

Type *type_result(TypeTable *table, Type *value, Type *error) {
    if (value->tag == T_ERROR) return value;
    if (error->tag == T_ERROR) return error;
    Type probe = {0};
    probe.tag = T_RESULT;
    probe.as.result.value = value;
    probe.as.result.error = error;
    return intern(table, &probe);
}

Type *type_fn(TypeTable *table, Type **params, size_t param_count, Type *ret, unsigned effects) {
    Type probe = {0};
    probe.tag = T_FN;
    probe.as.fn.params = params;
    probe.as.fn.param_count = param_count;
    probe.as.fn.ret = ret;
    probe.as.fn.effects = effects;
    return intern(table, &probe);
}

/* ---- Printing ---- */

typedef struct {
    char *items;
    size_t count;
    size_t cap;
} Buffer;

static void append(Buffer *buffer, const char *text, size_t length) {
    for (size_t i = 0; i < length; i++) SOSU_VEC_PUSH(buffer, text[i]);
}

static void append_text(Buffer *buffer, const char *text) { append(buffer, text, strlen(text)); }

static void append_type(Buffer *buffer, const Type *type);

/* The type under a suffix: a function type in parentheses, `(fn(i32) -> i32)[2]`. */
static void append_inner(Buffer *buffer, const Type *type) {
    if (type->tag == T_FN) append_text(buffer, "(");
    append_type(buffer, type);
    if (type->tag == T_FN) append_text(buffer, ")");
}

static void append_type(Buffer *buffer, const Type *type) {
    switch (type->tag) {
    case T_POINTER:
        append_inner(buffer, type->as.inner);
        append_text(buffer, "*");
        break;
    case T_OPTION:
        append_inner(buffer, type->as.inner);
        append_text(buffer, "?");
        break;
    case T_SLICE:
        append_inner(buffer, type->as.inner);
        append_text(buffer, "[]");
        break;
    case T_ARRAY: {
        char size[32];
        snprintf(size, sizeof size, "[%llu]", (unsigned long long)type->as.array.size);
        append_inner(buffer, type->as.array.element);
        append_text(buffer, size);
        break;
    }
    case T_RESULT:
        append_inner(buffer, type->as.result.value);
        append_text(buffer, "!");
        append_type(buffer, type->as.result.error);
        break;
    case T_FN:
        append_text(buffer, "fn(");
        for (size_t i = 0; i < type->as.fn.param_count; i++) {
            if (i) append_text(buffer, ", ");
            append_type(buffer, type->as.fn.params[i]);
        }
        append_text(buffer, ") -> ");
        append_type(buffer, type->as.fn.ret);
        if (type->as.fn.effects) {
            append_text(buffer, " uses ");
            bool first = true;
            for (unsigned bit = 1; bit <= EFFECT_THREADS; bit <<= 1) {
                if (!(type->as.fn.effects & bit)) continue;
                if (!first) append_text(buffer, ", ");
                append_text(buffer, effect_name(bit));
                first = false;
            }
            if (type->as.fn.effects & EFFECT_PARAMS) append_text(buffer, first ? "E" : ", E"); /* an effect parameter */
        }
        break;
    default:
        append(buffer, type->name.text, type->name.length);
        if (type->type_arg_count) {
            append_text(buffer, "<");
            for (size_t i = 0; i < type->type_arg_count; i++) {
                if (i) append_text(buffer, ", ");
                append_type(buffer, type->type_args[i]);
            }
            append_text(buffer, ">");
        }
        break;
    }
}

char *type_to_string(const Type *type) {
    Buffer buffer = {0};
    append_type(&buffer, type);
    SOSU_VEC_PUSH(&buffer, '\0');
    return buffer.items;
}

/* ---- Effects ---- */

static const char *const effect_names[] = { "io", "time", "random", "threads" };

unsigned effect_from_name(const char *name, size_t length) {
    for (unsigned i = 0; i < 4; i++) {
        if (strlen(effect_names[i]) == length && memcmp(effect_names[i], name, length) == 0) return 1u << i;
    }
    return 0;
}

const char *effect_name(unsigned effect) {
    for (unsigned i = 0; i < 4; i++) {
        if (effect == 1u << i) return effect_names[i];
    }
    return "?";
}
