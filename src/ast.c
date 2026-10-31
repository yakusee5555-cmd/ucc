#include "ast.h"

#include <stdio.h>
#include <string.h>

struct Node *node_new(struct Arena *a, enum NodeKind kind, int line, int col)
{
    struct Node *n = (struct Node *)arena_alloc(a, sizeof *n);
    memset(n, 0, sizeof *n);
    n->kind = kind;
    n->line = line;
    n->column = col;
    return n;
}

const char *node_kind_name(enum NodeKind k)
{
    static const char *names[] = {
        "IntLit", "StrLit", "Ident", "BinOp", "UnOp", "Postfix", "Assign",
        "Ternary", "Cast", "Sizeof", "Call", "Index", "Member", "Swap",
        "VarDecl", "FuncDef", "Proto", "Block", "If", "While", "DoWhile",
        "For", "Switch", "Case", "Default", "Break", "Continue", "Return",
        "ExprStmt", "StructDef", "InitList"
    };
    if ((int)k >= 0 && (int)k < NK_COUNT)
        return names[k];
    return "?";
}

struct Type *type_int(struct Arena *a)
{
    struct Type *t = (struct Type *)arena_alloc(a, sizeof *t);
    memset(t, 0, sizeof *t);
    t->kind = TY_INT;
    return t;
}

struct Type *type_char(struct Arena *a)
{
    struct Type *t = (struct Type *)arena_alloc(a, sizeof *t);
    memset(t, 0, sizeof *t);
    t->kind = TY_CHAR;
    return t;
}

struct Type *type_void(struct Arena *a)
{
    struct Type *t = (struct Type *)arena_alloc(a, sizeof *t);
    memset(t, 0, sizeof *t);
    t->kind = TY_VOID;
    return t;
}

struct Type *type_float(struct Arena *a)
{
    struct Type *t = (struct Type *)arena_alloc(a, sizeof *t);
    memset(t, 0, sizeof *t);
    t->kind = TY_FLOAT;
    return t;
}

struct Type *type_ptr(struct Arena *a, struct Type *of)
{
    struct Type *t = (struct Type *)arena_alloc(a, sizeof *t);
    memset(t, 0, sizeof *t);
    t->kind = TY_PTR;
    t->of = of;
    return t;
}

struct Type *type_array(struct Arena *a, struct Type *of, long long len)
{
    struct Type *t = (struct Type *)arena_alloc(a, sizeof *t);
    memset(t, 0, sizeof *t);
    t->kind = TY_ARRAY;
    t->of = of;
    t->arr_len = len;
    return t;
}

struct Type *type_struct_tag(struct Arena *a, const char *tag)
{
    struct Type *t = (struct Type *)arena_alloc(a, sizeof *t);
    memset(t, 0, sizeof *t);
    t->kind = TY_STRUCT;
    t->sname = arena_strdup(a, tag);
    return t;
}

long long ts_size(struct Type *t)
{
    if (!t)
        return 8;
    switch (t->kind) {
    case TY_CHAR:
        return 1;
    case TY_INT:
    case TY_FLOAT:
    case TY_PTR:
        return 8;
    case TY_VOID:
        return 8;
    case TY_ARRAY:
        if (t->arr_len < 0)
            return 0;
        return t->arr_len * ts_size(t->of);
    case TY_STRUCT:
        return t->struct_size;
    }
    return 8;
}

int ts_access(struct Type *t)
{
    if (t && t->kind == TY_CHAR)
        return 1;
    return 8;
}

int ts_is_numeric(struct Type *t)
{
    if (!t)
        return 0;
    return t->kind == TY_INT || t->kind == TY_CHAR || t->kind == TY_FLOAT;
}

int ts_is_ptr(struct Type *t)
{
    return t && t->kind == TY_PTR;
}

char *type_to_string(struct Arena *a, struct Type *t)
{
    char buf[256];
    if (!t) {
        snprintf(buf, sizeof buf, "<null>");
    } else {
        switch (t->kind) {
        case TY_INT: snprintf(buf, sizeof buf, "int"); break;
        case TY_CHAR: snprintf(buf, sizeof buf, "char"); break;
        case TY_VOID: snprintf(buf, sizeof buf, "void"); break;
        case TY_FLOAT: snprintf(buf, sizeof buf, "float"); break;
        case TY_PTR: {
            char *inner = type_to_string(a, t->of);
            snprintf(buf, sizeof buf, "%s*", inner);
            break;
        }
        case TY_ARRAY: {
            char *inner = type_to_string(a, t->of);
            if (t->arr_len >= 0)
                snprintf(buf, sizeof buf, "%s[%lld]", inner, t->arr_len);
            else
                snprintf(buf, sizeof buf, "%s[]", inner);
            break;
        }
        case TY_STRUCT:
            snprintf(buf, sizeof buf, "struct %s",
                     t->sname ? t->sname : "?");
            break;
        }
    }
    return arena_strdup(a, buf);
}
