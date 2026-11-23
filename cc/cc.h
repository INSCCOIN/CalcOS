/* CalcOS on-device C compiler.
 *
 * Single pass: the parser emits Thumb-2 as it goes, with no AST and no
 * intermediate representation. That keeps peak memory close to the size of
 * the symbol table, which is what matters on a 520 KB part.
 *
 * Value model: every expression lands in the accumulator, which is r0 for
 * integers and pointers and r0:r1 for doubles. Binary operators push the
 * left operand, evaluate the right, then pop. Temporaries are always pushed
 * 8 bytes at a time so the stack stays 8-byte aligned for AAPCS calls.
 */
#ifndef CC_H
#define CC_H

#include <stdint.h>
#include <stddef.h>
#include <setjmp.h>
#include "emit.h"

#define CC_MAXNAME   48
#define CC_MAXARGS   16
#define CC_MAXINC     8
#define CC_MAXPARAMS 12
#define CC_MAXCASES  64
#define CC_MAXBREAK  32

/* ------------------------------------------------------------------ */
/* types                                                               */
/* ------------------------------------------------------------------ */

typedef enum {
    T_VOID, T_CHAR, T_UCHAR, T_SHORT, T_USHORT,
    T_INT, T_UINT, T_LLONG, T_ULLONG, T_FLOAT, T_DOUBLE,
    T_PTR, T_ARRAY, T_STRUCT, T_UNION, T_FUNC
} TKind;

typedef struct Type Type;
typedef struct Member Member;

struct Type {
    TKind   kind;
    int     size;        /* sizeof        */
    int     align;
    Type   *to;          /* pointer/array element, function return */
    int     nelem;       /* array length, -1 if unspecified         */
    Member *members;     /* struct/union                            */
    char    tag[CC_MAXNAME];
    int     complete;
    /* T_FUNC: the signature, so a call through a function pointer can
     * convert its arguments exactly as a direct call does */
    Type  **ptypes;
    int     nparams;
    int     variadic;
    Type   *next;        /* allocation chain                        */
};

struct Member {
    char    name[CC_MAXNAME];
    Type   *type;
    int     offset;
    Member *next;
};

/* ------------------------------------------------------------------ */
/* symbols                                                            */
/* ------------------------------------------------------------------ */

typedef enum { S_LOCAL, S_GLOBAL, S_FUNC, S_EXTERN, S_ENUM, S_TYPEDEF } SKind;

/* a call site waiting for its target address to become known */
typedef struct Fixup {
    int           at;    /* offset of the movw of a movw/movt pair */
    int           reg;   /* register the pair loads into            */
    struct Fixup *next;
} Fixup;

typedef struct Sym {
    char   name[CC_MAXNAME];
    SKind  kind;
    Type  *type;
    int    offset;       /* S_LOCAL: frame offset                    */
    uint32_t addr;       /* S_GLOBAL/S_FUNC/S_EXTERN: absolute addr  */
    int    enumval;
    int    defined;
    int    code_off;     /* S_FUNC: offset in the code buffer        */
    Fixup *fixups;       /* forward calls to patch                   */
    /* signature, used to convert arguments at a call and to let the
     * calculator call compiled functions */
    int    nparams;
    int    param_dbl;    /* 1 if every parameter is a double         */
    int    ret_dbl;
    int    variadic;
    Type **ptypes;       /* nparams entries, NULL if not known       */
    struct Sym *next;
} Sym;

/* ------------------------------------------------------------------ */
/* tokens                                                             */
/* ------------------------------------------------------------------ */

enum {
    TK_EOF = 0, TK_NUM, TK_DNUM, TK_STR, TK_CHARLIT, TK_IDENT, TK_PUNCT,
    TK_KW
};

/* keywords */
enum {
    KW_INT = 1, KW_CHAR, KW_SHORT, KW_LONG, KW_UNSIGNED, KW_SIGNED,
    KW_FLOAT, KW_DOUBLE, KW_VOID, KW_STRUCT, KW_UNION, KW_ENUM,
    KW_IF, KW_ELSE, KW_WHILE, KW_FOR, KW_DO, KW_RETURN, KW_BREAK,
    KW_CONTINUE, KW_SWITCH, KW_CASE, KW_DEFAULT, KW_SIZEOF, KW_TYPEDEF,
    KW_STATIC, KW_EXTERN, KW_CONST, KW_GOTO
};

/* punctuation, multi-character ones get their own codes */
enum {
    P_ARROW = 256, P_INC, P_DEC, P_SHL, P_SHR, P_LE, P_GE, P_EQ, P_NE,
    P_ANDAND, P_OROR, P_ADDEQ, P_SUBEQ, P_MULEQ, P_DIVEQ, P_MODEQ,
    P_ANDEQ, P_OREQ, P_XOREQ, P_SHLEQ, P_SHREQ, P_ELLIPSIS
};

typedef struct {
    int      kind;
    int      val;        /* TK_PUNCT code, TK_KW id, TK_NUM value */
    long long llval;     /* TK_NUM, full width                    */
    int      is_ll;      /* the literal needs 64 bits             */
    int      is_uns;     /* the literal is unsigned               */
    double   dval;
    char     name[CC_MAXNAME];
    char    *str;        /* string literal contents */
    int      slen;
    int      line;
} Token;

/* ------------------------------------------------------------------ */
/* compiler state                                                     */
/* ------------------------------------------------------------------ */

typedef struct Macro {
    char   name[CC_MAXNAME];
    char  *body;
    int    nparams;
    char   params[CC_MAXPARAMS][CC_MAXNAME];
    int    funclike;
    struct Macro *next;
} Macro;

typedef struct InBuf {
    const char *p, *end;
    const char *fname;
    int    line;
    char  *owned;        /* freed when popped, if non-null */
    Macro *macro;        /* macro being expanded, for recursion guard */
} InBuf;

/* A symbol the firmware exports to compiled code. The address is kept as a
 * pointer so the table can be a static initializer; it is narrowed to 32
 * bits where it is emitted, which is exact on the target. */
typedef struct {
    const char *name;
    void       *fn;
    const char *decl;    /* return type: "int", "double" or "void" */
} RtSym;

typedef struct CC {
    /* input */
    InBuf  in[CC_MAXINC];
    int    nin;
    Token  tok, ahead;
    int    has_ahead;

    /* output */
    Emit   code;
    uint8_t *data;
    int    data_len, data_cap;
    uint32_t data_base;

    /* tables */
    Sym   *globals, *locals;
    Type  *types;        /* allocation chain */
    Type  *tags;         /* struct/union/enum tags */
    Macro *macros;

    /* current function */
    Sym   *curfn;
    int    frame;        /* bytes of locals allocated so far */
    int    maxframe;
    int    prologue_at;  /* patch site for 'subw sp, sp, #frame' */
    int    epilogue_at;  /* patch site for 'addw sp, sp, #frame' */
    int    epilogue_lbl;
    int    rettype_dbl;

    /* loop/switch context */
    int    brk[CC_MAXBREAK], nbrk;
    int    cont_target;
    int    brk_base;
    int    ret[CC_MAXBREAK], nret;   /* 'return' branches awaiting the epilogue */

    /* diagnostics */
    jmp_buf jb;
    char   err[160];
    int    errline;
    int    nerr;

    /* allocation */
    void  *arena;
    size_t arena_used, arena_cap;

    const RtSym *rt;
    int    nrt;

    /* #include: the compiler has no filesystem of its own, so the host
     * supplies one. The callback returns the file's length and fills buf,
     * or -1 if there is no such file. */
    int  (*readfile)(void *ud, const char *name, char *buf, int cap);
    void  *readfile_ud;
    char   incnames[CC_MAXINC][CC_MAXNAME];   /* guards include cycles */
    int    ninc;
} CC;

/* api */
int   cc_compile(CC *cc, const char *src, const char *fname,
                 uint8_t *code, int code_cap, uint32_t code_base,
                 uint8_t *data, int data_cap, uint32_t data_base,
                 const RtSym *rt, int nrt);
Sym  *cc_find_func(CC *cc, const char *name);
void  cc_error(CC *cc, const char *fmt, ...);

/* internals shared between lex.c and gen.c */
void  lex_init(CC *cc, const char *src, const char *fname);
void  lex_next(CC *cc);
Token *lex_peek(CC *cc);
void *cc_alloc(CC *cc, size_t n);
char *cc_strdup(CC *cc, const char *s, int n);

#endif
