#include "sema.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned long long align8(unsigned long long v)
{
    return (v + 7u) & ~7u;
}

/* ------------------------------------------------------------------ */
/* setup                                                               */
/* ------------------------------------------------------------------ */

void sema_init(struct Sema *s, struct Arena *a, struct DiagList *d,
               const char *file)
{
    memset(s, 0, sizeof *s);
    s->arena = a;
    s->diags = d;
    s->file = file;
    s->gscope = (struct Scope *)calloc(1, sizeof *s->gscope);
    if (!s->gscope)
        abort();
    s->scope = s->gscope;
}

/* Free one scope chain. The `parent` link serves as the chain link in
 * both the active chain and the retired list. */
static void free_scope_chain(struct Scope *sc)
{
    while (sc) {
        struct Scope *nx = sc->parent;
        for (int i = 0; i < sc->nsyms; i++)
            free(sc->syms[i]);
        free(sc->syms);
        free(sc);
        sc = nx;
    }
}

void sema_free(struct Sema *s)
{
    /* Popped scopes are retired rather than freed because AST nodes keep
     * `sym` pointers into them that codegen still needs. */
    free_scope_chain(s->scope);
    free_scope_chain(s->retired);
    s->scope = NULL;
    s->retired = NULL;
    free(s->funcs);
    free(s->structs);
    free(s->case_vals);
}

/* ------------------------------------------------------------------ */
/* scopes                                                              */
/* ------------------------------------------------------------------ */

static void scope_push(struct Sema *s)
{
    struct Scope *sc = (struct Scope *)calloc(1, sizeof *sc);
    if (!sc)
        abort();
    sc->parent = s->scope;
    s->scope = sc;
}

static void scope_pop(struct Sema *s)
{
    struct Scope *sc = s->scope;
    s->scope = sc->parent;
    /* Retire instead of freeing: AST nodes hold `sym` pointers into this
     * scope that codegen still needs. Freed later by sema_free(). */
    sc->parent = s->retired;
    s->retired = sc;
}

static struct Sym *scope_add(struct Sema *s, const char *name,
                             enum SymKind skind, struct Type *type, int line,
                             int col)
{
    struct Scope *sc = s->scope;
    for (int i = 0; i < sc->nsyms; i++) {
        if (strcmp(sc->syms[i]->name, name) == 0) {
            diag_error_at(s->diags, s->file, line, col,
                          "redefinition of '%s'", name);
            return sc->syms[i];
        }
    }
    if (sc->nsyms == sc->cap) {
        int ncap = sc->cap ? sc->cap * 2 : 8;
        struct Sym **ns = (struct Sym **)realloc(sc->syms,
                                                (size_t)ncap * sizeof *ns);
        if (!ns)
            abort();
        sc->syms = ns;
        sc->cap = ncap;
    }
    /* Each symbol is individually allocated so that `sym` pointers stored
     * in AST nodes stay valid even if this scope's array grows later. */
    struct Sym *sym = (struct Sym *)calloc(1, sizeof *sym);
    if (!sym)
        abort();
    sc->syms[sc->nsyms++] = sym;
    sym->name = arena_strdup(s->arena, name);
    sym->skind = skind;
    sym->type = type;
    return sym;
}

static struct Sym *scope_lookup(struct Sema *s, const char *name)
{
    for (struct Scope *sc = s->scope; sc; sc = sc->parent) {
        for (int i = 0; i < sc->nsyms; i++) {
            if (strcmp(sc->syms[i]->name, name) == 0)
                return sc->syms[i];
        }
    }
    return NULL;
}

struct FuncInfo *sema_find_func(struct Sema *s, const char *name)
{
    for (int i = 0; i < s->nfuncs; i++) {
        if (strcmp(s->funcs[i].name, name) == 0)
            return &s->funcs[i];
    }
    return NULL;
}

struct StructTag *sema_find_struct(struct Sema *s, const char *tag)
{
    for (int i = 0; i < s->nstructs; i++) {
        if (strcmp(s->structs[i].tag, tag) == 0)
            return &s->structs[i];
    }
    return NULL;
}

/* Resolve struct tags inside a type; returns 1 on success. */
static int resolve_type(struct Sema *s, struct Type *t, int line, int col)
{
    if (!t)
        return 1;
    if (t->kind == TY_STRUCT && !t->fields) {
        struct StructTag *st = sema_find_struct(s, t->sname);
        if (!st) {
            diag_error_at(s->diags, s->file, line, col,
                          "use of undefined struct '%s'", t->sname);
            return 0;
        }
        t->fields = st->fields;
        t->nfields = st->nfields;
        t->struct_size = st->size;
        return 1;
    }
    if (t->kind == TY_PTR || t->kind == TY_ARRAY)
        return resolve_type(s, t->of, line, col);
    return 1;
}

static int type_is_complete(struct Type *t)
{
    if (!t)
        return 0;
    if (t->kind == TY_VOID)
        return 0;
    if (t->kind == TY_ARRAY && t->arr_len < 0)
        return 0;
    if (t->kind == TY_STRUCT && !t->fields)
        return 0;
    return 1;
}

/* ------------------------------------------------------------------ */
/* constant evaluation                                                 */
/* ------------------------------------------------------------------ */

static struct Type *se(struct Sema *s, struct Node *n); /* fwd */

long long sema_const_eval(struct Sema *s, struct Node *n, int *ok)
{
    long long a, b;
    *ok = 1;
    if (!n) {
        *ok = 0;
        return 0;
    }
    switch (n->kind) {
    case NK_INT_LIT:
        return n->ival;
    case NK_UNOP:
        a = sema_const_eval(s, n->a, ok);
        if (!*ok)
            return 0;
        switch (n->op) {
        case TK_MINUS: return -a;
        case TK_PLUS: return a;
        case TK_BANG: return !a;
        case TK_TILDE: return ~a;
        default: *ok = 0; return 0;
        }
    case NK_BINOP:
        a = sema_const_eval(s, n->a, ok);
        if (!*ok)
            return 0;
        b = sema_const_eval(s, n->b, ok);
        if (!*ok)
            return 0;
        switch (n->op) {
        case TK_PLUS: return (long long)((unsigned long long)a + (unsigned long long)b);
        case TK_MINUS: return (long long)((unsigned long long)a - (unsigned long long)b);
        case TK_STAR: return (long long)((unsigned long long)a * (unsigned long long)b);
        case TK_SLASH: if (b == 0) { *ok = 0; return 0; } return a / b;
        case TK_PERCENT: if (b == 0) { *ok = 0; return 0; } return a % b;
        case TK_AMP: return a & b;
        case TK_PIPE: return a | b;
        case TK_CARET: return a ^ b;
        case TK_LSHIFT: return (long long)((unsigned long long)a << (b & 63));
        case TK_RSHIFT: return a >> (b & 63);
        case TK_EQEQ: return a == b;
        case TK_NEQ: return a != b;
        case TK_LT: return a < b;
        case TK_LE: return a <= b;
        case TK_GT: return a > b;
        case TK_GE: return a >= b;
        case TK_AMPAMP: return (a && b) ? 1 : 0;
        case TK_PIPEPIPE: return (a || b) ? 1 : 0;
        default: *ok = 0; return 0;
        }
    case NK_TERNARY: {
        a = sema_const_eval(s, n->a, ok);
        if (!*ok)
            return 0;
        return sema_const_eval(s, a ? n->b : n->c, ok);
    }
    case NK_CAST: {
        a = sema_const_eval(s, n->a, ok);
        if (!*ok)
            return 0;
        if (n->type && n->type->kind == TY_CHAR)
            return a & 0xFF;
        return a;
    }
    case NK_SIZEOF:
        /* Ensure analyzed: se() computes size_val (idempotent). */
        se(s, n);
        return n->size_val;
    default:
        *ok = 0;
        return 0;
    }
}

/* ------------------------------------------------------------------ */
/* declarations                                                        */
/* ------------------------------------------------------------------ */

/* Number of 8-byte units a parameter occupies. */
static unsigned int param_units(struct Type *t)
{
    if (t->kind == TY_STRUCT)
        return (unsigned int)((ts_size(t) + 7) / 8);
    return 1;
}

/* Slot bytes for a local variable. */
static unsigned long long local_slot(struct Type *t)
{
    if (t->kind == TY_ARRAY || t->kind == TY_STRUCT)
        return (unsigned long long)ts_size(t);
    return 8; /* scalars (incl. char) occupy 8-byte slots */
}

/* Resolve deferred (-2) array lengths via constant evaluation. */
static int resolve_array_len(struct Sema *s, struct Type *t, int line, int col)
{
    if (!t)
        return 1;
    if (t->kind == TY_ARRAY) {
        if (!resolve_array_len(s, t->of, line, col))
            return 0;
        if (t->arr_len == -2) {
            int ok = 0;
            int errs = s->diags->count;
            long long v = sema_const_eval(s, t->arr_len_expr, &ok);
            if (!ok || v <= 0 || v > 1000000) {
                if (s->diags->count == errs) {
                    diag_error_at(s->diags, s->file, line, col,
                                  "array size must be a positive integer "
                                  "constant");
                }
                return 0;
            }
            t->arr_len = v;
        }
        return 1;
    }
    if (t->kind == TY_PTR)
        return resolve_array_len(s, t->of, line, col);
    return 1;
}

static void declare_global(struct Sema *s, struct Node *vd)
{
    struct Type *t = vd->type;
    if (!resolve_type(s, t, vd->line, vd->column))
        return;
    if (!resolve_array_len(s, t, vd->line, vd->column))
        return;
    /* infer incomplete array size from initializer */
    if (t->kind == TY_ARRAY && t->arr_len < 0) {
        if (vd->a && vd->a->kind == NK_STR_LIT && t->of->kind == TY_CHAR) {
            t->arr_len = vd->a->ival + 1;
        } else if (vd->a && vd->a->kind == NK_INIT_LIST) {
            int cnt = 0;
            for (struct Node *e = vd->a->a; e; e = e->next)
                cnt++;
            t->arr_len = cnt;
        } else {
            diag_error_at(s->diags, s->file, vd->line, vd->column,
                          "array size missing in '%s'",
                          vd->sval ? vd->sval : "?");
            return;
        }
    }
    if (!type_is_complete(t)) {
        diag_error_at(s->diags, s->file, vd->line, vd->column,
                      "variable '%s' has incomplete type",
                      vd->sval ? vd->sval : "?");
        return;
    }
    if (!vd->sval) {
        diag_error_at(s->diags, s->file, vd->line, vd->column,
                      "expected identifier in declaration");
        return;
    }
    struct Sym *sym =
        scope_add(s, vd->sval, SYM_VAR, t, vd->line, vd->column);
    sym->is_global = 1;
    s->globals_end = align8(s->globals_end);
    sym->addr = s->globals_end;
    s->globals_end = align8(sym->addr + (unsigned long long)ts_size(t));
    vd->sym = sym;
}

static void declare_local(struct Sema *s, struct Node *vd, int is_param)
{
    struct Type *t = vd->type;
    if (!resolve_type(s, t, vd->line, vd->column))
        return;
    if (!resolve_array_len(s, t, vd->line, vd->column))
        return;
    if (t->kind == TY_ARRAY && t->arr_len < 0) {
        if (is_param) {
            /* C: array parameters decay to pointers */
            t = type_ptr(s->arena, t->of);
            vd->type = t;
        } else if (vd->a && vd->a->kind == NK_STR_LIT &&
                   t->of->kind == TY_CHAR) {
            t->arr_len = vd->a->ival + 1;
        } else if (vd->a && vd->a->kind == NK_INIT_LIST) {
            int cnt = 0;
            for (struct Node *e = vd->a->a; e; e = e->next)
                cnt++;
            t->arr_len = cnt;
        } else {
            diag_error_at(s->diags, s->file, vd->line, vd->column,
                          "array size missing in '%s'",
                          vd->sval ? vd->sval : "?");
            return;
        }
    }
    if (!type_is_complete(t)) {
        diag_error_at(s->diags, s->file, vd->line, vd->column,
                      "variable '%s' has incomplete type",
                      vd->sval ? vd->sval : "?");
        return;
    }
    const char *nm = vd->sval;
    char hidden[32];
    if (!nm) {
        /* unnamed parameter: give it a hidden name */
        snprintf(hidden, sizeof hidden, "$arg%u", s->cur_param_units);
        nm = hidden;
    }
    struct Sym *sym = scope_add(s, nm, is_param ? SYM_PARAM : SYM_VAR, t,
                                vd->line, vd->column);
    sym->is_global = 0;
    if (is_param) {
        unsigned int u = param_units(t);
        sym->offset = s->cur_param_units * 8;
        s->cur_param_units += u;
    } else {
        unsigned long long off = align8((unsigned long long)s->cur_local_bytes);
        sym->offset = s->cur_param_units * 8 + (unsigned int)off;
        s->cur_local_bytes = (unsigned int)(off + local_slot(t));
    }
    vd->sym = sym;
}

/* ------------------------------------------------------------------ */
/* expressions                                                         */
/* ------------------------------------------------------------------ */

static struct Type *se(struct Sema *s, struct Node *n); /* fwd */

static struct Type *decay(struct Sema *s, struct Node *n)
{
    struct Type *t = n->type;
    if (t && t->kind == TY_ARRAY) {
        /* Array-to-pointer decay: record the decayed type separately so
         * codegen can still see the original array type (it must emit
         * the address, not load a pointer). */
        struct Type *dt = type_ptr(s->arena, t->of);
        n->decayed_type = dt;
        n->is_lvalue = 0;
        return dt;
    }
    return t;
}

static void require_scalar(struct Sema *s, struct Node *n, const char *ctx)
{
    struct Type *t = n->type;
    if (!t || (!ts_is_numeric(t) && !ts_is_ptr(t))) {
        diag_error_at(s->diags, s->file, n->line, n->column,
                      "%s requires a scalar (integer or pointer) operand",
                      ctx);
    }
}

static struct Type *se_call(struct Sema *s, struct Node *n)
{
    int nargs = 0;
    for (struct Node *a = n->a; a; a = a->next)
        nargs++;

    if (strcmp(n->sval, "printf") == 0) {
        n->ival = 1;
        if (nargs < 1) {
            diag_error_at(s->diags, s->file, n->line, n->column,
                          "printf requires at least a format string");
            n->type = type_int(s->arena);
            return n->type;
        }
        struct Node *fmt = n->a;
        se(s, fmt);
        struct Type *ft = decay(s, fmt);
        if (!ts_is_ptr(ft)) {
            diag_error_at(s->diags, s->file, fmt->line, fmt->column,
                          "printf format must be a string");
        }
        for (struct Node *a = fmt->next; a; a = a->next) {
            se(s, a);
            decay(s, a);
            if (a->type && a->type->kind == TY_STRUCT) {
                diag_error_at(s->diags, s->file, a->line, a->column,
                              "cannot pass struct to printf");
            }
        }
        n->type = type_int(s->arena);
        return n->type;
    }
    if (strcmp(n->sval, "scanf") == 0) {
        n->ival = 2;
        if (nargs < 1) {
            diag_error_at(s->diags, s->file, n->line, n->column,
                          "scanf requires at least a format string");
            n->type = type_int(s->arena);
            return n->type;
        }
        struct Node *fmt = n->a;
        se(s, fmt);
        struct Type *ft = decay(s, fmt);
        if (!ts_is_ptr(ft)) {
            diag_error_at(s->diags, s->file, fmt->line, fmt->column,
                          "scanf format must be a string");
        }
        for (struct Node *a = fmt->next; a; a = a->next) {
            se(s, a);
            struct Type *at = decay(s, a);
            if (!ts_is_ptr(at)) {
                diag_error_at(s->diags, s->file, a->line, a->column,
                              "scanf argument must be a pointer - did you "
                              "forget '&'?");
            }
        }
        n->type = type_int(s->arena);
        return n->type;
    }

    struct FuncInfo *fi = sema_find_func(s, n->sval);
    if (!fi) {
        diag_error_at(s->diags, s->file, n->line, n->column,
                      "call to undefined function '%s'", n->sval);
        n->type = type_int(s->arena);
        return n->type;
    }
    if (nargs != fi->nparams) {
        diag_error_at(s->diags, s->file, n->line, n->column,
                      "'%s' expects %d argument(s) but got %d", n->sval,
                      fi->nparams, nargs);
    }
    int i = 0;
    for (struct Node *a = n->a; a; a = a->next, i++) {
        se(s, a);
        struct Type *at = decay(s, a);
        if (i < fi->nparams) {
            struct Type *pt = fi->ptypes[i];
            int ok = 0;
            if (ts_is_numeric(at) && ts_is_numeric(pt))
                ok = 1;
            else if (ts_is_ptr(at) && ts_is_ptr(pt))
                ok = 1;
            else if (pt->kind == TY_STRUCT && at->kind == TY_STRUCT &&
                     pt->sname && at->sname &&
                     strcmp(pt->sname, at->sname) == 0)
                ok = 1;
            else if (ts_is_ptr(pt) && ts_is_numeric(at))
                ok = 1; /* NULL / 0 */
            if (!ok) {
                diag_error_at(s->diags, s->file, a->line, a->column,
                              "incompatible argument %d to '%s': expected %s",
                              i + 1, n->sval,
                              type_to_string(s->arena, pt));
            }
        }
    }
    n->type = fi->ret;
    return n->type;
}

static void se_swap(struct Sema *s, struct Node *n)
{
    struct Type *t1 = se(s, n->a);
    struct Type *t2 = se(s, n->b);
    if (!t1 || !t2) {
        n->type = type_void(s->arena);
        return;
    }
    if (!n->a->is_lvalue) {
        diag_error_at(s->diags, s->file, n->a->line, n->a->column,
                      "swap requires modifiable l-values: first argument "
                      "cannot be swapped (not an l-value)");
    }
    if (!n->b->is_lvalue) {
        diag_error_at(s->diags, s->file, n->b->line, n->b->column,
                      "swap requires modifiable l-values: second argument "
                      "cannot be swapped (not an l-value)");
    }
    long long s1 = ts_size(t1);
    long long s2 = ts_size(t2);
    if (s1 != s2) {
        diag_error_at(s->diags, s->file, n->line, n->column,
                      "swap operands have different sizes (%lld vs %lld "
                      "bytes)",
                      s1, s2);
    }
    if (s1 <= 0 || s1 > 1048576) {
        diag_error_at(s->diags, s->file, n->line, n->column,
                      "swap of %lld bytes is not supported", s1);
    }
    /* stash byte size for codegen */
    n->ival = (int)s1;
    n->type = type_void(s->arena);
}

static struct Type *se_assign(struct Sema *s, struct Node *n)
{
    struct Type *lt = se(s, n->a);
    struct Type *rt = se(s, n->b);
    if (!lt || !rt) {
        n->type = type_int(s->arena);
        return n->type;
    }
    if (!n->a->is_lvalue) {
        diag_error_at(s->diags, s->file, n->a->line, n->a->column,
                      "left side of assignment is not a modifiable l-value");
        n->type = lt;
        return n->type;
    }
    if (lt->kind == TY_ARRAY) {
        diag_error_at(s->diags, s->file, n->a->line, n->a->column,
                      "array type '%s' is not assignable",
                      type_to_string(s->arena, lt));
        n->type = lt;
        return n->type;
    }
    if (n->op != TK_EQ) {
        /* compound: like binop then assign */
        int ok = 0;
        if (ts_is_numeric(lt) && ts_is_numeric(rt))
            ok = 1;
        else if (ts_is_ptr(lt) && ts_is_numeric(rt) &&
                 (n->op == TK_PLUSEQ || n->op == TK_MINUSEQ))
            ok = 1;
        if (!ok) {
            diag_error_at(s->diags, s->file, n->line, n->column,
                          "invalid operands to '%s'",
                          token_kind_name(n->op));
        }
        n->type = lt;
        n->is_lvalue = 0;
        return n->type;
    }
    rt = decay(s, n->b);
    int ok = 0;
    if (ts_is_numeric(lt) && ts_is_numeric(rt))
        ok = 1;
    else if (ts_is_ptr(lt) && ts_is_ptr(rt))
        ok = 1;
    else if (ts_is_ptr(lt) && ts_is_numeric(rt))
        ok = 1; /* p = 0 */
    else if (ts_is_numeric(lt) && ts_is_ptr(rt)) {
        diag_warn_at(s->diags, s->file, n->line, n->column,
                     "assignment makes integer from pointer");
        ok = 1;
    } else if (lt->kind == TY_STRUCT && rt->kind == TY_STRUCT && lt->sname &&
               rt->sname && strcmp(lt->sname, rt->sname) == 0) {
        ok = 1;
    }
    if (!ok) {
        diag_error_at(s->diags, s->file, n->line, n->column,
                      "incompatible types in assignment: %s = %s",
                      type_to_string(s->arena, lt),
                      type_to_string(s->arena, rt));
    }
    n->type = lt;
    n->is_lvalue = 0;
    return n->type;
}

static struct Type *se(struct Sema *s, struct Node *n)
{
    if (!n)
        return type_int(s->arena);
    switch (n->kind) {
    case NK_INT_LIT:
        n->type = type_int(s->arena);
        return n->type;
    case NK_STR_LIT:
        n->type = type_array(s->arena, type_char(s->arena), n->ival + 1);
        n->is_lvalue = 0;
        return n->type;
    case NK_IDENT: {
        struct Sym *sym = scope_lookup(s, n->sval);
        if (!sym) {
            diag_error_at(s->diags, s->file, n->line, n->column,
                          "use of undeclared identifier '%s'", n->sval);
            n->type = type_int(s->arena);
            return n->type;
        }
        if (sym->skind == SYM_FUNC) {
            diag_error_at(s->diags, s->file, n->line, n->column,
                          "'%s' is a function; use a call", n->sval);
            n->type = type_int(s->arena);
            return n->type;
        }
        n->sym = sym;
        n->type = sym->type;
        n->is_lvalue = 1;
        return n->type;
    }
    case NK_BINOP: {
        struct Type *l = se(s, n->a);
        struct Type *r = se(s, n->b);
        l = decay(s, n->a);
        r = decay(s, n->b);
        n->is_lvalue = 0;
        switch (n->op) {
        case TK_PLUS:
            if (ts_is_ptr(l) && ts_is_numeric(r)) {
                n->type = l;
                return n->type;
            }
            if (ts_is_numeric(l) && ts_is_ptr(r)) {
                n->type = r;
                return n->type;
            }
            /* fall through */
        case TK_MINUS:
            if (n->op == TK_MINUS && ts_is_ptr(l) && ts_is_ptr(r)) {
                n->type = type_int(s->arena);
                return n->type;
            }
            if (n->op == TK_MINUS && ts_is_ptr(l) && ts_is_numeric(r)) {
                n->type = l;
                return n->type;
            }
            /* fall through */
        case TK_STAR:
        case TK_SLASH:
        case TK_PERCENT:
        case TK_LSHIFT:
        case TK_RSHIFT:
        case TK_AMP:
        case TK_PIPE:
        case TK_CARET:
            if (!ts_is_numeric(l) || !ts_is_numeric(r)) {
                diag_error_at(s->diags, s->file, n->line, n->column,
                              "invalid operands to '%s'",
                              token_kind_name(n->op));
            }
            n->type = type_int(s->arena);
            return n->type;
        case TK_EQEQ:
        case TK_NEQ:
        case TK_LT:
        case TK_LE:
        case TK_GT:
        case TK_GE:
            if (!(ts_is_numeric(l) || ts_is_ptr(l)) ||
                !(ts_is_numeric(r) || ts_is_ptr(r))) {
                diag_error_at(s->diags, s->file, n->line, n->column,
                              "invalid operands to '%s'",
                              token_kind_name(n->op));
            }
            n->type = type_int(s->arena);
            return n->type;
        case TK_AMPAMP:
        case TK_PIPEPIPE:
            require_scalar(s, n->a, "'&&'/'||' left");
            require_scalar(s, n->b, "'&&'/'||' right");
            n->type = type_int(s->arena);
            return n->type;
        default:
            n->type = type_int(s->arena);
            return n->type;
        }
    }
    case NK_UNOP: {
        se(s, n->a);
        n->is_lvalue = 0;
        switch (n->op) {
        case TK_MINUS:
        case TK_PLUS:
        case TK_TILDE:
            if (!ts_is_numeric(decay(s, n->a))) {
                diag_error_at(s->diags, s->file, n->line, n->column,
                              "invalid operand to unary '%s'",
                              token_kind_name(n->op));
            }
            n->type = type_int(s->arena);
            return n->type;
        case TK_BANG:
            require_scalar(s, n->a, "'!'");
            n->type = type_int(s->arena);
            return n->type;
        case TK_STAR: {
            struct Type *po = decay(s, n->a);
            if (!ts_is_ptr(po)) {
                diag_error_at(s->diags, s->file, n->line, n->column,
                              "cannot dereference non-pointer type %s",
                              type_to_string(s->arena, po));
                n->type = type_int(s->arena);
                return n->type;
            }
            n->type = po->of;
            n->is_lvalue = 1;
            return n->type;
        }
        case TK_AMP: {
            if (!n->a->is_lvalue) {
                diag_error_at(s->diags, s->file, n->line, n->column,
                              "cannot take address of non-lvalue");
                n->type = type_ptr(s->arena, type_int(s->arena));
                return n->type;
            }
            struct Type *ao = n->a->type;
            if (ao->kind == TY_ARRAY)
                n->type = type_ptr(s->arena, ao->of);
            else
                n->type = type_ptr(s->arena, ao);
            return n->type;
        }
        case TK_PLUSPLUS:
        case TK_MINUSMINUS: {
            struct Type *po = decay(s, n->a);
            if (!n->a->is_lvalue) {
                diag_error_at(s->diags, s->file, n->line, n->column,
                              "operand of '%s' must be a modifiable l-value",
                              token_kind_name(n->op));
            } else if (!ts_is_numeric(po) && !ts_is_ptr(po)) {
                diag_error_at(s->diags, s->file, n->line, n->column,
                              "invalid operand to '%s'",
                              token_kind_name(n->op));
            }
            n->type = po;
            return n->type;
        }
        default:
            n->type = type_int(s->arena);
            return n->type;
        }
    }
    case NK_POSTFIX: {
        struct Type *o = se(s, n->a);
        o = decay(s, n->a);
        if (!n->a->is_lvalue) {
            diag_error_at(s->diags, s->file, n->line, n->column,
                          "operand of '%s' must be a modifiable l-value",
                          token_kind_name(n->op));
        } else if (!ts_is_numeric(o) && !ts_is_ptr(o)) {
            diag_error_at(s->diags, s->file, n->line, n->column,
                          "invalid operand to '%s'",
                          token_kind_name(n->op));
        }
        n->type = o;
        n->is_lvalue = 0;
        return n->type;
    }
    case NK_ASSIGN:
        return se_assign(s, n);
    case NK_TERNARY: {
        se(s, n->a);
        require_scalar(s, n->a, "'?:' condition");
        se(s, n->b);
        struct Type *b = decay(s, n->b);
        se(s, n->c);
        struct Type *c = decay(s, n->c);
        n->is_lvalue = 0;
        if ((b && b->kind == TY_STRUCT) || (c && c->kind == TY_STRUCT)) {
            diag_error_at(s->diags, s->file, n->line, n->column,
                          "ternary operator with struct operands is not "
                          "supported");
            n->type = type_int(s->arena);
            return n->type;
        }
        if (ts_is_ptr(b) || ts_is_ptr(c))
            n->type = ts_is_ptr(b) ? b : c;
        else
            n->type = type_int(s->arena);
        return n->type;
    }
    case NK_CAST: {
        resolve_type(s, n->type, n->line, n->column);
        struct Type *o = se(s, n->a);
        o = decay(s, n->a);
        int ok = (ts_is_numeric(n->type) || ts_is_ptr(n->type)) &&
                 (ts_is_numeric(o) || ts_is_ptr(o));
        if (!ok) {
            diag_error_at(s->diags, s->file, n->line, n->column,
                          "invalid cast from %s to %s",
                          type_to_string(s->arena, o),
                          type_to_string(s->arena, n->type));
        }
        n->is_lvalue = 0;
        return n->type;
    }
    case NK_SIZEOF: {
        long long sz = 0;
        if (n->ival) {
            resolve_type(s, n->type, n->line, n->column);
            sz = ts_size(n->type);
        } else {
            se(s, n->a);
            sz = ts_size(n->a->type);
        }
        n->size_val = sz;
        n->type = type_int(s->arena);
        n->is_lvalue = 0;
        return n->type;
    }
    case NK_CALL:
        n->is_lvalue = 0;
        return se_call(s, n);
    case NK_INDEX: {
        struct Type *b = se(s, n->a);
        struct Type *ix = se(s, n->b);
        ix = decay(s, n->b);
        if (!ts_is_numeric(ix)) {
            diag_error_at(s->diags, s->file, n->b->line, n->b->column,
                          "array index must have integer type");
        }
        if (b->kind == TY_ARRAY) {
            n->ival = 0; /* array base */
            n->type = b->of;
            n->is_lvalue = 1;
            return n->type;
        }
        if (b->kind == TY_PTR) {
            n->ival = 1; /* pointer base */
            n->type = b->of;
            n->is_lvalue = 1;
            return n->type;
        }
        b = decay(s, n->a);
        if (b->kind == TY_PTR) {
            n->ival = 1;
            n->type = b->of;
            n->is_lvalue = 1;
            return n->type;
        }
        diag_error_at(s->diags, s->file, n->line, n->column,
                      "cannot index type %s",
                      type_to_string(s->arena, n->a->type));
        n->type = type_int(s->arena);
        return n->type;
    }
    case NK_MEMBER: {
        struct Type *b = se(s, n->a);
        struct Type *st = b;
        if (n->ival == 1) { /* -> */
            b = decay(s, n->a);
            if (!ts_is_ptr(b)) {
                diag_error_at(s->diags, s->file, n->line, n->column,
                              "'->' requires a pointer to struct");
                n->type = type_int(s->arena);
                return n->type;
            }
            st = b->of;
        }
        if (st->kind != TY_STRUCT || !st->fields) {
            diag_error_at(s->diags, s->file, n->line, n->column,
                          "request for member '%s' in non-struct type %s",
                          n->sval, type_to_string(s->arena, st));
            n->type = type_int(s->arena);
            return n->type;
        }
        struct Field *f = NULL;
        for (int i = 0; i < st->nfields; i++) {
            if (strcmp(st->fields[i].name, n->sval) == 0) {
                f = &st->fields[i];
                break;
            }
        }
        if (!f) {
            diag_error_at(s->diags, s->file, n->line, n->column,
                          "struct '%s' has no member named '%s'",
                          st->sname ? st->sname : "?", n->sval);
            n->type = type_int(s->arena);
            return n->type;
        }
        n->field = f;
        n->type = f->type;
        n->is_lvalue = n->a->is_lvalue;
        return n->type;
    }
    case NK_SWAP:
        diag_error_at(s->diags, s->file, n->line, n->column,
                      "swap can only be used as a statement");
        n->type = type_void(s->arena);
        return n->type;
    default:
        diag_error_at(s->diags, s->file, n->line, n->column,
                      "unexpected expression node %s",
                      node_kind_name(n->kind));
        n->type = type_int(s->arena);
        return n->type;
    }
}

/* ------------------------------------------------------------------ */
/* statements                                                          */
/* ------------------------------------------------------------------ */

static void ss(struct Sema *s, struct Node *n); /* fwd */

static void check_scalar_cond(struct Sema *s, struct Node *cond,
                              const char *what)
{
    se(s, cond);
    struct Type *t = decay(s, cond);
    if (!ts_is_numeric(t) && !ts_is_ptr(t)) {
        diag_error_at(s->diags, s->file, cond->line, cond->column,
                      "%s condition must be scalar", what);
    }
}

static void check_init(struct Sema *s, struct Node *vd)
{
    struct Type *t = vd->type;
    struct Node *init = vd->a;
    if (!init)
        return;
    if (t->kind == TY_ARRAY) {
        if (init->kind == NK_STR_LIT && t->of->kind == TY_CHAR) {
            if (init->ival + 1 > t->arr_len) {
                diag_error_at(s->diags, s->file, init->line, init->column,
                              "initializer string too long for array of %lld",
                              t->arr_len);
            }
            return;
        }
        if (init->kind != NK_INIT_LIST) {
            diag_error_at(s->diags, s->file, init->line, init->column,
                          "array initializer must be a brace-enclosed list");
            return;
        }
        long long total = ts_size(t) / (ts_size(t->of) ? ts_size(t->of) : 1);
        int i = 0;
        int is_global = vd->sym && vd->sym->is_global;
        for (struct Node *e = init->a; e; e = e->next, i++) {
            if ((long long)i >= total) {
                diag_error_at(s->diags, s->file, e->line, e->column,
                              "excess elements in array initializer");
                break;
            }
            struct Type *et = se(s, e);
            et = decay(s, e);
            if (!ts_is_numeric(et) && t->of->kind != TY_STRUCT) {
                diag_error_at(s->diags, s->file, e->line, e->column,
                              "invalid array element initializer");
            }
            if (is_global && e->kind != NK_STR_LIT) {
                int cok = 0;
                (void)sema_const_eval(s, e, &cok);
                if (!cok) {
                    diag_error_at(s->diags, s->file, e->line, e->column,
                                  "global initializer must be constant");
                }
            }
        }
        return;
    }
    if (t->kind == TY_STRUCT) {
        if (init->kind != NK_INIT_LIST) {
            diag_error_at(s->diags, s->file, init->line, init->column,
                          "struct initializer must be a brace-enclosed list");
            return;
        }
        int i = 0;
        int is_global = vd->sym && vd->sym->is_global;
        for (struct Node *e = init->a; e; e = e->next, i++) {
            if (i >= t->nfields) {
                diag_error_at(s->diags, s->file, e->line, e->column,
                              "excess elements in struct initializer");
                break;
            }
            struct Type *et = se(s, e);
            et = decay(s, e);
            struct Type *ft = t->fields[i].type;
            if (!(ts_is_numeric(et) && ts_is_numeric(ft)) &&
                !(ts_is_ptr(et) && ts_is_ptr(ft))) {
                diag_error_at(s->diags, s->file, e->line, e->column,
                              "incompatible struct field initializer");
            }
            if (is_global) {
                int cok = 0;
                (void)sema_const_eval(s, e, &cok);
                if (!cok) {
                    diag_error_at(s->diags, s->file, e->line, e->column,
                                  "global initializer must be constant");
                }
            }
        }
        return;
    }
    /* scalar */
    struct Type *it = se(s, init);
    it = decay(s, init);
    int ok = 0;
    if (ts_is_numeric(t) && ts_is_numeric(it))
        ok = 1;
    else if (ts_is_ptr(t) && ts_is_ptr(it))
        ok = 1;
    else if (ts_is_ptr(t) && ts_is_numeric(it))
        ok = 1;
    else if (ts_is_numeric(t) && ts_is_ptr(it)) {
        diag_warn_at(s->diags, s->file, init->line, init->column,
                     "initialization makes integer from pointer");
        ok = 1;
    }
    if (!ok) {
        diag_error_at(s->diags, s->file, init->line, init->column,
                      "incompatible initializer: %s from %s",
                      type_to_string(s->arena, t),
                      type_to_string(s->arena, it));
    }
    if (vd->sym && vd->sym->is_global) {
        /* String literals live in global memory, so they are fine as
         * global initializers; everything else must be constant. */
        if (init->kind != NK_STR_LIT) {
            int cok = 0;
            (void)sema_const_eval(s, init, &cok);
            if (!cok) {
                diag_error_at(s->diags, s->file, init->line, init->column,
                              "global initializer must be constant");
            }
        }
    }
}

static void ss(struct Sema *s, struct Node *n)
{
    if (!n)
        return;
    switch (n->kind) {
    case NK_BLOCK:
        scope_push(s);
        for (struct Node *st = n->a; st; st = st->next)
            ss(s, st);
        scope_pop(s);
        break;
    case NK_VAR_DECL: {
        int is_global = s->scope == s->gscope;
        if (is_global)
            declare_global(s, n);
        else
            declare_local(s, n, 0);
        check_init(s, n);
        break;
    }
    case NK_EXPR_STMT:
        if (n->a)
            se(s, n->a);
        break;
    case NK_IF:
        check_scalar_cond(s, n->a, "if");
        ss(s, n->b);
        ss(s, n->c);
        break;
    case NK_WHILE:
        check_scalar_cond(s, n->a, "while");
        s->in_loop++;
        ss(s, n->b);
        s->in_loop--;
        break;
    case NK_DO:
        s->in_loop++;
        ss(s, n->a);
        s->in_loop--;
        check_scalar_cond(s, n->b, "do-while");
        break;
    case NK_FOR:
        scope_push(s);
        ss(s, n->a); /* init: decl list or expr stmt */
        if (n->b)
            check_scalar_cond(s, n->b, "for");
        if (n->c)
            se(s, n->c);
        s->in_loop++;
        ss(s, n->d);
        s->in_loop--;
        scope_pop(s);
        break;
    case NK_SWITCH: {
        se(s, n->a);
        struct Type *t = decay(s, n->a);
        if (!ts_is_numeric(t)) {
            diag_error_at(s->diags, s->file, n->a->line, n->a->column,
                          "switch expression must have integer type");
        }
        s->in_switch++;
        int save_ncase = s->ncase;
        int save_hasdef = s->has_default;
        s->ncase = 0;
        s->has_default = 0;
        ss(s, n->b);
        s->ncase = save_ncase;
        s->has_default = save_hasdef;
        s->in_switch--;
        break;
    }
    case NK_CASE: {
        if (!s->in_switch) {
            diag_error_at(s->diags, s->file, n->line, n->column,
                          "'case' outside of switch");
            break;
        }
        int errs = s->diags->count;
        int ok = 0;
        long long v = sema_const_eval(s, n->a, &ok);
        if (!ok || s->diags->count > errs) {
            if (s->diags->count == errs)
                diag_error_at(s->diags, s->file, n->a->line, n->a->column,
                              "case label must be an integer constant");
        } else {
            for (int i = 0; i < s->ncase; i++) {
                if (s->case_vals[i] == v) {
                    diag_error_at(s->diags, s->file, n->line, n->column,
                                  "duplicate case value %lld", v);
                    break;
                }
            }
            if (s->ncase == s->capcase) {
                int ncap = s->capcase ? s->capcase * 2 : 16;
                long long *nv = (long long *)realloc(
                    s->case_vals, (size_t)ncap * sizeof *nv);
                if (!nv)
                    abort();
                s->case_vals = nv;
                s->capcase = ncap;
            }
            s->case_vals[s->ncase++] = v;
        }
        break;
    }
    case NK_DEFAULT:
        if (!s->in_switch) {
            diag_error_at(s->diags, s->file, n->line, n->column,
                          "'default' outside of switch");
        } else if (s->has_default) {
            diag_error_at(s->diags, s->file, n->line, n->column,
                          "duplicate 'default' label");
        } else {
            s->has_default = 1;
        }
        break;
    case NK_BREAK:
        if (!s->in_loop && !s->in_switch) {
            diag_error_at(s->diags, s->file, n->line, n->column,
                          "'break' outside of loop or switch");
        }
        break;
    case NK_CONTINUE:
        if (!s->in_loop) {
            diag_error_at(s->diags, s->file, n->line, n->column,
                          "'continue' outside of loop");
        }
        break;
    case NK_RETURN: {
        if (!s->cur_func) {
            diag_error_at(s->diags, s->file, n->line, n->column,
                          "'return' outside of function");
            break;
        }
        struct Type *rt = s->cur_func->ret;
        if (!n->a) {
            if (rt->kind != TY_VOID) {
                diag_warn_at(s->diags, s->file, n->line, n->column,
                             "return with no value in function returning %s",
                             type_to_string(s->arena, rt));
            }
        } else {
            if (rt->kind == TY_VOID) {
                diag_error_at(s->diags, s->file, n->line, n->column,
                              "return with a value in void function");
            } else {
                struct Type *et = se(s, n->a);
                et = decay(s, n->a);
                if (!(ts_is_numeric(et) && ts_is_numeric(rt)) &&
                    !(ts_is_ptr(et) && ts_is_ptr(rt)) &&
                    !(ts_is_ptr(rt) && ts_is_numeric(et))) {
                    diag_error_at(s->diags, s->file, n->line, n->column,
                                  "incompatible return type");
                }
            }
        }
        break;
    }
    case NK_SWAP:
        se_swap(s, n);
        break;
    default:
        diag_error_at(s->diags, s->file, n->line, n->column,
                      "unexpected statement node %s",
                      node_kind_name(n->kind));
        break;
    }
}

/* ------------------------------------------------------------------ */
/* top level                                                           */
/* ------------------------------------------------------------------ */

static void register_struct(struct Sema *s, struct Node *n)
{
    if (sema_find_struct(s, n->sval)) {
        diag_error_at(s->diags, s->file, n->line, n->column,
                      "redefinition of struct '%s'", n->sval);
        return;
    }
    if (s->nstructs == s->capstructs) {
        int ncap = s->capstructs ? s->capstructs * 2 : 8;
        struct StructTag *ns = (struct StructTag *)realloc(
            s->structs, (size_t)ncap * sizeof *ns);
        if (!ns)
            abort();
        s->structs = ns;
        s->capstructs = ncap;
    }
    struct StructTag *st = &s->structs[s->nstructs++];
    memset(st, 0, sizeof *st);
    st->tag = arena_strdup(s->arena, n->sval);
    /* count fields */
    int nf = 0;
    for (struct Node *f = n->a; f; f = f->next)
        nf++;
    st->fields = (struct Field *)arena_alloc(s->arena,
                                            sizeof *st->fields *
                                                (size_t)(nf ? nf : 1));
    long long off = 0;
    int i = 0;
    for (struct Node *f = n->a; f; f = f->next, i++) {
        if (!resolve_type(s, f->type, f->line, f->column))
            continue;
        if (f->type->kind == TY_VOID) {
            diag_error_at(s->diags, s->file, f->line, f->column,
                          "invalid struct field type");
            continue;
        }
        for (int j = 0; j < i; j++) {
            if (st->fields[j].name &&
                strcmp(st->fields[j].name, f->sval) == 0) {
                diag_error_at(s->diags, s->file, f->line, f->column,
                              "duplicate field '%s'", f->sval);
            }
        }
        off = (long long)align8((unsigned long long)off);
        st->fields[i].name = arena_strdup(s->arena, f->sval ? f->sval : "?");
        st->fields[i].type = f->type;
        st->fields[i].offset = off;
        st->fields[i].access = ts_access(f->type);
        long long units = (ts_size(f->type) + 7) / 8;
        if (units < 1)
            units = 1;
        off += 8 * units;
    }
    st->nfields = nf;
    st->size = (long long)align8((unsigned long long)off);
}

static struct FuncInfo *register_func(struct Sema *s, struct Node *n,
                                      int is_def)
{
    const char *name = n->sval;
    if (!name) {
        diag_error_at(s->diags, s->file, n->line, n->column,
                      "function definition requires a name");
        return NULL;
    }
    if (strcmp(name, "printf") == 0 || strcmp(name, "scanf") == 0 ||
        strcmp(name, "swap") == 0) {
        diag_error_at(s->diags, s->file, n->line, n->column,
                      "'%s' is a builtin and cannot be redefined", name);
        return NULL;
    }
    struct FuncInfo *fi = sema_find_func(s, name);
    int nparams = 0;
    for (struct Node *p = n->a; p; p = p->next)
        nparams++;
    struct Type **pts = (struct Type **)arena_alloc(
        s->arena, sizeof *pts * (size_t)(nparams ? nparams : 1));
    int i = 0;
    for (struct Node *p = n->a; p; p = p->next, i++) {
        resolve_type(s, p->type, p->line, p->column);
        if (p->type->kind == TY_ARRAY) {
            /* parameters decay */
            p->type = type_ptr(s->arena, p->type->of);
        }
        pts[i] = p->type;
    }
    if (!fi) {
        if (s->nfuncs == s->capfuncs) {
            int ncap = s->capfuncs ? s->capfuncs * 2 : 16;
            struct FuncInfo *nf = (struct FuncInfo *)realloc(
                s->funcs, (size_t)ncap * sizeof *nf);
            if (!nf)
                abort();
            s->funcs = nf;
            s->capfuncs = ncap;
        }
        fi = &s->funcs[s->nfuncs++];
        memset(fi, 0, sizeof *fi);
        fi->name = arena_strdup(s->arena, name);
        if (n->type->kind == TY_STRUCT) {
            diag_error_at(s->diags, s->file, n->line, n->column,
                          "returning struct by value is not supported");
        }
        fi->ret = n->type;
        fi->ptypes = pts;
        fi->nparams = nparams;
    } else {
        /* prototype/definition merge: check signature compatibility */
        if (fi->nparams != nparams) {
            diag_error_at(s->diags, s->file, n->line, n->column,
                          "conflicting signature for '%s'", name);
        }
        if (is_def) {
            if (fi->defined) {
                diag_error_at(s->diags, s->file, n->line, n->column,
                              "redefinition of '%s'", name);
                return fi;
            }
            fi->def = n;
            fi->defined = 1;
        }
        return fi;
    }
    if (is_def) {
        fi->def = n;
        fi->defined = 1;
    }
    resolve_type(s, fi->ret, n->line, n->column);
    return fi;
}

static void check_func_body(struct Sema *s, struct FuncInfo *fi)
{
    struct Node *n = fi->def;
    s->cur_func = fi;
    s->cur_param_units = 0;
    s->cur_local_bytes = 0;
    scope_push(s);
    /* declare parameters */
    for (struct Node *p = n->a; p; p = p->next)
        declare_local(s, p, 1);
    fi->param_units = s->cur_param_units;
    ss(s, n->b);
    scope_pop(s);
    unsigned int aligned =
        (unsigned int)align8((unsigned long long)s->cur_local_bytes);
    fi->temp_base = fi->param_units * 8 + aligned;
    fi->locals_size = aligned + 32; /* 4 hidden 8-byte temp slots */
    s->cur_func = NULL;
}

void sema_program(struct Sema *s, struct Node *prog)
{
    /* phase 1: register structs, functions, globals */
    for (struct Node *e = prog; e; e = e->next) {
        switch (e->kind) {
        case NK_STRUCTDEF:
            register_struct(s, e);
            break;
        case NK_FUNCDEF:
            register_func(s, e, 1);
            break;
        case NK_PROTO:
            register_func(s, e, 0);
            break;
        case NK_VAR_DECL:
            declare_global(s, e);
            break;
        default:
            diag_error_at(s->diags, s->file, e->line, e->column,
                          "unexpected top-level node %s",
                          node_kind_name(e->kind));
            break;
        }
    }
    if (s->diags->has_error)
        return;
    /* phase 2: check global initializers */
    for (struct Node *e = prog; e; e = e->next) {
        if (e->kind == NK_VAR_DECL && e->sym)
            check_init(s, e);
    }
    if (s->diags->has_error)
        return;
    /* phase 3: check function bodies */
    for (int i = 0; i < s->nfuncs; i++) {
        if (s->funcs[i].def)
            check_func_body(s, &s->funcs[i]);
    }
    if (!sema_find_func(s, "main")) {
        diag_error_at(s->diags, s->file, 1, 1,
                      "undefined reference to 'main'");
    } else {
        struct FuncInfo *m = sema_find_func(s, "main");
        if (m->nparams != 0) {
            diag_error_at(s->diags, s->file, m->def->line, m->def->column,
                          "'main' must be declared as main() or main(void)");
        }
    }
}
