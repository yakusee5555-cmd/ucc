#ifndef UCC_VM_H
#define UCC_VM_H

#include <stddef.h>

/* Opcode map. Operands are little-endian and follow the 1-byte opcode.
 * OP_DUP/OP_XCHG/OP_RESERVE/OP_RELEASE are ucc extensions beyond the
 * base 0x00-0x26 set. */
enum Opcode {
    OP_HALT = 0x00,
    OP_PUSH_IMM = 0x01,   /* i64 */
    OP_POP = 0x02,
    OP_LOAD_LOCAL = 0x03, /* u32 off, u8 size: push zero-extended mem[fp+16+off,size) */
    OP_STORE_LOCAL = 0x04,/* u32 off, u8 size: pop v, store low bytes */
    OP_LOAD_GLOBAL = 0x05,/* u64 addr, u8 size */
    OP_STORE_GLOBAL = 0x06,/* u64 addr, u8 size */
    OP_ADDR_LOCAL = 0x07, /* u32 off: push fp+16+off */
    OP_ADDR_GLOBAL = 0x08,/* u64 addr: push addr */
    OP_LOAD_IND = 0x09,   /* u8 size: a=pop, push mem[a,size) */
    OP_STORE_IND = 0x0A,  /* u8 size: v=pop, a=pop, store */
    OP_ADD = 0x0B,
    OP_SUB = 0x0C,
    OP_MUL = 0x0D,        /* wrapping i64 */
    OP_DIV = 0x0E,        /* divisor 0 -> runtime error */
    OP_MOD = 0x0F,
    OP_NEG = 0x10,
    OP_AND = 0x11,
    OP_OR = 0x12,
    OP_XOR = 0x13,
    OP_NOT = 0x14,
    OP_SHL = 0x15,        /* count masked with 63 */
    OP_SHR = 0x16,        /* arithmetic, count masked with 63 */
    OP_CMP_EQ = 0x17,
    OP_CMP_NE = 0x18,
    OP_CMP_LT = 0x19,
    OP_CMP_LE = 0x1A,
    OP_CMP_GT = 0x1B,
    OP_CMP_GE = 0x1C,     /* signed, push 1/0 */
    OP_JMP = 0x1D,        /* u64 target (absolute bytecode offset) */
    OP_JZ = 0x1E,         /* u64 target */
    OP_JNZ = 0x1F,        /* u64 target */
    OP_CALL = 0x20,       /* u64 addr, u8 nargs: pop args, build frame, pc=addr */
    OP_RET = 0x21,        /* leave one value on the value stack */
    OP_ENTER = 0x22,      /* u32 nbytes: zero callee locals region */
    OP_SWAP = 0x23,       /* u8 size: pop b-addr, pop a-addr, exchange bytes */
    OP_SYSCALL_PRINTF = 0x24, /* u8 nargs */
    OP_SYSCALL_SCANF = 0x25,  /* u8 nargs */
    OP_MEMCPY = 0x26,     /* u32 size: src=pop, dst=pop, checked memcpy */
    OP_DUP = 0x27,        /* duplicate top of value stack */
    OP_XCHG = 0x28,       /* swap top two values */
    OP_RESERVE = 0x29,    /* u32 nbytes: sp -= n (caller reserves callee locals) */
    OP_RELEASE = 0x2A     /* u32 nbytes: sp += n */
};

struct VM {
    unsigned char *mem;
    size_t memsize;
    unsigned long long globals_end;
    unsigned char *code;
    size_t codelen;

    long long *vstack;
    size_t vcap;
    size_t vsp;

    unsigned long long pc;
    unsigned long long fp;
    unsigned long long sp;

    /* C-level call metadata (nargs per frame); ret_pc/old_fp live in mem. */
    struct VMFrame {
        unsigned int nargs;
    } *frames;
    int depth;
    int fcap;

    unsigned long long steps;
    unsigned long long max_steps;

    /* stdout capture (1MB cap, then truncated with a note) */
    char *outbuf;
    size_t outlen;
    size_t outcap;
    int out_truncated;

    /* stdin streaming */
    char *inbuf;
    size_t inlen;
    size_t incap;
    size_t inpos;
    int ineof;

    char errmsg[256];
};

int vm_init(struct VM *vm, size_t memsize, unsigned long long max_steps);
int vm_load(struct VM *vm, unsigned char *code, size_t codelen,
            unsigned char *gimg, size_t glen);
int vm_run(struct VM *vm); /* 0 = halted ok; -1 = runtime error (errmsg) */
long long vm_exit_code(struct VM *vm);
void vm_free(struct VM *vm);
void vm_disasm(unsigned char *code, size_t len);

#endif
