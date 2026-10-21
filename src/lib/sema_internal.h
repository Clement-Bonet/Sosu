#ifndef SOSU_SEMA_INTERNAL_H
#define SOSU_SEMA_INTERNAL_H

/* Shared between sema.c (declarations) and sema_body.c (function bodies). */

#include "sema.h"

typedef enum {
    POSITION_VALUE,   /* a variable, field or parameter type */
    POSITION_RETURN,  /* a return type: `void` is allowed */
    POSITION_POINTEE, /* behind `*`: classes and interfaces are allowed */
} TypePosition;

Diagnostic *sema_verror(Sema *s, const Unit *unit, const char *code, size_t offset, size_t length,
                        const char *fmt, va_list args);
bool sema_name_eq(Name a, Name b);
size_t sema_edit_distance(Name a, Name b);
size_t sema_line_of(const Unit *unit, size_t offset);

Scope *sema_scope(Sema *s, const Unit *unit);
Symbol *sema_scope_find(Scope *scope, Name name);

Type *sema_resolve_type(Sema *s, Unit *unit, const TypeExpr *te, TypePosition position);
Type *sema_resolve_named(Sema *s, Unit *unit, Path path, size_t offset, size_t length);

/* Checks the bodies of every function, method and test (sema_body.c). */
void sema_check_bodies(Sema *s);

#endif
