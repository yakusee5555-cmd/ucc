#ifndef UCC_DIAG_H
#define UCC_DIAG_H

/* Structured compiler diagnostics with exact source locations.
 * Collected in a list so the CLI can print them as text or as JSON
 * (for the web IDE's error squiggles). */

enum DiagSev {
    DIAG_WARN = 0,
    DIAG_ERROR = 1
};

struct Diag {
    char *file;
    int line;
    int column;
    enum DiagSev severity;
    char *message;
};

struct DiagList {
    struct Diag *items;
    int count;
    int cap;
    int has_error;
};

void diag_init(struct DiagList *d);
void diag_add(struct DiagList *d, const char *file, int line, int column,
              enum DiagSev sev, const char *fmt, ...);
void diag_warn_at(struct DiagList *d, const char *file, int line, int column,
                  const char *fmt, ...);
void diag_error_at(struct DiagList *d, const char *file, int line, int column,
                   const char *fmt, ...);
void diag_print_human(struct DiagList *d);
void diag_print_json(struct DiagList *d);
void diag_free(struct DiagList *d);

#endif
