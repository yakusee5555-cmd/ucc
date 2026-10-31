#define _POSIX_C_SOURCE 200809L
#include "pp.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* small string buffer                                                 */
/* ------------------------------------------------------------------ */

struct StrBuf {
    char *b;
    size_t len;
    size_t cap;
};

static void sb_init(struct StrBuf *s)
{
    s->b = NULL;
    s->len = 0;
    s->cap = 0;
}

static void sb_reserve(struct StrBuf *s, size_t extra)
{
    if (s->len + extra + 1 > s->cap) {
        size_t ncap = s->cap ? s->cap : 64;
        while (ncap < s->len + extra + 1)
            ncap *= 2;
        char *nb = (char *)realloc(s->b, ncap);
        if (!nb)
            abort();
        s->b = nb;
        s->cap = ncap;
    }
}

static void sb_putc(struct StrBuf *s, char c)
{
    sb_reserve(s, 1);
    s->b[s->len++] = c;
}

static void sb_putn(struct StrBuf *s, const char *p, size_t n)
{
    sb_reserve(s, n);
    memcpy(s->b + s->len, p, n);
    s->len += n;
}

static void sb_puts(struct StrBuf *s, const char *p)
{
    sb_putn(s, p, strlen(p));
}

static char *sb_finish(struct StrBuf *s)
{
    sb_reserve(s, 1);
    s->b[s->len] = '\0';
    return s->b;
}

/* ------------------------------------------------------------------ */
/* preprocessor state                                                  */
/* ------------------------------------------------------------------ */

struct Macro {
    char *name;
    int is_func;
    char **params;
    int nparams;
    char *body;
    struct Macro *next;
};

struct CondFrame {
    int parent_active;
    int taken;
    int active;
    int else_seen;
};

struct PP {
    struct DiagList *diags;
    struct Macro *macros;
    struct CondFrame *conds;
    int nconds;
    int capconds;
    int depth;
};

static int is_ident_start(int c)
{
    return isalpha(c) || c == '_';
}

static int is_ident_char(int c)
{
    return isalnum(c) || c == '_';
}

/* ------------------------------------------------------------------ */
/* virtual standard headers                                            */
/* ------------------------------------------------------------------ */

static const char *virtual_header(const char *name)
{
    if (strcmp(name, "stdio.h") == 0)
        return "/* ucc builtin: printf/scanf are compiler intrinsics */\n";
    if (strcmp(name, "stdlib.h") == 0)
        return "/* ucc builtin */\n";
    if (strcmp(name, "string.h") == 0)
        return "/* ucc builtin */\n";
    if (strcmp(name, "stdbool.h") == 0)
        return "#define bool int\n#define true 1\n#define false 0\n";
    if (strcmp(name, "stddef.h") == 0)
        return "#define NULL 0\n#define size_t int\n";
    return NULL;
}

static char *read_file(const char *path, struct DiagList *diags,
                       const char *ref_file, int line)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        diag_error_at(diags, ref_file, line, 1, "cannot open '%s'", path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0)
        n = 0;
    char *buf = (char *)malloc((size_t)n + 1);
    if (!buf)
        abort();
    size_t got = fread(buf, 1, (size_t)n, f);
    buf[got] = '\0';
    fclose(f);
    return buf;
}

/* ------------------------------------------------------------------ */
/* macro expansion                                                     */
/* ------------------------------------------------------------------ */

static struct Macro *find_macro(struct PP *pp, const char *name, size_t nlen)
{
    for (struct Macro *m = pp->macros; m; m = m->next) {
        if (strlen(m->name) == nlen && memcmp(m->name, name, nlen) == 0)
            return m;
    }
    return NULL;
}

static int is_disabled(const char **dis, int ndis, const char *name, size_t nlen)
{
    for (int i = 0; i < ndis; i++) {
        if (strlen(dis[i]) == nlen && memcmp(dis[i], name, nlen) == 0)
            return 1;
    }
    return 0;
}

static char *pp_expand(struct PP *pp, const char *s, const char **dis,
                       int ndis);

/* Substitute function-macro parameters (identifier-aware) then rescan. */
static char *apply_func_macro(struct PP *pp, struct Macro *m, char **args,
                              const char **dis, int ndis)
{
    struct StrBuf out;
    sb_init(&out);
    const char *p = m->body;
    while (*p) {
        if (*p == '"' || *p == '\'') {
            char q = *p;
            sb_putc(&out, *p++);
            while (*p && *p != q) {
                if (*p == '\\' && p[1]) {
                    sb_putc(&out, *p++);
                    sb_putc(&out, *p++);
                } else {
                    sb_putc(&out, *p++);
                }
            }
            if (*p)
                sb_putc(&out, *p++);
        } else if (is_ident_start((unsigned char)*p)) {
            const char *st = p;
            while (is_ident_char((unsigned char)*p))
                p++;
            size_t nl = (size_t)(p - st);
            int pi = -1;
            for (int i = 0; i < m->nparams; i++) {
                if (strlen(m->params[i]) == nl &&
                    memcmp(m->params[i], st, nl) == 0) {
                    pi = i;
                    break;
                }
            }
            if (pi >= 0)
                sb_puts(&out, args[pi]);
            else
                sb_putn(&out, st, nl);
        } else {
            sb_putc(&out, *p++);
        }
    }
    char *sub = sb_finish(&out);
    const char **ndis2 = (const char **)malloc((size_t)(ndis + 1) *
                                              sizeof *ndis2);
    if (!ndis2)
        abort();
    memcpy(ndis2, dis, (size_t)ndis * sizeof *ndis2);
    ndis2[ndis] = m->name;
    char *res = pp_expand(pp, sub, ndis2, ndis + 1);
    free(ndis2);
    free(sub);
    return res;
}

/* Parse a parenthesized argument list at *pp (which points at '(').
 * Returns malloc'd arg strings; *pp advanced past ')'. */
static char **parse_macro_args(const char *pp_, int nparams, const char **endp)
{
    const char *p = pp_;
    char **args = (char **)calloc(nparams > 0 ? nparams : 1, sizeof *args);
    if (!args)
        abort();
    int ai = 0;
    p++; /* '(' */
    struct StrBuf cur;
    sb_init(&cur);
    int depth = 0;
    for (;;) {
        char c = *p;
        if (c == '\0')
            break; /* unterminated; caller tolerates */
        if (c == '"' || c == '\'') {
            char q = c;
            sb_putc(&cur, *p++);
            while (*p && *p != q) {
                if (*p == '\\' && p[1]) {
                    sb_putc(&cur, *p++);
                    sb_putc(&cur, *p++);
                } else {
                    sb_putc(&cur, *p++);
                }
            }
            if (*p)
                sb_putc(&cur, *p++);
            continue;
        }
        if (c == '(')
            depth++;
        else if (c == ')') {
            if (depth == 0) {
                p++;
                break;
            }
            depth--;
        } else if (c == ',' && depth == 0) {
            if (ai < nparams)
                args[ai++] = sb_finish(&cur);
            else
                free(sb_finish(&cur));
            sb_init(&cur);
            p++;
            continue;
        }
        sb_putc(&cur, c);
        p++;
    }
    if (ai < nparams)
        args[ai++] = sb_finish(&cur);
    else
        free(sb_finish(&cur));
    while (ai < nparams)
        args[ai++] = strdup("");
    *endp = p;
    return args;
}

static char *pp_expand(struct PP *pp, const char *s, const char **dis,
                       int ndis)
{
    struct StrBuf out;
    sb_init(&out);
    const char *p = s;
    while (*p) {
        unsigned char c = (unsigned char)*p;
        if (c == '"' || c == '\'') {
            char q = *p;
            sb_putc(&out, *p++);
            while (*p && *p != q) {
                if (*p == '\\' && p[1]) {
                    sb_putc(&out, *p++);
                    sb_putc(&out, *p++);
                } else {
                    sb_putc(&out, *p++);
                }
            }
            if (*p)
                sb_putc(&out, *p++);
        } else if (is_ident_start(c)) {
            const char *st = p;
            while (is_ident_char((unsigned char)*p))
                p++;
            size_t nl = (size_t)(p - st);
            struct Macro *m = find_macro(pp, st, nl);
            if (m && !is_disabled(dis, ndis, st, nl)) {
                if (m->is_func) {
                    const char *q = p;
                    while (*q == ' ' || *q == '\t')
                        q++;
                    if (*q == '(') {
                        const char *endp;
                        char **args =
                            parse_macro_args(q, m->nparams, &endp);
                        for (int i = 0; i < m->nparams; i++) {
                            char *ea = pp_expand(pp, args[i], dis, ndis);
                            free(args[i]);
                            args[i] = ea;
                        }
                        char *res =
                            apply_func_macro(pp, m, args, dis, ndis);
                        sb_puts(&out, res);
                        free(res);
                        for (int i = 0; i < m->nparams; i++)
                            free(args[i]);
                        free(args);
                        p = endp;
                    } else {
                        sb_putn(&out, st, nl);
                    }
                } else {
                    const char **ndis2 =
                        (const char **)malloc((size_t)(ndis + 1) *
                                              sizeof *ndis2);
                    if (!ndis2)
                        abort();
                    memcpy(ndis2, dis, (size_t)ndis * sizeof *ndis2);
                    ndis2[ndis] = m->name;
                    char *e = pp_expand(pp, m->body, ndis2, ndis + 1);
                    sb_puts(&out, e);
                    free(e);
                    free(ndis2);
                }
            } else {
                sb_putn(&out, st, nl);
            }
        } else {
            sb_putc(&out, *p++);
        }
    }
    return sb_finish(&out);
}

/* ------------------------------------------------------------------ */
/* #if integer constant expression evaluator                           */
/* ------------------------------------------------------------------ */

struct CExpr {
    const char *p;
    struct PP *pp;
    const char *file;
    int line;
};

static void ce_skip(struct CExpr *e)
{
    while (*e->p == ' ' || *e->p == '\t')
        e->p++;
}

static long long ce_expr(struct CExpr *e);

static long long ce_primary(struct CExpr *e)
{
    ce_skip(e);
    if (strncmp(e->p, "defined", 7) == 0 &&
        !is_ident_char((unsigned char)e->p[7])) {
        e->p += 7;
        ce_skip(e);
        int paren = 0;
        if (*e->p == '(') {
            paren = 1;
            e->p++;
            ce_skip(e);
        }
        const char *st = e->p;
        while (is_ident_char((unsigned char)*e->p))
            e->p++;
        size_t nl = (size_t)(e->p - st);
        int def = find_macro(e->pp, st, nl) != NULL;
        ce_skip(e);
        if (paren && *e->p == ')')
            e->p++;
        return def ? 1 : 0;
    }
    if (*e->p == '(') {
        e->p++;
        long long v = ce_expr(e);
        ce_skip(e);
        if (*e->p == ')')
            e->p++;
        return v;
    }
    if (isdigit((unsigned char)*e->p)) {
        char *end;
        long long v = strtoll(e->p, &end, 0);
        e->p = end;
        return v;
    }
    if (*e->p == '\'') {
        /* simple char constant in #if */
        e->p++;
        long long v = (unsigned char)*e->p;
        if (*e->p == '\\') {
            e->p++;
            switch (*e->p) {
            case 'n': v = '\n'; break;
            case 't': v = '\t'; break;
            case '0': v = 0; break;
            default: v = (unsigned char)*e->p; break;
            }
        }
        if (*e->p)
            e->p++;
        if (*e->p == '\'')
            e->p++;
        return v;
    }
    if (is_ident_start((unsigned char)*e->p)) {
        while (is_ident_char((unsigned char)*e->p))
            e->p++;
        return 0; /* undefined identifiers are 0 */
    }
    return 0;
}

static long long ce_unary(struct CExpr *e)
{
    ce_skip(e);
    if (*e->p == '!') {
        e->p++;
        return !ce_unary(e);
    }
    if (*e->p == '~') {
        e->p++;
        return ~ce_unary(e);
    }
    if (*e->p == '-') {
        e->p++;
        return -ce_unary(e);
    }
    if (*e->p == '+') {
        e->p++;
        return ce_unary(e);
    }
    return ce_primary(e);
}

static long long ce_mul(struct CExpr *e)
{
    long long v = ce_unary(e);
    for (;;) {
        ce_skip(e);
        if (*e->p == '*') {
            e->p++;
            v *= ce_unary(e);
        } else if (*e->p == '/') {
            e->p++;
            long long r = ce_unary(e);
            v = r == 0 ? 0 : v / r;
        } else if (*e->p == '%') {
            e->p++;
            long long r = ce_unary(e);
            v = r == 0 ? 0 : v % r;
        } else {
            return v;
        }
    }
}

static long long ce_add(struct CExpr *e)
{
    long long v = ce_mul(e);
    for (;;) {
        ce_skip(e);
        if (*e->p == '+') {
            e->p++;
            v += ce_mul(e);
        } else if (*e->p == '-') {
            e->p++;
            v -= ce_mul(e);
        } else {
            return v;
        }
    }
}

static long long ce_shift(struct CExpr *e)
{
    long long v = ce_add(e);
    for (;;) {
        ce_skip(e);
        if (strncmp(e->p, "<<", 2) == 0) {
            e->p += 2;
            v <<= ce_add(e) & 63;
        } else if (strncmp(e->p, ">>", 2) == 0) {
            e->p += 2;
            v >>= ce_add(e) & 63;
        } else {
            return v;
        }
    }
}

static long long ce_rel(struct CExpr *e)
{
    long long v = ce_shift(e);
    for (;;) {
        ce_skip(e);
        if (strncmp(e->p, "<=", 2) == 0) {
            e->p += 2;
            v = v <= ce_shift(e);
        } else if (strncmp(e->p, ">=", 2) == 0) {
            e->p += 2;
            v = v >= ce_shift(e);
        } else if (*e->p == '<') {
            e->p++;
            v = v < ce_shift(e);
        } else if (*e->p == '>') {
            e->p++;
            v = v > ce_shift(e);
        } else {
            return v;
        }
    }
}

static long long ce_eq(struct CExpr *e)
{
    long long v = ce_rel(e);
    for (;;) {
        ce_skip(e);
        if (strncmp(e->p, "==", 2) == 0) {
            e->p += 2;
            v = v == ce_rel(e);
        } else if (strncmp(e->p, "!=", 2) == 0) {
            e->p += 2;
            v = v != ce_rel(e);
        } else {
            return v;
        }
    }
}

static long long ce_band(struct CExpr *e)
{
    long long v = ce_eq(e);
    for (;;) {
        ce_skip(e);
        if (*e->p == '&' && e->p[1] != '&') {
            e->p++;
            v &= ce_eq(e);
        } else {
            return v;
        }
    }
}

static long long ce_xor(struct CExpr *e)
{
    long long v = ce_band(e);
    for (;;) {
        ce_skip(e);
        if (*e->p == '^') {
            e->p++;
            v ^= ce_band(e);
        } else {
            return v;
        }
    }
}

static long long ce_bor(struct CExpr *e)
{
    long long v = ce_xor(e);
    for (;;) {
        ce_skip(e);
        if (*e->p == '|' && e->p[1] != '|') {
            e->p++;
            v |= ce_xor(e);
        } else {
            return v;
        }
    }
}

static long long ce_and(struct CExpr *e)
{
    long long v = ce_bor(e);
    for (;;) {
        ce_skip(e);
        if (strncmp(e->p, "&&", 2) == 0) {
            e->p += 2;
            long long r = ce_bor(e);
            v = (v && r) ? 1 : 0;
        } else {
            return v;
        }
    }
}

static long long ce_expr(struct CExpr *e)
{
    long long v = ce_and(e);
    for (;;) {
        ce_skip(e);
        if (strncmp(e->p, "||", 2) == 0) {
            e->p += 2;
            long long r = ce_and(e);
            v = (v || r) ? 1 : 0;
        } else {
            return v;
        }
    }
}

/* ------------------------------------------------------------------ */
/* comment stripping + line continuation                               */
/* ------------------------------------------------------------------ */

static char *strip_comments(const char *src)
{
    struct StrBuf out;
    sb_init(&out);
    const char *p = src;
    while (*p) {
        if (*p == '"' || *p == '\'') {
            char q = *p;
            sb_putc(&out, *p++);
            while (*p && *p != q) {
                if (*p == '\\' && p[1]) {
                    sb_putc(&out, *p++);
                    sb_putc(&out, *p++);
                } else {
                    sb_putc(&out, *p++);
                }
            }
            if (*p)
                sb_putc(&out, *p++);
        } else if (*p == '/' && p[1] == '/') {
            while (*p && *p != '\n')
                p++;
        } else if (*p == '/' && p[1] == '*') {
            p += 2;
            while (*p && !(*p == '*' && p[1] == '/')) {
                if (*p == '\n')
                    sb_putc(&out, '\n');
                p++;
            }
            if (*p)
                p += 2;
        } else {
            sb_putc(&out, *p++);
        }
    }
    return sb_finish(&out);
}

static char *join_continuations(const char *src)
{
    struct StrBuf out;
    sb_init(&out);
    const char *p = src;
    while (*p) {
        if (*p == '\\' && p[1] == '\n') {
            p += 2;
        } else if (*p == '\\' && p[1] == '\r' && p[2] == '\n') {
            p += 3;
        } else {
            sb_putc(&out, *p++);
        }
    }
    return sb_finish(&out);
}

/* ------------------------------------------------------------------ */
/* directive processing                                                */
/* ------------------------------------------------------------------ */

static void cond_push(struct PP *pp, int cond_true)
{
    if (pp->nconds == pp->capconds) {
        int ncap = pp->capconds ? pp->capconds * 2 : 8;
        struct CondFrame *nc = (struct CondFrame *)realloc(
            pp->conds, (size_t)ncap * sizeof *nc);
        if (!nc)
            abort();
        pp->conds = nc;
        pp->capconds = ncap;
    }
    int parent = pp->nconds == 0 ? 1 : pp->conds[pp->nconds - 1].active;
    struct CondFrame *f = &pp->conds[pp->nconds++];
    f->parent_active = parent;
    f->taken = parent && cond_true;
    f->active = parent && cond_true;
    f->else_seen = 0;
}

static int pp_active(struct PP *pp)
{
    if (pp->nconds == 0)
        return 1;
    return pp->conds[pp->nconds - 1].active;
}

static void define_macro(struct PP *pp, const char *name, int is_func,
                         char **params, int nparams, const char *body,
                         const char *file, int line)
{
    /* replace existing */
    struct Macro **mp = &pp->macros;
    while (*mp) {
        if (strcmp((*mp)->name, name) == 0) {
            struct Macro *old = *mp;
            *mp = old->next;
            for (int i = 0; i < old->nparams; i++)
                free(old->params[i]);
            free(old->params);
            free(old->name);
            free(old->body);
            free(old);
            break;
        }
        mp = &(*mp)->next;
    }
    (void)file;
    (void)line;
    struct Macro *m = (struct Macro *)malloc(sizeof *m);
    if (!m)
        abort();
    m->name = strdup(name);
    m->is_func = is_func;
    m->params = params;
    m->nparams = nparams;
    m->body = strdup(body);
    m->next = pp->macros;
    pp->macros = m;
}

static void undef_macro(struct PP *pp, const char *name)
{
    struct Macro **mp = &pp->macros;
    while (*mp) {
        if (strcmp((*mp)->name, name) == 0) {
            struct Macro *old = *mp;
            *mp = old->next;
            for (int i = 0; i < old->nparams; i++)
                free(old->params[i]);
            free(old->params);
            free(old->name);
            free(old->body);
            free(old);
            return;
        }
        mp = &(*mp)->next;
    }
}

/* forward */
static char *pp_process_inner(struct PP *pp, const char *filename,
                              const char *src);

static void handle_include(struct PP *pp, const char *rest, const char *file,
                           int line, struct StrBuf *out)
{
    while (*rest == ' ' || *rest == '\t')
        rest++;
    char *exp = pp_expand(pp, rest, NULL, 0);
    const char *e = exp;
    while (*e == ' ' || *e == '\t')
        e++;
    if (*e == '<') {
        e++;
        const char *st = e;
        while (*e && *e != '>')
            e++;
        char *name = (char *)malloc((size_t)(e - st) + 1);
        memcpy(name, st, (size_t)(e - st));
        name[e - st] = '\0';
        const char *body = virtual_header(name);
        if (!body) {
            diag_error_at(pp->diags, file, line, 1,
                          "unknown system header <%s>", name);
        } else if (pp->depth < 16) {
            pp->depth++;
            char *r = pp_process_inner(pp, file, body);
            sb_puts(out, r);
            free(r);
            pp->depth--;
        }
        free(name);
    } else if (*e == '"') {
        e++;
        const char *st = e;
        while (*e && *e != '"')
            e++;
        size_t nl = (size_t)(e - st);
        char *inc = (char *)malloc(nl + 1);
        memcpy(inc, st, nl);
        inc[nl] = '\0';
        /* resolve relative to including file's directory */
        char *dir = strdup(file);
        char *slash = strrchr(dir, '/');
#if defined(_WIN32)
        char *bs = strrchr(dir, '\\');
        if (bs && (!slash || bs > slash))
            slash = bs;
#endif
        char *path;
        if (slash) {
            slash[1] = '\0';
            path = (char *)malloc(strlen(dir) + nl + 1);
            strcpy(path, dir);
            strcat(path, inc);
        } else {
            path = strdup(inc);
        }
        free(dir);
        if (pp->depth >= 16) {
            diag_error_at(pp->diags, file, line, 1,
                          "#include nested too deeply");
        } else {
            char *content = read_file(path, pp->diags, file, line);
            if (content) {
                pp->depth++;
                char *r = pp_process_inner(pp, path, content);
                sb_puts(out, r);
                free(r);
                pp->depth--;
                free(content);
            }
        }
        free(path);
        free(inc);
    } else {
        diag_error_at(pp->diags, file, line, 1,
                      "malformed #include directive");
    }
    free(exp);
}

static void handle_define(struct PP *pp, const char *rest, const char *file,
                          int line)
{
    while (*rest == ' ' || *rest == '\t')
        rest++;
    const char *st = rest;
    if (!is_ident_start((unsigned char)*rest)) {
        diag_error_at(pp->diags, file, line, 1,
                      "expected macro name after #define");
        return;
    }
    while (is_ident_char((unsigned char)*rest))
        rest++;
    size_t nl = (size_t)(rest - st);
    char *name = (char *)malloc(nl + 1);
    memcpy(name, st, nl);
    name[nl] = '\0';
    int is_func = 0;
    char **params = NULL;
    int nparams = 0;
    if (*rest == '(') {
        /* function-like: '(' must immediately follow the name */
        is_func = 1;
        rest++;
        for (;;) {
            while (*rest == ' ' || *rest == '\t')
                rest++;
            if (*rest == ')') {
                rest++;
                break;
            }
            if (*rest == '\0') {
                diag_error_at(pp->diags, file, line, 1,
                              "unterminated #define parameter list");
                break;
            }
            const char *ps = rest;
            if (!is_ident_start((unsigned char)*rest)) {
                diag_error_at(pp->diags, file, line, 1,
                              "expected parameter name in #define");
                break;
            }
            while (is_ident_char((unsigned char)*rest))
                rest++;
            size_t pl = (size_t)(rest - ps);
            params = (char **)realloc(params,
                                      (size_t)(nparams + 1) * sizeof *params);
            if (!params)
                abort();
            params[nparams] = (char *)malloc(pl + 1);
            memcpy(params[nparams], ps, pl);
            params[nparams][pl] = '\0';
            nparams++;
            while (*rest == ' ' || *rest == '\t')
                rest++;
            if (*rest == ',') {
                rest++;
                continue;
            }
            if (*rest == ')') {
                rest++;
                break;
            }
            diag_error_at(pp->diags, file, line, 1,
                          "expected ',' or ')' in #define");
            break;
        }
    }
    while (*rest == ' ' || *rest == '\t')
        rest++;
    /* trim trailing whitespace of body */
    const char *be = rest + strlen(rest);
    while (be > rest && (be[-1] == ' ' || be[-1] == '\t'))
        be--;
    size_t bl = (size_t)(be - rest);
    char *body = (char *)malloc(bl + 1);
    memcpy(body, rest, bl);
    body[bl] = '\0';
    define_macro(pp, name, is_func, params, nparams, body, file, line);
    free(name);
    free(body);
}

static long long eval_if_expr(struct PP *pp, const char *rest,
                              const char *file, int line)
{
    char *exp = pp_expand(pp, rest, NULL, 0);
    struct CExpr e;
    e.p = exp;
    e.pp = pp;
    e.file = file;
    e.line = line;
    long long v = ce_expr(&e);
    free(exp);
    return v;
}

static char *pp_process_inner(struct PP *pp, const char *filename,
                              const char *src)
{
    char *nocmt = strip_comments(src);
    char *joined = join_continuations(nocmt);
    free(nocmt);

    struct StrBuf out;
    sb_init(&out);

    const char *p = joined;
    int line = 0;
    while (*p) {
        line++;
        const char *le = strchr(p, '\n');
        size_t llen = le ? (size_t)(le - p) : strlen(p);
        char *lbuf = (char *)malloc(llen + 1);
        memcpy(lbuf, p, llen);
        lbuf[llen] = '\0';
        p = le ? le + 1 : p + llen;

        /* directive? */
        const char *q = lbuf;
        while (*q == ' ' || *q == '\t')
            q++;
        if (*q == '#') {
            q++;
            while (*q == ' ' || *q == '\t')
                q++;
            const char *ds = q;
            while (is_ident_char((unsigned char)*q))
                q++;
            size_t dl = (size_t)(q - ds);
            char dname[32];
            if (dl >= sizeof dname)
                dl = sizeof dname - 1;
            memcpy(dname, ds, dl);
            dname[dl] = '\0';
            const char *rest = q;

            int is_cond = strcmp(dname, "if") == 0 ||
                          strcmp(dname, "ifdef") == 0 ||
                          strcmp(dname, "ifndef") == 0 ||
                          strcmp(dname, "elif") == 0 ||
                          strcmp(dname, "else") == 0 ||
                          strcmp(dname, "endif") == 0;

            if (!pp_active(pp) && !is_cond) {
                /* skip silently, keep line number */
            } else if (strcmp(dname, "if") == 0) {
                cond_push(pp, eval_if_expr(pp, rest, filename, line) != 0);
            } else if (strcmp(dname, "ifdef") == 0) {
                while (*rest == ' ' || *rest == '\t')
                    rest++;
                const char *ns = rest;
                while (is_ident_char((unsigned char)*rest))
                    rest++;
                cond_push(pp,
                          find_macro(pp, ns, (size_t)(rest - ns)) != NULL);
            } else if (strcmp(dname, "ifndef") == 0) {
                while (*rest == ' ' || *rest == '\t')
                    rest++;
                const char *ns = rest;
                while (is_ident_char((unsigned char)*rest))
                    rest++;
                cond_push(pp,
                          find_macro(pp, ns, (size_t)(rest - ns)) == NULL);
            } else if (strcmp(dname, "elif") == 0) {
                if (pp->nconds == 0) {
                    diag_error_at(pp->diags, filename, line, 1,
                                  "#elif without #if");
                } else {
                    struct CondFrame *f = &pp->conds[pp->nconds - 1];
                    if (f->else_seen) {
                        diag_error_at(pp->diags, filename, line, 1,
                                      "#elif after #else");
                    } else if (f->taken) {
                        f->active = 0;
                    } else {
                        int c = eval_if_expr(pp, rest, filename, line) != 0;
                        f->taken = f->parent_active && c;
                        f->active = f->parent_active && c;
                    }
                }
            } else if (strcmp(dname, "else") == 0) {
                if (pp->nconds == 0) {
                    diag_error_at(pp->diags, filename, line, 1,
                                  "#else without #if");
                } else {
                    struct CondFrame *f = &pp->conds[pp->nconds - 1];
                    if (f->else_seen) {
                        diag_error_at(pp->diags, filename, line, 1,
                                      "duplicate #else");
                    }
                    f->else_seen = 1;
                    f->active = f->parent_active && !f->taken;
                    f->taken = 1;
                }
            } else if (strcmp(dname, "endif") == 0) {
                if (pp->nconds == 0) {
                    diag_error_at(pp->diags, filename, line, 1,
                                  "#endif without #if");
                } else {
                    pp->nconds--;
                }
            } else if (!pp_active(pp)) {
                /* inactive: ignore other directives */
            } else if (strcmp(dname, "include") == 0) {
                handle_include(pp, rest, filename, line, &out);
            } else if (strcmp(dname, "define") == 0) {
                handle_define(pp, rest, filename, line);
            } else if (strcmp(dname, "undef") == 0) {
                while (*rest == ' ' || *rest == '\t')
                    rest++;
                const char *ns = rest;
                while (is_ident_char((unsigned char)*rest))
                    rest++;
                char *nm = (char *)malloc((size_t)(rest - ns) + 1);
                memcpy(nm, ns, (size_t)(rest - ns));
                nm[rest - ns] = '\0';
                undef_macro(pp, nm);
                free(nm);
            } else if (strcmp(dname, "error") == 0) {
                while (*rest == ' ' || *rest == '\t')
                    rest++;
                diag_error_at(pp->diags, filename, line, 1, "#error %s",
                              rest);
            } else {
                diag_warn_at(pp->diags, filename, line, 1,
                             "unknown preprocessor directive '#%s' ignored",
                             dname);
            }
            sb_putc(&out, '\n');
        } else if (!pp_active(pp)) {
            sb_putc(&out, '\n');
        } else {
            char *exp = pp_expand(pp, lbuf, NULL, 0);
            sb_puts(&out, exp);
            sb_putc(&out, '\n');
            free(exp);
        }
        free(lbuf);
    }

    if (pp->nconds != 0) {
        diag_error_at(pp->diags, filename, line, 1,
                      "unterminated conditional directive");
        pp->nconds = 0;
    }

    free(joined);
    return sb_finish(&out);
}

char *pp_process(const char *filename, const char *src,
                 struct DiagList *diags)
{
    struct PP pp;
    memset(&pp, 0, sizeof pp);
    pp.diags = diags;
    pp.depth = 0;
    char *res = pp_process_inner(&pp, filename, src);
    /* free macros */
    struct Macro *m = pp.macros;
    while (m) {
        struct Macro *nx = m->next;
        for (int i = 0; i < m->nparams; i++)
            free(m->params[i]);
        free(m->params);
        free(m->name);
        free(m->body);
        free(m);
        m = nx;
    }
    free(pp.conds);
    return res;
}
