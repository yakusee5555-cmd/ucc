#include "codegen.h"
#include "vm.h"

#include <stdlib.h>
#include <string.h>

/* Bytecode emitter for the annotated AST.
 *
 * Frame layout (stack grows downward, fp = lowest address of frame):
 *   [fp, fp+16)                 header: ret_pc u64, old_fp u64
 *   [fp+16, fp+16+8*nargs)      params (8-byte units)
 *   [fp+16+8*nargs, +locals)    locals (ADDR_LOCAL off = fp+16+off)
 * The caller reserves the callee's locals with OP_RESERVE before CALL and
 * releases with OP_RELEASE after return; the callee's ENTER zeroes them. */

static void cg_grow(struct Codegen *cg, size_t extra)
{
    if (cg->clen + extra > cg->ccap) {
        size_t ncap = cg->ccap ? cg->ccap : 1024;
        while (ncap < cg->clen + extra)
            ncap *= 2;
        unsigned char *nb = (unsigned char *)realloc(cg->code, ncap);
        if (!nb)
            abort();
        cg->code = nb;
        cg->ccap = ncap;
    }
}

static void cg_u8(struct Codegen *cg, unsigned v)
{
    cg_grow(cg, 1);
    cg->code[cg->clen++] = (unsigned char)v;
}

static void cg_u32(struct Codegen *cg, unsigned long long v)
{
    cg_grow(cg, 4);
    for (int i = 0; i < 4; i++) {
        cg->code[cg->clen++] = (unsigned char)(v & 0xFF);
        v >>= 8;
    }
}

static void cg_u64(struct Codegen *cg, unsigned long long v)
{
    cg_grow(cg, 8);
    for (int i = 0; i < 8; i++) {
        cg->code[cg->clen++] = (unsigned char)(v & 0xFF);
        v >>= 8;
    }
}

static void cg_i64(struct Codegen *cg, long long v)
{
    cg_u64(cg, (unsigned long long)v);
}

static void cg_push_imm(struct Codegen *cg, long long v)
{
    cg_u8(cg, OP_PUSH_IMM);
    cg_i64(cg, v);
}

/* ------------------------------------------------------------------ */
/* labels                                                              */
/* ------------------------------------------------------------------ */

static int cg_label(struct Codegen *cg)
{
    if (cg->nlabels == cg->caplabels) {
        int ncap = cg->caplabels ? cg->caplabels * 2 : 32;
        long long *np = (long long *)realloc(cg->label_pos,
                                            (size_t)ncap * sizeof *np);
        if (!np)
            abort();
        cg->label_pos = np;
        cg->caplabels = ncap;
    }
    cg->label_pos[cg->nlabels] = -1;
    return cg->nlabels++;
}

static void cg_mark(struct Codegen *cg, int id)
{
    cg->label_pos[id] = (long long)cg->clen;
}

static void cg_jmp(struct Codegen *cg, unsigned op, int id)
{
    cg_u8(cg, op);
    if (cg->njmpfix == cg->capjmpfix) {
        int ncap = cg->capjmpfix ? cg->capjmpfix * 2 : 32;
        struct JmpFix *nj = (struct JmpFix *)realloc(
            cg->jmpfix, (size_t)ncap * sizeof *nj);
        if (!nj)
            abort();
        cg->jmpfix = nj;
        cg->capjmpfix = ncap;
    }
    cg->jmpfix[cg->njmpfix].label = id;
    cg->jmpfix[cg->njmpfix].at = cg->clen;
    cg->njmpfix++;
    cg_u64(cg, 0);
}

static void cg_resolve(struct Codegen *cg)
{
    for (int i = 0; i < cg->njmpfix; i++) {
        int id = cg->jmpfix[i].label;
        long long pos = cg->label_pos[id];
        if (pos < 0) {
            diag_error_at(cg->diags, cg->sema->file, 1, 1,
                          "internal error: unresolved label");
            pos = 0;
        }
        size_t at = cg->jmpfix[i].at;
        unsigned long long v = (unsigned long long)pos;
        for (int b = 0; b < 8; b++) {
            cg->code[at++] = (unsigned char)(v & 0xFF);
            v >>= 8;
        }
    }
}

static void cg_labels_reset(struct Codegen *cg)
{
    cg->nlabels = 0;
    cg->njmpfix = 0;
    cg->nbreak = 0;
    cg->ncont = 0;
}

static void cg_push_brk(struct Codegen *cg, int id)
{
    if (cg->nbreak == cg->capbreak) {
        int ncap = cg->capbreak ? cg->capbreak * 2 : 8;
        int *nb = (int *)realloc(cg->bstack, (size_t)ncap * sizeof *nb);
        if (!nb)
            abort();
        cg->bstack = nb;
        cg->capbreak = ncap;
    }
    cg->bstack[cg->nbreak++] = id;
}

static void cg_push_cont(struct Codegen *cg, int id)
{
    if (cg->ncont == cg->capcont) {
        int ncap = cg->capcont ? cg->capcont * 2 : 8;
        int *nb = (int *)realloc(cg->cstack, (size_t)ncap * sizeof *nb);
        if (!nb)
            abort();
        cg->cstack = nb;
        cg->capcont = ncap;
    }
    cg->cstack[cg->ncont++] = id;
}

/* ------------------------------------------------------------------ */
/* globals image + string interning                                    */
/* ------------------------------------------------------------------ */

static void gimg_reserve(struct Codegen *cg, size_t extra)
{
    if (cg->glen + extra > cg->gcap) {
        size_t ncap = cg->gcap ? cg->gcap : 1024;
        while (ncap < cg->glen + extra)
            ncap *= 2;
        unsigned char *nb = (unsigned char *)realloc(cg->gimg, ncap);
        if (!nb)
            abort();
        /* zero the newly allocated region */
        memset(nb + cg->gcap, 0, ncap - cg->gcap);
        cg->gimg = nb;
        cg->gcap = ncap;
    }
}

/* Intern a byte string as a global; returns its address. */
static unsigned long long cg_str(struct Codegen *cg, const char *bytes,
                                 int len)
{
    for (int i = 0; i < cg->nstrs; i++) {
        if (cg->strs[i].len == len &&
            memcmp(cg->strs[i].bytes, bytes, (size_t)len) == 0)
            return cg->strs[i].addr;
    }
    if (cg->nstrs == cg->capstrs) {
        int ncap = cg->capstrs ? cg->capstrs * 2 : 16;
        struct StrEntry *ns = (struct StrEntry *)realloc(
            cg->strs, (size_t)ncap * sizeof *ns);
        if (!ns)
            abort();
        cg->strs = ns;
        cg->capstrs = ncap;
    }
    unsigned long long addr = (cg->glen + 7u) & ~7u;
    gimg_reserve(cg, (size_t)(addr - cg->glen) + (size_t)len + 1);
    cg->glen = (size_t)addr;
    memcpy(cg->gimg + cg->glen, bytes, (size_t)len);
    cg->gimg[cg->glen + (size_t)len] = '\0';
    struct StrEntry *e = &cg->strs[cg->nstrs++];
    e->bytes = (char *)malloc((size_t)len + 1);
    if (!e->bytes)
        abort();
    memcpy(e->bytes, bytes, (size_t)len);
    e->bytes[len] = '\0';
    e->len = len;
    e->addr = addr;
    cg->glen += (size_t)len + 1;
    return addr;
}

/* Intern arbitrary bytes (hidden init literals); returns address. */
static unsigned long long cg_bytes(struct Codegen *cg,
                                   const unsigned char *bytes, size_t len)
{
    unsigned long long addr = (cg->glen + 7u) & ~7u;
    gimg_reserve(cg, (size_t)(addr - cg->glen) + len);
    cg->glen = (size_t)addr;
    memcpy(cg->gimg + cg->glen, bytes, len);
    cg->glen += len;
    return addr;
}

static void gimg_write(struct Codegen *cg, unsigned long long addr,
                       unsigned long long v, int nbytes)
{
    for (int i = 0; i < nbytes; i++) {
        cg->gimg[addr + (unsigned long long)i] = (unsigned char)(v & 0xFF);
        v >>= 8;
    }
}

/* Flatten an initializer into buf (size = ts_size(t) bytes). */
static void flatten_buf(struct Codegen *cg, struct Type *t, struct Node *init,
                        unsigned char *buf)
{
    if (t->kind == TY_ARRAY) {
        struct Type *et = t->of;
        long long esz = ts_size(et);
        long long n = t->arr_len;
        if (init && init->kind == NK_STR_LIT && et->kind == TY_CHAR) {
            long long copy = init->ival + 1; /* include NUL */
            if (copy > n)
                copy = n;
            memcpy(buf, init->sval, (size_t)copy);
            return;
        }
        for (long long i = 0; i < n; i++) {
            struct Node *e = NULL;
            if (init && init->kind == NK_INIT_LIST) {
                e = init->a;
                for (long long k = 0; k < i && e; k++)
                    e = e->next;
            }
            flatten_buf(cg, et, e, buf + (size_t)(i * esz));
        }
        return;
    }
    if (t->kind == TY_STRUCT) {
        for (int i = 0; i < t->nfields; i++) {
            struct Node *e = NULL;
            if (init && init->kind == NK_INIT_LIST) {
                e = init->a;
                for (int k = 0; k < i && e; k++)
                    e = e->next;
            }
            flatten_buf(cg, t->fields[i].type, e,
                        buf + (size_t)t->fields[i].offset);
        }
        return;
    }
    /* scalar */
    unsigned long long v = 0;
    int aw = ts_access(t);
    if (init) {
        if (init->kind == NK_STR_LIT && t->kind == TY_PTR) {
            v = cg_str(cg, init->sval, (int)init->ival);
            aw = 8;
        } else {
            int ok = 0;
            long long c = sema_const_eval(cg->sema, init, &ok);
            if (ok)
                v = (unsigned long long)c;
        }
    }
    for (int i = 0; i < aw; i++) {
        buf[i] = (unsigned char)(v & 0xFF);
        v >>= 8;
    }
}

static void cg_emit_global_init(struct Codegen *cg, struct Sym *sym,
                                struct Node *init)
{
    if (!init)
        return;
    struct Type *t = sym->type;
    if (t->kind == TY_ARRAY || t->kind == TY_STRUCT) {
        long long sz = ts_size(t);
        unsigned char *buf = (unsigned char *)calloc(1, (size_t)sz);
        if (!buf)
            abort();
        flatten_buf(cg, t, init, buf);
        memcpy(cg->gimg + sym->addr, buf, (size_t)sz);
        free(buf);
        return;
    }
    /* scalar */
    unsigned long long v = 0;
    int aw = ts_access(t);
    if (init->kind == NK_STR_LIT && t->kind == TY_PTR) {
        v = cg_str(cg, init->sval, (int)init->ival);
        aw = 8;
    } else {
        int ok = 0;
        long long c = sema_const_eval(cg->sema, init, &ok);
        if (ok)
            v = (unsigned long long)c;
    }
    gimg_write(cg, sym->addr, v, aw);
}

/* ------------------------------------------------------------------ */
/* expression codegen                                                  */
/* ------------------------------------------------------------------ */

static void gen_expr(struct Codegen *cg, struct Node *n);
static void gen_addr(struct Codegen *cg, struct Node *n);
static void gen_stmt(struct Codegen *cg, struct Node *n);

static struct FuncInfo *cg_cur(struct Codegen *cg)
{
    return cg->sema->cur_func;
}

static unsigned temp_off(struct Codegen *cg, int i)
{
    return cg_cur(cg)->temp_base + (unsigned)(i * 8);
}

/* Hidden temp slots are always 8 bytes.
 * Allocation (lifetimes must not overlap):
 *   slot 0: assignment lhs address; switch discriminant (dead at
 *           different times, never live simultaneously)
 *   slot 1: spare
 *   slot 2: ++/-- operand address
 *   slot 3: ++/-- old value
 * Expression temps (2,3) are never live across an assignment's address
 * temp (0), so `a[i] = j++` is safe. */
static void cg_store_temp(struct Codegen *cg, int i)
{
    cg_u8(cg, OP_STORE_LOCAL);
    cg_u32(cg, temp_off(cg, i));
    cg_u8(cg, 8);
}

static void cg_load_temp(struct Codegen *cg, int i)
{
    cg_u8(cg, OP_LOAD_LOCAL);
    cg_u32(cg, temp_off(cg, i));
    cg_u8(cg, 8);
}

/* Effective type for codegen: the decayed type if sema decayed the node. */
static struct Type *cg_etype(struct Node *n)
{
    return n->decayed_type ? n->decayed_type : n->type;
}

static void gen_index_addr(struct Codegen *cg, struct Node *n)
{
    /* n is NK_INDEX; n->ival: 0 = array base, 1 = pointer base */
    struct Type *bt = n->a->type;
    struct Type *elem = bt->of;
    long long esz = ts_size(elem);
    if (esz <= 0)
        esz = 8;
    if (n->ival == 0)
        gen_addr(cg, n->a);
    else
        gen_expr(cg, n->a);
    gen_expr(cg, n->b);
    cg_push_imm(cg, esz);
    cg_u8(cg, OP_MUL);
    cg_u8(cg, OP_ADD);
}

static void gen_addr(struct Codegen *cg, struct Node *n)
{
    switch (n->kind) {
    case NK_IDENT: {
        struct Sym *sym = n->sym;
        if (!sym) {
            cg_push_imm(cg, 0);
            return;
        }
        if (sym->is_global) {
            cg_u8(cg, OP_ADDR_GLOBAL);
            cg_u64(cg, sym->addr);
        } else {
            cg_u8(cg, OP_ADDR_LOCAL);
            cg_u32(cg, sym->offset);
        }
        return;
    }
    case NK_UNOP: /* *p */
        gen_expr(cg, n->a);
        return;
    case NK_INDEX:
        gen_index_addr(cg, n);
        return;
    case NK_MEMBER:
        gen_addr(cg, n->a);
        cg_push_imm(cg, n->field ? n->field->offset : 0);
        cg_u8(cg, OP_ADD);
        return;
    case NK_STR_LIT: {
        unsigned long long a = cg_str(cg, n->sval, (int)n->ival);
        cg_u8(cg, OP_ADDR_GLOBAL);
        cg_u64(cg, a);
        return;
    }
    default:
        diag_error_at(cg->diags, cg->sema->file, n->line, n->column,
                      "cannot take address of %s", node_kind_name(n->kind));
        cg_push_imm(cg, 0);
        return;
    }
}

/* Push an argument value; arrays decay to their address. */
static void gen_arg_value(struct Codegen *cg, struct Node *n)
{
    if (n->type && n->type->kind == TY_ARRAY)
        gen_addr(cg, n);
    else
        gen_expr(cg, n);
}

static void gen_ptr_step(struct Codegen *cg, struct Type *pt, int is_inc)
{
    long long step = pt && pt->of ? ts_size(pt->of) : 8;
    if (step <= 0)
        step = 8;
    if (!is_inc)
        step = -step;
    cg_push_imm(cg, step);
    cg_u8(cg, OP_ADD);
}

static void gen_call(struct Codegen *cg, struct Node *n)
{
    if (n->ival == 1) {
        /* printf */
        struct Node *fmt = n->a;
        gen_arg_value(cg, fmt);
        int nargs = 0;
        for (struct Node *a = fmt->next; a; a = a->next) {
            gen_arg_value(cg, a);
            nargs++;
        }
        cg_u8(cg, OP_SYSCALL_PRINTF);
        cg_u8(cg, (unsigned)nargs);
        return;
    }
    if (n->ival == 2) {
        /* scanf */
        struct Node *fmt = n->a;
        gen_arg_value(cg, fmt);
        int nargs = 0;
        for (struct Node *a = fmt->next; a; a = a->next) {
            struct Node *base = a;
            if (a->kind == NK_UNOP && a->op == TK_AMP)
                base = a->a;
            if (a->kind == NK_UNOP && a->op == TK_AMP)
                gen_addr(cg, a->a);
            else
                gen_expr(cg, a);
            long long maxlen = 4096;
            struct Type *bt = base->type;
            if (bt && bt->kind == TY_ARRAY && bt->of->kind == TY_CHAR &&
                bt->arr_len > 0)
                maxlen = bt->arr_len;
            cg_push_imm(cg, maxlen);
            nargs++;
        }
        cg_u8(cg, OP_SYSCALL_SCANF);
        cg_u8(cg, (unsigned)nargs);
        return;
    }
    struct FuncInfo *fi = sema_find_func(cg->sema, n->sval);
    if (!fi) {
        diag_error_at(cg->diags, cg->sema->file, n->line, n->column,
                      "internal error: unknown function '%s'", n->sval);
        cg_push_imm(cg, 0);
        return;
    }
    /* count 8-byte argument units */
    unsigned nargs_units = 0;
    for (struct Node *a = n->a; a; a = a->next) {
        if (a->type && a->type->kind == TY_STRUCT)
            nargs_units += (unsigned)((ts_size(a->type) + 7) / 8);
        else
            nargs_units += 1;
    }
    cg_u8(cg, OP_RESERVE);
    cg_u32(cg, fi->locals_size);
    for (struct Node *a = n->a; a; a = a->next) {
        if (a->type && a->type->kind == TY_STRUCT) {
            unsigned units = (unsigned)((ts_size(a->type) + 7) / 8);
            gen_addr(cg, a);
            for (unsigned i = 0; i < units; i++) {
                cg_u8(cg, OP_DUP);
                cg_push_imm(cg, (long long)(i * 8));
                cg_u8(cg, OP_ADD);
                cg_u8(cg, OP_LOAD_IND);
                cg_u8(cg, 8);
            }
        } else {
            gen_arg_value(cg, a);
        }
    }
    cg_u8(cg, OP_CALL);
    size_t at = cg->clen;
    cg_u64(cg, 0);
    cg_u8(cg, nargs_units);
    struct CgFixup *fx = (struct CgFixup *)malloc(sizeof *fx);
    if (!fx)
        abort();
    fx->name = fi->name;
    fx->at = at;
    fx->next = cg->fixups;
    cg->fixups = fx;
    cg_u8(cg, OP_RELEASE);
    cg_u32(cg, 16u + 8u * nargs_units + fi->locals_size);
}

static void gen_assign(struct Codegen *cg, struct Node *n)
{
    struct Type *lt = n->a->type;
    int access = ts_access(lt);
    if (lt->kind == TY_STRUCT) {
        /* struct assignment: only valid as a statement */
        gen_addr(cg, n->a);
        gen_addr(cg, n->b);
        cg_u8(cg, OP_MEMCPY);
        cg_u32(cg, (unsigned long long)ts_size(lt));
        return;
    }
    if (n->op != TK_EQ) {
        /* compound assignment: tA holds lhs address; compute old OP rhs */
        gen_addr(cg, n->a);
                        cg_store_temp(cg, 0);
                        cg_load_temp(cg, 0);
        cg_u8(cg, OP_LOAD_IND);
        cg_u8(cg, (unsigned)access);
        gen_expr(cg, n->b);
        int is_ptr = ts_is_ptr(lt);
        long long esz = is_ptr ? ts_size(lt->of) : 0;
        switch (n->op) {
        case TK_PLUSEQ:
        case TK_MINUSEQ:
            if (is_ptr) {
                cg_push_imm(cg, esz);
                cg_u8(cg, OP_MUL);
            }
            cg_u8(cg, n->op == TK_PLUSEQ ? OP_ADD : OP_SUB);
            break;
        case TK_STAREQ: cg_u8(cg, OP_MUL); break;
        case TK_SLASHEQ: cg_u8(cg, OP_DIV); break;
        case TK_PERCENTEQ: cg_u8(cg, OP_MOD); break;
        case TK_AMPEQ: cg_u8(cg, OP_AND); break;
        case TK_PIPEEQ: cg_u8(cg, OP_OR); break;
        case TK_CARETEQ: cg_u8(cg, OP_XOR); break;
        case TK_LSHIFTEQ: cg_u8(cg, OP_SHL); break;
        case TK_RSHIFTEQ: cg_u8(cg, OP_SHR); break;
        default: break;
        }
    } else {
        /* simple assignment */
        gen_addr(cg, n->a);
                        cg_store_temp(cg, 0);
        gen_expr(cg, n->b);
    }
    /* stack: [newval]; store to *tA, leaving newval on the stack */
    cg_u8(cg, OP_DUP);
            cg_load_temp(cg, 0);
    cg_u8(cg, OP_XCHG);
    cg_u8(cg, OP_STORE_IND);
    cg_u8(cg, (unsigned)access);
    if (access == 1) {
        /* the assignment expression yields the converted (truncated) value */
        cg_push_imm(cg, 0xFF);
        cg_u8(cg, OP_AND);
    }
}

static void gen_expr(struct Codegen *cg, struct Node *n)
{
    if (!n) {
        cg_push_imm(cg, 0);
        return;
    }
    switch (n->kind) {
    case NK_INT_LIT:
        cg_push_imm(cg, n->ival);
        return;
    case NK_STR_LIT: {
        unsigned long long a = cg_str(cg, n->sval, (int)n->ival);
        cg_u8(cg, OP_ADDR_GLOBAL);
        cg_u64(cg, a);
        return;
    }
    case NK_IDENT: {
        if (n->type && n->type->kind == TY_ARRAY) {
            gen_addr(cg, n); /* decay */
            return;
        }
        gen_addr(cg, n);
        cg_u8(cg, OP_LOAD_IND);
        cg_u8(cg, (unsigned)ts_access(n->type));
        return;
    }
    case NK_BINOP: {
        int op = n->op;
        if (op == TK_AMPAMP || op == TK_PIPEPIPE) {
            int l_false = cg_label(cg);
            int l_true = cg_label(cg);
            int l_end = cg_label(cg);
            gen_expr(cg, n->a);
            if (op == TK_AMPAMP) {
                cg_jmp(cg, OP_JZ, l_false);
                gen_expr(cg, n->b);
                cg_jmp(cg, OP_JZ, l_false);
                cg_jmp(cg, OP_JMP, l_true);
                cg_mark(cg, l_false);
                cg_push_imm(cg, 0);
                cg_jmp(cg, OP_JMP, l_end);
                cg_mark(cg, l_true);
                cg_push_imm(cg, 1);
                cg_mark(cg, l_end);
            } else {
                cg_jmp(cg, OP_JNZ, l_true);
                gen_expr(cg, n->b);
                cg_jmp(cg, OP_JNZ, l_true);
                cg_mark(cg, l_false);
                cg_push_imm(cg, 0);
                cg_jmp(cg, OP_JMP, l_end);
                cg_mark(cg, l_true);
                cg_push_imm(cg, 1);
                cg_mark(cg, l_end);
            }
            return;
        }
        struct Type *lt = cg_etype(n->a);
        struct Type *rt = cg_etype(n->b);
        int lptr = ts_is_ptr(lt);
        int rptr = ts_is_ptr(rt);
        gen_expr(cg, n->a);
        gen_expr(cg, n->b);
        switch (op) {
        case TK_PLUS:
            if (lptr && !rptr) {
                cg_push_imm(cg, ts_size(lt->of));
                cg_u8(cg, OP_MUL);
                cg_u8(cg, OP_ADD);
            } else if (!lptr && rptr) {
                /* a + p: stack [a, p] -> want p + a*esz */
                cg_u8(cg, OP_XCHG);
                cg_push_imm(cg, ts_size(rt->of));
                cg_u8(cg, OP_MUL);
                cg_u8(cg, OP_ADD);
            } else {
                cg_u8(cg, OP_ADD);
            }
            return;
        case TK_MINUS:
            if (lptr && rptr) {
                cg_u8(cg, OP_SUB);
                cg_push_imm(cg, ts_size(lt->of));
                cg_u8(cg, OP_DIV);
            } else if (lptr && !rptr) {
                cg_push_imm(cg, ts_size(lt->of));
                cg_u8(cg, OP_MUL);
                cg_u8(cg, OP_SUB);
            } else {
                cg_u8(cg, OP_SUB);
            }
            return;
        case TK_STAR: cg_u8(cg, OP_MUL); return;
        case TK_SLASH: cg_u8(cg, OP_DIV); return;
        case TK_PERCENT: cg_u8(cg, OP_MOD); return;
        case TK_LSHIFT: cg_u8(cg, OP_SHL); return;
        case TK_RSHIFT: cg_u8(cg, OP_SHR); return;
        case TK_AMP: cg_u8(cg, OP_AND); return;
        case TK_PIPE: cg_u8(cg, OP_OR); return;
        case TK_CARET: cg_u8(cg, OP_XOR); return;
        case TK_EQEQ: cg_u8(cg, OP_CMP_EQ); return;
        case TK_NEQ: cg_u8(cg, OP_CMP_NE); return;
        case TK_LT: cg_u8(cg, OP_CMP_LT); return;
        case TK_LE: cg_u8(cg, OP_CMP_LE); return;
        case TK_GT: cg_u8(cg, OP_CMP_GT); return;
        case TK_GE: cg_u8(cg, OP_CMP_GE); return;
        default:
            diag_error_at(cg->diags, cg->sema->file, n->line, n->column,
                          "internal error: bad binop");
            return;
        }
    }
    case NK_UNOP: {
        switch (n->op) {
        case TK_MINUS:
            gen_expr(cg, n->a);
            cg_u8(cg, OP_NEG);
            return;
        case TK_PLUS:
            gen_expr(cg, n->a);
            return;
        case TK_BANG:
            gen_expr(cg, n->a);
            cg_push_imm(cg, 0);
            cg_u8(cg, OP_CMP_EQ);
            return;
        case TK_TILDE:
            gen_expr(cg, n->a);
            cg_u8(cg, OP_NOT);
            return;
        case TK_STAR:
            gen_expr(cg, n->a);
            cg_u8(cg, OP_LOAD_IND);
            cg_u8(cg, (unsigned)ts_access(n->type));
            return;
        case TK_AMP:
            gen_addr(cg, n->a);
            return;
        case TK_PLUSPLUS:
        case TK_MINUSMINUS: {
            int is_inc = n->op == TK_PLUSPLUS;
            struct Type *ot = n->a->type;
            int access = ts_access(ot);
            gen_addr(cg, n->a);
                                    cg_store_temp(cg, 2); /* tA = address */
                                    cg_load_temp(cg, 2);
            cg_u8(cg, OP_LOAD_IND);
            cg_u8(cg, (unsigned)access); /* [v] */
            if (ts_is_ptr(ot))
                gen_ptr_step(cg, ot, is_inc);
            else {
                cg_push_imm(cg, is_inc ? 1 : -1);
                cg_u8(cg, OP_ADD);
            }
            cg_u8(cg, OP_DUP); /* [v+1, v+1] */
                                    cg_load_temp(cg, 2); /* [v+1, v+1, a] */
            cg_u8(cg, OP_XCHG);          /* [v+1, a, v+1] */
            cg_u8(cg, OP_STORE_IND);
            cg_u8(cg, (unsigned)access); /* [v+1]: prefix yields new value */
            return;
        }
        default:
            return;
        }
    }
    case NK_POSTFIX: {
        int is_inc = n->op == TK_PLUSPLUS;
        struct Type *ot = n->a->type;
        int access = ts_access(ot);
        gen_addr(cg, n->a);
                        cg_store_temp(cg, 2); /* tA = address */
                        cg_load_temp(cg, 2);
        cg_u8(cg, OP_LOAD_IND);
        cg_u8(cg, (unsigned)access); /* [v] */
                        cg_store_temp(cg, 3); /* tV = old value */
                        cg_load_temp(cg, 2); /* [a] */
                        cg_load_temp(cg, 3); /* [a, v] */
        if (ts_is_ptr(ot))
            gen_ptr_step(cg, ot, is_inc);
        else {
            cg_push_imm(cg, is_inc ? 1 : -1);
            cg_u8(cg, OP_ADD);
        }
        cg_u8(cg, OP_STORE_IND); /* mem[a] = v+1 */
        cg_u8(cg, (unsigned)access);
                        cg_load_temp(cg, 3); /* [v]: postfix yields old value */
        return;
    }
    case NK_ASSIGN:
        gen_assign(cg, n);
        return;
    case NK_TERNARY: {
        int l_else = cg_label(cg);
        int l_end = cg_label(cg);
        gen_expr(cg, n->a);
        cg_jmp(cg, OP_JZ, l_else);
        gen_expr(cg, n->b);
        cg_jmp(cg, OP_JMP, l_end);
        cg_mark(cg, l_else);
        gen_expr(cg, n->c);
        cg_mark(cg, l_end);
        return;
    }
    case NK_CAST:
        gen_expr(cg, n->a);
        if (n->type && n->type->kind == TY_CHAR) {
            cg_push_imm(cg, 0xFF);
            cg_u8(cg, OP_AND);
        }
        return;
    case NK_SIZEOF:
        cg_push_imm(cg, n->size_val);
        return;
    case NK_CALL:
        gen_call(cg, n);
        return;
    case NK_INDEX:
        gen_index_addr(cg, n);
        cg_u8(cg, OP_LOAD_IND);
        cg_u8(cg, (unsigned)ts_access(n->type));
        return;
    case NK_MEMBER:
        gen_addr(cg, n);
        cg_u8(cg, OP_LOAD_IND);
        cg_u8(cg, (unsigned)(n->field ? n->field->access : 8));
        return;
    default:
        diag_error_at(cg->diags, cg->sema->file, n->line, n->column,
                      "internal error: cannot generate %s",
                      node_kind_name(n->kind));
        cg_push_imm(cg, 0);
        return;
    }
}

/* ------------------------------------------------------------------ */
/* statement codegen                                                   */
/* ------------------------------------------------------------------ */

struct CaseInfo {
    long long val;
    int label;
    int is_default;
};

static void collect_cases(struct Codegen *cg, struct Node *n,
                          struct CaseInfo **out, int *ncase, int *capcase)
{
    for (; n; n = n->next) {
        if (n->kind == NK_SWITCH)
            continue; /* do not descend into nested switches */
        if (n->kind == NK_CASE) {
            int ok = 0;
            long long v = sema_const_eval(cg->sema, n->a, &ok);
            if (!ok)
                v = 0;
            int lab = cg_label(cg);
            n->ival = lab;
            if (*ncase == *capcase) {
                int ncap = *capcase ? *capcase * 2 : 16;
                struct CaseInfo *ni = (struct CaseInfo *)realloc(
                    *out, (size_t)ncap * sizeof *ni);
                if (!ni)
                    abort();
                *out = ni;
                *capcase = ncap;
            }
            (*out)[*ncase].val = v;
            (*out)[*ncase].label = lab;
            (*out)[*ncase].is_default = 0;
            (*ncase)++;
            continue;
        }
        if (n->kind == NK_DEFAULT) {
            int lab = cg_label(cg);
            n->ival = lab;
            if (*ncase == *capcase) {
                int ncap = *capcase ? *capcase * 2 : 16;
                struct CaseInfo *ni = (struct CaseInfo *)realloc(
                    *out, (size_t)ncap * sizeof *ni);
                if (!ni)
                    abort();
                *out = ni;
                *capcase = ncap;
            }
            (*out)[*ncase].val = 0;
            (*out)[*ncase].label = lab;
            (*out)[*ncase].is_default = 1;
            (*ncase)++;
            continue;
        }
        collect_cases(cg, n->a, out, ncase, capcase);
        collect_cases(cg, n->b, out, ncase, capcase);
        collect_cases(cg, n->c, out, ncase, capcase);
        collect_cases(cg, n->d, out, ncase, capcase);
    }
}

static void cg_local_init(struct Codegen *cg, struct Node *vd)
{
    struct Sym *sym = vd->sym;
    struct Node *init = vd->a;
    if (!sym || !init)
        return;
    struct Type *t = sym->type;
    if (t->kind == TY_ARRAY || t->kind == TY_STRUCT) {
        long long sz = ts_size(t);
        if (sz <= 0)
            return;
        unsigned char *buf = (unsigned char *)calloc(1, (size_t)sz);
        if (!buf)
            abort();
        flatten_buf(cg, t, init, buf);
        unsigned long long la = cg_bytes(cg, buf, (size_t)sz);
        free(buf);
        cg_u8(cg, OP_ADDR_LOCAL);
        cg_u32(cg, sym->offset);
        cg_u8(cg, OP_ADDR_GLOBAL);
        cg_u64(cg, la);
        cg_u8(cg, OP_MEMCPY);
        cg_u32(cg, (unsigned long long)sz);
        return;
    }
    gen_expr(cg, init);
    if (sym->is_global) {
        cg_u8(cg, OP_STORE_GLOBAL);
        cg_u64(cg, sym->addr);
        cg_u8(cg, (unsigned)ts_access(t));
    } else {
        cg_u8(cg, OP_STORE_LOCAL);
        cg_u32(cg, sym->offset);
        cg_u8(cg, (unsigned)ts_access(t));
    }
}

static void gen_stmt(struct Codegen *cg, struct Node *n)
{
    if (!n)
        return;
    switch (n->kind) {
    case NK_BLOCK:
        for (struct Node *st = n->a; st; st = st->next)
            gen_stmt(cg, st);
        return;
    case NK_VAR_DECL:
        if (n->sym && !n->sym->is_global)
            cg_local_init(cg, n);
        return;
    case NK_EXPR_STMT:
        if (n->a) {
            gen_expr(cg, n->a);
            if (!n->a->type || n->a->type->kind != TY_STRUCT)
                cg_u8(cg, OP_POP);
        }
        return;
    case NK_IF: {
        int l_else = cg_label(cg);
        int l_end = cg_label(cg);
        gen_expr(cg, n->a);
        cg_jmp(cg, OP_JZ, l_else);
        gen_stmt(cg, n->b);
        if (n->c) {
            cg_jmp(cg, OP_JMP, l_end);
            cg_mark(cg, l_else);
            gen_stmt(cg, n->c);
            cg_mark(cg, l_end);
        } else {
            cg_mark(cg, l_else);
        }
        return;
    }
    case NK_WHILE: {
        int l_cond = cg_label(cg);
        int l_end = cg_label(cg);
        cg_mark(cg, l_cond);
        gen_expr(cg, n->a);
        cg_jmp(cg, OP_JZ, l_end);
        cg_push_brk(cg, l_end);
        cg_push_cont(cg, l_cond);
        gen_stmt(cg, n->b);
        cg->nbreak--;
        cg->ncont--;
        cg_jmp(cg, OP_JMP, l_cond);
        cg_mark(cg, l_end);
        return;
    }
    case NK_DO: {
        int l_body = cg_label(cg);
        int l_cond = cg_label(cg);
        int l_end = cg_label(cg);
        cg_mark(cg, l_body);
        cg_push_brk(cg, l_end);
        cg_push_cont(cg, l_cond);
        gen_stmt(cg, n->a);
        cg->nbreak--;
        cg->ncont--;
        cg_mark(cg, l_cond);
        gen_expr(cg, n->b);
        cg_jmp(cg, OP_JNZ, l_body);
        cg_mark(cg, l_end);
        return;
    }
    case NK_FOR: {
        for (struct Node *d = n->a; d; d = d->next)
            gen_stmt(cg, d);
        int l_cond = cg_label(cg);
        int l_cont = cg_label(cg);
        int l_end = cg_label(cg);
        cg_mark(cg, l_cond);
        if (n->b) {
            gen_expr(cg, n->b);
            cg_jmp(cg, OP_JZ, l_end);
        }
        cg_push_brk(cg, l_end);
        cg_push_cont(cg, l_cont);
        gen_stmt(cg, n->d);
        cg->nbreak--;
        cg->ncont--;
        cg_mark(cg, l_cont);
        if (n->c) {
            gen_expr(cg, n->c);
            cg_u8(cg, OP_POP);
        }
        cg_jmp(cg, OP_JMP, l_cond);
        cg_mark(cg, l_end);
        return;
    }
    case NK_SWITCH: {
        struct CaseInfo *cases = NULL;
        int ncase = 0, capcase = 0;
        collect_cases(cg, n->b, &cases, &ncase, &capcase);
        int l_end = cg_label(cg);
        int l_default = -1;
        gen_expr(cg, n->a);
                        cg_store_temp(cg, 0);
        for (int i = 0; i < ncase; i++) {
            if (cases[i].is_default) {
                l_default = cases[i].label;
                continue;
            }
            cg_u8(cg, OP_ADDR_LOCAL);
            cg_u32(cg, temp_off(cg, 0));
            cg_u8(cg, OP_LOAD_IND);
            cg_u8(cg, 8);
            cg_push_imm(cg, cases[i].val);
            cg_u8(cg, OP_CMP_EQ);
            cg_jmp(cg, OP_JNZ, cases[i].label);
        }
        if (l_default >= 0)
            cg_jmp(cg, OP_JMP, l_default);
        else
            cg_jmp(cg, OP_JMP, l_end);
        free(cases);
        cg_push_brk(cg, l_end);
        gen_stmt(cg, n->b);
        cg->nbreak--;
        cg_mark(cg, l_end);
        return;
    }
    case NK_CASE:
    case NK_DEFAULT:
        cg_mark(cg, (int)n->ival);
        return;
    case NK_BREAK:
        if (cg->nbreak > 0)
            cg_jmp(cg, OP_JMP, cg->bstack[cg->nbreak - 1]);
        return;
    case NK_CONTINUE:
        if (cg->ncont > 0)
            cg_jmp(cg, OP_JMP, cg->cstack[cg->ncont - 1]);
        return;
    case NK_RETURN:
        if (n->a)
            gen_expr(cg, n->a);
        else
            cg_push_imm(cg, 0);
        cg_u8(cg, OP_RET);
        return;
    case NK_SWAP:
        gen_addr(cg, n->a);
        gen_addr(cg, n->b);
        cg_u8(cg, OP_SWAP);
        cg_u8(cg, (unsigned)n->ival);
        return;
    default:
        diag_error_at(cg->diags, cg->sema->file, n->line, n->column,
                      "internal error: cannot generate statement %s",
                      node_kind_name(n->kind));
        return;
    }
}

/* ------------------------------------------------------------------ */
/* functions + program                                                 */
/* ------------------------------------------------------------------ */

static void cg_function(struct Codegen *cg, struct FuncInfo *fi)
{
    fi->code_addr = cg->clen;
    cg->sema->cur_func = fi;
    cg_labels_reset(cg);
    cg_u8(cg, OP_ENTER);
    cg_u32(cg, fi->locals_size);
    gen_stmt(cg, fi->def->b);
    cg_push_imm(cg, 0);
    cg_u8(cg, OP_RET);
    cg_resolve(cg);
    cg->sema->cur_func = NULL;
}

void cg_init(struct Codegen *cg, struct Sema *sema, struct DiagList *diags)
{
    memset(cg, 0, sizeof *cg);
    cg->sema = sema;
    cg->diags = diags;
}

int cg_program(struct Codegen *cg, struct Node *prog)
{
    struct Sema *s = cg->sema;
    /* globals image: pre-size to sema's globals_end, zeroed */
    size_t gn = (size_t)s->globals_end;
    cg->gimg = (unsigned char *)calloc(gn ? gn : 1, 1);
    if (!cg->gimg)
        abort();
    cg->gcap = gn ? gn : 1;
    cg->glen = gn;
    for (struct Node *e = prog; e; e = e->next) {
        if (e->kind == NK_VAR_DECL && e->sym && e->sym->is_global)
            cg_emit_global_init(cg, e->sym, e->a);
    }

    struct FuncInfo *main = sema_find_func(s, "main");
    if (!main || !main->def) {
        diag_error_at(cg->diags, s->file, 1, 1,
                      "undefined reference to 'main'");
        return 0;
    }
    /* startup: RESERVE main-locals; CALL main,0; RELEASE; HALT */
    cg_u8(cg, OP_RESERVE);
    cg_u32(cg, main->locals_size);
    cg_u8(cg, OP_CALL);
    size_t at = cg->clen;
    cg_u64(cg, 0);
    cg_u8(cg, 0);
    struct CgFixup *fx = (struct CgFixup *)malloc(sizeof *fx);
    if (!fx)
        abort();
    fx->name = main->name;
    fx->at = at;
    fx->next = cg->fixups;
    cg->fixups = fx;
    cg_u8(cg, OP_RELEASE);
    cg_u32(cg, 16u + main->locals_size);
    cg_u8(cg, OP_HALT);

    for (int i = 0; i < s->nfuncs; i++) {
        if (s->funcs[i].def)
            cg_function(cg, &s->funcs[i]);
    }

    /* resolve CALL fixups */
    for (struct CgFixup *f = cg->fixups; f; f = f->next) {
        struct FuncInfo *fi = sema_find_func(s, f->name);
        if (!fi || !fi->def) {
            diag_error_at(cg->diags, s->file, 1, 1,
                          "undefined reference to '%s'", f->name);
            return 0;
        }
        unsigned long long v = fi->code_addr;
        for (int b = 0; b < 8; b++) {
            cg->code[f->at + (size_t)b] = (unsigned char)(v & 0xFF);
            v >>= 8;
        }
    }
    return 1;
}

void cg_free(struct Codegen *cg)
{
    free(cg->code);
    free(cg->gimg);
    struct CgFixup *f = cg->fixups;
    while (f) {
        struct CgFixup *nx = f->next;
        free(f);
        f = nx;
    }
    free(cg->label_pos);
    free(cg->jmpfix);
    free(cg->bstack);
    free(cg->cstack);
    for (int i = 0; i < cg->nstrs; i++)
        free(cg->strs[i].bytes);
    free(cg->strs);
}
