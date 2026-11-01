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
    TypeExpr **args; /* NAME: the type arguments `Pair<A, B>` (spec §5.4) */
    size_t arg_count;
    union {
        Path name;
        TypeExpr *inner; /* POINTER, OPTION, SLICE */
        struct { TypeExpr *element; uint64_t size; Path size_name; } array; /* size_name: `ui8[NAME]` */
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
    EXPR_ARRAY, /* [1, 2, 3] or [0; 64] */
    EXPR_LAMBDA, /* (i32 a) => a + 1, (a) => { ... } */
    EXPR_TYPE,   /* a type given to a call, which a name alone cannot be: `arena.alloc_slice(ui8*, n)` */
} ExprKind;

typedef struct {
    Name name;
    Expr *value;
} FieldInit;

/* A variable of the enclosing function that a lambda copies (spec §5.5), recorded by semantic
   analysis; `self` is captured as a pointer, with `is_self`. */
typedef struct {
    Name name;
    struct Type *type;
    bool is_self;
} Capture;

typedef struct Param Param;

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

/* What a name, a member or a callee refers to, recorded by semantic analysis. */
typedef enum {
    RESOLVED_NONE,        /* an ordinary value */
    RESOLVED_LOCAL,       /* a variable or parameter */
    RESOLVED_SELF_FIELD,  /* a field of `self` used without `self.` (target: FieldInfo) */
    RESOLVED_SELF_METHOD, /* a method of `self` called without `self.` (target: MethodInfo) */
    RESOLVED_FN,          /* target: FnInfo */
    RESOLVED_METHOD,      /* `value.method(...)` (target: MethodInfo) */
    RESOLVED_VARIANT,     /* target: TypeInfo of the enum, target_index: the variant */
    RESOLVED_BUILTIN,     /* target: the name, e.g. "some" */
    RESOLVED_PRINT,       /* io.print */
    RESOLVED_CONST,       /* a constant of a module (target: ConstInfo) */
} Resolution;

struct Expr {
    ExprKind kind;
    size_t offset;
    size_t length;
    struct Type *type; /* set by semantic analysis */
    Resolution resolution;
    const void *target;
    size_t target_index;
    TypeExpr **type_args; /* NAME and FIELD: written after a generic name, `f<i32>(x)` */
    size_t type_arg_count;
    struct Type **type_values; /* set by semantic analysis: the arguments of a generic function called */
    size_t type_value_count;
    unsigned *effect_values; /* and its effect parameters (one mask each) */
    size_t effect_value_count;
    union {
        uint64_t int_value;
        uint32_t char_value;
        bool bool_value;
        Name name;
        Expr *inner; /* TRY */
        TypeExpr *type_expr; /* TYPE */
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
        /* [items...], or [repeat; repeat_count] when `repeat` is not NULL */
        struct { Expr **items; size_t count; Expr *repeat; uint64_t repeat_count; } array;
        /* the body is an expression or a block; a parameter without a type takes the one
           expected (its `type` is NULL) */
        struct {
            Param *params;
            size_t param_count;
            Expr *body;
            Stmt *block;
            Capture *captures; /* set by semantic analysis */
            size_t capture_count;
        } lambda;
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

struct Param {
    bool is_mut;
    TypeExpr *type;
    Name name;
};

/* A type parameter `T: Ordered + Hash`, or an effect parameter `uses E` (spec §5.4). */
typedef struct {
    Name name;
    Path *constraints;
    size_t constraint_count;
    bool is_effect;
} TypeParam;

typedef struct {
    TypeParam *items;
    size_t count;
} TypeParams;

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
    bool is_export; /* `export "C"`: offered to other languages under its own name (spec §9.2) */
    MethodKind method_kind;
    TypeExpr *ret;
    Name name;
    TypeParams type_params;
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
    DECL_CONST, /* `pub ui32 NAME = value;` (spec §11.6) */
} DeclKind;

typedef struct {
    DeclKind kind;
    bool is_pub;
    size_t offset;
    size_t length;
    TypeParams type_params; /* STRUCT, CLASS, INTERFACE, ENUM */
    union {
        Path import;
        /* STRUCT, CLASS and INTERFACE (methods only) */
        struct {
            Name name;
            Path *bases; /* class: parent class first, then interfaces */
            TypeExpr ***base_args; /* the type arguments of each base (`Iterator<i32>`), NULL without */
            size_t *base_arg_counts;
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
        struct { TypeExpr *type; Name name; Expr *value; } const_;
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
