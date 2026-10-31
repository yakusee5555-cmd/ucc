#define _POSIX_C_SOURCE 200809L
#include "diag.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void diag_init(struct DiagList *d)
{
    d->items = NULL;
    d->count = 0;
    d->cap = 0;
    d->has_error = 0;
}

static void diag_push(struct DiagList *d, const char *file, int line, int column,
                      enum DiagSev sev, const char *msg)
{
    if (d->count == d->cap) {
        int ncap = d->cap ? d->cap * 2 : 16;
        struct Diag *ni = (struct Diag *)realloc(d->items,
                                                (size_t)ncap * sizeof *ni);
        if (!ni)
            abort();
        d->items = ni;
        d->cap = ncap;
    }
    struct Diag *it = &d->items[d->count++];
    it->file = file ? strdup(file) : strdup("<unknown>");
    it->line = line;
    it->column = column;
    it->severity = sev;
    it->message = strdup(msg);
    if (sev == DIAG_ERROR)
        d->has_error = 1;
}

static void diag_vadd(struct DiagList *d, const char *file, int line, int column,
                      enum DiagSev sev, const char *fmt, va_list ap)
{
    char buf[1024];
    vsnprintf(buf, sizeof buf, fmt, ap);
    diag_push(d, file, line, column, sev, buf);
}

void diag_add(struct DiagList *d, const char *file, int line, int column,
              enum DiagSev sev, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    diag_vadd(d, file, line, column, sev, fmt, ap);
    va_end(ap);
}

void diag_warn_at(struct DiagList *d, const char *file, int line, int column,
                  const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    diag_vadd(d, file, line, column, DIAG_WARN, fmt, ap);
    va_end(ap);
}

void diag_error_at(struct DiagList *d, const char *file, int line, int column,
                   const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    diag_vadd(d, file, line, column, DIAG_ERROR, fmt, ap);
    va_end(ap);
}

void diag_print_human(struct DiagList *d)
{
    for (int i = 0; i < d->count; i++) {
        struct Diag *it = &d->items[i];
        fprintf(stderr, "%s:%d:%d: %s: %s\n", it->file, it->line, it->column,
                it->severity == DIAG_ERROR ? "error" : "warning", it->message);
    }
}

static void json_escape(FILE *f, const char *s)
{
    fputc('"', f);
    for (; *s; s++) {
        switch (*s) {
        case '"': fputs("\\\"", f); break;
        case '\\': fputs("\\\\", f); break;
        case '\n': fputs("\\n", f); break;
        case '\r': fputs("\\r", f); break;
        case '\t': fputs("\\t", f); break;
        default:
            if ((unsigned char)*s < 0x20)
                fprintf(f, "\\u%04x", (unsigned)*s);
            else
                fputc(*s, f);
        }
    }
    fputc('"', f);
}

void diag_print_json(struct DiagList *d)
{
    printf("[");
    for (int i = 0; i < d->count; i++) {
        struct Diag *it = &d->items[i];
        if (i)
            printf(",");
        printf("{\"file\":");
        json_escape(stdout, it->file);
        printf(",\"line\":%d,\"column\":%d,\"severity\":\"%s\",\"message\":",
               it->line, it->column,
               it->severity == DIAG_ERROR ? "error" : "warning");
        json_escape(stdout, it->message);
        printf("}");
    }
    printf("]\n");
}

void diag_free(struct DiagList *d)
{
    for (int i = 0; i < d->count; i++) {
        free(d->items[i].file);
        free(d->items[i].message);
    }
    free(d->items);
    d->items = NULL;
    d->count = 0;
    d->cap = 0;
}
