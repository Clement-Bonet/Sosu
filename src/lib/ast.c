#include "ast.h"

/* S-expression dump: declarations and statements one per line, expressions inline.
   (fn i32 add ((i32 a) (i32 b))
     (block
       (return (+ a b)))) */

typedef struct {
    const SourceFile *file;
    FILE *out;
} Printer;

static void print_stmt(Printer *pr, const Stmt *stmt, int indent);

static void newline(Printer *pr, int indent) {
    fputc('\n', pr->out);
    for (int i = 0; i < indent; i++) fputs("  ", pr->out);
}

static void print_name(Printer *pr, Name name) {
    fprintf(pr->out, "%.*s", (int)name.length, name.text);
}

static void print_path(Printer *pr, Path path) {
    for (size_t i = 0; i < path.count; i++) {
        if (i) fputc('.', pr->out);
        print_name(pr, path.items[i]);
    }
}

static void print_source(Printer *pr, size_t offset, size_t length) {
    fprintf(pr->out, "%.*s", (int)length, pr->file->text + offset);
}

static void print_type(Printer *pr, const TypeExpr *type) {
    if (!type) {
        fputs("auto", pr->out);
        return;
    }
    switch (type->kind) {
    case TYPE_NAME:
        print_path(pr, type->as.name);
        break;
    case TYPE_POINTER:
        print_type(pr, type->as.inner);
        fputc('*', pr->out);
        break;
    case TYPE_OPTION:
        print_type(pr, type->as.inner);
        fputc('?', pr->out);
        break;
    case TYPE_ARRAY:
        print_type(pr, type->as.array.element);
        fprintf(pr->out, "[%llu]", (unsigned long long)type->as.array.size);
        break;
    case TYPE_SLICE:
        print_type(pr, type->as.inner);
        fputs("[]", pr->out);
        break;
    case TYPE_RESULT:
        print_type(pr, type->as.result.value);
        fputc('!', pr->out);
        print_type(pr, type->as.result.error);
        break;
    case TYPE_FN:
        fputs("fn(", pr->out);
        for (size_t i = 0; i < type->as.fn.param_count; i++) {
            if (i) fputs(", ", pr->out);
            print_type(pr, type->as.fn.params[i]);
        }
        fputs(") -> ", pr->out);
        print_type(pr, type->as.fn.ret);
        if (type->as.fn.effects.count) {
            fputs(" uses ", pr->out);
            for (size_t i = 0; i < type->as.fn.effects.count; i++) {
                if (i) fputs(", ", pr->out);
                print_name(pr, type->as.fn.effects.items[i]);
            }
        }
        break;
    }
}

static void print_pattern(Printer *pr, const Pattern *pattern) {
    if (pattern->kind == PATTERN_WILDCARD) {
        fputc('_', pr->out);
        return;
    }
    print_path(pr, pattern->path);
    if (!pattern->has_parens) return;
    fputc('(', pr->out);
    for (size_t i = 0; i < pattern->binding_count; i++) {
        if (i) fputs(", ", pr->out);
        print_name(pr, pattern->bindings[i]);
    }
    fputc(')', pr->out);
}

static void print_expr(Printer *pr, const Expr *expr, int indent) {
    FILE *out = pr->out;
    if (!expr) {
        fputc('_', out);
        return;
    }
    switch (expr->kind) {
    case EXPR_ERROR: fputs("<error>", out); break;
    case EXPR_INT:
    case EXPR_FLOAT:
    case EXPR_STRING:
    case EXPR_CHAR: print_source(pr, expr->offset, expr->length); break;
    case EXPR_BOOL: fputs(expr->as.bool_value ? "true" : "false", out); break;
    case EXPR_NAME: print_name(pr, expr->as.name); break;
    case EXPR_SELF: fputs("self", out); break;
    case EXPR_BASE: fputs("base", out); break;
    case EXPR_RESULT: fputs("result", out); break;
    case EXPR_UNARY:
        fprintf(out, "(%s ", token_kind_spelling(expr->as.unary.op));
        print_expr(pr, expr->as.unary.operand, indent);
        fputc(')', out);
        break;
    case EXPR_BINARY:
        fprintf(out, "(%s ", token_kind_spelling(expr->as.binary.op));
        print_expr(pr, expr->as.binary.left, indent);
        fputc(' ', out);
        print_expr(pr, expr->as.binary.right, indent);
        fputc(')', out);
        break;
    case EXPR_CAST:
    case EXPR_TRY_CAST:
        fputs(expr->kind == EXPR_CAST ? "(as " : "(as? ", out);
        print_expr(pr, expr->as.cast.value, indent);
        fputc(' ', out);
        print_type(pr, expr->as.cast.type);
        fputc(')', out);
        break;
    case EXPR_TERNARY:
        fputs("(?: ", out);
        print_expr(pr, expr->as.ternary.cond, indent);
        fputc(' ', out);
        print_expr(pr, expr->as.ternary.then_expr, indent);
        fputc(' ', out);
        print_expr(pr, expr->as.ternary.else_expr, indent);
        fputc(')', out);
        break;
    case EXPR_RANGE:
        fputs(expr->as.range.inclusive ? "(..= " : "(.. ", out);
        print_expr(pr, expr->as.range.start, indent);
        fputc(' ', out);
        print_expr(pr, expr->as.range.end, indent);
        fputc(')', out);
        break;
    case EXPR_CALL:
        fputs("(call ", out);
        print_expr(pr, expr->as.call.callee, indent);
        for (size_t i = 0; i < expr->as.call.arg_count; i++) {
            fputc(' ', out);
            print_expr(pr, expr->as.call.args[i], indent);
        }
        fputc(')', out);
        break;
    case EXPR_INDEX:
        fputs("(index ", out);
        print_expr(pr, expr->as.index.object, indent);
        fputc(' ', out);
        print_expr(pr, expr->as.index.index, indent);
        fputc(')', out);
        break;
    case EXPR_FIELD:
        fputs("(. ", out);
        print_expr(pr, expr->as.field.object, indent);
        fputc(' ', out);
        print_name(pr, expr->as.field.name);
        fputc(')', out);
        break;
    case EXPR_STRUCT_LIT:
        fputs("(lit ", out);
        print_type(pr, expr->as.struct_lit.type);
        for (size_t i = 0; i < expr->as.struct_lit.field_count; i++) {
            fputs(" (", out);
            print_name(pr, expr->as.struct_lit.fields[i].name);
            fputc(' ', out);
            print_expr(pr, expr->as.struct_lit.fields[i].value, indent);
            fputc(')', out);
        }
        fputc(')', out);
        break;
    case EXPR_MATCH:
        fputs("(match ", out);
        print_expr(pr, expr->as.match.subject, indent);
        for (size_t i = 0; i < expr->as.match.arm_count; i++) {
            const MatchArm *arm = &expr->as.match.arms[i];
            newline(pr, indent + 1);
            fputs("(arm ", out);
            print_pattern(pr, &arm->pattern);
            if (arm->block) {
                newline(pr, indent + 2);
                print_stmt(pr, arm->block, indent + 2);
            } else {
                fputc(' ', out);
                print_expr(pr, arm->expr, indent + 1);
            }
            fputc(')', out);
        }
        fputc(')', out);
        break;
    case EXPR_TRY:
        fputs("(try ", out);
        print_expr(pr, expr->as.inner, indent);
        fputc(')', out);
        break;
    case EXPR_ARRAY:
        if (expr->as.array.repeat) {
            fputs("(repeat ", out);
            print_expr(pr, expr->as.array.repeat, indent);
            fprintf(out, " %llu)", (unsigned long long)expr->as.array.repeat_count);
            break;
        }
        fputs("(array", out);
        for (size_t i = 0; i < expr->as.array.count; i++) {
            fputc(' ', out);
            print_expr(pr, expr->as.array.items[i], indent);
        }
        fputc(')', out);
        break;
    }
}

static void print_stmt(Printer *pr, const Stmt *stmt, int indent) {
    FILE *out = pr->out;
    switch (stmt->kind) {
    case STMT_BLOCK:
        fputs("(block", out);
        for (size_t i = 0; i < stmt->as.block.count; i++) {
            newline(pr, indent + 1);
            print_stmt(pr, stmt->as.block.items[i], indent + 1);
        }
        fputc(')', out);
        break;
    case STMT_VAR:
        fputs(stmt->as.var.is_mut ? "(var mut " : "(var ", out);
        print_type(pr, stmt->as.var.type);
        fputc(' ', out);
        print_name(pr, stmt->as.var.name);
        fputc(' ', out);
        print_expr(pr, stmt->as.var.init, indent);
        fputc(')', out);
        break;
    case STMT_ASSIGN:
        fprintf(out, "(%s ", token_kind_spelling(stmt->as.assign.op));
        print_expr(pr, stmt->as.assign.target, indent);
        fputc(' ', out);
        print_expr(pr, stmt->as.assign.value, indent);
        fputc(')', out);
        break;
    case STMT_EXPR:
        print_expr(pr, stmt->as.expr, indent);
        break;
    case STMT_IF:
        fputs("(if ", out);
        print_expr(pr, stmt->as.if_.cond, indent);
        newline(pr, indent + 1);
        print_stmt(pr, stmt->as.if_.then_block, indent + 1);
        if (stmt->as.if_.else_stmt) {
            newline(pr, indent + 1);
            print_stmt(pr, stmt->as.if_.else_stmt, indent + 1);
        }
        fputc(')', out);
        break;
    case STMT_WHILE:
        fputs("(while ", out);
        print_expr(pr, stmt->as.while_.cond, indent);
        newline(pr, indent + 1);
        print_stmt(pr, stmt->as.while_.body, indent + 1);
        fputc(')', out);
        break;
    case STMT_FOR:
        fputs("(for ", out);
        if (stmt->as.for_.init) print_stmt(pr, stmt->as.for_.init, indent);
        else fputc('_', out);
        fputc(' ', out);
        print_expr(pr, stmt->as.for_.cond, indent);
        fputc(' ', out);
        if (stmt->as.for_.step) print_stmt(pr, stmt->as.for_.step, indent);
        else fputc('_', out);
        newline(pr, indent + 1);
        print_stmt(pr, stmt->as.for_.body, indent + 1);
        fputc(')', out);
        break;
    case STMT_FOR_IN:
        fputs(stmt->as.for_in.is_mut ? "(for-in mut " : "(for-in ", out);
        print_type(pr, stmt->as.for_in.type);
        fputc(' ', out);
        print_name(pr, stmt->as.for_in.name);
        fputc(' ', out);
        print_expr(pr, stmt->as.for_in.iterable, indent);
        newline(pr, indent + 1);
        print_stmt(pr, stmt->as.for_in.body, indent + 1);
        fputc(')', out);
        break;
    case STMT_RETURN:
        fputs("(return", out);
        if (stmt->as.expr) {
            fputc(' ', out);
            print_expr(pr, stmt->as.expr, indent);
        }
        fputc(')', out);
        break;
    case STMT_BREAK: fputs("(break)", out); break;
    case STMT_CONTINUE: fputs("(continue)", out); break;
    case STMT_DEFER:
    case STMT_UNSAFE:
        fputs(stmt->kind == STMT_DEFER ? "(defer " : "(unsafe ", out);
        print_stmt(pr, stmt->as.inner, indent);
        fputc(')', out);
        break;
    }
}

static void print_params(Printer *pr, const Param *params, size_t count) {
    fputc('(', pr->out);
    for (size_t i = 0; i < count; i++) {
        if (i) fputc(' ', pr->out);
        fputs(params[i].is_mut ? "(mut " : "(", pr->out);
        print_type(pr, params[i].type);
        fputc(' ', pr->out);
        print_name(pr, params[i].name);
        fputc(')', pr->out);
    }
    fputc(')', pr->out);
}

static void print_fn(Printer *pr, const FnDecl *fn, int indent) {
    FILE *out = pr->out;
    fputs("(fn ", out);
    if (fn->is_pub) fputs("pub ", out);
    if (fn->is_unsafe) fputs("unsafe ", out);
    if (fn->method_kind == METHOD_VIRTUAL) fputs("virtual ", out);
    if (fn->method_kind == METHOD_OVERRIDE) fputs("override ", out);
    print_type(pr, fn->ret);
    fputc(' ', out);
    print_name(pr, fn->name);
    fputc(' ', out);
    print_params(pr, fn->params, fn->param_count);
    if (fn->effects.count) {
        fputs(" (uses", out);
        for (size_t i = 0; i < fn->effects.count; i++) {
            fputc(' ', out);
            print_name(pr, fn->effects.items[i]);
        }
        fputc(')', out);
    }
    for (size_t i = 0; i < fn->contract_count; i++) {
        newline(pr, indent + 1);
        fputs(fn->contracts[i].kind == CONTRACT_REQUIRES ? "(requires " : "(ensures ", out);
        print_expr(pr, fn->contracts[i].expr, indent + 1);
        fputc(')', out);
    }
    if (fn->body) {
        newline(pr, indent + 1);
        print_stmt(pr, fn->body, indent + 1);
    }
    fputc(')', out);
}

static void print_decl(Printer *pr, const Decl *decl) {
    FILE *out = pr->out;
    switch (decl->kind) {
    case DECL_IMPORT:
        fputs("(import ", out);
        print_path(pr, decl->as.import);
        fputc(')', out);
        break;
    case DECL_STRUCT:
    case DECL_CLASS:
    case DECL_INTERFACE:
        fputs(decl->kind == DECL_STRUCT ? "(struct " : decl->kind == DECL_CLASS ? "(class " : "(interface ", out);
        if (decl->is_pub) fputs("pub ", out);
        print_name(pr, decl->as.type.name);
        if (decl->as.type.base_count) {
            fputs(" (:", out);
            for (size_t i = 0; i < decl->as.type.base_count; i++) {
                fputc(' ', out);
                print_path(pr, decl->as.type.bases[i]);
            }
            fputc(')', out);
        }
        for (size_t i = 0; i < decl->as.type.field_count; i++) {
            const Field *field = &decl->as.type.fields[i];
            newline(pr, 1);
            fputs("(field ", out);
            if (field->is_pub) fputs("pub ", out);
            if (field->is_mut) fputs("mut ", out);
            print_type(pr, field->type);
            fputc(' ', out);
            print_name(pr, field->name);
            if (field->init) {
                fputc(' ', out);
                print_expr(pr, field->init, 1);
            }
            fputc(')', out);
        }
        for (size_t i = 0; i < decl->as.type.method_count; i++) {
            newline(pr, 1);
            print_fn(pr, &decl->as.type.methods[i], 1);
        }
        fputc(')', out);
        break;
    case DECL_ENUM:
        fputs("(enum ", out);
        if (decl->is_pub) fputs("pub ", out);
        print_name(pr, decl->as.enum_.name);
        for (size_t i = 0; i < decl->as.enum_.variant_count; i++) {
            const Variant *variant = &decl->as.enum_.variants[i];
            newline(pr, 1);
            print_name(pr, variant->name);
            if (variant->has_parens) print_params(pr, variant->fields, variant->field_count);
        }
        fputc(')', out);
        break;
    case DECL_FN:
        print_fn(pr, &decl->as.fn, 0);
        break;
    case DECL_EXTERN:
        fputs("(extern ", out);
        print_name(pr, decl->as.extern_.abi);
        for (size_t i = 0; i < decl->as.extern_.fn_count; i++) {
            newline(pr, 1);
            print_fn(pr, &decl->as.extern_.fns[i], 1);
        }
        fputc(')', out);
        break;
    case DECL_TEST:
        fputs("(test ", out);
        print_name(pr, decl->as.test.name);
        newline(pr, 1);
        print_stmt(pr, decl->as.test.body, 1);
        fputc(')', out);
        break;
    }
}

void ast_print_module(const Module *module, const SourceFile *file, FILE *out) {
    Printer pr = { file, out };
    for (size_t i = 0; i < module->count; i++) {
        print_decl(&pr, module->decls[i]);
        fputc('\n', out);
    }
}

void ast_print_expr(const Expr *expr, const SourceFile *file, FILE *out) {
    Printer pr = { file, out };
    print_expr(&pr, expr, 0);
}
