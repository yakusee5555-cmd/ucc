#include "arena.h"

#include <stdlib.h>
#include <string.h>

#define ARENA_INIT_CAP 65536u

void arena_init(struct Arena *a)
{
    a->base = NULL;
    a->cap = 0;
    a->off = 0;
}

static void arena_grow(struct Arena *a, size_t need)
{
    size_t ncap = a->cap ? a->cap : ARENA_INIT_CAP;
    while (ncap < a->off + need)
        ncap *= 2;
    char *nbase = (char *)realloc(a->base, ncap);
    if (!nbase) {
        /* Out of memory: nothing sensible to do in a bump allocator. */
        abort();
    }
    a->base = nbase;
    a->cap = ncap;
}

void *arena_alloc(struct Arena *a, size_t n)
{
    /* Keep every allocation 8-byte aligned so structs stay aligned. */
    size_t aligned = (n + 7u) & ~(size_t)7u;
    if (a->off + aligned > a->cap)
        arena_grow(a, aligned);
    void *p = a->base + a->off;
    a->off += aligned;
    return p;
}

char *arena_strdup(struct Arena *a, const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = (char *)arena_alloc(a, n);
    memcpy(p, s, n);
    return p;
}

char *arena_strndup(struct Arena *a, const char *s, size_t n)
{
    char *p = (char *)arena_alloc(a, n + 1);
    memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

void arena_reset(struct Arena *a)
{
    a->off = 0;
}

void arena_free(struct Arena *a)
{
    free(a->base);
    a->base = NULL;
    a->cap = 0;
    a->off = 0;
}
