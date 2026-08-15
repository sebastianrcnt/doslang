#include "diag.h"

void fe_diag_error(FeDiags *d, FeLoc loc, const char *msg)
{
    d->errors++;
    fprintf(stderr, "%s:%lu:%lu: error: %s\n", loc.file ? loc.file : "<source>", loc.line, loc.col, msg);
}

void fe_diag_errorf(FeDiags *d, FeLoc loc, const char *msg, const char *arg)
{
    d->errors++;
    fprintf(stderr, "%s:%lu:%lu: error: ", loc.file ? loc.file : "<source>", loc.line, loc.col);
    fprintf(stderr, msg, arg);
    fputc('\n', stderr);
}

void fe_diag_note(FeLoc loc, const char *msg)
{
    fprintf(stderr, "%s:%lu:%lu: note: %s\n", loc.file ? loc.file : "<source>", loc.line, loc.col, msg);
}
