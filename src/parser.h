#ifndef UCC_PARSER_H
#define UCC_PARSER_H

#include "arena.h"
#include "ast.h"
#include "diag.h"

/* Parse preprocessed source into an AST (list of external declarations
 * chained via Node.next). Never crashes on malformed input: diagnostics
 * are collected and parsing recovers in panic mode. */
struct Node *parse_source(const char *src, const char *file,
                          struct Arena *arena, struct DiagList *diags);

#endif
