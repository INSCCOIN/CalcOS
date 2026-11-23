/* Compiled C programs: compiling, running, and calling into them from the
 * calculator. */
#ifndef CPROG_H
#define CPROG_H

#include <stdint.h>
#include <stddef.h>
#include "cc.h"

typedef struct CProg CProg;

/* Compile source text. Returns 0 on success; on failure the message and
 * line are left in cprog_error()/cprog_errline(). */
int         cprog_compile(const char *src, const char *fname);
const char *cprog_error(void);
int         cprog_errline(void);

/* true once a program has been compiled successfully */
int         cprog_loaded(void);
const char *cprog_name(void);
void        cprog_unload(void);

/* run main(), returning its exit value (0 if there is no main) */
int         cprog_run_main(void);

/* list the functions the program defines */
int         cprog_nfuncs(void);
const char *cprog_func_name(int i);
int         cprog_func_nargs(int i);

/* Call a compiled function from the calculator. args are doubles; they are
 * converted to int for a function whose parameters are ints. Returns 0 if
 * the call could be made, -1 if the name is unknown, -2 if the signature
 * cannot be bridged, -3 on an argument-count mismatch. */
int         cprog_call(const char *name, const double *args, int nargs,
                       double *result);

/* bytes of code and data the loaded program occupies */
int         cprog_code_size(void);
int         cprog_data_size(void);

/* Save a compiled program, and bring one back.
 *
 * A snapshot holds the machine code, the data segment and the function
 * table, stamped with the firmware build and the addresses the code was
 * assembled against. cprog_restore refuses anything from a different
 * build, because compiled code carries absolute addresses into this
 * firmware image and a stale one would jump into the wrong place. */
enum { CPROG_RESTORE_OK = 0, CPROG_RESTORE_BAD = -1, CPROG_RESTORE_BUILD = -2 };

int         cprog_snapshot_size(void);
int         cprog_snapshot(void *out, int cap);
int         cprog_restore(const void *in, int len);

/* The runtime symbol table, exposed so host tools compile against exactly
 * what the firmware provides rather than a copy of it. */
const RtSym *cprog_rt_table(int *count);

#endif