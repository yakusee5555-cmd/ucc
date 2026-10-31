#include "lexer.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static const struct {
    const char *word;
    enum TokenKind kind;
} keywords[] = {
    {"int", TK_KW_INT}, {"char", TK_KW_CHAR}, {"void", TK_KW_VOID},
    {"float", TK_KW_FLOAT}, {"double", TK_KW_DOUBLE},
    {"struct", TK_KW_STRUCT}, {"if", TK_KW_IF}, {"else", TK_KW_ELSE},
    {"while", TK_KW_WHILE}, {"do", TK_KW_DO}, {"for", TK_KW_FOR},
    {"switch", TK_KW_SWITCH}, {"case", TK_KW_CASE}, {"default", TK_KW_DEFAULT},
    {"break", TK_KW_BREAK}, {"continue", TK_KW_CONTINUE},
    {"return", TK_KW_RETURN}, {"sizeof", TK_KW_SIZEOF},
    {"long", TK_KW_LONG}, {"short", TK_KW_SHORT},
    {"unsigned", TK_KW_UNSIGNED}, {"signed", TK_KW_SIGNED},
    {"const", TK_KW_CONST}, {"static", TK_KW_STATIC},
    {"extern", TK_KW_EXTERN}, {"typedef", TK_KW_TYPEDEF},
    {"enum", TK_KW_ENUM},
    {NULL, TK_EOF}
};

const char *token_kind_name(enum TokenKind k)
{
    static const char *names[] = {
        "EOF", "identifier", "integer literal", "float literal",
        "character literal", "string literal",
        "'int'", "'char'", "'void'", "'float'", "'double'",
        "'struct'", "'if'", "'else'", "'while'", "'do'", "'for'",
        "'switch'", "'case'", "'default'", "'break'", "'continue'",
        "'return'", "'sizeof'", "'long'", "'short'", "'unsigned'",
        "'signed'", "'const'", "'static'", "'extern'", "'typedef'",
        "'enum'",
        "'+'", "'-'", "'*'", "'/'", "'%'",
        "'='", "'=='", "'!='", "'<'", "'<='", "'>'", "'>='",
        "'<<'", "'>>'",
        "'&'", "'&&'", "'|'", "'||'", "'^'", "'~'", "'!'",
        "'?'", "':'",
        "'++'", "'--'", "'->'", "'.'",
        "'+='", "'-='", "'*='", "'/='", "'%='",
        "'&='", "'|='", "'^='", "'<<='", "'>>='",
        "'('", "')'", "'['", "']'", "'{'", "'}'",
        "';'", "','", "'...'"
    };
    if ((int)k >= 0 && (int)k < TK_COUNT)
        return names[k];
    return "?";
}

int token_is_type_start(enum TokenKind k)
{
    switch (k) {
    case TK_KW_INT: case TK_KW_CHAR: case TK_KW_VOID:
    case TK_KW_FLOAT: case TK_KW_DOUBLE: case TK_KW_STRUCT:
    case TK_KW_LONG: case TK_KW_SHORT: case TK_KW_UNSIGNED:
    case TK_KW_SIGNED: case TK_KW_CONST: case TK_KW_STATIC:
    case TK_KW_EXTERN:
        return 1;
    default:
        return 0;
    }
}

void lexer_init(struct Lexer *lx, const char *src, const char *file,
                struct DiagList *diags, struct Arena *arena)
{
    lx->src = src;
    lx->len = strlen(src);
    lx->pos = 0;
    lx->line = 1;
    lx->col = 1;
    lx->file = file;
    lx->diags = diags;
    lx->arena = arena;
}

static int lx_peek(struct Lexer *lx)
{
    if (lx->pos >= lx->len)
        return -1;
    return (unsigned char)lx->src[lx->pos];
}

static int lx_peek2(struct Lexer *lx)
{
    if (lx->pos + 1 >= lx->len)
        return -1;
    return (unsigned char)lx->src[lx->pos + 1];
}

static int lx_next(struct Lexer *lx)
{
    int c = lx_peek(lx);
    if (c < 0)
        return c;
    lx->pos++;
    if (c == '\n') {
        lx->line++;
        lx->col = 1;
    } else {
        lx->col++;
    }
    return c;
}

static void lx_skip_ws_comments(struct Lexer *lx)
{
    for (;;) {
        int c = lx_peek(lx);
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' ||
            c == '\v') {
            lx_next(lx);
        } else if (c == '/' && lx_peek2(lx) == '/') {
            while (c != '\n' && c >= 0) {
                lx_next(lx);
                c = lx_peek(lx);
            }
        } else if (c == '/' && lx_peek2(lx) == '*') {
            int l = lx->line, col = lx->col;
            lx_next(lx);
            lx_next(lx);
            int prev = 0;
            for (;;) {
                c = lx_peek(lx);
                if (c < 0) {
                    diag_error_at(lx->diags, lx->file, l, col,
                                  "unterminated block comment");
                    return;
                }
                lx_next(lx);
                if (prev == '*' && c == '/')
                    break;
                prev = c;
            }
        } else {
            break;
        }
    }
}

/* Decode one escape sequence; *pp advanced past it. Returns the byte value,
 * or -1 with *bad set on error. Handles \x hex escapes of any length. */
static int decode_escape(struct Lexer *lx, const char **pp, int line, int col,
                         int *bad)
{
    const char *p = *pp;
    int c = (unsigned char)*p++;
    int v = 0;
    switch (c) {
    case 'n': v = '\n'; break;
    case 't': v = '\t'; break;
    case 'r': v = '\r'; break;
    case '0': v = '\0'; break;
    case '\\': v = '\\'; break;
    case '\'': v = '\''; break;
    case '"': v = '"'; break;
    case 'a': v = '\a'; break;
    case 'b': v = '\b'; break;
    case 'f': v = '\f'; break;
    case 'v': v = '\v'; break;
    case 'x': {
        int nd = 0;
        v = 0;
        while (isxdigit((unsigned char)*p)) {
            int d = (unsigned char)*p++;
            v = v * 16 + (d <= '9' ? d - '0'
                                   : (d <= 'F' ? d - 'A' + 10 : d - 'a' + 10));
            nd++;
        }
        if (!nd) {
            *bad = 1;
            diag_error_at(lx->diags, lx->file, line, col,
                          "expected hex digits after \\x");
        }
        v &= 0xFF;
        break;
    }
    default:
        if (c >= '0' && c <= '7') {
            /* octal escape, up to 3 digits */
            v = c - '0';
            for (int i = 0; i < 2 && *p >= '0' && *p <= '7'; i++)
                v = v * 8 + (*p++ - '0');
            v &= 0xFF;
        } else {
            *bad = 1;
            diag_error_at(lx->diags, lx->file, line, col,
                          "unknown escape sequence '\\%c'", c);
        }
        break;
    }
    *pp = p;
    return v;
}

struct Token lexer_next(struct Lexer *lx)
{
    struct Token t;
    memset(&t, 0, sizeof t);
    lx_skip_ws_comments(lx);
    t.line = lx->line;
    t.column = lx->col;

    int c = lx_peek(lx);
    if (c < 0) {
        t.kind = TK_EOF;
        t.text = arena_strdup(lx->arena, "<eof>");
        return t;
    }

    size_t start = lx->pos;

    /* identifier / keyword */
    if (isalpha(c) || c == '_') {
        while (isalnum(lx_peek(lx)) || lx_peek(lx) == '_')
            lx_next(lx);
        t.text = arena_strndup(lx->arena, lx->src + start, lx->pos - start);
        t.kind = TK_IDENT;
        for (int i = 0; keywords[i].word; i++) {
            if (strcmp(t.text, keywords[i].word) == 0) {
                t.kind = keywords[i].kind;
                break;
            }
        }
        return t;
    }

    /* numbers */
    if (isdigit(c) || (c == '.' && isdigit(lx_peek2(lx)))) {
        int is_float = 0;
        int base = 10;
        if (c == '0' && (lx_peek2(lx) == 'x' || lx_peek2(lx) == 'X')) {
            base = 16;
            lx_next(lx);
            lx_next(lx);
            while (isxdigit(lx_peek(lx)))
                lx_next(lx);
        } else {
            if (c == '0' && isdigit(lx_peek2(lx)) && lx_peek2(lx) != '8' &&
                lx_peek2(lx) != '9') {
                /* looks like an octal literal; validate below */
                base = 8;
            }
            while (isdigit(lx_peek(lx))) {
                int d = lx_peek(lx);
                if (base == 8 && (d == '8' || d == '9')) {
                    diag_error_at(lx->diags, lx->file, lx->line, lx->col,
                                  "invalid digit '%c' in octal literal", d);
                    base = 10; /* recover: parse as decimal */
                }
                lx_next(lx);
            }
            if (lx_peek(lx) == '.') {
                is_float = 1;
                lx_next(lx);
                while (isdigit(lx_peek(lx)))
                    lx_next(lx);
            }
            if (lx_peek(lx) == 'e' || lx_peek(lx) == 'E') {
                is_float = 1;
                lx_next(lx);
                if (lx_peek(lx) == '+' || lx_peek(lx) == '-')
                    lx_next(lx);
                while (isdigit(lx_peek(lx)))
                    lx_next(lx);
            }
        }
        while (lx_peek(lx) == 'u' || lx_peek(lx) == 'U' ||
               lx_peek(lx) == 'l' || lx_peek(lx) == 'L' ||
               lx_peek(lx) == 'f' || lx_peek(lx) == 'F')
            lx_next(lx);
        t.text = arena_strndup(lx->arena, lx->src + start, lx->pos - start);
        if (is_float) {
            t.kind = TK_FLOAT;
            t.fval = strtod(t.text, NULL);
        } else {
            t.kind = TK_INT;
            unsigned long long v = 0;
            const char *p = t.text;
            if (base == 16)
                p += 2;
            while (*p) {
                int d;
                if (*p >= '0' && *p <= '9')
                    d = *p - '0';
                else if (*p >= 'a' && *p <= 'f')
                    d = *p - 'a' + 10;
                else if (*p >= 'A' && *p <= 'F')
                    d = *p - 'A' + 10;
                else
                    break;
                if (d >= base)
                    break;
                v = v * (unsigned)base + (unsigned)d;
                p++;
            }
            t.ival = (long long)v;
        }
        return t;
    }

    /* character literal */
    if (c == '\'') {
        lx_next(lx); /* ' */
        int bad = 0;
        int v;
        int pc = lx_peek(lx);
        if (pc == '\\') {
            lx_next(lx);
            const char *p = lx->src + lx->pos;
            /* advance lx past the escape by decoding length manually */
            int save_line = lx->line, save_col = lx->col;
            v = decode_escape(lx, &p, save_line, save_col, &bad);
            while (lx->src + lx->pos < p)
                lx_next(lx);
        } else if (pc == '\'' || pc == '\n' || pc < 0) {
            bad = 1;
            diag_error_at(lx->diags, lx->file, t.line, t.column,
                          "empty character literal");
            v = 0;
        } else {
            v = pc;
            lx_next(lx);
        }
        if (lx_peek(lx) == '\'')
            lx_next(lx);
        else if (!bad)
            diag_error_at(lx->diags, lx->file, t.line, t.column,
                          "unterminated character literal");
        t.kind = TK_CHAR;
        t.ival = bad ? 0 : v;
        t.text = arena_strndup(lx->arena, lx->src + start, lx->pos - start);
        return t;
    }

    /* string literal */
    if (c == '"') {
        lx_next(lx); /* " */
        /* decoded bytes go into a growable malloc buffer, then arena */
        size_t bcap = 32, blen = 0;
        char *buf = (char *)malloc(bcap);
        int bad = 0;
        if (!buf)
            abort();
        for (;;) {
            int ch = lx_peek(lx);
            if (ch < 0 || ch == '\n') {
                diag_error_at(lx->diags, lx->file, t.line, t.column,
                              "unterminated string literal");
                bad = 1;
                break;
            }
            if (ch == '"') {
                lx_next(lx);
                break;
            }
            int v;
            if (ch == '\\') {
                lx_next(lx);
                const char *p = lx->src + lx->pos;
                v = decode_escape(lx, &p, lx->line, lx->col, &bad);
                while (lx->src + lx->pos < p)
                    lx_next(lx);
            } else {
                v = ch;
                lx_next(lx);
            }
            if (blen + 1 >= bcap) {
                bcap *= 2;
                char *nb = (char *)realloc(buf, bcap);
                if (!nb)
                    abort();
                buf = nb;
            }
            buf[blen++] = (char)v;
        }
        t.kind = TK_STR;
        t.text = arena_strndup(lx->arena, lx->src + start, lx->pos - start);
        t.sval = (char *)arena_alloc(lx->arena, blen + 1);
        memcpy(t.sval, buf, blen);
        t.sval[blen] = '\0';
        t.slen = (int)blen;
        free(buf);
        (void)bad;
        return t;
    }

    /* operators & punctuation: longest match */
    lx_next(lx);
    int c2 = lx_peek(lx);
#define TWO(k2, kk)                                   \
    if (c2 == (k2)) {                                 \
        lx_next(lx);                                  \
        t.kind = (kk);                                \
    }
    switch (c) {
    case '+': TWO('+', TK_PLUSPLUS) else TWO('=', TK_PLUSEQ) else t.kind = TK_PLUS; break;
    case '-':
        if (c2 == '-') { lx_next(lx); t.kind = TK_MINUSMINUS; }
        else if (c2 == '=') { lx_next(lx); t.kind = TK_MINUSEQ; }
        else if (c2 == '>') { lx_next(lx); t.kind = TK_ARROW; }
        else t.kind = TK_MINUS;
        break;
    case '*': TWO('=', TK_STAREQ) else t.kind = TK_STAR; break;
    case '/': TWO('=', TK_SLASHEQ) else t.kind = TK_SLASH; break;
    case '%': TWO('=', TK_PERCENTEQ) else t.kind = TK_PERCENT; break;
    case '=': TWO('=', TK_EQEQ) else t.kind = TK_EQ; break;
    case '!': TWO('=', TK_NEQ) else t.kind = TK_BANG; break;
    case '<':
        if (c2 == '=') { lx_next(lx); t.kind = TK_LE; }
        else if (c2 == '<') {
            lx_next(lx);
            if (lx_peek(lx) == '=') { lx_next(lx); t.kind = TK_LSHIFTEQ; }
            else t.kind = TK_LSHIFT;
        } else t.kind = TK_LT;
        break;
    case '>':
        if (c2 == '=') { lx_next(lx); t.kind = TK_GE; }
        else if (c2 == '>') {
            lx_next(lx);
            if (lx_peek(lx) == '=') { lx_next(lx); t.kind = TK_RSHIFTEQ; }
            else t.kind = TK_RSHIFT;
        } else t.kind = TK_GT;
        break;
    case '&':
        if (c2 == '&') { lx_next(lx); t.kind = TK_AMPAMP; }
        else if (c2 == '=') { lx_next(lx); t.kind = TK_AMPEQ; }
        else t.kind = TK_AMP;
        break;
    case '|':
        if (c2 == '|') { lx_next(lx); t.kind = TK_PIPEPIPE; }
        else if (c2 == '=') { lx_next(lx); t.kind = TK_PIPEEQ; }
        else t.kind = TK_PIPE;
        break;
    case '^': TWO('=', TK_CARETEQ) else t.kind = TK_CARET; break;
    case '~': t.kind = TK_TILDE; break;
    case '?': t.kind = TK_QUESTION; break;
    case ':': t.kind = TK_COLON; break;
    case '.':
        if (c2 == '.' && lx_peek2(lx) == '.') {
            lx_next(lx); lx_next(lx); t.kind = TK_ELLIPSIS;
        } else t.kind = TK_DOT;
        break;
    case '(': t.kind = TK_LPAREN; break;
    case ')': t.kind = TK_RPAREN; break;
    case '[': t.kind = TK_LBRACKET; break;
    case ']': t.kind = TK_RBRACKET; break;
    case '{': t.kind = TK_LBRACE; break;
    case '}': t.kind = TK_RBRACE; break;
    case ';': t.kind = TK_SEMI; break;
    case ',': t.kind = TK_COMMA; break;
    default:
        diag_error_at(lx->diags, lx->file, t.line, t.column,
                      "unexpected character '%c'", c);
        t.kind = TK_EOF; /* force stop on garbage; parser will report */
        break;
    }
#undef TWO
    if (!t.text)
        t.text = arena_strndup(lx->arena, lx->src + start, lx->pos - start);
    return t;
}
