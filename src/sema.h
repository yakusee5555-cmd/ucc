#ifndef UCC_SEMA_H
#define UCC_SEMA_H

#include "ast.h"
#include "diag.h"

/* Semantic analysis: scoped symbol tables, type checking, lvalue
 * validation, and stack-frame layout (global addresses + local offsets).
 * Plain enums only (no typedef enum). */

enum SymKind {
    SYM_VAR = 0,
    SYM_PARAM,
    SYM_FUNC,
    SYM_STRUCT_TAG
};

struct Sym {
    char *name;
    enum SymKind skind;
    struct Type *type;
    int is_global;
    unsigned long long addr; /* globals: absolute address in vm mem */
    unsigned int offset;     /* locals/params: byte offset from fp+16 */
    struct Type **param_types;/* SYM_FUNC */
    int nparams;             /* SYM_FUNC */
    int defined;             /* SYM_FUNC */
    struct Node *def;        /* SYM_FUNC: NK_FUNCDEF or NULL */
};

struct Scope {
    /* Array of pointers to individually heap-allocated symbols, so that
     * `sym` pointers stored in AST nodes stay valid even when the array
     * grows (realloc) or the scope is popped (retired, not freed). */
    struct Sym **syms;
    int nsyms;
    int cap;
    struct Scope *parent;
};

struct StructTag {
    char *tag;
    struct Field *fields;
    int nfields;
    long long size;
};

struct FuncInfo {
    char *name;
    struct Type *ret;
    struct Type **ptypes;
    int nparams;
    struct Node *def; /* NK_FUNCDEF or NULL */
    int defined;
    unsigned int param_units; /* 8-byte units occupied by params */
    unsigned int locals_size; /* L for ENTER / RESERVE (incl. 32B temps) */
    unsigned int temp_base;   /* offset (from fp+16) of hidden temp slot 0 */
    unsigned long long code_addr; /* filled in by codegen */
};

struct Sema {
    struct Arena *arena;
    struct DiagList *diags;
    const char *file;
    struct Scope *scope;  /* current scope */
    struct Scope *gscope; /* global scope */
    struct Scope *retired; /* popped scopes; freed after codegen in sema_free */
    struct FuncInfo *funcs;
    int nfuncs;
    int capfuncs;
    struct StructTag *structs;
    int nstructs;
    int capstructs;
    unsigned long long globals_end;
    struct FuncInfo *cur_func;
    unsigned int cur_param_units;
    unsigned int cur_local_bytes;
    int in_loop;
    int in_switch;
    long long *case_vals;
    int ncase;
    int capcase;
    int has_default;
};

void sema_init(struct Sema *s, struct Arena *a, struct DiagList *d,
               const char *file);
void sema_program(struct Sema *s, struct Node *prog);
void sema_free(struct Sema *s);

/* Integer constant evaluation (array sizes, case labels, global inits). */
long long sema_const_eval(struct Sema *s, struct Node *n, int *ok);
struct FuncInfo *sema_find_func(struct Sema *s, const char *name);
struct StructTag *sema_find_struct(struct Sema *s, const char *tag);

#endif
