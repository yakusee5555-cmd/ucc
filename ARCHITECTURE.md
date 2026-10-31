# ucc — Architecture Blueprint

`ucc` is a self-contained C-subset compiler and stack-based virtual machine.
One binary (`build/ucc`) runs the full pipeline: preprocess → lex → parse →
semantic analysis → bytecode codegen → VM execution. Written in C11, zero
dependencies beyond libc.

## 1. Pipeline stages

```
source.c
  │  pp.c        text-to-text preprocessor (line-preserving)
  ▼
tokens           lexer.c → Token[] (lexed up-front for lookahead)
  │  parser.c    recursive descent + Pratt expressions, panic-mode recovery
  ▼
AST              ast.h / ast.c — arena-allocated nodes
  │  sema.c      3-phase semantic analysis (register, define, check)
  ▼
bytecode         codegen.c → flat byte buffer, labels patched
  │  vm.c        stack-machine interpreter
  ▼
exit code        main()'s return value, clamped to 0..255
```

CLI flags select how far the pipeline runs: `--dump-tokens` stops after
lexing, `--dump-ast` after parsing, `--dump-bytecode` after codegen,
`--json-diagnostics` after sema (diagnostics only, no execution).

## 2. Preprocessor (pp.c)

Text-to-text, newline-preserving (a removed line becomes a blank line) so all
later stages report correct line numbers.

- `#include <stdio.h|stdlib.h|string.h>` — accepted, no-op (intrinsics need no
  prototypes). `#include <stdbool.h>` injects `bool`/`true`/`false` macros.
  `#include <stddef.h>` injects `NULL` and `size_t`. Unknown system headers
  are a compile error.
- `#include "file"` — loaded relative to the including file, max depth 16.
- `#define` object-like and function-like macros (with `#`/`##` *not*
  supported — out of subset), `#undef`.
- `#ifdef` / `#ifndef` / `#if` / `#elif` / `#else` / `#endif`. `#if`
  conditions support `defined(X)` and integer constant expressions
  (`+ - * / % << >> & | ^ == != < <= > >= ! ~ && ||`, parentheses).
- `#error` — hard compile error. Unknown directives — warning, ignored.

## 3. Lexer (lexer.c)

Produces `Token{kind, line, column, text, ival, fval}`. Keywords include the
full C set relevant to the subset (`int char void struct if else while for do
switch case default break continue return sizeof`). `float`/`double` lex as
keywords; floating literals lex as `TK_FLOAT` with a truncation warning at
parse time. Integer literals support decimal, `0x` hex, octal, and `u/l`
suffixes (ignored). Char literals handle standard escapes; string literals
handle escapes and are interned to VM globals later.

## 4. Grammar (parser.c)

Recursive descent with a Pratt expression parser (binding powers). Panic-mode
recovery: on error inside a block, skip to `;`/`}`; at top level, skip to `;`
or balanced `}`. Recovery triggers on **errors only** — warnings never discard
nodes. The parser never segfaults on malformed input (all advances are
bounds-checked against the token array).

```
program      := external*
external     := struct_def ';' | funcdef | prototype | global_var_decl ';'
funcdef      := declspec declarator '(' params ')' block
statement    := block | if_stmt | while_stmt | do_stmt | for_stmt
              | switch_stmt | break ';' | continue ';' | return [expr] ';'
              | swap_stmt | expr ';' | var_decl
swap_stmt    := 'swap' '(' expr ',' expr ')' ';'   // ident text must be "swap"
var_decl     := declspec declarator (',' declarator)* ';'
declarator   := '*'* ident ('[' const_expr? ']')* ('=' initializer)?
initializer  := expr | '{' initializer (',' initializer)* [','] '}'
```

Expressions (precedence, low → high):
`= += -= *= /= %=  →  ?:  →  ||  →  &&  →  |  →  ^  →  &  →  == !=  →
< <= > >=  →  << >>  →  + -  →  * / %  →  unary - ! ~ * & ++ -- (prefix)  →
postfix ++ --  →  call, index, member (. ->), cast`.

Multi-dimensional arrays: `int m[2][3]` collects dims `[2,3]` then builds the
type inside-out in reverse → `array[2] of array[3] of int`, so `m[i][j]` =
`*(m + i*3 + j)`. Non-literal constant dims (`int a[2+3]`, `int a[N]` with
`#define N`) are const-evaluated by sema.

## 5. Type system (ast.h / sema.c)

| Type | Size | Notes |
|---|---|---|
| `int` | 8 bytes | 64-bit signed, the only arithmetic type |
| `char` | 1 byte | zero-extended to 64-bit on load |
| `void` | 8 (placeholder) | only as return type |
| `float`/`double` | 8 bytes | accepted syntactically, stored as int64; float literals truncated with a warning |
| `long`/`short`/`unsigned`/`signed` | 8 bytes | accepted as aliases of `int` (signedness ignored — all arithmetic is signed 64-bit) |
| pointer (any depth) | 8 bytes | byte address into VM memory |
| array | `elem_size × count` | packed, no padding |
| struct | `8 × nfields` | every field 8-byte aligned; incomplete until defined |

Sema runs in three phases: (1) register all globals/functions/structs,
(2) define function bodies with params+locals, (3) full body checking.
Array-to-pointer decay is recorded per-node (`decayed_type`) without mutating
the original type, so codegen can still distinguish "address of array" from
"load pointer". `sizeof` works on types and expressions (sema precomputes
`size_val`; structs report `8 × nfields`).

`swap(a,b)` intrinsic: parsed only in statement position when the identifier
text is exactly `swap` followed by `(`. Sema requires both operands to be
modifiable l-values of identical byte size, else a precise compile error.
Codegen emits both addresses + `OP_SWAP size`.

`printf`/`scanf` are variadic intrinsics (no prototypes). `printf` supports
`%d %i %u %x %X %c %s %p %% %f %lf` (`%f` prints `<n>.000000`; `%n` is a
runtime error). `scanf` requires every argument after the format to be
`&lvalue` or pointer-typed — a bare l-value is the compile error
`scanf argument must be a pointer — did you forget '&'?`. `%c` does not skip
whitespace; `%s` is bounded by the known array size (or 4096).

## 6. Bytecode / opcode reference (codegen.c)

1-byte opcode, little-endian operands. All runtime values are int64.

| Op | Encoding | Effect |
|---|---|---|
| HALT 0x00 | — | stop; top of value stack = exit code |
| PUSH_IMM 0x01 | i64 | push constant |
| POP 0x02 | — | discard top |
| LOAD_LOCAL 0x03 | u32 off, u8 size | push zero-extended `mem[fp+16+off, size)` |
| STORE_LOCAL 0x04 | u32 off, u8 size | pop v; store low `size` bytes |
| LOAD_GLOBAL 0x05 | u64 addr, u8 size | push zero-extended `mem[addr, size)` |
| STORE_GLOBAL 0x06 | u64 addr, u8 size | pop v; store low bytes |
| ADDR_LOCAL 0x07 | u32 off | push `fp+16+off` |
| ADDR_GLOBAL 0x08 | u64 addr | push addr |
| LOAD_IND 0x09 | u8 size | `a=pop`; push zero-extended `mem[a,size)` |
| STORE_IND 0x0A | u8 size | `v=pop; a=pop`; store low bytes at `mem[a]` |
| ADD 0x0B SUB 0x0C MUL 0x0D | — | wrapping i64 |
| DIV 0x0E MOD 0x0F | — | divisor 0 → runtime error |
| NEG 0x10 | — | arithmetic negation |
| AND 0x11 OR 0x12 XOR 0x13 NOT 0x14 | — | bitwise |
| SHL 0x15 SHR 0x16 | — | count masked with 63; SHR arithmetic |
| CMP_EQ…CMP_GE 0x17–0x1C | — | signed compare, push 1/0 |
| JMP 0x1D / JZ 0x1E / JNZ 0x1F | u64 target | absolute bytecode offsets |
| CALL 0x20 | u64 addr, u8 nargs | push ret_pc, old_fp; `fp=sp`; `pc=addr` |
| RET 0x21 | — | pop v; `sp=fp`; restore fp/ret_pc; push v |
| ENTER 0x22 | u32 nbytes | `sp-=nbytes`; zero frame; stack-overflow check |
| SWAP 0x23 | u8 size | `b=pop addr; a=pop addr`; exchange via temp buffer (self-swap safe) |
| SYSCALL_PRINTF 0x24 | u8 nargs | pop args + fmt addr; render (1MB cap) |
| SYSCALL_SCANF 0x25 | u8 nargs | pop (addr,maxlen) pairs + fmt; push items assigned |
| MEMCPY 0x26 | u32 size | `src=pop; dst=pop`; bounds-checked copy (struct assign) |
| DUP 0x27 | — | duplicate top of stack |
| XCHG 0x28 | — | swap top two stack slots |
| RESERVE 0x29 | u32 nbytes | `sp-=nbytes` (caller reserves callee frame) |
| RELEASE 0x2A | u32 nbytes | `sp+=nbytes` |

Codegen details: the caller reserves the callee's locals (`RESERVE` before
`CALL`, `RELEASE` after), so `ADDR_LOCAL off` = `fp+16+off` is uniform for
params and locals. Four hidden 8-byte temp slots per frame: slot 0 =
assignment lhs address / switch discriminant, slot 2/3 = `++`/`--` address and
old value (never live across slot 0). Short-circuit `&&`/`||`, ternary, and
loop control use patched forward jumps. String literals are interned to
globals (deduped, NUL-terminated). Calls leave exactly one value on the stack
(void → 0; statement calls get a `POP`).

## 7. VM execution + safety model (vm.c)

- **Memory**: one flat byte-addressable `mem` (default 8MB, `--mem-size MB`).
  Globals grow up from address 0 (8-byte aligned, string literals included).
  The call stack grows down from the top: frame = 16-byte header
  (`ret_pc:u64`, `old_fp:u64`) + params at `fp+16+8*i` + locals.
- **Value stack**: separate int64 stack, 1M entries, heap-allocated.
- **Startup**: offset 0 emits `RESERVE <main frame>; CALL main,0; RELEASE;
  HALT`. `ENTER` zeroes locals, so uninitialized variables read as 0.
- **Safety checks** (all → `runtime error: <msg>` on stderr, exit 2):
  - instruction step limit (default 10,000,000, `--max-steps N`):
    `instruction step limit exceeded (possible infinite loop)`
  - call depth 10000: `call stack depth exceeded`
  - every `mem` read/write bounds-checked: `out of bounds memory access`
  - `ENTER` against `globals_end`: `stack overflow`
  - value-stack overflow/underflow checked
  - `DIV`/`MOD` by zero: `division by zero`
  - `SWAP` size > 1MB: allocation error; both ranges bounds-checked
  - `printf` `%n`: `format specifier %n is not supported`
  - stdout buffered, capped at 1MB with a truncation note
- **I/O**: program stdout → process stdout; `scanf` tokenizes process stdin
  (VM flushes stdout before blocking reads).

## 8. CLI (main.c)

`ucc [options] file.c` — compile errors: `file.c:3:5: error: message` on
stderr, exit 1. Runtime errors: `runtime error: message`, exit 2.

| Flag | Effect |
|---|---|
| `--dump-tokens` | `line:col KIND 'lexeme'` per token |
| `--dump-ast` | AST as JSON (`{"kind","line","column",…}`) |
| `--dump-bytecode` | `offset: MNEMONIC operands` disassembly |
| `--json-diagnostics` | `[{"file","line","column","severity","message"}]` |
| `--max-steps N` | step limit (default 10000000) |
| `--mem-size MB` | VM memory (default 8) |
| `--help` | usage |

## 9. Web IDE topology (summary)

The binary is CLI-first and IDE-friendly by design: `--dump-tokens`,
`--dump-ast`, and `--json-diagnostics` give a web frontend everything needed
for syntax highlighting, structure views, and error squiggles without running
code. A browser IDE can ship the compiler via WASI/Emscripten (the codebase
is plain C11 with no platform-specific calls beyond libc) or proxy to a
server-side `build/ucc` sandbox; `--dump-bytecode` feeds a bytecode inspector
pane, and `--max-steps`/`--mem-size` bound untrusted programs.

## 10. Packaging layout

```
~/workspace/ucc/
  src/            arena.h/c diag.h/c lexer.h/c pp.h/c ast.h/c
                  parser.h parser.c sema.h/c codegen.h/c vm.h/c main.c
  build/ucc       the binary (gitignored; see Makefile)
  Makefile        `make` runs: gcc -std=c11 -O2 -Wall -Wextra -Isrc src/*.c -o build/ucc
  ARCHITECTURE.md this file
```

Build: `gcc -std=c11 -O2 -Wall -Wextra -Isrc src/*.c -o build/ucc`
(zero warnings required). No `typedef enum` anywhere in the codebase.
