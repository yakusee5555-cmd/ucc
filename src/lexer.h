#ifndef UCC_LEXER_H
#define UCC_LEXER_H

#include "arena.h"
#include "diag.h"

/* Plain enum (no typedef): token kinds for the whole front end. */
enum TokenKind {
    TK_EOF = 0,
    TK_IDENT,
    TK_INT,      /* integer literal (decimal, hex, octal) */
    TK_FLOAT,    /* float literal: parsed, truncated to int with warning */
    TK_CHAR,     /* character literal */
    TK_STR,      /* string literal */

    /* keywords */
    TK_KW_INT, TK_KW_CHAR, TK_KW_VOID, TK_KW_FLOAT, TK_KW_DOUBLE,
    TK_KW_STRUCT, TK_KW_IF, TK_KW_ELSE, TK_KW_WHILE, TK_KW_DO, TK_KW_FOR,
    TK_KW_SWITCH, TK_KW_CASE, TK_KW_DEFAULT, TK_KW_BREAK, TK_KW_CONTINUE,
    TK_KW_RETURN, TK_KW_SIZEOF, TK_KW_LONG, TK_KW_SHORT, TK_KW_UNSIGNED,
    TK_KW_SIGNED, TK_KW_CONST, TK_KW_STATIC, TK_KW_EXTERN, TK_KW_TYPEDEF,
    TK_KW_ENUM,

    /* operators */
    TK_PLUS, TK_MINUS, TK_STAR, TK_SLASH, TK_PERCENT,
    TK_EQ, TK_EQEQ, TK_NEQ, TK_LT, TK_LE, TK_GT, TK_GE,
    TK_LSHIFT, TK_RSHIFT,
    TK_AMP, TK_AMPAMP, TK_PIPE, TK_PIPEPIPE, TK_CARET, TK_TILDE, TK_BANG,
    TK_QUESTION, TK_COLON,
    TK_PLUSPLUS, TK_MINUSMINUS, TK_ARROW, TK_DOT,
    TK_PLUSEQ, TK_MINUSEQ, TK_STAREQ, TK_SLASHEQ, TK_PERCENTEQ,
    TK_AMPEQ, TK_PIPEEQ, TK_CARETEQ, TK_LSHIFTEQ, TK_RSHIFTEQ,

    /* punctuation */
    TK_LPAREN, TK_RPAREN, TK_LBRACKET, TK_RBRACKET, TK_LBRACE, TK_RBRACE,
    TK_SEMI, TK_COMMA, TK_ELLIPSIS,

    TK_COUNT
};

struct Token {
    enum TokenKind kind;
    int line;
    int column;
    long long ival;  /* value for TK_INT / TK_CHAR */
    double fval;     /* value for TK_FLOAT */
    char *text;      /* raw lexeme (arena allocated); for TK_STR the raw
                      * source text incl. quotes, TK_STR also uses sval */
    char *sval;      /* unescaped bytes for TK_STR / TK_CHAR; NULL otherwise */
    int slen;        /* byte length of sval for TK_STR */
};

struct Lexer {
    const char *src;
    size_t len;
    size_t pos;
    int line;
    int col;
    const char *file;
    struct DiagList *diags;
    struct Arena *arena;
};

void lexer_init(struct Lexer *lx, const char *src, const char *file,
                struct DiagList *diags, struct Arena *arena);
struct Token lexer_next(struct Lexer *lx);
const char *token_kind_name(enum TokenKind k);
int token_is_type_start(enum TokenKind k);

#endif
