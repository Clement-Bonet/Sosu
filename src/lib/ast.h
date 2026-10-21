#ifndef SOSU_AST_H
#define SOSU_AST_H

#include <stdio.h>

#include "source.h"
#include "token.h"

/* Every node records its byte span in the source file (offset, length) for diagnostics.
   Names point into the source text, which must outlive the AST. */

typedef struct TypeExpr TypeExpr;
typedef struct Expr Expr;
typedef struct Stmt Stmt;

typedef struct {
    const char *text;
    size_t length;
    size_t offset;
} Name;

/* Dotted names (`std.io.File`), also used for effect lists. */
typedef struct {
    Name *items;
    size_t count;
} Path;

/* ---- Types ---- */

typedef enum {
    TYPE_NAME,    /* i32, Point, std.io.File */
    TYPE_POINTER, /* T* */
    TYPE_OPTION,  /* T? */
    TYPE_ARRAY,   /* T[N] */
    TYPE_SLICE,   /* T[] */
    TYPE_RESULT,  /* T!E */
    TYPE_FN,      /* fn(A, B) -> R uses io */
} TypeKind;

struct TypeExpr {
    TypeKind kind;
    size_t offset;
    size_t length;
    union {
        Path name;
        TypeExpr *inner; /* POINTER, OPTION, SLICE */
        struct { TypeExpr *element; uint64_t size; } array;
        struct { TypeExpr *value; TypeExpr *error; } result;
        struct { TypeExpr **params; size_t param_count; TypeExpr *ret; Path effects; } fn;
    } as;
};

/* ---- Expressions ---- */

typedef enum {
    EXPR_ERROR, /* invalid input, already reported */
    EXPR_INT,
    EXPR_FLOAT,
    EXPR_STRING,
    EXPR_CHAR,
    EXPR_BOOL,
    EXPR_NAME,
    EXPR_SELF,
    EXPR_BASE,
    EXPR_RESULT, /* `result` in `ensures` */
    EXPR_UNARY,
    EXPR_BINARY,
    EXPR_CAST,     /* x as T */
    EXPR_TRY_CAST, /* x as? T */
    EXPR_TERNARY,
    EXPR_RANGE,
    EXPR_CALL,
    EXPR_INDEX,
    EXPR_FIELD,
    EXPR_STRUCT_LIT, /* Point{ x: 1.0, y: 2.0 } */
    EXPR_MATCH,
    EXPR_TRY,
} ExprKind;

typedef struct {
    Name name;
    Expr *value;
} FieldInit;

typedef enum {
    PATTERN_WILDCARD, /* _ */
    PATTERN_VARIANT,  /* Empty, Circle(r), Rect(w, h) */
} PatternKind;

typedef struct {
    PatternKind kind;
    Path path;
    Name *bindings;
    size_t binding_count;
    bool has_parens;
    size_t offset;
    size_t length;
} Pattern;

typedef struct {
    Pattern pattern;
    Expr *expr;  /* either an expression... */
    Stmt *block; /* ...or a block */
} MatchArm;

struct Expr {
    ExprKind kind;
    size_t offset;
    size_t length;
    struct Type *type; /* set by semantic analysis */
    union {
        uint64_t int_value;
        uint32_t char_value;
        bool bool_value;
        Name name;
        Expr *inner; /* TRY */
        struct { TokenKind op; Expr *operand; } unary;
        struct { TokenKind op; Expr *left; Expr *right; } binary;
        struct { Expr *value; TypeExpr *type; } cast;
        struct { Expr *cond; Expr *then_expr; Expr *else_expr; } ternary;
        struct { Expr *start; Expr *end; bool inclusive; } range;
        struct { Expr *callee; Expr **args; size_t arg_count; } call;
        struct { Expr *object; Expr *index; } index;
        struct { Expr *object; Name name; } field;
        struct { TypeExpr *type; FieldInit *fields; size_t field_count; } struct_lit;
        struct { Expr *subject; MatchArm *arms; size_t arm_count; } match;
    } as;
};

/* ---- Statements ---- */

typedef enum {
    STMT_BLOCK,
    STMT_VAR,
    STMT_ASSIGN,
    STMT_EXPR,
    STMT_IF,
    STMT_WHILE,
    STMT_FOR,
    STMT_FOR_IN,
    STMT_RETURN,
    STMT_BREAK,
    STMT_CONTINUE,
    STMT_DEFER,
    STMT_UNSAFE,
} StmtKind;

struct Stmt {
    StmtKind kind;
    size_t offset;
    size_t length;
    union {
        struct { Stmt **items; size_t count; } block;
        struct { bool is_mut; TypeExpr *type; Name name; Expr *init; } var; /* type NULL for `auto` */
        struct { TokenKind op; Expr *target; Expr *value; } assign;
        Expr *expr; /* EXPR, RETURN (NULL for a bare `return`) */
        struct { Expr *cond; Stmt *then_block; Stmt *else_stmt; } if_;
        struct { Expr *cond; Stmt *body; } while_;
        struct { Stmt *init; Expr *cond; Stmt *step; Stmt *body; } for_; /* each part may be NULL */
        struct { bool is_mut; TypeExpr *type; Name name; Expr *iterable; Stmt *body; } for_in;
        Stmt *inner; /* DEFER, UNSAFE */
    } as;
};

/* ---- Declarations ---- */

typedef struct {
    bool is_mut;
    TypeExpr *type;
    Name name;
} Param;

typedef enum {
    CONTRACT_REQUIRES,
    CONTRACT_ENSURES,
} ContractKind;

typedef struct {
    ContractKind kind;
    Expr *expr;
} Contract;

typedef enum {
    METHOD_PLAIN,
    METHOD_VIRTUAL,
    METHOD_OVERRIDE,
} MethodKind;

typedef struct {
    bool is_pub;
    bool is_unsafe;
    MethodKind method_kind;
    TypeExpr *ret;
    Name name;
    Param *params;
    size_t param_count;
    Path effects;
    Contract *contracts;
    size_t contract_count;
    Stmt *body; /* NULL for signatures (interfaces, extern blocks) */
    size_t offset;
    size_t length;
} FnDecl;

typedef struct {
    bool is_pub;
    bool is_mut;
    TypeExpr *type;
    Name name;
    Expr *init; /* default value, or NULL */
} Field;

typedef struct {
    Name name;
    Param *fields;
    size_t field_count;
    bool has_parens;
} Variant;

typedef enum {
    DECL_IMPORT,
    DECL_STRUCT,
    DECL_CLASS,
    DECL_INTERFACE,
    DECL_ENUM,
    DECL_FN,
    DECL_EXTERN,
    DECL_TEST,
} DeclKind;

typedef struct {
    DeclKind kind;
    bool is_pub;
    size_t offset;
    size_t length;
    union {
        Path import;
        /* STRUCT, CLASS and INTERFACE (methods only) */
        struct {
            Name name;
            Path *bases; /* class: parent class first, then interfaces */
            size_t base_count;
            Field *fields;
            size_t field_count;
            FnDecl *methods;
            size_t method_count;
        } type;
        struct { Name name; Variant *variants; size_t variant_count; } enum_;
        FnDecl fn;
        struct { Name abi; FnDecl *fns; size_t fn_count; } extern_; /* abi: string literal, quotes included */
        struct { Name name; Stmt *body; } test;                     /* name: string literal, quotes included */
    } as;
} Decl;

typedef struct {
    Decl **decls;
    size_t count;
} Module;

/* S-expression dumps, used by `sosuc parse` and the tests. */
void ast_print_module(const Module *module, const SourceFile *file, FILE *out);
void ast_print_expr(const Expr *expr, const SourceFile *file, FILE *out);

#endif
