#define _POSIX_C_SOURCE 200809L
#include "vm.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define VSTACK_CAP (1024u * 1024u)
#define FCAP 10000
#define OUT_CAP0 8192
#define OUT_MAX (1024u * 1024u)
#define IN_CAP0 8192

static int vm_fail(struct VM *vm, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(vm->errmsg, sizeof vm->errmsg, fmt, ap);
    va_end(ap);
    return -1;
}

int vm_init(struct VM *vm, size_t memsize, unsigned long long max_steps)
{
    memset(vm, 0, sizeof *vm);
    vm->mem = (unsigned char *)calloc(memsize ? memsize : 1, 1);
    vm->vstack = (long long *)malloc(VSTACK_CAP * sizeof(long long));
    vm->frames =
        (struct VMFrame *)malloc(FCAP * sizeof(struct VMFrame));
    vm->outbuf = (char *)malloc(OUT_CAP0);
    vm->inbuf = (char *)malloc(IN_CAP0);
    if (!vm->mem || !vm->vstack || !vm->frames || !vm->outbuf || !vm->inbuf) {
        vm_free(vm);
        return -1;
    }
    vm->memsize = memsize;
    vm->vcap = VSTACK_CAP;
    vm->fcap = FCAP;
    vm->outcap = OUT_CAP0;
    vm->incap = IN_CAP0;
    vm->max_steps = max_steps;
    vm->sp = memsize;
    return 0;
}

void vm_free(struct VM *vm)
{
    free(vm->mem);
    free(vm->code);
    free(vm->vstack);
    free(vm->frames);
    free(vm->outbuf);
    free(vm->inbuf);
    memset(vm, 0, sizeof *vm);
}

int vm_load(struct VM *vm, unsigned char *code, size_t codelen,
            unsigned char *gimg, size_t glen)
{
    if (glen > vm->memsize)
        return vm_fail(vm, "globals image larger than vm memory");
    vm->code = (unsigned char *)malloc(codelen ? codelen : 1);
    if (!vm->code)
        return vm_fail(vm, "out of memory");
    memcpy(vm->code, code, codelen);
    vm->codelen = codelen;
    memcpy(vm->mem, gimg, glen);
    vm->globals_end = glen;
    vm->pc = 0;
    vm->fp = 0;
    vm->sp = vm->memsize;
    vm->depth = 0;
    vm->vsp = 0;
    vm->steps = 0;
    return 0;
}

long long vm_exit_code(struct VM *vm)
{
    if (vm->vsp > 0)
        return vm->vstack[vm->vsp - 1];
    return 0;
}

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */

static int mem_ok(struct VM *vm, unsigned long long a,
                  unsigned long long size)
{
    return size <= vm->memsize && a <= vm->memsize - size;
}

static unsigned long long r_u64(struct VM *vm, int *err)
{
    if (vm->pc + 8 > vm->codelen) {
        *err = vm_fail(vm, "truncated bytecode");
        return 0;
    }
    unsigned long long v = 0;
    for (int i = 0; i < 8; i++)
        v |= (unsigned long long)vm->code[vm->pc + i]
             << (unsigned)(8 * i);
    vm->pc += 8;
    return v;
}

static unsigned long long r_u32(struct VM *vm, int *err)
{
    if (vm->pc + 4 > vm->codelen) {
        *err = vm_fail(vm, "truncated bytecode");
        return 0;
    }
    unsigned long long v = 0;
    for (int i = 0; i < 4; i++)
        v |= (unsigned long long)vm->code[vm->pc + i]
             << (unsigned)(8 * i);
    vm->pc += 4;
    return v;
}

static unsigned r_u8(struct VM *vm, int *err)
{
    if (vm->pc + 1 > vm->codelen) {
        *err = vm_fail(vm, "truncated bytecode");
        return 0;
    }
    return vm->code[vm->pc++];
}

static unsigned long long mget(struct VM *vm, unsigned long long a,
                               unsigned size, int *err)
{
    if (!mem_ok(vm, a, size)) {
        *err = vm_fail(vm, "out of bounds memory access");
        return 0;
    }
    unsigned long long v = 0;
    for (unsigned i = 0; i < size; i++)
        v |= (unsigned long long)vm->mem[a + i] << (unsigned)(8 * i);
    return v;
}

static void mput(struct VM *vm, unsigned long long a, unsigned size,
                 unsigned long long v, int *err)
{
    if (!mem_ok(vm, a, size)) {
        *err = vm_fail(vm, "out of bounds memory access");
        return;
    }
    for (unsigned i = 0; i < size; i++) {
        vm->mem[a + i] = (unsigned char)(v & 0xFF);
        v >>= 8;
    }
}

static void vpush(struct VM *vm, long long v, int *err)
{
    if (vm->vsp >= vm->vcap) {
        *err = vm_fail(vm, "value stack overflow");
        return;
    }
    vm->vstack[vm->vsp++] = v;
}

static long long vpop(struct VM *vm, int *err)
{
    if (vm->vsp == 0) {
        *err = vm_fail(vm, "value stack underflow");
        return 0;
    }
    return vm->vstack[--vm->vsp];
}

/* ------------------------------------------------------------------ */
/* output capture                                                      */
/* ------------------------------------------------------------------ */

static void vm_out_reserve(struct VM *vm, size_t extra)
{
    if (vm->outlen + extra > vm->outcap) {
        size_t ncap = vm->outcap ? vm->outcap : OUT_CAP0;
        while (ncap < vm->outlen + extra)
            ncap *= 2;
        char *nb = (char *)realloc(vm->outbuf, ncap);
        if (!nb)
            abort();
        vm->outbuf = nb;
        vm->outcap = ncap;
    }
}

static void vm_out(struct VM *vm, const char *s, size_t n)
{
    if (vm->out_truncated)
        return;
    if (vm->outlen + n > OUT_MAX) {
        n = OUT_MAX - vm->outlen;
        vm->out_truncated = 1;
    }
    vm_out_reserve(vm, n + 64);
    memcpy(vm->outbuf + vm->outlen, s, n);
    vm->outlen += n;
    if (vm->out_truncated) {
        const char *note = "\n[output truncated at 1MB]\n";
        size_t nl = strlen(note);
        vm_out_reserve(vm, nl);
        memcpy(vm->outbuf + vm->outlen, note, nl);
        vm->outlen += nl;
    }
}

/* Flush captured stdout to the real stdout (used before blocking reads). */
static void vm_flush(struct VM *vm)
{
    if (vm->outlen > 0) {
        size_t off = 0;
        while (off < vm->outlen) {
            ssize_t w = write(1, vm->outbuf + off, vm->outlen - off);
            if (w <= 0)
                break;
            off += (size_t)w;
        }
        vm->outlen = 0;
    }
    fflush(stdout);
}

/* ------------------------------------------------------------------ */
/* stdin streaming                                                     */
/* ------------------------------------------------------------------ */

static int vm_getc(struct VM *vm)
{
    if (vm->inpos < vm->inlen)
        return (unsigned char)vm->inbuf[vm->inpos++];
    if (vm->ineof)
        return -1;
    vm_flush(vm); /* show any prompt before blocking on input */
    if (vm->inlen == vm->incap) {
        size_t ncap = vm->incap * 2;
        char *nb = (char *)realloc(vm->inbuf, ncap);
        if (!nb)
            abort();
        vm->inbuf = nb;
        vm->incap = ncap;
    }
    ssize_t r = read(0, vm->inbuf + vm->inlen, vm->incap - vm->inlen);
    if (r <= 0) {
        vm->ineof = 1;
        return -1;
    }
    vm->inlen += (size_t)r;
    return (unsigned char)vm->inbuf[vm->inpos++];
}

static void vm_ungetc(struct VM *vm)
{
    if (vm->inpos > 0)
        vm->inpos--;
}

/* ------------------------------------------------------------------ */
/* printf implementation                                               */
/* ------------------------------------------------------------------ */

static int vm_read_cstr(struct VM *vm, unsigned long long addr, char *out,
                        size_t max)
{
    size_t i = 0;
    while (i + 1 < max) {
        if (!mem_ok(vm, addr + i, 1))
            return -1;
        char c = (char)vm->mem[addr + i];
        out[i++] = c;
        if (c == '\0')
            return 0;
    }
    out[max - 1] = '\0';
    return -1;
}

static int vm_printf(struct VM *vm, unsigned long long faddr, long long *args,
                     unsigned nargs, int *err)
{
    char fmt[4096];
    if (vm_read_cstr(vm, faddr, fmt, sizeof fmt) != 0)
        return *err = vm_fail(vm, "printf: bad format string address");
    unsigned ai = 0;
    long long written = 0;
    char num[96];
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') {
            vm_out(vm, p, 1);
            written++;
            continue;
        }
        p++;
        if (*p == '%') {
            vm_out(vm, "%", 1);
            written++;
            continue;
        }
        if (*p == 'n')
            return *err = vm_fail(vm, "format specifier %n is not supported");
        int lng = 0;
        if (*p == 'l') {
            lng = 1;
            p++;
        }
        (void)lng;
        if (ai >= nargs)
            return *err = vm_fail(vm,
                                  "printf: not enough arguments for format");
        long long v = args[ai++];
        size_t nl = 0;
        switch (*p) {
        case 'd':
        case 'i':
            nl = (size_t)snprintf(num, sizeof num, "%lld", v);
            break;
        case 'u':
            nl = (size_t)snprintf(num, sizeof num, "%llu",
                                  (unsigned long long)v);
            break;
        case 'x':
            nl = (size_t)snprintf(num, sizeof num, "%llx",
                                  (unsigned long long)v);
            break;
        case 'X':
            nl = (size_t)snprintf(num, sizeof num, "%llX",
                                  (unsigned long long)v);
            break;
        case 'c':
            num[0] = (char)(v & 0xFF);
            nl = 1;
            break;
        case 's': {
            unsigned long long a = (unsigned long long)v;
            size_t cnt = 0;
            for (;;) {
                if (!mem_ok(vm, a + cnt, 1))
                    return *err = vm_fail(vm,
                                          "out of bounds memory access");
                char c = (char)vm->mem[a + cnt];
                if (c == '\0')
                    break;
                vm_out(vm, &c, 1);
                written++;
                cnt++;
                if (cnt > OUT_MAX)
                    break;
            }
            continue;
        }
        case 'p':
            nl = (size_t)snprintf(num, sizeof num, "0x%llx",
                                  (unsigned long long)v);
            break;
        case 'f':
            if (v < 0) {
                unsigned long long m =
                    (unsigned long long)(-(v + 1)) + 1u;
                nl = (size_t)snprintf(num, sizeof num, "-%llu.000000", m);
            } else {
                nl = (size_t)snprintf(num, sizeof num, "%llu.000000",
                                      (unsigned long long)v);
            }
            break;
        default:
            return *err =
                       vm_fail(vm, "unsupported format specifier '%%%c'", *p);
        }
        vm_out(vm, num, nl);
        written += (long long)nl;
    }
    vpush(vm, written, err);
    return *err;
}

/* ------------------------------------------------------------------ */
/* scanf implementation                                                */
/* ------------------------------------------------------------------ */

static int scan_digits(struct VM *vm, int base, int allow_sign,
                       int *is_neg, unsigned long long *acc)
{
    int c;
    do {
        c = vm_getc(vm);
    } while (c >= 0 && isspace(c));
    if (c < 0)
        return 0;
    *is_neg = 0;
    if (allow_sign && (c == '+' || c == '-')) {
        *is_neg = (c == '-');
        c = vm_getc(vm);
    }
    if (base == 16 && c == '0') {
        int c2 = vm_getc(vm);
        if (c2 == 'x' || c2 == 'X') {
            c = vm_getc(vm);
        } else {
            vm_ungetc(vm);
        }
    }
    if (base == 0) {
        /* %i auto-detect */
        if (c == '0') {
            int c2 = vm_getc(vm);
            if (c2 == 'x' || c2 == 'X') {
                base = 16;
                c = vm_getc(vm);
            } else {
                vm_ungetc(vm);
                base = 8;
            }
        } else {
            base = 10;
        }
    }
    unsigned long long v = 0;
    int nd = 0;
    int overflow = 0;
    for (;;) {
        int d = -1;
        if (c >= '0' && c <= '9')
            d = c - '0';
        else if (c >= 'a' && c <= 'f')
            d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F')
            d = c - 'A' + 10;
        if (d < 0 || d >= base)
            break;
        if (v > (0xFFFFFFFFFFFFFFFFull - (unsigned)d) / (unsigned)base)
            overflow = 1;
        else
            v = v * (unsigned)base + (unsigned)d;
        nd++;
        c = vm_getc(vm);
    }
    if (c >= 0)
        vm_ungetc(vm);
    if (nd == 0)
        return 0;
    if (overflow)
        v = 0xFFFFFFFFFFFFFFFFull;
    *acc = v;
    return 1;
}

/* returns: 1 assigned, 0 matching failure, -1 error */
static int vm_scanf(struct VM *vm, const char *fmt,
                    unsigned long long *addrs, long long *maxlens,
                    unsigned nargs, int *err)
{
    int ai = 0;
    int assigned = 0;
    const char *p = fmt;
    while (*p) {
        if (isspace((unsigned char)*p)) {
            while (isspace((unsigned char)*p))
                p++;
            int c;
            do {
                c = vm_getc(vm);
            } while (c >= 0 && isspace(c));
            if (c >= 0)
                vm_ungetc(vm);
            continue;
        }
        if (*p != '%') {
            int c = vm_getc(vm);
            if (c < 0 || c != (unsigned char)*p) {
                if (c >= 0)
                    vm_ungetc(vm);
                break;
            }
            p++;
            continue;
        }
        p++;
        if (*p == '%') {
            int c = vm_getc(vm);
            if (c != '%') {
                if (c >= 0)
                    vm_ungetc(vm);
                break;
            }
            p++;
            continue;
        }
        char spec = *p;
        if (spec == 'l' &&
            (p[1] == 'd' || p[1] == 'i' || p[1] == 'u' || p[1] == 'x' ||
             p[1] == 'f')) {
            spec = p[1];
            p++;
        }
        p++;
        if (spec != 'd' && spec != 'i' && spec != 'u' && spec != 'x' &&
            spec != 'c' && spec != 's' && spec != 'f') {
            *err = vm_fail(vm, "unsupported scanf specifier '%%%c'", spec);
            return -1;
        }
        if (ai >= (int)nargs)
            break;
        unsigned long long addr = addrs[ai];
        long long maxlen = maxlens[ai];
        int c;
        switch (spec) {
        case 'd':
        case 'i':
        case 'u':
        case 'x':
        case 'f': {
            int base = spec == 'x' ? 16 : spec == 'i' ? 0 : 10;
            int is_neg = 0;
            unsigned long long acc = 0;
            /* %f: allow a single '.' with fraction digits (ignored) */
            if (spec == 'f') {
                /* parse integer part with scan_digits, then skip fraction */
                if (!scan_digits(vm, 10, 1, &is_neg, &acc))
                    goto match_fail;
                /* peek for '.' */
                c = vm_getc(vm);
                if (c == '.') {
                    do {
                        c = vm_getc(vm);
                    } while (c >= 0 && isdigit(c));
                }
                if (c >= 0)
                    vm_ungetc(vm);
            } else {
                if (!scan_digits(vm, base, 1, &is_neg, &acc))
                    goto match_fail;
            }
            long long sv;
            if (spec == 'u' || spec == 'x')
                sv = (long long)acc;
            else
                sv = is_neg ? -(long long)(acc & 0x7FFFFFFFFFFFFFFFull)
                            : (long long)(acc & 0x7FFFFFFFFFFFFFFFull);
            if (!mem_ok(vm, addr, 8)) {
                *err = vm_fail(vm, "out of bounds memory access");
                return -1;
            }
            unsigned long long uv = (unsigned long long)sv;
            for (int i = 0; i < 8; i++) {
                vm->mem[addr + (unsigned)i] = (unsigned char)(uv & 0xFF);
                uv >>= 8;
            }
            assigned++;
            ai++;
            break;
        }
        case 'c': {
            /* %c does not skip whitespace */
            c = vm_getc(vm);
            if (c < 0)
                goto match_fail;
            if (!mem_ok(vm, addr, 1)) {
                *err = vm_fail(vm, "out of bounds memory access");
                return -1;
            }
            vm->mem[addr] = (unsigned char)c;
            assigned++;
            ai++;
            break;
        }
        case 's': {
            do {
                c = vm_getc(vm);
            } while (c >= 0 && isspace(c));
            if (c < 0)
                goto match_fail;
            long long cap = maxlen > 0 ? maxlen : 4096;
            if (cap > 1048576)
                cap = 1048576;
            long long i = 0;
            while (c >= 0 && !isspace(c)) {
                if (i + 1 < cap) {
                    if (!mem_ok(vm, addr + (unsigned long long)i, 1)) {
                        *err = vm_fail(vm, "out of bounds memory access");
                        return -1;
                    }
                    vm->mem[addr + (unsigned long long)i] = (unsigned char)c;
                }
                i++;
                c = vm_getc(vm);
            }
            if (c >= 0)
                vm_ungetc(vm);
            long long term = i + 1 < cap ? i : cap - 1;
            if (term < 0)
                term = 0;
            if (!mem_ok(vm, addr + (unsigned long long)term, 1)) {
                *err = vm_fail(vm, "out of bounds memory access");
                return -1;
            }
            vm->mem[addr + (unsigned long long)term] = '\0';
            assigned++;
            ai++;
            break;
        }
        }
        continue;
    match_fail:
        break;
    }
    return assigned;
}

/* ------------------------------------------------------------------ */
/* main interpreter loop                                               */
/* ------------------------------------------------------------------ */

static long long shr_arith(long long a, long long b)
{
    unsigned s = (unsigned)(b & 63);
    if (s == 0)
        return a;
    if (a >= 0)
        return (long long)((unsigned long long)a >> s);
    return ~((long long)(((unsigned long long)~a) >> s));
}

int vm_run(struct VM *vm)
{
    int err = 0;
    for (;;) {
        vm->steps++;
        if (vm->steps > vm->max_steps)
            return vm_fail(vm, "instruction step limit exceeded (possible "
                               "infinite loop)");
        if (vm->pc >= vm->codelen)
            return vm_fail(vm, "program counter out of bounds");
        unsigned op = vm->code[vm->pc++];
        err = 0;
        switch (op) {
        case OP_HALT:
            return 0;
        case OP_PUSH_IMM: {
            long long v = (long long)r_u64(vm, &err);
            if (err)
                return -1;
            vpush(vm, v, &err);
            if (err)
                return -1;
            break;
        }
        case OP_POP:
            vpop(vm, &err);
            if (err)
                return -1;
            break;
        case OP_LOAD_LOCAL: {
            unsigned long long off = r_u32(vm, &err);
            unsigned sz = r_u8(vm, &err);
            if (err)
                return -1;
            unsigned long long v = mget(vm, vm->fp + 16 + off, sz, &err);
            if (err)
                return -1;
            vpush(vm, (long long)v, &err);
            if (err)
                return -1;
            break;
        }
        case OP_STORE_LOCAL: {
            unsigned long long off = r_u32(vm, &err);
            unsigned sz = r_u8(vm, &err);
            long long v = 0;
            if (!err)
                v = vpop(vm, &err);
            if (err)
                return -1;
            mput(vm, vm->fp + 16 + off, sz, (unsigned long long)v, &err);
            if (err)
                return -1;
            break;
        }
        case OP_LOAD_GLOBAL: {
            unsigned long long addr = r_u64(vm, &err);
            unsigned sz = r_u8(vm, &err);
            if (err)
                return -1;
            unsigned long long v = mget(vm, addr, sz, &err);
            if (err)
                return -1;
            vpush(vm, (long long)v, &err);
            if (err)
                return -1;
            break;
        }
        case OP_STORE_GLOBAL: {
            unsigned long long addr = r_u64(vm, &err);
            unsigned sz = r_u8(vm, &err);
            long long v = 0;
            if (!err)
                v = vpop(vm, &err);
            if (err)
                return -1;
            mput(vm, addr, sz, (unsigned long long)v, &err);
            if (err)
                return -1;
            break;
        }
        case OP_ADDR_LOCAL: {
            unsigned long long off = r_u32(vm, &err);
            if (err)
                return -1;
            vpush(vm, (long long)(vm->fp + 16 + off), &err);
            if (err)
                return -1;
            break;
        }
        case OP_ADDR_GLOBAL: {
            unsigned long long addr = r_u64(vm, &err);
            if (err)
                return -1;
            vpush(vm, (long long)addr, &err);
            if (err)
                return -1;
            break;
        }
        case OP_LOAD_IND: {
            unsigned sz = r_u8(vm, &err);
            unsigned long long a = 0;
            if (!err)
                a = (unsigned long long)vpop(vm, &err);
            if (err)
                return -1;
            unsigned long long v = mget(vm, a, sz, &err);
            if (err)
                return -1;
            vpush(vm, (long long)v, &err);
            if (err)
                return -1;
            break;
        }
        case OP_STORE_IND: {
            unsigned sz = r_u8(vm, &err);
            long long v = 0;
            unsigned long long a = 0;
            if (!err)
                v = vpop(vm, &err);
            if (!err)
                a = (unsigned long long)vpop(vm, &err);
            if (err)
                return -1;
            mput(vm, a, sz, (unsigned long long)v, &err);
            if (err)
                return -1;
            break;
        }
        case OP_ADD: {
            long long b = vpop(vm, &err);
            long long a = 0;
            if (!err)
                a = vpop(vm, &err);
            if (err)
                return -1;
            vpush(vm,
                  (long long)((unsigned long long)a +
                              (unsigned long long)b),
                  &err);
            if (err)
                return -1;
            break;
        }
        case OP_SUB: {
            long long b = vpop(vm, &err);
            long long a = 0;
            if (!err)
                a = vpop(vm, &err);
            if (err)
                return -1;
            vpush(vm,
                  (long long)((unsigned long long)a -
                              (unsigned long long)b),
                  &err);
            if (err)
                return -1;
            break;
        }
        case OP_MUL: {
            long long b = vpop(vm, &err);
            long long a = 0;
            if (!err)
                a = vpop(vm, &err);
            if (err)
                return -1;
            vpush(vm,
                  (long long)((unsigned long long)a *
                              (unsigned long long)b),
                  &err);
            if (err)
                return -1;
            break;
        }
        case OP_DIV: {
            long long b = vpop(vm, &err);
            long long a = 0;
            if (!err)
                a = vpop(vm, &err);
            if (err)
                return -1;
            if (b == 0)
                return vm_fail(vm, "division by zero");
            long long q;
            if (a == (-9223372036854775807LL - 1) && b == -1)
                q = a; /* avoid UB: wraps to INT64_MIN */
            else
                q = a / b;
            vpush(vm, q, &err);
            if (err)
                return -1;
            break;
        }
        case OP_MOD: {
            long long b = vpop(vm, &err);
            long long a = 0;
            if (!err)
                a = vpop(vm, &err);
            if (err)
                return -1;
            if (b == 0)
                return vm_fail(vm, "division by zero");
            long long r;
            if (a == (-9223372036854775807LL - 1) && b == -1)
                r = 0;
            else
                r = a % b;
            vpush(vm, r, &err);
            if (err)
                return -1;
            break;
        }
        case OP_NEG: {
            long long a = vpop(vm, &err);
            if (err)
                return -1;
            vpush(vm, (long long)(0ull - (unsigned long long)a), &err);
            if (err)
                return -1;
            break;
        }
        case OP_AND: {
            long long b = vpop(vm, &err);
            long long a = 0;
            if (!err)
                a = vpop(vm, &err);
            if (err)
                return -1;
            vpush(vm, a & b, &err);
            if (err)
                return -1;
            break;
        }
        case OP_OR: {
            long long b = vpop(vm, &err);
            long long a = 0;
            if (!err)
                a = vpop(vm, &err);
            if (err)
                return -1;
            vpush(vm, a | b, &err);
            if (err)
                return -1;
            break;
        }
        case OP_XOR: {
            long long b = vpop(vm, &err);
            long long a = 0;
            if (!err)
                a = vpop(vm, &err);
            if (err)
                return -1;
            vpush(vm, a ^ b, &err);
            if (err)
                return -1;
            break;
        }
        case OP_NOT: {
            long long a = vpop(vm, &err);
            if (err)
                return -1;
            vpush(vm, ~a, &err);
            if (err)
                return -1;
            break;
        }
        case OP_SHL: {
            long long b = vpop(vm, &err);
            long long a = 0;
            if (!err)
                a = vpop(vm, &err);
            if (err)
                return -1;
            vpush(vm,
                  (long long)((unsigned long long)a
                              << (unsigned)(b & 63)),
                  &err);
            if (err)
                return -1;
            break;
        }
        case OP_SHR: {
            long long b = vpop(vm, &err);
            long long a = 0;
            if (!err)
                a = vpop(vm, &err);
            if (err)
                return -1;
            vpush(vm, shr_arith(a, b), &err);
            if (err)
                return -1;
            break;
        }
        case OP_CMP_EQ:
        case OP_CMP_NE:
        case OP_CMP_LT:
        case OP_CMP_LE:
        case OP_CMP_GT:
        case OP_CMP_GE: {
            long long b = vpop(vm, &err);
            long long a = 0;
            if (!err)
                a = vpop(vm, &err);
            if (err)
                return -1;
            long long r = 0;
            switch (op) {
            case OP_CMP_EQ: r = (a == b); break;
            case OP_CMP_NE: r = (a != b); break;
            case OP_CMP_LT: r = (a < b); break;
            case OP_CMP_LE: r = (a <= b); break;
            case OP_CMP_GT: r = (a > b); break;
            case OP_CMP_GE: r = (a >= b); break;
            default: break;
            }
            vpush(vm, r, &err);
            if (err)
                return -1;
            break;
        }
        case OP_JMP: {
            unsigned long long t = r_u64(vm, &err);
            if (err)
                return -1;
            if (t >= vm->codelen)
                return vm_fail(vm, "jump target out of bounds");
            vm->pc = t;
            break;
        }
        case OP_JZ:
        case OP_JNZ: {
            unsigned long long t = r_u64(vm, &err);
            long long v = 0;
            if (!err)
                v = vpop(vm, &err);
            if (err)
                return -1;
            if (t >= vm->codelen)
                return vm_fail(vm, "jump target out of bounds");
            if ((op == OP_JZ && v == 0) || (op == OP_JNZ && v != 0))
                vm->pc = t;
            break;
        }
        case OP_CALL: {
            unsigned long long addr = r_u64(vm, &err);
            unsigned nargs = 0;
            if (!err)
                nargs = r_u8(vm, &err);
            if (err)
                return -1;
            if (vm->depth >= vm->fcap)
                return vm_fail(vm, "call stack depth exceeded");
            if (vm->vsp < nargs)
                return vm_fail(vm, "value stack underflow");
            long long tmp[256];
            for (unsigned i = 0; i < nargs; i++)
                tmp[nargs - 1 - i] = vpop(vm, &err);
            if (err)
                return -1;
            unsigned long long need = 16 + 8 * (unsigned long long)nargs;
            if (need > vm->sp - vm->globals_end)
                return vm_fail(vm, "stack overflow");
            unsigned long long nfp = vm->sp - need;
            mput(vm, nfp, 8, vm->pc, &err);
            if (!err)
                mput(vm, nfp + 8, 8, vm->fp, &err);
            for (unsigned i = 0; i < nargs && !err; i++)
                mput(vm, nfp + 16 + 8 * i, 8, (unsigned long long)tmp[i],
                     &err);
            if (err)
                return -1;
            vm->frames[vm->depth].nargs = nargs;
            vm->depth++;
            vm->fp = nfp;
            vm->sp = nfp;
            if (addr >= vm->codelen)
                return vm_fail(vm, "call target out of bounds");
            vm->pc = addr;
            break;
        }
        case OP_RET: {
            long long v = vpop(vm, &err);
            if (err)
                return -1;
            if (vm->depth <= 0)
                return vm_fail(vm, "return outside of function call");
            vm->depth--;
            unsigned long long ret_pc = mget(vm, vm->fp, 8, &err);
            unsigned long long old_fp = 0;
            if (!err)
                old_fp = mget(vm, vm->fp + 8, 8, &err);
            if (err)
                return -1;
            vm->sp = vm->fp;
            vm->fp = old_fp;
            vm->pc = ret_pc;
            vpush(vm, v, &err);
            if (err)
                return -1;
            break;
        }
        case OP_ENTER: {
            unsigned long long nbytes = r_u32(vm, &err);
            if (err)
                return -1;
            if (vm->depth <= 0)
                return vm_fail(vm, "enter outside of function call");
            unsigned nargs = vm->frames[vm->depth - 1].nargs;
            unsigned long long start =
                vm->fp + 16 + 8 * (unsigned long long)nargs;
            if (nbytes > vm->memsize || start > vm->memsize - nbytes)
                return vm_fail(vm, "stack overflow");
            memset(vm->mem + start, 0, (size_t)nbytes);
            break;
        }
        case OP_SWAP: {
            unsigned sz = r_u8(vm, &err);
            unsigned long long b = 0, a = 0;
            if (!err)
                b = (unsigned long long)vpop(vm, &err);
            if (!err)
                a = (unsigned long long)vpop(vm, &err);
            if (err)
                return -1;
            if (sz == 0 || sz > 1048576)
                return vm_fail(vm, "bad swap size");
            if (!mem_ok(vm, a, sz) || !mem_ok(vm, b, sz))
                return vm_fail(vm, "out of bounds memory access");
            /* temp buffer makes self-swap safe by construction */
            unsigned char *tmp = (unsigned char *)malloc(sz);
            if (!tmp)
                return vm_fail(vm, "out of memory");
            memcpy(tmp, vm->mem + a, sz);
            memcpy(vm->mem + a, vm->mem + b, sz);
            memcpy(vm->mem + b, tmp, sz);
            free(tmp);
            break;
        }
        case OP_SYSCALL_PRINTF: {
            unsigned nargs = r_u8(vm, &err);
            if (err)
                return -1;
            if (nargs > 255 || vm->vsp < (size_t)nargs + 1)
                return vm_fail(vm, "value stack underflow");
            long long args[256];
            for (unsigned i = 0; i < nargs; i++)
                args[nargs - 1 - i] = vpop(vm, &err);
            unsigned long long faddr = 0;
            if (!err)
                faddr = (unsigned long long)vpop(vm, &err);
            if (err)
                return -1;
            if (vm_printf(vm, faddr, args, nargs, &err) != 0 || err)
                return -1;
            break;
        }
        case OP_SYSCALL_SCANF: {
            unsigned nargs = r_u8(vm, &err);
            if (err)
                return -1;
            if (nargs > 255 || vm->vsp < (size_t)2 * nargs + 1)
                return vm_fail(vm, "value stack underflow");
            unsigned long long addrs[256];
            long long maxlens[256];
            for (int i = (int)nargs - 1; i >= 0; i--) {
                maxlens[i] = vpop(vm, &err);
                if (!err)
                    addrs[i] = (unsigned long long)vpop(vm, &err);
                if (err)
                    return -1;
            }
            unsigned long long faddr = (unsigned long long)vpop(vm, &err);
            if (err)
                return -1;
            char fmt[4096];
            if (vm_read_cstr(vm, faddr, fmt, sizeof fmt) != 0)
                return vm_fail(vm, "scanf: bad format string address");
            int assigned = vm_scanf(vm, fmt, addrs, maxlens, nargs, &err);
            if (err || assigned < 0)
                return -1;
            vpush(vm, assigned, &err);
            if (err)
                return -1;
            break;
        }
        case OP_MEMCPY: {
            unsigned long long n = r_u32(vm, &err);
            unsigned long long src = 0, dst = 0;
            if (!err)
                src = (unsigned long long)vpop(vm, &err);
            if (!err)
                dst = (unsigned long long)vpop(vm, &err);
            if (err)
                return -1;
            if (!mem_ok(vm, src, n) || !mem_ok(vm, dst, n))
                return vm_fail(vm, "out of bounds memory access");
            memmove(vm->mem + dst, vm->mem + src, (size_t)n);
            break;
        }
        case OP_DUP: {
            if (vm->vsp == 0)
                return vm_fail(vm, "value stack underflow");
            long long v = vm->vstack[vm->vsp - 1];
            vpush(vm, v, &err);
            if (err)
                return -1;
            break;
        }
        case OP_XCHG: {
            if (vm->vsp < 2)
                return vm_fail(vm, "value stack underflow");
            long long t = vm->vstack[vm->vsp - 1];
            vm->vstack[vm->vsp - 1] = vm->vstack[vm->vsp - 2];
            vm->vstack[vm->vsp - 2] = t;
            break;
        }
        case OP_RESERVE: {
            unsigned long long n = r_u32(vm, &err);
            if (err)
                return -1;
            if (n > vm->sp - vm->globals_end)
                return vm_fail(vm, "stack overflow");
            vm->sp -= n;
            break;
        }
        case OP_RELEASE: {
            unsigned long long n = r_u32(vm, &err);
            if (err)
                return -1;
            if (vm->sp + n > vm->memsize)
                return vm_fail(vm, "internal error: stack pointer escaped");
            vm->sp += n;
            break;
        }
        default:
            return vm_fail(vm, "invalid opcode 0x%02x", op);
        }
    }
}

/* ------------------------------------------------------------------ */
/* disassembler                                                        */
/* ------------------------------------------------------------------ */

static const char *op_name(unsigned op)
{
    switch (op) {
    case OP_HALT: return "HALT";
    case OP_PUSH_IMM: return "PUSH_IMM";
    case OP_POP: return "POP";
    case OP_LOAD_LOCAL: return "LOAD_LOCAL";
    case OP_STORE_LOCAL: return "STORE_LOCAL";
    case OP_LOAD_GLOBAL: return "LOAD_GLOBAL";
    case OP_STORE_GLOBAL: return "STORE_GLOBAL";
    case OP_ADDR_LOCAL: return "ADDR_LOCAL";
    case OP_ADDR_GLOBAL: return "ADDR_GLOBAL";
    case OP_LOAD_IND: return "LOAD_IND";
    case OP_STORE_IND: return "STORE_IND";
    case OP_ADD: return "ADD";
    case OP_SUB: return "SUB";
    case OP_MUL: return "MUL";
    case OP_DIV: return "DIV";
    case OP_MOD: return "MOD";
    case OP_NEG: return "NEG";
    case OP_AND: return "AND";
    case OP_OR: return "OR";
    case OP_XOR: return "XOR";
    case OP_NOT: return "NOT";
    case OP_SHL: return "SHL";
    case OP_SHR: return "SHR";
    case OP_CMP_EQ: return "CMP_EQ";
    case OP_CMP_NE: return "CMP_NE";
    case OP_CMP_LT: return "CMP_LT";
    case OP_CMP_LE: return "CMP_LE";
    case OP_CMP_GT: return "CMP_GT";
    case OP_CMP_GE: return "CMP_GE";
    case OP_JMP: return "JMP";
    case OP_JZ: return "JZ";
    case OP_JNZ: return "JNZ";
    case OP_CALL: return "CALL";
    case OP_RET: return "RET";
    case OP_ENTER: return "ENTER";
    case OP_SWAP: return "SWAP";
    case OP_SYSCALL_PRINTF: return "SYSCALL_PRINTF";
    case OP_SYSCALL_SCANF: return "SYSCALL_SCANF";
    case OP_MEMCPY: return "MEMCPY";
    case OP_DUP: return "DUP";
    case OP_XCHG: return "XCHG";
    case OP_RESERVE: return "RESERVE";
    case OP_RELEASE: return "RELEASE";
    default: return "?";
    }
}

void vm_disasm(unsigned char *code, size_t len)
{
    size_t pc = 0;
    while (pc < len) {
        size_t off = pc;
        unsigned op = code[pc++];
        printf("%6zu: %-14s", off, op_name(op));
        unsigned long long u;
        switch (op) {
        case OP_PUSH_IMM:
            u = 0;
            for (int i = 0; i < 8 && pc < len; i++)
                u |= (unsigned long long)code[pc++] << (unsigned)(8 * i);
            printf("%lld", (long long)u);
            break;
        case OP_JMP:
        case OP_JZ:
        case OP_JNZ:
        case OP_ADDR_GLOBAL:
        case OP_LOAD_GLOBAL:
        case OP_STORE_GLOBAL:
            u = 0;
            for (int i = 0; i < 8 && pc < len; i++)
                u |= (unsigned long long)code[pc++] << (unsigned)(8 * i);
            printf("%llu", u);
            break;
        case OP_CALL:
            u = 0;
            for (int i = 0; i < 8 && pc < len; i++)
                u |= (unsigned long long)code[pc++] << (unsigned)(8 * i);
            printf("%llu", u);
            if (pc < len)
                printf(", %u", code[pc++]);
            break;
        case OP_LOAD_LOCAL:
        case OP_STORE_LOCAL:
        case OP_ADDR_LOCAL: {
            unsigned long long o = 0;
            for (int i = 0; i < 4 && pc < len; i++)
                o |= (unsigned long long)code[pc++] << (unsigned)(8 * i);
            printf("%llu", o);
            if (pc < len && (op == OP_LOAD_LOCAL || op == OP_STORE_LOCAL))
                printf(", %u", code[pc++]);
            break;
        }
        case OP_LOAD_IND:
        case OP_STORE_IND:
        case OP_SWAP:
        case OP_SYSCALL_PRINTF:
        case OP_SYSCALL_SCANF:
            if (pc < len)
                printf("%u", code[pc++]);
            break;
        case OP_ENTER:
        case OP_MEMCPY:
        case OP_RESERVE:
        case OP_RELEASE: {
            unsigned long long o = 0;
            for (int i = 0; i < 4 && pc < len; i++)
                o |= (unsigned long long)code[pc++] << (unsigned)(8 * i);
            printf("%llu", o);
            break;
        }
        default:
            break;
        }
        printf("\n");
    }
}
