#ifndef FE_DIAG_H
#define FE_DIAG_H

#include <stdio.h>

typedef struct FeLoc {
    const char *file;
    unsigned long line;
    unsigned long col;
} FeLoc;

typedef struct FeDiags {
    unsigned long errors;
    unsigned long warnings;
    const char *source;
    unsigned long source_len;
} FeDiags;

/* Stream diagnostics are written to.  Defaults to stderr; returns stdout when
   FE_DIAG_STDOUT is set in the environment.  DOS offers no way to redirect
   handle 2 -- COMMAND.COM understands ">" and nothing else -- so under the test
   runner every error message would otherwise be written straight to the screen
   and lost.  Interactive use is unaffected: both streams reach the console. */
FILE *fe_diag_stream(void);

void fe_diags_init(FeDiags *d, const char *source, unsigned long source_len);

/* Point the excerpt printer at a different file. A build spans several
   units, and an excerpt drawn from the wrong buffer is worse than none. */
void fe_diags_source(FeDiags *d, const char *source, unsigned long source_len);
void fe_diag_error(FeDiags *d, FeLoc loc, const char *msg);
void fe_diag_errorf(FeDiags *d, FeLoc loc, const char *msg, const char *arg);
void fe_diag_note(FeLoc loc, const char *msg);
void fe_diag_note_src(FeDiags *d, FeLoc loc, const char *msg);

#endif
