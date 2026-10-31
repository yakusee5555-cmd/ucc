#ifndef UCC_PP_H
#define UCC_PP_H

#include "diag.h"

/* Text-to-text preprocessor. Returns malloc'd preprocessed source with all
 * newlines preserved so downstream line numbers stay exact. Handles
 * #include (virtualized standard headers + relative files), #define
 * (object-like and function-like), #undef, #ifdef/#ifndef/#if/#elif/#else/
 * #endif with defined() and integer constant expressions, and #error.
 * Unknown directives produce a warning and are ignored. */
char *pp_process(const char *filename, const char *src, struct DiagList *diags);

#endif
