#ifndef UCC_CODEGEN_H
#define UCC_CODEGEN_H

#include "sema.h"

/* Bytecode emitter. Consumes the sema-annotated AST and produces:
 *  - code: the instruction stream (with CALL fixups resolved)
 *  - gimg: the globals image (variables + interned string literals) */
struct CgFixup {
    char *name;
    size_t at;
    struct CgFixup *next;
};

struct JmpFix {
    int label;
    size_t at;
};

struct StrEntry {
    char *bytes;
    int len;
    unsigned long long addr;
};

struct Codegen {
    struct Sema *sema;
    struct DiagList *diags;
    unsigned char *code;
    size_t clen;
    size_t ccap;
    unsigned char *gimg;
    size_t glen;
    size_t gcap;
    struct CgFixup *fixups;
    long long *label_pos;
    int nlabels;
    int caplabels;
    struct JmpFix *jmpfix;
    int njmpfix;
    int capjmpfix;
    int *bstack;
    int nbreak;
    int capbreak;
    int *cstack;
    int ncont;
    int capcont;
    struct StrEntry *strs;
    int nstrs;
    int capstrs;
};

void cg_init(struct Codegen *cg, struct Sema *sema, struct DiagList *diags);
int cg_program(struct Codegen *cg, struct Node *prog); /* 1 ok, 0 error */
void cg_free(struct Codegen *cg);

#endif
