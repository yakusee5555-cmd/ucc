#ifndef UCC_ARENA_H
#define UCC_ARENA_H

#include <stddef.h>

/* Bump allocator: all AST / type / token memory comes from here so a whole
 * compilation unit can be freed with a single arena_free(). No per-node
 * free is ever needed (zero-overhead cleanup between compilations). */
struct Arena {
    char *base;
    size_t cap;
    size_t off;
};

void arena_init(struct Arena *a);
void *arena_alloc(struct Arena *a, size_t n);
char *arena_strdup(struct Arena *a, const char *s);
char *arena_strndup(struct Arena *a, const char *s, size_t n);
void arena_reset(struct Arena *a);
void arena_free(struct Arena *a);

#endif
