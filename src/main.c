#include "arena.h"
#include "codegen.h"
#include "diag.h"
#include "lexer.h"
#include "parser.h"
#include "pp.h"
#include "sema.h"
#include "vm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(const char *prog)
{
    printf("usage: %s [options] file.c\n", prog);
    printf("options:\n");
    printf("  --dump-tokens       print token stream (line:col KIND 'lexeme')\n");
    printf("  --dump-ast          print AST as JSON\n");
    printf("  --dump-bytecode     print disassembled bytecode\n");
    printf("  --json-diagnostics  print diagnostics as JSON, do not run\n");
    printf("  --max-steps N       instruction step limit (default 10000000)\n");
    printf("  --mem-size MB       VM memory in megabytes (default 8)\n");
    printf("  --help              show this help\n");
}

static char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "error: cannot open '%s'\n", path);
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
/* --dump-tokens                                                       */
/* ------------------------------------------------------------------ */

static void dump_tokens(const char *file, const char *src,
                        struct Arena *arena, struct DiagList *diags)
{
    struct Lexer lx;
    lexer_init(&lx, src, file, diags, arena);
    for (;;) {
        struct Token t = lexer_next(&lx);
        /* lexeme on one line: escape newlines just in case */
        printf("%d:%d %s '", t.line, t.column, token_kind_name(t.kind));
        for (const char *p = t.text; *p; p++) {
            if (*p == '\n')
                printf("\\n");
            else
                putchar(*p);
        }
        printf("'\n");
        if (t.kind == TK_EOF)
            break;
    }
}

/* ------------------------------------------------------------------ */
/* --dump-ast (JSON)                                                   */
/* ------------------------------------------------------------------ */

static void jstr(const char *s)
{
    putchar('"');
    if (s) {
        for (; *s; s++) {
            switch (*s) {
            case '"': printf("\\\""); break;
            case '\\': printf("\\\\"); break;
            case '\n': printf("\\n"); break;
            case '\r': printf("\\r"); break;
            case '\t': printf("\\t"); break;
            default:
                if ((unsigned char)*s < 0x20)
                    printf("\\u%04x", (unsigned)*s);
                else
                    putchar(*s);
            }
        }
    }
    putchar('"');
}

static struct Arena *jasm_arena;

static void jtype(struct Type *t)
{
    jstr(type_to_string(jasm_arena, t));
}

static void jnode(struct Node *n);

static void jlist(struct Node *n)
{
    putchar('[');
    int first = 1;
    for (; n; n = n->next) {
        if (!first)
            putchar(',');
        first = 0;
        jnode(n);
    }
    putchar(']');
}

static void jopt(struct Node *n)
{
    if (n)
        jnode(n);
    else
        printf("null");
}

static void jnode(struct Node *n)
{
    printf("{\"kind\":");
    jstr(node_kind_name(n->kind));
    printf(",\"line\":%d,\"column\":%d", n->line, n->column);
    switch (n->kind) {
    case NK_INT_LIT:
        printf(",\"value\":%lld", n->ival);
        break;
    case NK_STR_LIT:
        printf(",\"value\":");
        jstr(n->sval);
        break;
    case NK_IDENT:
        printf(",\"name\":");
        jstr(n->sval);
        break;
    case NK_BINOP:
    case NK_ASSIGN:
        printf(",\"op\":");
        jstr(token_kind_name(n->op));
        printf(",\"left\":");
        jnode(n->a);
        printf(",\"right\":");
        jnode(n->b);
        break;
    case NK_UNOP:
    case NK_POSTFIX:
        printf(",\"op\":");
        jstr(token_kind_name(n->op));
        printf(",\"operand\":");
        jnode(n->a);
        break;
    case NK_TERNARY:
        printf(",\"cond\":");
        jnode(n->a);
        printf(",\"then\":");
        jnode(n->b);
        printf(",\"else\":");
        jnode(n->c);
        break;
    case NK_CAST:
        printf(",\"to\":");
        jtype(n->type);
        printf(",\"expr\":");
        jnode(n->a);
        break;
    case NK_SIZEOF:
        if (n->ival) {
            printf(",\"ofType\":");
            jtype(n->type);
        } else {
            printf(",\"ofExpr\":");
            jnode(n->a);
        }
        break;
    case NK_CALL:
        printf(",\"name\":");
        jstr(n->sval);
        printf(",\"args\":");
        jlist(n->a);
        break;
    case NK_INDEX:
        printf(",\"base\":");
        jnode(n->a);
        printf(",\"index\":");
        jnode(n->b);
        break;
    case NK_MEMBER:
        printf(",\"base\":");
        jnode(n->a);
        printf(",\"field\":");
        jstr(n->sval);
        printf(",\"arrow\":%s", n->ival ? "true" : "false");
        break;
    case NK_SWAP:
        printf(",\"a\":");
        jnode(n->a);
        printf(",\"b\":");
        jnode(n->b);
        break;
    case NK_VAR_DECL:
        printf(",\"name\":");
        jstr(n->sval ? n->sval : "");
        printf(",\"type\":");
        jtype(n->type);
        printf(",\"init\":");
        jopt(n->a);
        break;
    case NK_FUNCDEF:
    case NK_PROTO:
        printf(",\"name\":");
        jstr(n->sval);
        printf(",\"returnType\":");
        jtype(n->type);
        printf(",\"params\":");
        jlist(n->a);
        if (n->kind == NK_FUNCDEF) {
            printf(",\"body\":");
            jnode(n->b);
        }
        break;
    case NK_BLOCK:
        printf(",\"statements\":");
        jlist(n->a);
        break;
    case NK_IF:
        printf(",\"cond\":");
        jnode(n->a);
        printf(",\"then\":");
        jopt(n->b);
        printf(",\"else\":");
        jopt(n->c);
        break;
    case NK_WHILE:
        printf(",\"cond\":");
        jnode(n->a);
        printf(",\"body\":");
        jopt(n->b);
        break;
    case NK_DO:
        printf(",\"body\":");
        jopt(n->a);
        printf(",\"cond\":");
        jnode(n->b);
        break;
    case NK_FOR:
        printf(",\"init\":");
        jopt(n->a);
        printf(",\"cond\":");
        jopt(n->b);
        printf(",\"incr\":");
        jopt(n->c);
        printf(",\"body\":");
        jopt(n->d);
        break;
    case NK_SWITCH:
        printf(",\"expr\":");
        jnode(n->a);
        printf(",\"body\":");
        jopt(n->b);
        break;
    case NK_CASE:
        printf(",\"value\":");
        jnode(n->a);
        break;
    case NK_DEFAULT:
    case NK_BREAK:
    case NK_CONTINUE:
        break;
    case NK_RETURN:
        printf(",\"expr\":");
        jopt(n->a);
        break;
    case NK_EXPR_STMT:
        printf(",\"expr\":");
        jopt(n->a);
        break;
    case NK_STRUCTDEF:
        printf(",\"tag\":");
        jstr(n->sval);
        printf(",\"fields\":");
        jlist(n->a);
        break;
    case NK_INIT_LIST:
        printf(",\"elements\":");
        jlist(n->a);
        break;
    default:
        break;
    }
    putchar('}');
}

static void dump_ast(struct Node *prog, struct Arena *arena)
{
    jasm_arena = arena;
    jlist(prog);
    putchar('\n');
}

/* ------------------------------------------------------------------ */
/* driver                                                              */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    int dump_tokens_f = 0, dump_ast_f = 0, dump_bc_f = 0, json_diags_f = 0;
    unsigned long long max_steps = 10000000ull;
    size_t mem_mb = 8;
    const char *file = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--dump-tokens") == 0)
            dump_tokens_f = 1;
        else if (strcmp(argv[i], "--dump-ast") == 0)
            dump_ast_f = 1;
        else if (strcmp(argv[i], "--dump-bytecode") == 0)
            dump_bc_f = 1;
        else if (strcmp(argv[i], "--json-diagnostics") == 0)
            json_diags_f = 1;
        else if (strcmp(argv[i], "--max-steps") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "error: --max-steps needs a value\n");
                return 1;
            }
            max_steps = strtoull(argv[i], NULL, 10);
        } else if (strcmp(argv[i], "--mem-size") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "error: --mem-size needs a value\n");
                return 1;
            }
            mem_mb = (size_t)strtoull(argv[i], NULL, 10);
            if (mem_mb < 1)
                mem_mb = 1;
        } else if (strcmp(argv[i], "--help") == 0 ||
                   strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
            return 0;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "error: unknown option '%s'\n", argv[i]);
            return 1;
        } else if (!file) {
            file = argv[i];
        } else {
            fprintf(stderr, "error: only one input file supported\n");
            return 1;
        }
    }
    if (!file) {
        usage(argv[0]);
        return 1;
    }

    char *src = read_file(file);
    if (!src)
        return 1;

    struct Arena arena;
    arena_init(&arena);
    struct DiagList diags;
    diag_init(&diags);

    /* --- preprocess --- */
    char *ppsrc = pp_process(file, src, &diags);
    free(src);
    if (diags.has_error && !json_diags_f) {
        diag_print_human(&diags);
        free(ppsrc);
        diag_free(&diags);
        arena_free(&arena);
        return 1;
    }

    /* --- tokens --- */
    if (dump_tokens_f) {
        dump_tokens(file, ppsrc, &arena, &diags);
        int rc = diags.has_error ? 1 : 0;
        if (!json_diags_f && diags.has_error)
            diag_print_human(&diags);
        free(ppsrc);
        diag_free(&diags);
        arena_free(&arena);
        return rc;
    }

    /* --- parse --- */
    struct Node *prog = parse_source(ppsrc, file, &arena, &diags);
    free(ppsrc);

    if (dump_ast_f) {
        dump_ast(prog, &arena);
        int arc = diags.has_error ? 1 : 0;
        diag_free(&diags);
        arena_free(&arena);
        return arc;
    }

    /* --- sema --- */
    struct Sema sema;
    sema_init(&sema, &arena, &diags, file);
    sema_program(&sema, prog);

    if (json_diags_f) {
        diag_print_json(&diags);
        int jrc = diags.has_error ? 1 : 0;
        sema_free(&sema);
        diag_free(&diags);
        arena_free(&arena);
        return jrc;
    }
    if (diags.has_error) {
        diag_print_human(&diags);
        sema_free(&sema);
        diag_free(&diags);
        arena_free(&arena);
        return 1;
    }

    /* --- codegen --- */
    struct Codegen cg;
    cg_init(&cg, &sema, &diags);
    int ok = cg_program(&cg, prog);
    if (!ok || diags.has_error) {
        diag_print_human(&diags);
        cg_free(&cg);
        sema_free(&sema);
        diag_free(&diags);
        arena_free(&arena);
        return 1;
    }

    if (dump_bc_f) {
        vm_disasm(cg.code, cg.clen);
        cg_free(&cg);
        sema_free(&sema);
        diag_free(&diags);
        arena_free(&arena);
        return 0;
    }

    /* --- run --- */
    struct VM vm;
    if (vm_init(&vm, mem_mb * 1024u * 1024u, max_steps) != 0) {
        fprintf(stderr, "error: cannot initialize VM\n");
        cg_free(&cg);
        sema_free(&sema);
        diag_free(&diags);
        arena_free(&arena);
        return 1;
    }
    if (vm_load(&vm, cg.code, cg.clen, cg.gimg, cg.glen) != 0) {
        fprintf(stderr, "runtime error: %s\n", vm.errmsg);
        vm_free(&vm);
        cg_free(&cg);
        sema_free(&sema);
        diag_free(&diags);
        arena_free(&arena);
        return 2;
    }
    int rc = vm_run(&vm);
    /* flush captured stdout */
    if (vm.outlen > 0)
        fwrite(vm.outbuf, 1, vm.outlen, stdout);
    fflush(stdout);
    if (rc != 0) {
        fprintf(stderr, "runtime error: %s\n", vm.errmsg);
        vm_free(&vm);
        cg_free(&cg);
        sema_free(&sema);
        diag_free(&diags);
        arena_free(&arena);
        return 2;
    }
    long long ec = vm_exit_code(&vm);
    int exit_code = (int)((ec % 256 + 256) % 256);
    vm_free(&vm);
    cg_free(&cg);
    sema_free(&sema);
    diag_free(&diags);
    arena_free(&arena);
    return exit_code;
}
