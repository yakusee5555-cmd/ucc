#ifndef UCC_AST_H
#define UCC_AST_H

#include "arena.h"
#include "lexer.h"

/* ------------------------------------------------------------------ */
/* types                                                               */
/* ------------------------------------------------------------------ */

/* Plain enum (no typedef). The compiled language is integer-focused:
 * int is a 64-bit signed integer; char is 1 byte (zero-extended on load);
 * float/double are accepted syntactically but stored as 64-bit ints. */
enum TypeKind {
    TY_INT = 0,
    TY_CHAR,
    TY_VOID,
    TY_FLOAT, /* syntactic only: stored as int64 */
    TY_PTR,
    TY_ARRAY,
    TY_STRUCT
};

struct Field {
    char *name;
    struct Type *type;
    long long offset; /* byte offset inside the struct */
    int access;       /* load/store access size: 1 for char fields, else 8 */
};

struct Type {
    enum TypeKind kind;
    struct Type *of;     /* TY_PTR / TY_ARRAY element type */
    long long arr_len;   /* TY_ARRAY: element count, -1 = incomplete,
                          * -2 = deferred (see arr_len_expr) */
    struct Node *arr_len_expr; /* TY_ARRAY: size expression for -2 */
    char *sname;         /* TY_STRUCT: tag name (before resolution) */
    struct Field *fields;/* TY_STRUCT: resolved fields */
    int nfields;
    long long struct_size; /* TY_STRUCT: total size in bytes */
};

struct Type *type_int(struct Arena *a);
struct Type *type_char(struct Arena *a);
struct Type *type_void(struct Arena *a);
struct Type *type_float(struct Arena *a);
struct Type *type_ptr(struct Arena *a, struct Type *of);
struct Type *type_array(struct Arena *a, struct Type *of, long long len);
struct Type *type_struct_tag(struct Arena *a, const char *tag);

/* Storage size in bytes: char=1, int/float/ptr=8, array=len*elem,
 * struct=struct_size. */
long long ts_size(struct Type *t);
/* Load/store access width for a scalar: 1 for char, 8 otherwise. */
int ts_access(struct Type *t);
int ts_is_numeric(struct Type *t);
int ts_is_ptr(struct Type *t);
char *type_to_string(struct Arena *a, struct Type *t);

/* ------------------------------------------------------------------ */
/* AST nodes                                                           */
/* ------------------------------------------------------------------ */

/* Plain enum (no typedef). Field conventions per kind:
 *
 * NK_INT_LIT   ival=value
 * NK_STR_LIT   sval=bytes, ival=byte length
 * NK_IDENT     sval=name
 * NK_BINOP     op=token kind, a=left, b=right
 * NK_UNOP      op=token kind (- ! ~ * & ++ -- prefix), a=operand
 * NK_POSTFIX   op=++ / --, a=operand
 * NK_ASSIGN    op==,+=,-=,*=,/=,%=,&=,|=,^=,<<=,>>= ; a=lhs, b=rhs
 * NK_TERNARY   a=cond, b=then, c=else
 * NK_CAST      type=target, a=expr
 * NK_SIZEOF    type set => sizeof(type) [ival=1]; else a=expr [ival=0]
 * NK_CALL      sval=func name, a=arg list; ival: 0 normal, 1 printf, 2 scanf
 * NK_INDEX     a=base, b=index
 * NK_MEMBER    a=base, sval=field name, ival=0 '.' / 1 '->'
 * NK_SWAP      a,b=operands (intrinsic statement)
 * NK_VAR_DECL  sval=name (NULL=abstract), type, a=initializer or NULL
 * NK_FUNCDEF   sval=name, type=return type, a=params, b=body block
 * NK_PROTO     sval=name, type=return type, a=params
 * NK_BLOCK     a=statement list
 * NK_IF        a=cond, b=then, c=else-or-NULL
 * NK_WHILE     a=cond, b=body
 * NK_DO        a=body, b=cond
 * NK_FOR       a=init (stmt/decl-list/NULL), b=cond/NULL, c=incr/NULL, d=body
 * NK_SWITCH    a=expr, b=body
 * NK_CASE      a=constant expr
 * NK_DEFAULT   (none)
 * NK_BREAK / NK_CONTINUE (none)
 * NK_RETURN    a=expr or NULL
 * NK_EXPR_STMT a=expr (NULL=empty ';')
 * NK_STRUCTDEF sval=tag, a=field declarations
 * NK_INIT_LIST a=element list
 *
 * `next' chains sibling nodes (statement lists, declarators, arguments).
 * Sema annotates: type (expr type / declared type), sym, is_lvalue. */
enum NodeKind {
    NK_INT_LIT = 0,
    NK_STR_LIT,
    NK_IDENT,
    NK_BINOP,
    NK_UNOP,
    NK_POSTFIX,
    NK_ASSIGN,
    NK_TERNARY,
    NK_CAST,
    NK_SIZEOF,
    NK_CALL,
    NK_INDEX,
    NK_MEMBER,
    NK_SWAP,
    NK_VAR_DECL,
    NK_FUNCDEF,
    NK_PROTO,
    NK_BLOCK,
    NK_IF,
    NK_WHILE,
    NK_DO,
    NK_FOR,
    NK_SWITCH,
    NK_CASE,
    NK_DEFAULT,
    NK_BREAK,
    NK_CONTINUE,
    NK_RETURN,
    NK_EXPR_STMT,
    NK_STRUCTDEF,
    NK_INIT_LIST,
    NK_COUNT
};

struct Sym; /* defined in sema.h */

struct Node {
    enum NodeKind kind;
    int line;
    int column;
    int op;
    long long ival;
    long long size_val;  /* NK_SIZEOF: byte size computed by sema */
    char *sval;
    struct Type *type;
    struct Type *decayed_type; /* set by sema decay(): array -> pointer,
                                * while type keeps the original */
    struct Sym *sym;
    struct Field *field; /* NK_MEMBER: resolved field */
    int is_lvalue;
    struct Node *a;
    struct Node *b;
    struct Node *c;
    struct Node *d;
    struct Node *next;
};

struct Node *node_new(struct Arena *a, enum NodeKind kind, int line, int col);
const char *node_kind_name(enum NodeKind k);

#endif
