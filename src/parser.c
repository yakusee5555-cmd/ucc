#include "ast.h"
#include "diag.h"
#include "parser.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Recursive-descent parser with Pratt expression parsing.
 * Panic-mode recovery: on error inside a block / at top level we skip to
 * the next ';' or '}' so one bad declaration cannot cascade, and the
 * parser never segfaults on malformed input. */

struct Parser {
    struct Token *toks;
    int ntoks;
    int pos;
    struct Arena *arena;
    struct DiagList *diags;
    const char *file;
};

struct Declarator {
    char *name;
    struct Type *type;
    struct Node *params; /* NK_VAR_DECL list, for functions */
    int is_func;
    int line;
    int col;
};

/* ------------------------------------------------------------------ */
/* token helpers                                                       */
/* ------------------------------------------------------------------ */

static struct Token *peek_at(struct Parser *p, int k)
{
    int i = p->pos + k;
    if (i >= p->ntoks)
        i = p->ntoks - 1;
    return &p->toks[i];
}

static struct Token *peek(struct Parser *p)
{
    return peek_at(p, 0);
}

static struct Token *advance(struct Parser *p)
{
    struct Token *t = peek(p);
    if (t->kind != TK_EOF)
        p->pos++;
    return t;
}

static int at(struct Parser *p, enum TokenKind k)
{
    return peek(p)->kind == k;
}

static void perr(struct Parser *p, struct Token *t, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    diag_add(p->diags, p->file, t->line, t->column, DIAG_ERROR, "%s", buf);
}

static int expect(struct Parser *p, enum TokenKind k)
{
    struct Token *t = peek(p);
    if (t->kind == k) {
        advance(p);
        return 1;
    }
    perr(p, t, "expected %s but found %s", token_kind_name(k),
         token_kind_name(t->kind));
    return 0;
}

/* ------------------------------------------------------------------ */
/* forward declarations                                                */
/* ------------------------------------------------------------------ */

static struct Node *parse_expr(struct Parser *p, int min_bp);
static struct Node *parse_statement(struct Parser *p);
static struct Node *parse_block(struct Parser *p);
static struct Type *parse_declspec(struct Parser *p);
static struct Declarator parse_declarator(struct Parser *p, struct Type *base,
                                          int allow_abstract);

/* ------------------------------------------------------------------ */
/* expressions (Pratt)                                                 */
/* ------------------------------------------------------------------ */

static int infix_bp(enum TokenKind k)
{
    switch (k) {
    case TK_EQ: case TK_PLUSEQ: case TK_MINUSEQ: case TK_STAREQ:
    case TK_SLASHEQ: case TK_PERCENTEQ: case TK_AMPEQ: case TK_PIPEEQ:
    case TK_CARETEQ: case TK_LSHIFTEQ: case TK_RSHIFTEQ:
        return 10;
    case TK_QUESTION:
        return 20;
    case TK_PIPEPIPE:
        return 30;
    case TK_AMPAMP:
        return 40;
    case TK_PIPE:
        return 50;
    case TK_CARET:
        return 60;
    case TK_AMP:
        return 70;
    case TK_EQEQ: case TK_NEQ:
        return 80;
    case TK_LT: case TK_LE: case TK_GT: case TK_GE:
        return 90;
    case TK_LSHIFT: case TK_RSHIFT:
        return 100;
    case TK_PLUS: case TK_MINUS:
        return 110;
    case TK_STAR: case TK_SLASH: case TK_PERCENT:
        return 120;
    default:
        return -1;
    }
}

static int is_assign_op(enum TokenKind k)
{
    switch (k) {
    case TK_EQ: case TK_PLUSEQ: case TK_MINUSEQ: case TK_STAREQ:
    case TK_SLASHEQ: case TK_PERCENTEQ: case TK_AMPEQ: case TK_PIPEEQ:
    case TK_CARETEQ: case TK_LSHIFTEQ: case TK_RSHIFTEQ:
        return 1;
    default:
        return 0;
    }
}

/* True if tokens at pos form "( type-name )". */
static int is_cast_ahead(struct Parser *p)
{
    int i = p->pos + 1;
    int saw = 0;
    for (;;) {
        if (i >= p->ntoks)
            return 0;
        enum TokenKind k = p->toks[i].kind;
        if (k == TK_KW_CONST || k == TK_KW_STATIC || k == TK_KW_EXTERN) {
            i++;
            continue;
        }
        if (k == TK_KW_INT || k == TK_KW_CHAR || k == TK_KW_VOID ||
            k == TK_KW_FLOAT || k == TK_KW_DOUBLE || k == TK_KW_LONG ||
            k == TK_KW_SHORT || k == TK_KW_UNSIGNED || k == TK_KW_SIGNED) {
            saw = 1;
            i++;
            continue;
        }
        if (k == TK_KW_STRUCT) {
            i++;
            if (i < p->ntoks && p->toks[i].kind == TK_IDENT)
                i++;
            saw = 1;
            continue;
        }
        break;
    }
    if (!saw)
        return 0;
    while (i < p->ntoks && p->toks[i].kind == TK_STAR)
        i++;
    return i < p->ntoks && p->toks[i].kind == TK_RPAREN;
}

static struct Node *parse_postfix(struct Parser *p, struct Node *node)
{
    for (;;) {
        struct Token *t = peek(p);
        if (t->kind == TK_LPAREN) {
            advance(p);
            struct Node *head = NULL, **tail = &head;
            if (!at(p, TK_RPAREN)) {
                for (;;) {
                    struct Node *arg = parse_expr(p, 0);
                    *tail = arg;
                    tail = &arg->next;
                    if (at(p, TK_COMMA)) {
                        advance(p);
                        continue;
                    }
                    break;
                }
            }
            expect(p, TK_RPAREN);
            if (node->kind != NK_IDENT) {
                perr(p, t, "cannot call a non-function expression");
                continue;
            }
            struct Node *call =
                node_new(p->arena, NK_CALL, t->line, t->column);
            call->sval = node->sval;
            call->a = head;
            node = call;
        } else if (t->kind == TK_LBRACKET) {
            advance(p);
            struct Node *idx = parse_expr(p, 0);
            struct Token *rc = peek(p);
            expect(p, TK_RBRACKET);
            struct Node *n = node_new(p->arena, NK_INDEX, t->line, t->column);
            n->a = node;
            n->b = idx;
            (void)rc;
            node = n;
        } else if (t->kind == TK_DOT || t->kind == TK_ARROW) {
            int arrow = t->kind == TK_ARROW;
            advance(p);
            struct Token *f = peek(p);
            if (!expect(p, TK_IDENT))
                return node;
            struct Node *n = node_new(p->arena, NK_MEMBER, t->line, t->column);
            n->a = node;
            n->sval = f->text;
            n->ival = arrow ? 1 : 0;
            node = n;
        } else if (t->kind == TK_PLUSPLUS || t->kind == TK_MINUSMINUS) {
            advance(p);
            struct Node *n =
                node_new(p->arena, NK_POSTFIX, t->line, t->column);
            n->op = t->kind;
            n->a = node;
            node = n;
        } else {
            break;
        }
    }
    return node;
}

static struct Node *parse_sizeof(struct Parser *p, struct Token *t)
{
    struct Node *n = node_new(p->arena, NK_SIZEOF, t->line, t->column);
    if (at(p, TK_LPAREN) && is_cast_ahead(p)) {
        advance(p);
        n->type = parse_declspec(p);
        while (at(p, TK_STAR)) {
            advance(p);
            n->type = type_ptr(p->arena, n->type);
        }
        expect(p, TK_RPAREN);
        n->ival = 1;
    } else {
        n->a = parse_expr(p, 130);
        n->ival = 0;
    }
    return n;
}

static struct Node *parse_prefix(struct Parser *p)
{
    struct Token *t = peek(p);
    struct Node *n;
    switch (t->kind) {
    case TK_INT:
        advance(p);
        n = node_new(p->arena, NK_INT_LIT, t->line, t->column);
        n->ival = t->ival;
        return parse_postfix(p, n);
    case TK_FLOAT:
        advance(p);
        diag_warn_at(p->diags, p->file, t->line, t->column,
                     "floating literal '%s' truncated to integer %lld",
                     t->text, (long long)t->fval);
        n = node_new(p->arena, NK_INT_LIT, t->line, t->column);
        n->ival = (long long)t->fval;
        return parse_postfix(p, n);
    case TK_CHAR:
        advance(p);
        n = node_new(p->arena, NK_INT_LIT, t->line, t->column);
        n->ival = t->ival;
        return parse_postfix(p, n);
    case TK_STR:
        advance(p);
        n = node_new(p->arena, NK_STR_LIT, t->line, t->column);
        n->sval = t->sval;
        n->ival = t->slen;
        return parse_postfix(p, n);
    case TK_IDENT:
        advance(p);
        n = node_new(p->arena, NK_IDENT, t->line, t->column);
        n->sval = t->text;
        return parse_postfix(p, n);
    case TK_LPAREN: {
        if (is_cast_ahead(p)) {
            advance(p);
            struct Type *ty = parse_declspec(p);
            while (at(p, TK_STAR)) {
                advance(p);
                ty = type_ptr(p->arena, ty);
            }
            expect(p, TK_RPAREN);
            struct Node *e = parse_expr(p, 130);
            n = node_new(p->arena, NK_CAST, t->line, t->column);
            n->type = ty;
            n->a = e;
            return n; /* casts don't take postfix in our subset... */
        }
        advance(p);
        struct Node *e = parse_expr(p, 0);
        expect(p, TK_RPAREN);
        return parse_postfix(p, e);
    }
    case TK_MINUS:
    case TK_PLUS:
    case TK_BANG:
    case TK_TILDE:
    case TK_STAR:
    case TK_AMP:
    case TK_PLUSPLUS:
    case TK_MINUSMINUS: {
        advance(p);
        if (t->kind == TK_PLUS) /* unary plus: no-op */
            return parse_expr(p, 130);
        struct Node *e = parse_expr(p, 130);
        n = node_new(p->arena, NK_UNOP, t->line, t->column);
        n->op = t->kind;
        n->a = e;
        return n;
    }
    case TK_KW_SIZEOF:
        advance(p);
        return parse_sizeof(p, t);
    default:
        perr(p, t, "expected expression but found %s",
             token_kind_name(t->kind));
        advance(p);
        n = node_new(p->arena, NK_INT_LIT, t->line, t->column);
        n->ival = 0;
        return n;
    }
}

static struct Node *parse_expr(struct Parser *p, int min_bp)
{
    struct Node *lhs = parse_prefix(p);
    for (;;) {
        struct Token *t = peek(p);
        int bp = infix_bp(t->kind);
        if (bp < min_bp)
            break;
        if (t->kind == TK_QUESTION) {
            advance(p);
            struct Node *then_e = parse_expr(p, 20);
            expect(p, TK_COLON);
            struct Node *else_e = parse_expr(p, 20);
            struct Node *n = node_new(p->arena, NK_TERNARY, t->line, t->column);
            n->a = lhs;
            n->b = then_e;
            n->c = else_e;
            lhs = n;
            continue;
        }
        advance(p);
        int right_assoc = is_assign_op(t->kind);
        struct Node *rhs = parse_expr(p, right_assoc ? bp : bp + 1);
        struct Node *n;
        if (right_assoc) {
            n = node_new(p->arena, NK_ASSIGN, t->line, t->column);
        } else {
            n = node_new(p->arena, NK_BINOP, t->line, t->column);
        }
        n->op = t->kind;
        n->a = lhs;
        n->b = rhs;
        lhs = n;
    }
    return lhs;
}

/* ------------------------------------------------------------------ */
/* declarations                                                        */
/* ------------------------------------------------------------------ */

static struct Type *parse_declspec(struct Parser *p)
{
    struct Type *t = NULL;
    int saw = 0;
    for (;;) {
        struct Token *tok = peek(p);
        switch (tok->kind) {
        case TK_KW_INT:
        case TK_KW_LONG:
        case TK_KW_SHORT:
        case TK_KW_UNSIGNED:
        case TK_KW_SIGNED:
            /* long/short/unsigned/signed are accepted as aliases of int */
            t = type_int(p->arena);
            saw = 1;
            advance(p);
            break;
        case TK_KW_CHAR:
            t = type_char(p->arena);
            saw = 1;
            advance(p);
            break;
        case TK_KW_VOID:
            t = type_void(p->arena);
            saw = 1;
            advance(p);
            break;
        case TK_KW_FLOAT:
        case TK_KW_DOUBLE:
            /* accepted syntactically; stored as 64-bit int */
            t = type_float(p->arena);
            saw = 1;
            advance(p);
            break;
        case TK_KW_STRUCT: {
            advance(p);
            struct Token *tag = peek(p);
            if (!expect(p, TK_IDENT))
                return type_int(p->arena);
            t = type_struct_tag(p->arena, tag->text);
            saw = 1;
            break;
        }
        case TK_KW_CONST:
        case TK_KW_STATIC:
        case TK_KW_EXTERN:
            advance(p); /* accepted, no effect in this subset */
            break;
        case TK_KW_TYPEDEF:
            perr(p, tok, "'typedef' is not supported in this subset");
            advance(p);
            break;
        default:
            goto done;
        }
    }
done:
    if (!saw) {
        perr(p, peek(p), "expected type specifier");
        return type_int(p->arena);
    }
    return t;
}

static struct Node *parse_params(struct Parser *p)
{
    /* '(' already consumed */
    struct Node *head = NULL, **tail = &head;
    if (at(p, TK_RPAREN))
        return NULL;
    if (at(p, TK_KW_VOID) && peek_at(p, 1)->kind == TK_RPAREN) {
        advance(p);
        return NULL;
    }
    for (;;) {
        struct Token *t = peek(p);
        struct Type *bt = parse_declspec(p);
        struct Declarator d = parse_declarator(p, bt, 1);
        struct Node *pd = node_new(p->arena, NK_VAR_DECL, t->line, t->column);
        pd->sval = d.name;
        pd->type = d.type;
        *tail = pd;
        tail = &pd->next;
        if (at(p, TK_COMMA)) {
            advance(p);
            if (at(p, TK_ELLIPSIS)) {
                advance(p); /* varargs: accepted, ignored */
                break;
            }
            continue;
        }
        break;
    }
    return head;
}

static struct Declarator parse_declarator(struct Parser *p, struct Type *base,
                                          int allow_abstract)
{
    struct Declarator d;
    memset(&d, 0, sizeof d);
    struct Token *t = peek(p);
    d.line = t->line;
    d.col = t->column;
    while (at(p, TK_STAR)) {
        advance(p);
        base = type_ptr(p->arena, base);
    }
    if (at(p, TK_IDENT)) {
        d.name = advance(p)->text;
    } else if (at(p, TK_LPAREN)) {
        perr(p, peek(p),
             "parenthesized declarators (e.g. function pointers) are not "
             "supported");
        /* recover: skip balanced parens */
        int depth = 0;
        do {
            if (at(p, TK_LPAREN))
                depth++;
            else if (at(p, TK_RPAREN))
                depth--;
            advance(p);
        } while (depth > 0 && !at(p, TK_EOF));
    } else if (!allow_abstract) {
        perr(p, peek(p), "expected identifier in declaration");
    }
    if (at(p, TK_LPAREN)) {
        /* function declarator */
        advance(p);
        d.params = parse_params(p);
        expect(p, TK_RPAREN);
        d.is_func = 1;
        d.type = base;
        return d;
    }
    long long dims[16];
    struct Node *dim_exprs[16];
    int ndims = 0;
    while (at(p, TK_LBRACKET)) {
        struct Token *lb = advance(p);
        long long len = -1;
        struct Node *len_expr = NULL;
        if (!at(p, TK_RBRACKET)) {
            struct Node *e = parse_expr(p, 0);
            /* constant-fold simple literals here; sema evaluates the rest */
            if (e->kind == NK_INT_LIT) {
                len = e->ival;
            } else {
                len = -2; /* deferred: sema const-evaluates */
                len_expr = e;
            }
        }
        expect(p, TK_RBRACKET);
        if (ndims >= 16) {
            perr(p, lb, "too many array dimensions");
            break;
        }
        dims[ndims] = len;
        dim_exprs[ndims] = len_expr;
        ndims++;
        (void)lb;
    }
    /* Build inside-out in reverse so int m[2][3] is array[2] of array[3]. */
    for (int i = ndims - 1; i >= 0; i--) {
        struct Type *at = type_array(p->arena, base, dims[i]);
        at->arr_len_expr = dim_exprs[i];
        base = at;
    }
    d.type = base;
    return d;
}

static struct Node *parse_initializer(struct Parser *p)
{
    struct Token *t = peek(p);
    if (at(p, TK_LBRACE)) {
        advance(p);
        struct Node *n = node_new(p->arena, NK_INIT_LIST, t->line, t->column);
        struct Node **tail = &n->a;
        if (!at(p, TK_RBRACE)) {
            for (;;) {
                struct Node *e = parse_initializer(p);
                *tail = e;
                tail = &e->next;
                if (at(p, TK_COMMA)) {
                    advance(p);
                    if (at(p, TK_RBRACE))
                        break;
                    continue;
                }
                break;
            }
        }
        expect(p, TK_RBRACE);
        return n;
    }
    return parse_expr(p, 0);
}

/* Parses "type d1 [=init], d2 [=init], ... ;" -> head of NK_VAR_DECL list. */
static struct Node *parse_var_decl_list(struct Parser *p, int is_global)
{
    struct Type *bt = parse_declspec(p);
    struct Node *head = NULL, **tail = &head;
    (void)is_global;
    for (;;) {
        struct Token *t = peek(p);
        struct Declarator d = parse_declarator(p, bt, 0);
        if (d.is_func) {
            perr(p, t, "function declarator not allowed here");
            return head;
        }
        struct Node *vd = node_new(p->arena, NK_VAR_DECL, d.line, d.col);
        vd->sval = d.name;
        vd->type = d.type;
        if (at(p, TK_EQ)) {
            advance(p);
            vd->a = parse_initializer(p);
        }
        *tail = vd;
        tail = &vd->next;
        if (at(p, TK_COMMA)) {
            advance(p);
            continue;
        }
        break;
    }
    expect(p, TK_SEMI);
    return head;
}

/* forward for struct fields */
static struct Node *parse_struct_definition(struct Parser *p)
{
    struct Token *kw = advance(p); /* struct */
    struct Token *tag = peek(p);
    char *tagname = NULL;
    if (at(p, TK_IDENT)) {
        tagname = advance(p)->text;
    } else {
        perr(p, peek(p), "anonymous structs are not supported");
        return NULL;
    }
    expect(p, TK_LBRACE);
    struct Node *fields = NULL, **tail = &fields;
    while (!at(p, TK_RBRACE) && !at(p, TK_EOF)) {
        struct Token *ft = peek(p);
        struct Type *bt = parse_declspec(p);
        for (;;) {
            struct Declarator d = parse_declarator(p, bt, 0);
            if (d.is_func) {
                perr(p, ft, "function member not allowed in struct");
                break;
            }
            if (at(p, TK_COLON)) {
                perr(p, peek(p), "bit-fields are not supported");
                advance(p);
                parse_expr(p, 0);
            }
            struct Node *fd =
                node_new(p->arena, NK_VAR_DECL, d.line, d.col);
            fd->sval = d.name;
            fd->type = d.type;
            *tail = fd;
            tail = &fd->next;
            if (at(p, TK_COMMA)) {
                advance(p);
                continue;
            }
            break;
        }
        expect(p, TK_SEMI);
    }
    expect(p, TK_RBRACE);
    (void)tag;
    struct Node *n = node_new(p->arena, NK_STRUCTDEF, kw->line, kw->column);
    n->sval = tagname;
    n->a = fields;
    return n;
}

/* ------------------------------------------------------------------ */
/* statements                                                          */
/* ------------------------------------------------------------------ */

static struct Node *parse_swap_stmt(struct Parser *p)
{
    struct Token *kw = advance(p); /* swap */
    expect(p, TK_LPAREN);
    struct Node *a = parse_expr(p, 0);
    expect(p, TK_COMMA);
    struct Node *b = parse_expr(p, 0);
    expect(p, TK_RPAREN);
    expect(p, TK_SEMI);
    struct Node *n = node_new(p->arena, NK_SWAP, kw->line, kw->column);
    n->a = a;
    n->b = b;
    return n;
}

static struct Node *parse_statement(struct Parser *p)
{
    struct Token *t = peek(p);
    switch (t->kind) {
    case TK_LBRACE:
        return parse_block(p);
    case TK_KW_IF: {
        advance(p);
        expect(p, TK_LPAREN);
        struct Node *cond = parse_expr(p, 0);
        expect(p, TK_RPAREN);
        struct Node *then_s = parse_statement(p);
        struct Node *else_s = NULL;
        if (at(p, TK_KW_ELSE)) {
            advance(p);
            else_s = parse_statement(p);
        }
        struct Node *n = node_new(p->arena, NK_IF, t->line, t->column);
        n->a = cond;
        n->b = then_s;
        n->c = else_s;
        return n;
    }
    case TK_KW_WHILE: {
        advance(p);
        expect(p, TK_LPAREN);
        struct Node *cond = parse_expr(p, 0);
        expect(p, TK_RPAREN);
        struct Node *body = parse_statement(p);
        struct Node *n = node_new(p->arena, NK_WHILE, t->line, t->column);
        n->a = cond;
        n->b = body;
        return n;
    }
    case TK_KW_DO: {
        advance(p);
        struct Node *body = parse_statement(p);
        expect(p, TK_KW_WHILE);
        expect(p, TK_LPAREN);
        struct Node *cond = parse_expr(p, 0);
        expect(p, TK_RPAREN);
        expect(p, TK_SEMI);
        struct Node *n = node_new(p->arena, NK_DO, t->line, t->column);
        n->a = body;
        n->b = cond;
        return n;
    }
    case TK_KW_FOR: {
        advance(p);
        expect(p, TK_LPAREN);
        struct Node *init = NULL;
        if (at(p, TK_SEMI)) {
            advance(p);
        } else if (token_is_type_start(peek(p)->kind)) {
            init = parse_var_decl_list(p, 0);
        } else {
            struct Node *e = parse_expr(p, 0);
            expect(p, TK_SEMI);
            init = node_new(p->arena, NK_EXPR_STMT, t->line, t->column);
            init->a = e;
        }
        struct Node *cond = NULL;
        if (!at(p, TK_SEMI))
            cond = parse_expr(p, 0);
        expect(p, TK_SEMI);
        struct Node *incr = NULL;
        if (!at(p, TK_RPAREN))
            incr = parse_expr(p, 0);
        expect(p, TK_RPAREN);
        struct Node *body = parse_statement(p);
        struct Node *n = node_new(p->arena, NK_FOR, t->line, t->column);
        n->a = init;
        n->b = cond;
        n->c = incr;
        n->d = body;
        return n;
    }
    case TK_KW_SWITCH: {
        advance(p);
        expect(p, TK_LPAREN);
        struct Node *e = parse_expr(p, 0);
        expect(p, TK_RPAREN);
        struct Node *body = parse_statement(p);
        struct Node *n = node_new(p->arena, NK_SWITCH, t->line, t->column);
        n->a = e;
        n->b = body;
        return n;
    }
    case TK_KW_CASE: {
        advance(p);
        struct Node *e = parse_expr(p, 0);
        expect(p, TK_COLON);
        struct Node *n = node_new(p->arena, NK_CASE, t->line, t->column);
        n->a = e;
        return n;
    }
    case TK_KW_DEFAULT: {
        advance(p);
        expect(p, TK_COLON);
        return node_new(p->arena, NK_DEFAULT, t->line, t->column);
    }
    case TK_KW_BREAK:
        advance(p);
        expect(p, TK_SEMI);
        return node_new(p->arena, NK_BREAK, t->line, t->column);
    case TK_KW_CONTINUE:
        advance(p);
        expect(p, TK_SEMI);
        return node_new(p->arena, NK_CONTINUE, t->line, t->column);
    case TK_KW_RETURN: {
        advance(p);
        struct Node *n = node_new(p->arena, NK_RETURN, t->line, t->column);
        if (!at(p, TK_SEMI))
            n->a = parse_expr(p, 0);
        expect(p, TK_SEMI);
        return n;
    }
    case TK_SEMI:
        advance(p);
        return NULL;
    case TK_IDENT:
        if (strcmp(t->text, "swap") == 0 &&
            peek_at(p, 1)->kind == TK_LPAREN)
            return parse_swap_stmt(p);
        /* fall through */
    default:
        if (token_is_type_start(t->kind))
            return parse_var_decl_list(p, 0);
        {
            struct Node *e = parse_expr(p, 0);
            expect(p, TK_SEMI);
            struct Node *n =
                node_new(p->arena, NK_EXPR_STMT, t->line, t->column);
            n->a = e;
            return n;
        }
    }
}

static struct Node *parse_block(struct Parser *p)
{
    struct Token *t = peek(p);
    expect(p, TK_LBRACE);
    struct Node *blk = node_new(p->arena, NK_BLOCK, t->line, t->column);
    struct Node **tail = &blk->a;
    while (!at(p, TK_RBRACE) && !at(p, TK_EOF)) {
        int had_error = p->diags->has_error;
        struct Node *s = parse_statement(p);
        if (p->diags->has_error > had_error) {
            /* panic-mode: skip to next ';' or '}' */
            while (!at(p, TK_SEMI) && !at(p, TK_RBRACE) &&
                   !at(p, TK_EOF))
                advance(p);
            if (at(p, TK_SEMI))
                advance(p);
            continue;
        }
        if (s) {
            /* splice declaration lists / flatten nested expr stmts */
            struct Node *it = s;
            while (it) {
                struct Node *nx = it->next;
                it->next = NULL;
                *tail = it;
                tail = &it->next;
                it = nx;
            }
        }
    }
    expect(p, TK_RBRACE);
    return blk;
}

/* ------------------------------------------------------------------ */
/* top level                                                           */
/* ------------------------------------------------------------------ */

static struct Node *parse_external(struct Parser *p)
{
    struct Token *t = peek(p);
    if (t->kind == TK_EOF)
        return NULL;
    /* struct definition */
    if (t->kind == TK_KW_STRUCT && peek_at(p, 1)->kind == TK_IDENT &&
        peek_at(p, 2)->kind == TK_LBRACE) {
        struct Node *sd = parse_struct_definition(p);
        expect(p, TK_SEMI);
        return sd;
    }
    struct Type *bt = parse_declspec(p);
    if (at(p, TK_SEMI)) {
        advance(p); /* e.g. "struct S;" or stray "int;" */
        return NULL;
    }
    struct Declarator d = parse_declarator(p, bt, 0);
    if (d.is_func) {
        if (at(p, TK_LBRACE)) {
            struct Node *body = parse_block(p);
            struct Node *fn =
                node_new(p->arena, NK_FUNCDEF, d.line, d.col);
            fn->sval = d.name;
            fn->type = bt;
            fn->a = d.params;
            fn->b = body;
            return fn;
        }
        expect(p, TK_SEMI);
        struct Node *pr = node_new(p->arena, NK_PROTO, d.line, d.col);
        pr->sval = d.name;
        pr->type = bt;
        pr->a = d.params;
        return pr;
    }
    /* global variable(s) */
    struct Node *head = NULL, **tail = &head;
    for (;;) {
        struct Node *vd = node_new(p->arena, NK_VAR_DECL, d.line, d.col);
        vd->sval = d.name;
        vd->type = d.type;
        if (at(p, TK_EQ)) {
            advance(p);
            vd->a = parse_initializer(p);
        }
        *tail = vd;
        tail = &vd->next;
        if (at(p, TK_COMMA)) {
            advance(p);
            d = parse_declarator(p, bt, 0);
            if (d.is_func) {
                perr(p, peek(p), "function declarator not allowed here");
                break;
            }
            continue;
        }
        break;
    }
    expect(p, TK_SEMI);
    return head;
}

static struct Node *parse_program_inner(struct Parser *p, const char *src,
                                        const char *file, struct Arena *arena,
                                        struct DiagList *diags)
{
    p->arena = arena;
    p->diags = diags;
    p->file = file;
    p->pos = 0;
    /* lex everything up front for easy lookahead */
    struct Lexer lx;
    lexer_init(&lx, src, file, diags, arena);
    int cap = 256, n = 0;
    struct Token *arr = (struct Token *)malloc((size_t)cap * sizeof *arr);
    if (!arr)
        abort();
    for (;;) {
        if (n == cap) {
            cap *= 2;
            struct Token *na = (struct Token *)realloc(
                arr, (size_t)cap * sizeof *arr);
            if (!na)
                abort();
            arr = na;
        }
        arr[n] = lexer_next(&lx);
        if (arr[n].kind == TK_EOF) {
            n++;
            break;
        }
        n++;
    }
    p->toks = arr;
    p->ntoks = n;

    struct Node *head = NULL, **tail = &head;
    while (!at(p, TK_EOF)) {
        int had_error = diags->has_error;
        struct Node *e = parse_external(p);
        if (diags->has_error > had_error) {
            /* panic-mode: skip to next ';' or '}' at top level */
            int depth = 0;
            while (!at(p, TK_EOF)) {
                enum TokenKind k = peek(p)->kind;
                if (k == TK_LBRACE)
                    depth++;
                else if (k == TK_RBRACE) {
                    if (depth == 0) {
                        advance(p);
                        break;
                    }
                    depth--;
                } else if (k == TK_SEMI && depth == 0) {
                    advance(p);
                    break;
                }
                advance(p);
            }
            continue;
        }
        struct Node *it = e;
        while (it) {
            struct Node *nx = it->next;
            it->next = NULL;
            *tail = it;
            tail = &it->next;
            it = nx;
        }
    }
    free(arr);
    p->toks = NULL;
    return head;
}

struct Node *parse_source(const char *src, const char *file,
                          struct Arena *arena, struct DiagList *diags)
{
    struct Parser p;
    return parse_program_inner(&p, src, file, arena, diags);
}
