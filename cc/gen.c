/* Parser and Thumb-2 code generator.
 *
 * Accumulator discipline: r0 holds an integer or pointer result, r0:r1 holds
 * a double (low word first). Temporaries are pushed in 8-byte units so the
 * stack is always 8-byte aligned and a call can be made at any point.
 *
 * r7  locals base      (set after the frame is reserved)
 * r8  argument base    (sp at entry, for stack-passed parameters)
 * r12 scratch for indirect calls
 */
#include "cc.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>

/* ------------------------------------------------------------------ */
/* parser state                                                        */
/* ------------------------------------------------------------------ */

static int lval;               /* acc holds an address, not a value */

typedef struct Label {
    char name[CC_MAXNAME];
    int  off;                  /* -1 until defined */
    Fixup *pending;
    struct Label *next;
} Label;

static Label *labels;

static Type *ty_void, *ty_char, *ty_uchar, *ty_short, *ty_ushort;
static Type *ty_int, *ty_uint, *ty_float, *ty_double;
static Type *ty_llong, *ty_ullong;

/* ------------------------------------------------------------------ */
/* type helpers                                                        */
/* ------------------------------------------------------------------ */

static Type *new_type(CC *cc, TKind k, int size, int align)
{
    Type *t = (Type *)cc_alloc(cc, sizeof(Type));
    t->kind = k;
    t->size = size;
    t->align = align;
    t->nelem = -1;
    t->complete = 1;
    t->next = cc->types;
    cc->types = t;
    return t;
}

static void init_types(CC *cc)
{
    ty_void   = new_type(cc, T_VOID, 1, 1);
    ty_char   = new_type(cc, T_CHAR, 1, 1);
    ty_uchar  = new_type(cc, T_UCHAR, 1, 1);
    ty_short  = new_type(cc, T_SHORT, 2, 2);
    ty_ushort = new_type(cc, T_USHORT, 2, 2);
    ty_int    = new_type(cc, T_INT, 4, 4);
    ty_uint   = new_type(cc, T_UINT, 4, 4);
    ty_llong  = new_type(cc, T_LLONG, 8, 8);
    ty_ullong = new_type(cc, T_ULLONG, 8, 8);
    ty_float  = new_type(cc, T_FLOAT, 4, 4);
    ty_double = new_type(cc, T_DOUBLE, 8, 8);
}

static Type *ptr_to(CC *cc, Type *to)
{
    Type *t = new_type(cc, T_PTR, 4, 4);
    t->to = to;
    return t;
}

static Type *array_of(CC *cc, Type *of, int n)
{
    Type *t = new_type(cc, T_ARRAY, n < 0 ? 0 : of->size * n, of->align);
    t->to = of;
    t->nelem = n;
    return t;
}

static int is_dbl(Type *t)  { return t && (t->kind == T_DOUBLE || t->kind == T_FLOAT); }
/* 64-bit integers live in r0:r1 exactly as doubles do, and are passed the
 * same way, so most of the machinery only has to know they are "wide". */
static int is_ll(Type *t)   { return t && (t->kind == T_LLONG || t->kind == T_ULLONG); }
static int is_wide(Type *t) { return is_dbl(t) || is_ll(t); }

static int is_ptr(Type *t)  { return t && (t->kind == T_PTR || t->kind == T_ARRAY); }
static int is_int(Type *t)
{
    return t && (t->kind == T_CHAR || t->kind == T_UCHAR || t->kind == T_SHORT
              || t->kind == T_USHORT || t->kind == T_INT || t->kind == T_UINT);
}
static int is_unsigned(Type *t)
{
    return t && (t->kind == T_UCHAR || t->kind == T_USHORT || t->kind == T_UINT
              || t->kind == T_ULLONG || t->kind == T_PTR);
}

/* An array lvalue decays to a pointer rvalue: the address already in the
 * accumulator *is* the value, so the lvalue flag has to be cleared or the
 * next rvalue() would dereference it. */
static Type *decay(CC *cc, Type *t)
{
    if (t && t->kind == T_ARRAY) {
        lval = 0;
        return ptr_to(cc, t->to);
    }
    return t;
}

/* ------------------------------------------------------------------ */
/* symbols                                                             */
/* ------------------------------------------------------------------ */

static Sym *find_sym(CC *cc, const char *name)
{
    Sym *s;
    for (s = cc->locals; s; s = s->next)
        if (strcmp(s->name, name) == 0) return s;
    for (s = cc->globals; s; s = s->next)
        if (strcmp(s->name, name) == 0) return s;
    return NULL;
}

static Sym *add_local(CC *cc, const char *name, Type *t)
{
    Sym *s = (Sym *)cc_alloc(cc, sizeof(Sym));
    int align = t->align < 4 ? 4 : t->align;
    strncpy(s->name, name, CC_MAXNAME - 1);
    s->kind = S_LOCAL;
    s->type = t;
    cc->frame = (cc->frame + align - 1) & ~(align - 1);
    s->offset = cc->frame;
    cc->frame += (t->size + 3) & ~3;
    if (cc->frame > cc->maxframe) cc->maxframe = cc->frame;
    s->next = cc->locals;
    cc->locals = s;
    return s;
}

static Sym *add_global(CC *cc, const char *name, Type *t, SKind kind)
{
    Sym *s = find_sym(cc, name);
    if (s && s->kind != S_LOCAL) return s;
    s = (Sym *)cc_alloc(cc, sizeof(Sym));
    strncpy(s->name, name, CC_MAXNAME - 1);
    s->kind = kind;
    s->type = t;
    s->next = cc->globals;
    cc->globals = s;
    return s;
}

static uint32_t data_alloc(CC *cc, int size, int align)
{
    uint32_t a;
    cc->data_len = (cc->data_len + align - 1) & ~(align - 1);
    if (cc->data_len + size > cc->data_cap)
        cc_error(cc, "out of data space");
    a = cc->data_base + (uint32_t)cc->data_len;
    cc->data_len += size;
    return a;
}

/* ------------------------------------------------------------------ */
/* token helpers                                                       */
/* ------------------------------------------------------------------ */

static int is_punct(CC *cc, int p)
{
    return cc->tok.kind == TK_PUNCT && cc->tok.val == p;
}
static int is_kw(CC *cc, int k) { return cc->tok.kind == TK_KW && cc->tok.val == k; }

static int accept(CC *cc, int p)
{
    if (is_punct(cc, p)) { lex_next(cc); return 1; }
    return 0;
}
static int accept_kw(CC *cc, int k)
{
    if (is_kw(cc, k)) { lex_next(cc); return 1; }
    return 0;
}
static void expect(CC *cc, int p, const char *what)
{
    if (!accept(cc, p)) cc_error(cc, "expected %s", what);
}

/* ------------------------------------------------------------------ */
/* code emission helpers                                               */
/* ------------------------------------------------------------------ */

#define E (&cc->code)

static void push_acc(CC *cc)
{
    em_sub_sp(E, 8);
    em_str(E, R0, SP, 0);
    em_str(E, R1, SP, 4);
}

/* runtime helper addresses, resolved from the host symbol table */
static uint32_t rt_addr(CC *cc, const char *name)
{
    int i;
    for (i = 0; i < cc->nrt; i++)
        if (strcmp(cc->rt[i].name, name) == 0)
            return (uint32_t)(uintptr_t)cc->rt[i].fn;
    cc_error(cc, "runtime helper '%s' is missing", name);
    return 0;
}

static void call_rt(CC *cc, const char *name)
{
    em_call_reg(E, rt_addr(cc, name), R12);
}

/* int (r0) -> double (r0:r1) */
static void to_double(CC *cc, Type *from)
{
    if (!from) return;
    if (from->kind == T_DOUBLE) return;
    if (from->kind == T_FLOAT) { call_rt(cc, "__aeabi_f2d"); return; }
    if (is_unsigned(from)) call_rt(cc, "__aeabi_ui2d");
    else call_rt(cc, "__aeabi_i2d");
}

/* double (r0:r1) -> int (r0) */
static void to_int_from_double(CC *cc) { call_rt(cc, "__aeabi_d2iz"); }

/* Widen whatever is in the accumulator to a 64-bit integer in r0:r1.
 * The helpers are firmware functions rather than inline code: sign
 * extension and the conversions from double are small, but getting them
 * wrong is silent, and the firmware's versions are compiled by GCC. */
static void to_ll(CC *cc, Type *from)
{
    if (!from) return;
    if (is_ll(from)) return;
    if (is_dbl(from)) {
        if (from->kind == T_FLOAT) call_rt(cc, "__aeabi_f2d");
        call_rt(cc, "__cos_d2l");
        return;
    }
    if (is_unsigned(from)) call_rt(cc, "__cos_u2l");
    else call_rt(cc, "__cos_i2l");
}

/* 64-bit (r0:r1) -> double (r0:r1) */
static void ll_to_double(CC *cc, Type *from)
{
    call_rt(cc, is_unsigned(from) ? "__cos_ul2d" : "__cos_l2d");
}

/* 64-bit -> int is just the low word, which is already in r0. */
static void ll_to_int(CC *cc) { (void)cc; }

static void narrow(CC *cc, Type *t);

/* Collapse the accumulator to a single word that is zero exactly when the
 * value is false. A 64-bit value is false only if *both* halves are zero,
 * so testing r0 alone would call 2^32 false. */
static void to_truth_word(CC *cc, Type *t)
{
    if (is_dbl(t)) { to_int_from_double(cc); return; }
    if (is_ll(t)) { em_orr(E, R0, R0, R1); return; }
}

/* Convert whatever is in the accumulator from `from` to `to`. Every
 * assignment, argument, cast, initialiser and return goes through here, so
 * the widening rules live in one place instead of at each of them. */
static void convert(CC *cc, Type *from, Type *to)
{
    if (!from || !to) return;
    if (to->kind == T_VOID) return;
    if (is_dbl(to)) {
        if (is_ll(from)) { ll_to_double(cc, from); return; }
        to_double(cc, from);
        return;
    }
    if (is_ll(to)) { to_ll(cc, from); return; }
    /* to is a plain integer or pointer */
    if (is_dbl(from)) { to_int_from_double(cc); narrow(cc, to); return; }
    if (is_ll(from))  { ll_to_int(cc); narrow(cc, to); return; }
    narrow(cc, to);
}

/* narrow an integer in r0 to its declared width */
static void narrow(CC *cc, Type *t)
{
    switch (t->kind) {
    case T_CHAR:   em_sxtb(E, R0, R0); break;
    case T_UCHAR:  em_uxtb(E, R0, R0); break;
    case T_SHORT:  em_sxth(E, R0, R0); break;
    case T_USHORT: em_uxth(E, R0, R0); break;
    default: break;
    }
}

/* load the object whose address is in r0 */
static void load_indirect(CC *cc, Type *t)
{
    switch (t->kind) {
    case T_CHAR:   em_ldrsb(E, R0, R0, 0); break;
    case T_UCHAR:  em_ldrb(E, R0, R0, 0); break;
    case T_SHORT:  em_ldrsh(E, R0, R0, 0); break;
    case T_USHORT: em_ldrh(E, R0, R0, 0); break;
    case T_FLOAT:  em_ldr(E, R0, R0, 0); call_rt(cc, "__aeabi_f2d"); break;
    case T_DOUBLE:
    case T_LLONG:
    case T_ULLONG: em_ldr(E, R1, R0, 4); em_ldr(E, R0, R0, 0); break;
    case T_ARRAY:
    case T_STRUCT:
    case T_UNION:  break;                     /* address is the value */
    default:       em_ldr(E, R0, R0, 0); break;
    }
}

/* store the accumulator into the address held in rn */
static void store_indirect(CC *cc, Type *t, int rn)
{
    switch (t->kind) {
    case T_CHAR: case T_UCHAR:   em_strb(E, R0, rn, 0); break;
    case T_SHORT: case T_USHORT: em_strh(E, R0, rn, 0); break;
    case T_FLOAT:
        em_mov(E, R4, rn);
        call_rt(cc, "__aeabi_d2f");
        em_str(E, R0, R4, 0);
        break;
    case T_DOUBLE:
    case T_LLONG:
    case T_ULLONG:
        em_str(E, R0, rn, 0);
        em_str(E, R1, rn, 4);
        break;
    case T_STRUCT: case T_UNION: {
        /* r0 = source address, rn = destination; copy word by word */
        int i;
        for (i = 0; i + 4 <= t->size; i += 4) {
            em_ldr(E, R2, R0, i);
            em_str(E, R2, rn, i);
        }
        for (; i < t->size; i++) {
            em_ldrb(E, R2, R0, i);
            em_strb(E, R2, rn, i);
        }
        break;
    }
    default: em_str(E, R0, rn, 0); break;
    }
}

static Type *rvalue(CC *cc, Type *t)
{
    if (lval) {
        load_indirect(cc, t);
        lval = 0;
    }
    return t;
}

/* address of a symbol into register r */
static void sym_addr(CC *cc, Sym *s, int r)
{
    if (s->kind == S_LOCAL) {
        em_add_imm(E, r, R7, s->offset);
    } else {
        em_imm32(E, r, s->addr);
    }
}

/* ------------------------------------------------------------------ */
/* forward declarations                                                */
/* ------------------------------------------------------------------ */

static Type *expr(CC *cc);
static Type *assign_expr(CC *cc);
static Type *cond_expr(CC *cc);
static Type *unary(CC *cc);
static void  statement(CC *cc);
static Type *declspec(CC *cc, int *sclass);
static Type *declarator(CC *cc, Type *base, char *name);
static Type *type_name(CC *cc);
static int   is_type_start(CC *cc);
static int   prec_of(CC *cc);
static int   is_type_start(CC *cc);
static long  const_expr(CC *cc);

/* ------------------------------------------------------------------ */
/* calls                                                               */
/* ------------------------------------------------------------------ */

typedef struct { int off; int dbl; int size; } ArgInfo;

static void call_symbol(CC *cc, Sym *s)
{
    if (s->addr) {
        em_call_reg(E, s->addr, R12);
        return;
    }
    if (s->code_off >= 0 && s->defined) {
        em_call_reg(E, emit_addr(E, s->code_off) | 1u, R12);
        return;
    }
    {   /* forward reference: emit a placeholder and record it */
        Fixup *f = (Fixup *)cc_alloc(cc, sizeof(Fixup));
        f->at = emit_here(E);
        f->reg = R12;
        f->next = s->fixups;
        s->fixups = f;
        em_movw(E, R12, 0);
        em_movt(E, R12, 0);
        em_blx(E, R12);
    }
}

/* The address of a function as a value, for &f, f as an argument, or an
 * assignment to a function pointer. Thumb addresses carry their low bit
 * set, which is what makes the result callable with blx. */
static void fn_addr(CC *cc, Sym *s, int r)
{
    if (s->addr) { em_imm32(E, r, s->addr); return; }
    if (s->code_off >= 0 && s->defined) {
        em_imm32(E, r, emit_addr(E, s->code_off) | 1u);
        return;
    }
    {   /* forward reference: the same placeholder a call would leave */
        Fixup *f = (Fixup *)cc_alloc(cc, sizeof(Fixup));
        f->at = emit_here(E);
        f->reg = r;
        f->next = s->fixups;
        s->fixups = f;
        em_movw(E, r, 0);
        em_movt(E, r, 0);
    }
}

/* Build the function type a symbol denotes, so a call through a pointer to
 * it converts arguments exactly as a direct call would. */
static Type *fn_type_of(CC *cc, Sym *s)
{
    Type *ft = new_type(cc, T_FUNC, 1, 1);
    ft->to = s->type;
    ft->ptypes = s->ptypes;
    ft->nparams = s->nparams;
    ft->variadic = s->variadic;
    return ft;
}

static void patch_movw_movt(CC *cc, int at, int reg, uint32_t v)
{
    uint8_t *b = cc->code.buf;
    unsigned lo = v & 0xFFFF, hi = (v >> 16) & 0xFFFF;
    unsigned r = (unsigned)reg & 0xF, h1, h2;
    /* movw r, #lo */
    h1 = 0xF240u | (((lo >> 11) & 1) << 10) | ((lo >> 12) & 0xF);
    h2 = (((lo >> 8) & 7) << 12) | (r << 8) | (lo & 0xFF);
    b[at + 0] = (uint8_t)(h1 & 0xFF); b[at + 1] = (uint8_t)(h1 >> 8);
    b[at + 2] = (uint8_t)(h2 & 0xFF); b[at + 3] = (uint8_t)(h2 >> 8);
    /* movt r, #hi */
    h1 = 0xF2C0u | (((hi >> 11) & 1) << 10) | ((hi >> 12) & 0xF);
    h2 = (((hi >> 8) & 7) << 12) | (r << 8) | (hi & 0xFF);
    b[at + 4] = (uint8_t)(h1 & 0xFF); b[at + 5] = (uint8_t)(h1 >> 8);
    b[at + 6] = (uint8_t)(h2 & 0xFF); b[at + 7] = (uint8_t)(h2 >> 8);
}

/* Evaluate the arguments of a call, left to right, each pushed as 8 bytes.
 * Then shuffle them into AAPCS positions. Doubles take an even register pair;
 * anything that does not fit goes on the stack. */
/* Call a function.
 *
 * With `fn` set this is an ordinary call to a known symbol. With `fn` NULL
 * it is a call through a function pointer: the target address has already
 * been pushed by the caller and sits just below the arguments, so it is
 * reloaded into r12 after the argument registers are set up (it cannot stay
 * in r0-r3, which the arguments need). */
static Type *do_call(CC *cc, Sym *fn, Type *fnty)
{
    ArgInfo args[CC_MAXARGS];
    int nargs = 0, i;
    int ncore = 0, stack_bytes = 0;
    int core_of[CC_MAXARGS], stk_of[CC_MAXARGS];
    int indirect = (fn == NULL);
    Type **ptypes = NULL;
    int nptypes = 0;

    if (fn && fn->ptypes) { ptypes = fn->ptypes; nptypes = fn->nparams; }
    else if (fnty && fnty->kind == T_FUNC && fnty->ptypes) {
        ptypes = fnty->ptypes;
        nptypes = fnty->nparams;
    }

    expect(cc, '(', "'('");
    if (!is_punct(cc, ')')) {
        for (;;) {
            Type *t;
            if (nargs >= CC_MAXARGS) cc_error(cc, "too many arguments");
            t = assign_expr(cc);
            t = decay(cc, rvalue(cc, t));
            /* Convert to the declared parameter type. Without this a call
             * like f(1) to f(double) would pass an int in a core register
             * where the callee expects a register pair. */
            if (ptypes && nargs < nptypes) {
                Type *pt = ptypes[nargs];
                if (pt && !is_ptr(pt) && pt->kind != T_STRUCT && pt->kind != T_UNION) {
                    convert(cc, t, pt);
                    t = pt;
                }
            }
            /* default argument promotion for everything else */
            if (t->kind == T_FLOAT) { to_double(cc, t); t = ty_double; }
            args[nargs].dbl = is_wide(t);
            args[nargs].size = 8;
            push_acc(cc);
            nargs++;
            if (accept(cc, ',')) continue;
            break;
        }
    }
    expect(cc, ')', "')'");

    /* arguments now sit on the stack, last one at [sp+0] */
    for (i = 0; i < nargs; i++)
        args[i].off = (nargs - 1 - i) * 8;

    /* classify */
    for (i = 0; i < nargs; i++) {
        if (args[i].dbl) {
            if (ncore & 1) ncore++;            /* even alignment */
            if (ncore + 2 <= 4) { core_of[i] = ncore; ncore += 2; stk_of[i] = -1; }
            else {
                ncore = 4;
                stack_bytes = (stack_bytes + 7) & ~7;
                core_of[i] = -1; stk_of[i] = stack_bytes;
                stack_bytes += 8;
            }
        } else {
            if (ncore < 4) { core_of[i] = ncore++; stk_of[i] = -1; }
            else { core_of[i] = -1; stk_of[i] = stack_bytes; stack_bytes += 4; }
        }
    }

    if (stack_bytes) {
        /* make room for the outgoing block, then copy the stack arguments in;
         * r4 walks the saved values, which sit above the new block */
        stack_bytes = (stack_bytes + 7) & ~7;
        em_sub_sp(E, stack_bytes);
        for (i = 0; i < nargs; i++) {
            if (stk_of[i] < 0) continue;
            em_ldr(E, R2, SP, stack_bytes + args[i].off);
            em_str(E, R2, SP, stk_of[i]);
            if (args[i].dbl) {
                em_ldr(E, R2, SP, stack_bytes + args[i].off + 4);
                em_str(E, R2, SP, stk_of[i] + 4);
            }
        }
        /* load register arguments from the saved block */
        for (i = 0; i < nargs; i++) {
            if (core_of[i] < 0) continue;
            em_ldr(E, core_of[i], SP, stack_bytes + args[i].off);
            if (args[i].dbl)
                em_ldr(E, core_of[i] + 1, SP, stack_bytes + args[i].off + 4);
        }
    } else {
        for (i = 0; i < nargs; i++) {
            if (core_of[i] < 0) continue;
            em_ldr(E, core_of[i], SP, args[i].off);
            if (args[i].dbl) em_ldr(E, core_of[i] + 1, SP, args[i].off + 4);
        }
    }

    if (!indirect) {
        call_symbol(cc, fn);
    } else {
        /* the pushed target sits above the argument block */
        em_ldr(E, R12, SP, stack_bytes + nargs * 8);
        em_blx(E, R12);
    }

    if (stack_bytes) em_add_sp(E, stack_bytes);
    em_add_sp(E, nargs * 8);
    if (indirect) em_add_sp(E, 8);        /* drop the saved target */
    lval = 0;
    return fnty && fnty->to ? fnty->to : ty_int;
}

/* ------------------------------------------------------------------ */
/* primary expressions                                                 */
/* ------------------------------------------------------------------ */

static void emit_double_const(CC *cc, double d)
{
    union { double d; uint32_t w[2]; } u;
    u.d = d;
    em_imm32(E, R0, u.w[0]);
    em_imm32(E, R1, u.w[1]);
}

static Type *primary(CC *cc)
{
    if (accept(cc, '(')) {
        Type *t = expr(cc);
        expect(cc, ')', "')'");
        return t;
    }
    if (cc->tok.kind == TK_NUM) {
        if (cc->tok.is_ll) {
            unsigned long long v = (unsigned long long)cc->tok.llval;
            Type *t = cc->tok.is_uns ? ty_ullong : ty_llong;
            em_imm32(E, R0, (uint32_t)(v & 0xFFFFFFFFu));
            em_imm32(E, R1, (uint32_t)(v >> 32));
            lex_next(cc);
            lval = 0;
            return t;
        }
        em_imm32(E, R0, (uint32_t)cc->tok.val);
        {
            Type *t = cc->tok.is_uns ? ty_uint : ty_int;
            lex_next(cc);
            lval = 0;
            return t;
        }
    }
    if (cc->tok.kind == TK_DNUM) {
        emit_double_const(cc, cc->tok.dval);
        lex_next(cc);
        lval = 0;
        return ty_double;
    }
    if (cc->tok.kind == TK_STR) {
        uint32_t a = data_alloc(cc, cc->tok.slen + 1, 1);
        memcpy(cc->data + (a - cc->data_base), cc->tok.str, (size_t)cc->tok.slen + 1);
        em_imm32(E, R0, a);
        lex_next(cc);
        lval = 0;
        return ptr_to(cc, ty_char);
    }
    if (cc->tok.kind == TK_IDENT) {
        char name[CC_MAXNAME];
        Sym *s;
        strncpy(name, cc->tok.name, CC_MAXNAME - 1);
        name[CC_MAXNAME - 1] = 0;
        lex_next(cc);
        s = find_sym(cc, name);
        /* A name followed by '(' is a direct call only when the name is a
         * function. A function *pointer* variable falls through to be
         * loaded like any other value; postfix then calls through it. */
        if (is_punct(cc, '(') && (!s || s->kind == S_FUNC || s->kind == S_EXTERN)) {
            if (!s) cc_error(cc, "call to undeclared function '%s'", name);
            return do_call(cc, s, s->type);
        }
        if (!s) cc_error(cc, "undeclared identifier '%s'", name);
        if (s->kind == S_ENUM) {
            em_imm32(E, R0, (uint32_t)s->enumval);
            lval = 0;
            return ty_int;
        }
        if (s->kind == S_FUNC || s->kind == S_EXTERN) {
            /* a function used as a value decays to a pointer to itself */
            fn_addr(cc, s, R0);
            lval = 0;
            return ptr_to(cc, fn_type_of(cc, s));
        }
        sym_addr(cc, s, R0);
        lval = 1;
        return s->type;
    }
    cc_error(cc, "expected an expression");
    return ty_int;
}

static Member *find_member(Type *t, const char *name)
{
    Member *m;
    for (m = t->members; m; m = m->next)
        if (strcmp(m->name, name) == 0) return m;
    return NULL;
}

/* ++/-- applied to the lvalue whose address is in r0 */
static Type *incdec(CC *cc, Type *t, int delta, int postfix)
{
    int step = 1;
    if (is_ptr(t)) step = t->to ? t->to->size : 1;
    em_mov(E, R4, R0);                       /* keep the address */
    load_indirect(cc, t);
    if (is_dbl(t)) {
        if (t->kind == T_FLOAT) { /* already widened by load_indirect */ }
        if (postfix) { em_mov(E, R5, R0); em_mov(E, R6, R1); }
        em_imm32(E, R2, 0);
        em_imm32(E, R3, (uint32_t)(delta > 0 ? 0x3FF00000u : 0xBFF00000u));
        call_rt(cc, "__aeabi_dadd");
        store_indirect(cc, t, R4);
        if (postfix) { em_mov(E, R0, R5); em_mov(E, R1, R6); }
        lval = 0;
        return t->kind == T_FLOAT ? ty_double : t;
    }
    if (postfix) em_mov(E, R5, R0);
    em_add_imm(E, R0, R0, delta * step);
    narrow(cc, t);
    store_indirect(cc, t, R4);
    if (postfix) em_mov(E, R0, R5);
    lval = 0;
    return t;
}

static Type *postfix(CC *cc)
{
    Type *t = primary(cc);
    for (;;) {
        if (is_punct(cc, '[')) {
            Type *et;
            lex_next(cc);
            t = decay(cc, rvalue(cc, t));
            if (!is_ptr(t)) cc_error(cc, "subscripted value is not a pointer");
            et = t->to;
            push_acc(cc);
            {
                Type *it = rvalue(cc, expr(cc));
                if (is_dbl(it)) to_int_from_double(cc);
            }
            expect(cc, ']', "']'");
            if (et->size != 1) {
                em_imm32(E, R2, (uint32_t)et->size);
                em_mul(E, R0, R0, R2);
            }
            em_ldr(E, R2, SP, 0);
            em_add_sp(E, 8);
            em_add(E, R0, R2, R0);
            lval = 1;
            t = et;
            continue;
        }
        if (is_punct(cc, '(')) {
            /* calling the value of an expression: a function pointer, or a
             * function name that has already decayed to one */
            Type *ft = decay(cc, rvalue(cc, t));
            if (is_ptr(ft) && ft->to && ft->to->kind == T_FUNC) ft = ft->to;
            if (ft->kind != T_FUNC)
                cc_error(cc, "called object is not a function");
            push_acc(cc);
            t = do_call(cc, NULL, ft);
            continue;
        }
        if (is_punct(cc, '.') || is_punct(cc, P_ARROW)) {
            int arrow = is_punct(cc, P_ARROW);
            Member *m;
            lex_next(cc);
            if (arrow) {
                t = decay(cc, rvalue(cc, t));
                if (!is_ptr(t)) cc_error(cc, "'->' applied to a non-pointer");
                t = t->to;
                lval = 1;
            }
            if (t->kind != T_STRUCT && t->kind != T_UNION)
                cc_error(cc, "member access on a non-struct value");
            if (cc->tok.kind != TK_IDENT) cc_error(cc, "expected a member name");
            m = find_member(t, cc->tok.name);
            if (!m) cc_error(cc, "no member named '%s'", cc->tok.name);
            lex_next(cc);
            em_add_imm(E, R0, R0, m->offset);
            lval = 1;
            t = m->type;
            continue;
        }
        if (is_punct(cc, P_INC) || is_punct(cc, P_DEC)) {
            int d = is_punct(cc, P_INC) ? 1 : -1;
            lex_next(cc);
            if (!lval) cc_error(cc, "'++' needs an lvalue");
            t = incdec(cc, t, d, 1);
            continue;
        }
        break;
    }
    return t;
}

static Type *unary(CC *cc)
{
    if (accept(cc, '-')) {
        Type *t = rvalue(cc, decay(cc, unary(cc)));
        if (is_dbl(t)) {
            em_imm32(E, R2, 0x80000000u);
            em_eor(E, R1, R1, R2);
            return ty_double;
        }
        if (is_ll(t)) {
            /* 0 - value, with the operand moved to the second argument */
            em_mov(E, R2, R0);
            em_mov(E, R3, R1);
            em_imm32(E, R0, 0);
            em_imm32(E, R1, 0);
            call_rt(cc, "__cos_lsub");
            return t;
        }
        em_neg(E, R0, R0);
        return ty_int;
    }
    if (accept(cc, '+')) return rvalue(cc, unary(cc));
    if (accept(cc, '!')) {
        Type *t = rvalue(cc, decay(cc, unary(cc)));
        to_truth_word(cc, t);
        em_cmp_imm(E, R0, 0);
        {
            int b1 = em_bcc(E, CC_EQ, -1), b2;
            em_movs_imm8(E, R0, 0);
            b2 = em_b(E, -1);
            em_patch_branch(E, b1, emit_here(E));
            em_movs_imm8(E, R0, 1);
            em_patch_branch(E, b2, emit_here(E));
        }
        lval = 0;
        return ty_int;
    }
    if (accept(cc, '~')) {
        Type *t = rvalue(cc, decay(cc, unary(cc)));
        if (is_ll(t)) {
            em_mvn(E, R0, R0);
            em_mvn(E, R1, R1);
            lval = 0;
            return t;
        }
        em_mvn(E, R0, R0);
        lval = 0;
        return ty_int;
    }
    if (accept(cc, '*')) {
        Type *t = decay(cc, rvalue(cc, unary(cc)));
        if (!is_ptr(t)) cc_error(cc, "'*' applied to a non-pointer");
        lval = 1;
        return t->to;
    }
    if (accept(cc, '&')) {
        Type *t = unary(cc);
        if (!lval) cc_error(cc, "'&' needs an lvalue");
        lval = 0;
        return ptr_to(cc, t);
    }
    if (is_punct(cc, P_INC) || is_punct(cc, P_DEC)) {
        int d = is_punct(cc, P_INC) ? 1 : -1;
        Type *t;
        lex_next(cc);
        t = unary(cc);
        if (!lval) cc_error(cc, "'++' needs an lvalue");
        return incdec(cc, t, d, 0);
    }
    if (is_kw(cc, KW_SIZEOF)) {
        Type *t;
        lex_next(cc);
        if (is_punct(cc, '(')) {
            Token *p = lex_peek(cc);
            if (p->kind == TK_KW || (p->kind == TK_IDENT && 0)) {
                lex_next(cc);
                t = type_name(cc);
                expect(cc, ')', "')'");
                em_imm32(E, R0, (uint32_t)t->size);
                lval = 0;
                return ty_int;
            }
        }
        /* sizeof expr: compile it, then throw the code away */
        {
            int save = emit_here(E);
            t = unary(cc);
            cc->code.len = save;
            em_imm32(E, R0, (uint32_t)t->size);
            lval = 0;
            return ty_int;
        }
    }
    /* cast */
    if (is_punct(cc, '(')) {
        Token *p = lex_peek(cc);
        if (p->kind == TK_KW && p->val != KW_SIZEOF) {
            Type *ct, *vt;
            lex_next(cc);
            ct = type_name(cc);
            expect(cc, ')', "')'");
            vt = decay(cc, rvalue(cc, unary(cc)));
            if (is_ll(ct) || is_ll(vt) || is_dbl(ct) || is_dbl(vt)) {
                convert(cc, vt, ct);
                lval = 0;
                return ct;
            }
            if (is_int(ct)) narrow(cc, ct);
            lval = 0;
            return ct;
        }
    }
    return postfix(cc);
}

/* ------------------------------------------------------------------ */
/* binary operators                                                    */
/* ------------------------------------------------------------------ */

/* After this, r0:r1 = left, r2:r3 = right, both already converted.
 * Returns 0 for plain integers, 1 for doubles, 2 for 64-bit integers.
 * The left operand was pushed by the caller before the right was
 * evaluated, so both have to be shuffled into place here. */
static int binop_operands(CC *cc, Type *lt, Type *rt)
{
    if (is_dbl(lt) || is_dbl(rt)) {
        if (!is_dbl(rt)) { if (is_ll(rt)) ll_to_double(cc, rt); else to_double(cc, rt); }
        push_acc(cc);                       /* right */
        em_ldr(E, R0, SP, 8);
        em_ldr(E, R1, SP, 12);
        if (!is_dbl(lt)) { if (is_ll(lt)) ll_to_double(cc, lt); else to_double(cc, lt); }
        em_ldr(E, R2, SP, 0);
        em_ldr(E, R3, SP, 4);
        em_add_sp(E, 16);
        return 1;
    }
    if (is_ll(lt) || is_ll(rt)) {
        if (!is_ll(rt)) to_ll(cc, rt);
        push_acc(cc);                       /* right */
        em_ldr(E, R0, SP, 8);
        em_ldr(E, R1, SP, 12);
        if (!is_ll(lt)) to_ll(cc, lt);
        em_ldr(E, R2, SP, 0);
        em_ldr(E, R3, SP, 4);
        em_add_sp(E, 16);
        return 2;
    }
    em_mov(E, R2, R0);
    em_ldr(E, R0, SP, 0);
    em_add_sp(E, 8);
    return 0;
}

static void cmp_to_bool(CC *cc, int cond)
{
    int b1 = em_bcc(E, cond, -1), b2;
    em_movs_imm8(E, R0, 0);
    b2 = em_b(E, -1);
    em_patch_branch(E, b1, emit_here(E));
    em_movs_imm8(E, R0, 1);
    em_patch_branch(E, b2, emit_here(E));
}

static const char *dcmp_fn(int op, int *negate)
{
    *negate = 0;
    switch (op) {
    case '<':   return "__aeabi_dcmplt";
    case '>':   return "__aeabi_dcmpgt";
    case P_LE:  return "__aeabi_dcmple";
    case P_GE:  return "__aeabi_dcmpge";
    case P_EQ:  return "__aeabi_dcmpeq";
    case P_NE:  *negate = 1; return "__aeabi_dcmpeq";
    }
    return NULL;
}

static Type *do_binop(CC *cc, int op, Type *lt, Type *rt)
{
    /* pointer arithmetic */
    if ((op == '+' || op == '-') && (is_ptr(lt) || is_ptr(rt))) {
        Type *pt = is_ptr(lt) ? lt : rt;
        int esz = pt->to ? pt->to->size : 1;
        if (is_ptr(lt) && is_ptr(rt) && op == '-') {
            em_mov(E, R2, R0);
            em_ldr(E, R0, SP, 0);
            em_add_sp(E, 8);
            em_sub(E, R0, R0, R2);
            if (esz > 1) {
                em_imm32(E, R2, (uint32_t)esz);
                em_sdiv(E, R0, R0, R2);
            }
            lval = 0;
            return ty_int;
        }
        if (is_ptr(rt) && !is_ptr(lt)) {
            /* int + ptr: scale the left operand, which is on the stack */
            em_mov(E, R2, R0);               /* pointer */
            em_ldr(E, R0, SP, 0);            /* integer */
            em_add_sp(E, 8);
            if (esz > 1) {
                em_imm32(E, R3, (uint32_t)esz);
                em_mul(E, R0, R0, R3);
            }
            em_add(E, R0, R2, R0);
            lval = 0;
            return rt;
        }
        em_mov(E, R2, R0);                   /* integer */
        if (esz > 1) {
            em_imm32(E, R3, (uint32_t)esz);
            em_mul(E, R2, R2, R3);
        }
        em_ldr(E, R0, SP, 0);                /* pointer */
        em_add_sp(E, 8);
        if (op == '+') em_add(E, R0, R0, R2);
        else em_sub(E, R0, R0, R2);
        lval = 0;
        return lt;
    }

    {
        int dbl = binop_operands(cc, lt, rt);
        int uns = is_unsigned(lt) || is_unsigned(rt);
        lval = 0;
        if (dbl == 2) {
            Type *res = uns ? ty_ullong : ty_llong;
            switch (op) {
            case '+': call_rt(cc, "__cos_ladd"); return res;
            case '-': call_rt(cc, "__cos_lsub"); return res;
            case '*': call_rt(cc, "__cos_lmul"); return res;
            case '/': call_rt(cc, uns ? "__cos_uldiv" : "__cos_ldiv"); return res;
            case '%': call_rt(cc, uns ? "__cos_ulmod" : "__cos_lmod"); return res;
            case '&': call_rt(cc, "__cos_land"); return res;
            case '|': call_rt(cc, "__cos_lor");  return res;
            case '^': call_rt(cc, "__cos_lxor"); return res;
            case P_SHL: call_rt(cc, "__cos_lshl"); return res;
            case P_SHR: call_rt(cc, uns ? "__cos_lushr" : "__cos_lshr"); return res;
            default: {
                /* the comparison helper returns -1, 0 or 1, which turns
                 * every relational operator into a compare against zero */
                int cond;
                call_rt(cc, uns ? "__cos_ulcmp" : "__cos_lcmp");
                switch (op) {
                case '<':   cond = CC_LT; break;
                case '>':   cond = CC_GT; break;
                case P_LE:  cond = CC_LE; break;
                case P_GE:  cond = CC_GE; break;
                case P_EQ:  cond = CC_EQ; break;
                case P_NE:  cond = CC_NE; break;
                default: cc_error(cc, "operator not defined for long long");
                         cond = CC_EQ; break;
                }
                em_cmp_imm(E, R0, 0);
                cmp_to_bool(cc, cond);
                return ty_int;
            }
            }
        }
        if (dbl) {
            switch (op) {
            case '+': call_rt(cc, "__aeabi_dadd"); return ty_double;
            case '-': call_rt(cc, "__aeabi_dsub"); return ty_double;
            case '*': call_rt(cc, "__aeabi_dmul"); return ty_double;
            case '/': call_rt(cc, "__aeabi_ddiv"); return ty_double;
            default: {
                int neg;
                const char *fn = dcmp_fn(op, &neg);
                if (!fn) cc_error(cc, "operator not defined for doubles");
                call_rt(cc, fn);
                if (neg) {
                    em_cmp_imm(E, R0, 0);
                    cmp_to_bool(cc, CC_EQ);
                }
                return ty_int;
            }
            }
        }
        switch (op) {
        case '+': em_add(E, R0, R0, R2); return ty_int;
        case '-': em_sub(E, R0, R0, R2); return ty_int;
        case '*': em_mul(E, R0, R0, R2); return ty_int;
        case '/': if (uns) em_udiv(E, R0, R0, R2); else em_sdiv(E, R0, R0, R2);
                  return uns ? ty_uint : ty_int;
        case '%': {
            if (uns) em_udiv(E, R3, R0, R2); else em_sdiv(E, R3, R0, R2);
            em_mul(E, R3, R3, R2);
            em_sub(E, R0, R0, R3);
            return uns ? ty_uint : ty_int;
        }
        case '&': em_and(E, R0, R0, R2); return ty_int;
        case '|': em_orr(E, R0, R0, R2); return ty_int;
        case '^': em_eor(E, R0, R0, R2); return ty_int;
        case P_SHL: em_lsl(E, R0, R0, R2); return ty_int;
        case P_SHR: if (uns) em_lsr(E, R0, R0, R2); else em_asr(E, R0, R0, R2);
                    return ty_int;
        default: break;
        }
        /* comparisons */
        em_cmp(E, R0, R2);
        switch (op) {
        case '<':  cmp_to_bool(cc, uns ? CC_CC : CC_LT); break;
        case '>':  cmp_to_bool(cc, uns ? CC_HI : CC_GT); break;
        case P_LE: cmp_to_bool(cc, uns ? CC_LS : CC_LE); break;
        case P_GE: cmp_to_bool(cc, uns ? CC_CS : CC_GE); break;
        case P_EQ: cmp_to_bool(cc, CC_EQ); break;
        case P_NE: cmp_to_bool(cc, CC_NE); break;
        default: cc_error(cc, "unknown operator");
        }
        return ty_int;
    }
}

/* precedence climbing */
static int prec_of(CC *cc)
{
    if (cc->tok.kind != TK_PUNCT) return 0;
    switch (cc->tok.val) {
    case '*': case '/': case '%': return 10;
    case '+': case '-': return 9;
    case P_SHL: case P_SHR: return 8;
    case '<': case '>': case P_LE: case P_GE: return 7;
    case P_EQ: case P_NE: return 6;
    case '&': return 5;
    case '^': return 4;
    case '|': return 3;
    default: return 0;
    }
}

static Type *binary(CC *cc, int minprec)
{
    /* Not decayed here: with no operator following, the caller may be sizeof,
     * which needs the array type intact. The loop below decays as needed. */
    Type *lt = unary(cc);
    for (;;) {
        int p = prec_of(cc), op;
        Type *rt;
        if (p == 0 || p < minprec) break;
        op = cc->tok.val;
        lex_next(cc);
        lt = decay(cc, rvalue(cc, lt));
        push_acc(cc);
        rt = decay(cc, rvalue(cc, binary(cc, p + 1)));
        lt = do_binop(cc, op, lt, rt);
    }
    return lt;
}

static Type *logand(CC *cc)
{
    Type *t = binary(cc, 1);
    while (is_punct(cc, P_ANDAND)) {
        int bfalse, bend;
        lex_next(cc);
        t = rvalue(cc, decay(cc, t));
        to_truth_word(cc, t);
        em_cmp_imm(E, R0, 0);
        bfalse = em_bcc(E, CC_EQ, -1);
        t = rvalue(cc, decay(cc, binary(cc, 1)));
        to_truth_word(cc, t);
        em_cmp_imm(E, R0, 0);
        {
            int b2 = em_bcc(E, CC_EQ, -1);
            em_movs_imm8(E, R0, 1);
            bend = em_b(E, -1);
            em_patch_branch(E, bfalse, emit_here(E));
            em_patch_branch(E, b2, emit_here(E));
            em_movs_imm8(E, R0, 0);
            em_patch_branch(E, bend, emit_here(E));
        }
        lval = 0;
        t = ty_int;
    }
    return t;
}

static Type *logor(CC *cc)
{
    Type *t = logand(cc);
    while (is_punct(cc, P_OROR)) {
        int btrue, bend;
        lex_next(cc);
        t = rvalue(cc, decay(cc, t));
        to_truth_word(cc, t);
        em_cmp_imm(E, R0, 0);
        btrue = em_bcc(E, CC_NE, -1);
        t = rvalue(cc, decay(cc, logand(cc)));
        to_truth_word(cc, t);
        em_cmp_imm(E, R0, 0);
        {
            int b2 = em_bcc(E, CC_NE, -1);
            em_movs_imm8(E, R0, 0);
            bend = em_b(E, -1);
            em_patch_branch(E, btrue, emit_here(E));
            em_patch_branch(E, b2, emit_here(E));
            em_movs_imm8(E, R0, 1);
            em_patch_branch(E, bend, emit_here(E));
        }
        lval = 0;
        t = ty_int;
    }
    return t;
}

static Type *cond_expr(CC *cc)
{
    Type *t = logor(cc);
    if (is_punct(cc, '?')) {
        int bfalse, bend;
        Type *a, *b;
        lex_next(cc);
        t = rvalue(cc, decay(cc, t));
        to_truth_word(cc, t);
        em_cmp_imm(E, R0, 0);
        bfalse = em_bcc(E, CC_EQ, -1);
        a = rvalue(cc, decay(cc, expr(cc)));
        expect(cc, ':', "':'");
        bend = em_b(E, -1);
        em_patch_branch(E, bfalse, emit_here(E));
        b = rvalue(cc, decay(cc, cond_expr(cc)));
        em_patch_branch(E, bend, emit_here(E));
        lval = 0;
        return is_dbl(a) || is_dbl(b) ? ty_double : a;
    }
    return t;
}

static Type *assign_expr(CC *cc)
{
    Type *lt = cond_expr(cc);
    int op = 0;

    if (cc->tok.kind == TK_PUNCT) {
        switch (cc->tok.val) {
        case '=': op = '='; break;
        case P_ADDEQ: op = '+'; break;
        case P_SUBEQ: op = '-'; break;
        case P_MULEQ: op = '*'; break;
        case P_DIVEQ: op = '/'; break;
        case P_MODEQ: op = '%'; break;
        case P_ANDEQ: op = '&'; break;
        case P_OREQ:  op = '|'; break;
        case P_XOREQ: op = '^'; break;
        case P_SHLEQ: op = P_SHL; break;
        case P_SHREQ: op = P_SHR; break;
        default: return lt;
        }
    } else return lt;

    if (!lval) cc_error(cc, "left side of assignment is not an lvalue");
    lex_next(cc);

    if (op == '=') {
        Type *rt;
        push_acc(cc);                         /* destination address */
        rt = decay(cc, rvalue(cc, assign_expr(cc)));
        if (!is_ptr(lt) && lt->kind != T_STRUCT && lt->kind != T_UNION)
            convert(cc, rt, lt);
        em_ldr(E, R4, SP, 0);
        em_add_sp(E, 8);
        store_indirect(cc, lt, R4);
        lval = 0;
        return lt;
    }

    /* compound assignment: value = *addr OP rhs */
    {
        Type *rt, *res;
        push_acc(cc);                         /* address */
        em_mov(E, R0, R0);
        load_indirect(cc, lt);                /* current value in acc */
        lval = 0;
        push_acc(cc);                         /* left operand */
        rt = decay(cc, rvalue(cc, assign_expr(cc)));
        res = do_binop(cc, op, lt->kind == T_FLOAT ? ty_double : lt, rt);
        if (!is_ptr(lt)) convert(cc, res, lt);
        else narrow(cc, lt);
        em_ldr(E, R4, SP, 0);
        em_add_sp(E, 8);
        store_indirect(cc, lt, R4);
        lval = 0;
        return lt;
    }
}

static Type *expr(CC *cc)
{
    Type *t = assign_expr(cc);
    while (is_punct(cc, ',')) {
        lex_next(cc);
        rvalue(cc, t);
        t = assign_expr(cc);
    }
    return t;
}

/* Constant expressions.
 *
 * These are evaluated on the token stream rather than compiled, because
 * the places that need them — array sizes, case labels, enum values,
 * #if — need an integer now, not code. An earlier version compiled the
 * expression and read the constant back out of the instruction it
 * emitted, which worked for a literal and failed the moment anyone wrote
 * something as ordinary as `char grid[W * H]`.
 */
static long const_cond(CC *cc);

static long const_primary(CC *cc)
{
    long v;
    if (is_punct(cc, '(')) {
        lex_next(cc);
        /* a cast inside a constant expression is a no-op for our purposes */
        if (is_type_start(cc)) {
            type_name(cc);
            expect(cc, ')', "')'");
            return const_primary(cc);
        }
        v = const_cond(cc);
        expect(cc, ')', "')'");
        return v;
    }
    if (is_kw(cc, KW_SIZEOF)) {
        Type *t;
        lex_next(cc);
        if (is_punct(cc, '(')) {
            lex_next(cc);
            if (is_type_start(cc)) {
                t = type_name(cc);
                expect(cc, ')', "')'");
                return t->size;
            }
            v = const_cond(cc);
            expect(cc, ')', "')'");
            (void)v;
            return 4;
        }
        return 4;
    }
    if (cc->tok.kind == TK_NUM) {
        v = (long)cc->tok.llval;
        lex_next(cc);
        return v;
    }
    if (cc->tok.kind == TK_IDENT) {
        Sym *s = find_sym(cc, cc->tok.name);
        if (s && s->kind == S_ENUM) {
            v = s->enumval;
            lex_next(cc);
            return v;
        }
        cc_error(cc, "'%s' is not a constant", cc->tok.name);
    }
    cc_error(cc, "expression is not a constant");
    return 0;
}

static long const_unary(CC *cc)
{
    if (accept(cc, '-')) return -const_unary(cc);
    if (accept(cc, '+')) return const_unary(cc);
    if (accept(cc, '~')) return ~const_unary(cc);
    if (accept(cc, '!')) return !const_unary(cc);
    return const_primary(cc);
}

static long const_binary(CC *cc, int minprec)
{
    long lhs = const_unary(cc);
    for (;;) {
        int p = prec_of(cc), op;
        long rhs;
        if (p == 0 || p < minprec) break;
        op = cc->tok.val;
        lex_next(cc);
        rhs = const_binary(cc, p + 1);
        switch (op) {
        case '*': lhs = lhs * rhs; break;
        case '/': if (!rhs) cc_error(cc, "division by zero in a constant");
                  lhs = lhs / rhs; break;
        case '%': if (!rhs) cc_error(cc, "division by zero in a constant");
                  lhs = lhs % rhs; break;
        case '+': lhs = lhs + rhs; break;
        case '-': lhs = lhs - rhs; break;
        case P_SHL: lhs = lhs << (rhs & 63); break;
        case P_SHR: lhs = lhs >> (rhs & 63); break;
        case '<': lhs = lhs < rhs; break;
        case '>': lhs = lhs > rhs; break;
        case P_LE: lhs = lhs <= rhs; break;
        case P_GE: lhs = lhs >= rhs; break;
        case P_EQ: lhs = lhs == rhs; break;
        case P_NE: lhs = lhs != rhs; break;
        case '&': lhs = lhs & rhs; break;
        case '^': lhs = lhs ^ rhs; break;
        case '|': lhs = lhs | rhs; break;
        default: cc_error(cc, "operator not allowed in a constant");
        }
    }
    return lhs;
}

static long const_cond(CC *cc)
{
    long v = const_binary(cc, 1);
    while (is_punct(cc, P_ANDAND)) {
        lex_next(cc);
        v = (const_binary(cc, 1) != 0) && (v != 0);
    }
    while (is_punct(cc, P_OROR)) {
        lex_next(cc);
        v = (const_binary(cc, 1) != 0) || (v != 0);
    }
    if (is_punct(cc, '?')) {
        long a, b;
        lex_next(cc);
        a = const_cond(cc);
        expect(cc, ':', "':'");
        b = const_cond(cc);
        return v ? a : b;
    }
    return v;
}

static long const_expr(CC *cc) { return const_cond(cc); }

/* ------------------------------------------------------------------ */
/* types and declarations                                              */
/* ------------------------------------------------------------------ */

static int is_type_start(CC *cc)
{
    if (cc->tok.kind != TK_KW) {
        if (cc->tok.kind == TK_IDENT) {
            Sym *s = find_sym(cc, cc->tok.name);
            return s && s->kind == S_TYPEDEF;
        }
        return 0;
    }
    switch (cc->tok.val) {
    case KW_INT: case KW_CHAR: case KW_SHORT: case KW_LONG: case KW_UNSIGNED:
    case KW_SIGNED: case KW_FLOAT: case KW_DOUBLE: case KW_VOID:
    case KW_STRUCT: case KW_UNION: case KW_ENUM: case KW_TYPEDEF:
    case KW_STATIC: case KW_EXTERN: case KW_CONST:
        return 1;
    default: return 0;
    }
}

static Type *find_tag(CC *cc, const char *tag, TKind kind)
{
    Type *t;
    for (t = cc->tags; t; t = t->next)
        if (t->kind == kind && strcmp(t->tag, tag) == 0) return t;
    return NULL;
}

static Type *struct_decl(CC *cc, TKind kind)
{
    char tag[CC_MAXNAME];
    Type *t = NULL;
    int offset = 0, maxalign = 1;

    tag[0] = 0;
    lex_next(cc);
    if (cc->tok.kind == TK_IDENT) {
        strncpy(tag, cc->tok.name, CC_MAXNAME - 1);
        lex_next(cc);
        t = find_tag(cc, tag, kind);
    }
    if (!t) {
        t = (Type *)cc_alloc(cc, sizeof(Type));
        t->kind = kind;
        t->align = 1;
        t->nelem = -1;
        strncpy(t->tag, tag, CC_MAXNAME - 1);
        t->next = cc->tags;
        cc->tags = t;
    }
    if (!is_punct(cc, '{')) {
        if (!t->complete && !t->members) t->complete = 0;
        return t;
    }
    lex_next(cc);
    while (!is_punct(cc, '}')) {
        int sc = 0;
        Type *base = declspec(cc, &sc);
        for (;;) {
            char name[CC_MAXNAME];
            Member *m = (Member *)cc_alloc(cc, sizeof(Member));
            Type *mt = declarator(cc, base, name);
            int a = mt->align;
            strncpy(m->name, name, CC_MAXNAME - 1);
            m->type = mt;
            if (kind == T_STRUCT) {
                offset = (offset + a - 1) & ~(a - 1);
                m->offset = offset;
                offset += mt->size;
            } else {
                m->offset = 0;
                if (mt->size > offset) offset = mt->size;
            }
            if (a > maxalign) maxalign = a;
            /* append, so declaration order is preserved */
            if (!t->members) t->members = m;
            else {
                Member *q = t->members;
                while (q->next) q = q->next;
                q->next = m;
            }
            if (accept(cc, ',')) continue;
            break;
        }
        expect(cc, ';', "';' after a struct member");
    }
    lex_next(cc);
    t->size = (offset + maxalign - 1) & ~(maxalign - 1);
    t->align = maxalign;
    t->complete = 1;
    return t;
}

static void enum_decl(CC *cc)
{
    int val = 0;
    lex_next(cc);
    if (cc->tok.kind == TK_IDENT) lex_next(cc);       /* tag, unused */
    if (!accept(cc, '{')) return;
    while (!is_punct(cc, '}')) {
        char name[CC_MAXNAME];
        Sym *s;
        if (cc->tok.kind != TK_IDENT) cc_error(cc, "expected an enumerator");
        strncpy(name, cc->tok.name, CC_MAXNAME - 1);
        lex_next(cc);
        if (accept(cc, '=')) val = (int)const_expr(cc);
        s = (Sym *)cc_alloc(cc, sizeof(Sym));
        strncpy(s->name, name, CC_MAXNAME - 1);
        s->kind = S_ENUM;
        s->type = ty_int;
        s->enumval = val++;
        if (cc->curfn) { s->next = cc->locals; cc->locals = s; }
        else { s->next = cc->globals; cc->globals = s; }
        if (accept(cc, ',')) continue;
        break;
    }
    expect(cc, '}', "'}'");
}

static Type *declspec(CC *cc, int *sclass)
{
    int sign = 0, nlong = 0, nshort = 0;
    Type *base = NULL;
    if (sclass) *sclass = 0;

    for (;;) {
        if (is_kw(cc, KW_STATIC))  { if (sclass) *sclass = KW_STATIC;  lex_next(cc); continue; }
        if (is_kw(cc, KW_EXTERN))  { if (sclass) *sclass = KW_EXTERN;  lex_next(cc); continue; }
        if (is_kw(cc, KW_TYPEDEF)) { if (sclass) *sclass = KW_TYPEDEF; lex_next(cc); continue; }
        if (is_kw(cc, KW_CONST))   { lex_next(cc); continue; }
        if (is_kw(cc, KW_UNSIGNED)) { sign = -1; lex_next(cc); continue; }
        if (is_kw(cc, KW_SIGNED))   { sign = 1; lex_next(cc); continue; }
        if (is_kw(cc, KW_LONG))  { nlong++; lex_next(cc); continue; }
        if (is_kw(cc, KW_SHORT)) { nshort++; lex_next(cc); continue; }
        if (is_kw(cc, KW_INT))    { if (!base) base = ty_int; lex_next(cc); continue; }
        if (is_kw(cc, KW_CHAR))   { base = ty_char; lex_next(cc); continue; }
        if (is_kw(cc, KW_FLOAT))  { base = ty_float; lex_next(cc); continue; }
        if (is_kw(cc, KW_DOUBLE)) { base = ty_double; lex_next(cc); continue; }
        if (is_kw(cc, KW_VOID))   { base = ty_void; lex_next(cc); continue; }
        if (is_kw(cc, KW_STRUCT)) { base = struct_decl(cc, T_STRUCT); continue; }
        if (is_kw(cc, KW_UNION))  { base = struct_decl(cc, T_UNION); continue; }
        if (is_kw(cc, KW_ENUM))   { enum_decl(cc); base = ty_int; continue; }
        if (cc->tok.kind == TK_IDENT && !base) {
            Sym *s = find_sym(cc, cc->tok.name);
            if (s && s->kind == S_TYPEDEF) { base = s->type; lex_next(cc); continue; }
        }
        break;
    }
    if (!base) base = ty_int;
    if (base == ty_char && sign == -1) base = ty_uchar;
    if (nlong >= 2 && base == ty_int) base = (sign == -1) ? ty_ullong : ty_llong;
    else if (nshort && (base == ty_int)) base = (sign == -1) ? ty_ushort : ty_short;
    else if (base == ty_int && sign == -1) base = ty_uint;
    return base;
}

/* Read a parameter list that belongs to a function *type* rather than a
 * definition: only the types matter, names are allowed but discarded. */
static Type *func_type(CC *cc, Type *ret)
{
    Type *ft = new_type(cc, T_FUNC, 1, 1);
    Type *ptypes[CC_MAXPARAMS];
    int np = 0, variadic = 0;

    ft->to = ret;
    expect(cc, '(', "'('");
    if (!is_punct(cc, ')')) {
        for (;;) {
            char pname[CC_MAXNAME];
            Type *pt;
            if (is_punct(cc, P_ELLIPSIS)) { lex_next(cc); variadic = 1; break; }
            if (is_kw(cc, KW_VOID) && lex_peek(cc)->kind == TK_PUNCT
                && lex_peek(cc)->val == ')') { lex_next(cc); break; }
            pt = declarator(cc, declspec(cc, NULL), pname);
            if (pt->kind == T_ARRAY) pt = ptr_to(cc, pt->to);
            if (pt->kind == T_FLOAT) pt = ty_double;
            if (np < CC_MAXPARAMS) ptypes[np++] = pt;
            if (accept(cc, ',')) continue;
            break;
        }
    }
    expect(cc, ')', "')'");
    if (np) {
        int i;
        ft->ptypes = (Type **)cc_alloc(cc, sizeof(Type *) * (size_t)np);
        for (i = 0; i < np; i++) ft->ptypes[i] = ptypes[i];
    }
    ft->nparams = np;
    ft->variadic = variadic;
    return ft;
}

static Type *declarator(CC *cc, Type *base, char *name)
{
    Type *t = base;
    Type *hole = NULL, *inner = NULL;

    while (accept(cc, '*')) {
        while (is_kw(cc, KW_CONST)) lex_next(cc);
        t = ptr_to(cc, t);
    }
    if (name) name[0] = 0;

    /* A parenthesised declarator, as in double (*f)(double). What the
     * parentheses contain is applied to whatever the suffixes after them
     * build, so the inner declarator is parsed against a placeholder that
     * is filled in once the outer type is known. */
    if (is_punct(cc, '(')) {
        lex_next(cc);
        hole = new_type(cc, T_VOID, 1, 1);
        inner = declarator(cc, hole, name);
        expect(cc, ')', "')'");
    } else if (cc->tok.kind == TK_IDENT) {
        if (name) strncpy(name, cc->tok.name, CC_MAXNAME - 1);
        lex_next(cc);
    }

    /* array suffix; a plain function suffix is handled by the caller so it
     * can read the parameter list in the right scope, but inside a
     * parenthesised declarator it belongs here */
    for (;;) {
        if (is_punct(cc, '[')) {
            int n = -1;
            lex_next(cc);
            if (!is_punct(cc, ']')) n = (int)const_expr(cc);
            expect(cc, ']', "']'");
            /* build arrays outermost-first so int a[2][3] is 2 of (3 ints) */
            t = array_of(cc, t, n);
            continue;
        }
        if (hole && is_punct(cc, '(')) { t = func_type(cc, t); continue; }
        break;
    }

    if (hole) {
        /* The placeholder becomes the outer type, but it keeps its own
         * position in the allocation chain: taking t's would splice it in
         * ahead of itself and make the chain circular. */
        Type *keep = hole->next;
        *hole = *t;
        hole->next = keep;
        return inner;
    }
    return t;
}

static Type *type_name(CC *cc)
{
    int sc;
    Type *base = declspec(cc, &sc);
    while (accept(cc, '*')) {
        while (is_kw(cc, KW_CONST)) lex_next(cc);
        base = ptr_to(cc, base);
    }
    while (is_punct(cc, '[')) {
        int n = -1;
        lex_next(cc);
        if (!is_punct(cc, ']')) n = (int)const_expr(cc);
        expect(cc, ']', "']'");
        base = array_of(cc, base, n);
    }
    return base;
}

/* ------------------------------------------------------------------ */
/* initializers                                                        */
/* ------------------------------------------------------------------ */

static void store_const_to_data(CC *cc, uint32_t addr, Type *t, long iv, double dv, int isd)
{
    uint8_t *p = cc->data + (addr - cc->data_base);
    if (t->kind == T_DOUBLE) {
        union { double d; uint8_t b[8]; } u;
        u.d = isd ? dv : (double)iv;
        memcpy(p, u.b, 8);
        return;
    }
    if (t->kind == T_FLOAT) {
        union { float f; uint8_t b[4]; } u;
        u.f = (float)(isd ? dv : (double)iv);
        memcpy(p, u.b, 4);
        return;
    }
    {
        long v = isd ? (long)dv : iv;
        if (t->size >= 1) p[0] = (uint8_t)v;
        if (t->size >= 2) p[1] = (uint8_t)(v >> 8);
        if (t->size >= 4) { p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
    }
}

/* A constant initializer: a number, a string, or a brace list of them. */
static void global_init(CC *cc, uint32_t addr, Type *t)
{
    if (cc->tok.kind == TK_STR && t->kind == T_ARRAY && t->to->size == 1) {
        int n = cc->tok.slen + 1;
        if (t->nelem < 0) {
            /* size the array from the literal */
            t->nelem = n;
            t->size = n;
        }
        if (n > t->size) n = t->size;
        memcpy(cc->data + (addr - cc->data_base), cc->tok.str, (size_t)n);
        lex_next(cc);
        return;
    }
    if (accept(cc, '{')) {
        int i = 0;
        Type *et = t->kind == T_ARRAY ? t->to : NULL;
        Member *m = (t->kind == T_STRUCT || t->kind == T_UNION) ? t->members : NULL;
        while (!is_punct(cc, '}')) {
            if (et) {
                global_init(cc, addr + (uint32_t)(i * et->size), et);
                i++;
            } else if (m) {
                global_init(cc, addr + (uint32_t)m->offset, m->type);
                m = m->next;
            } else cc_error(cc, "too many initializers");
            if (accept(cc, ',')) continue;
            break;
        }
        expect(cc, '}', "'}'");
        if (t->kind == T_ARRAY && t->nelem < 0) {
            t->nelem = i;
            t->size = i * t->to->size;
        }
        return;
    }
    if (cc->tok.kind == TK_DNUM) {
        store_const_to_data(cc, addr, t, 0, cc->tok.dval, 1);
        lex_next(cc);
        return;
    }
    {
        long v = const_expr(cc);
        store_const_to_data(cc, addr, t, v, 0, 0);
    }
}

/* Local initializer: emits code. */
static void local_init(CC *cc, Sym *s, Type *t, int base_off)
{
    if (cc->tok.kind == TK_STR && t->kind == T_ARRAY && t->to->size == 1) {
        uint32_t a = data_alloc(cc, cc->tok.slen + 1, 1);
        int n = cc->tok.slen + 1, i;
        memcpy(cc->data + (a - cc->data_base), cc->tok.str, (size_t)n);
        if (t->nelem < 0) { t->nelem = n; t->size = n; }
        if (n > t->size) n = t->size;
        em_imm32(E, R1, a);
        for (i = 0; i < n; i++) {
            em_ldrb(E, R2, R1, i);
            em_add_imm(E, R3, R7, base_off + i);
            em_strb(E, R2, R3, 0);
        }
        lex_next(cc);
        return;
    }
    if (accept(cc, '{')) {
        int i = 0;
        Type *et = t->kind == T_ARRAY ? t->to : NULL;
        Member *m = (t->kind == T_STRUCT || t->kind == T_UNION) ? t->members : NULL;
        while (!is_punct(cc, '}')) {
            if (et) { local_init(cc, s, et, base_off + i * et->size); i++; }
            else if (m) { local_init(cc, s, m->type, base_off + m->offset); m = m->next; }
            else cc_error(cc, "too many initializers");
            if (accept(cc, ',')) continue;
            break;
        }
        expect(cc, '}', "'}'");
        return;
    }
    {
        Type *rt = decay(cc, rvalue(cc, assign_expr(cc)));
        if (!is_ptr(t) && t->kind != T_STRUCT && t->kind != T_UNION)
            convert(cc, rt, t);
        em_add_imm(E, R4, R7, base_off);
        store_indirect(cc, t, R4);
        lval = 0;
    }
}

/* ------------------------------------------------------------------ */
/* statements                                                          */
/* ------------------------------------------------------------------ */

static Label *find_label(CC *cc, const char *name)
{
    Label *l;
    for (l = labels; l; l = l->next)
        if (strcmp(l->name, name) == 0) return l;
    l = (Label *)cc_alloc(cc, sizeof(Label));
    strncpy(l->name, name, CC_MAXNAME - 1);
    l->off = -1;
    l->next = labels;
    labels = l;
    return l;
}

static void local_decl(CC *cc)
{
    int sc = 0;
    Type *base = declspec(cc, &sc);
    if (accept(cc, ';')) return;
    for (;;) {
        char name[CC_MAXNAME];
        Type *t = declarator(cc, base, name);
        if (sc == KW_TYPEDEF) {
            Sym *s = (Sym *)cc_alloc(cc, sizeof(Sym));
            strncpy(s->name, name, CC_MAXNAME - 1);
            s->kind = S_TYPEDEF;
            s->type = t;
            s->next = cc->locals;
            cc->locals = s;
        } else if (sc == KW_STATIC) {
            /* A static local is a global that only this function can name:
             * it lives in the data segment, is initialised once when the
             * program is loaded rather than on every call, and so keeps its
             * value between calls. Putting the symbol on the local list is
             * what scopes the name to the function. */
            Sym *s = (Sym *)cc_alloc(cc, sizeof(Sym));
            if (is_punct(cc, '=') && t->kind == T_ARRAY && t->nelem < 0) {
                Token *p = lex_peek(cc);
                if (p->kind == TK_STR) {
                    t->nelem = p->slen + 1;
                    t->size = t->nelem * t->to->size;
                }
            }
            strncpy(s->name, name, CC_MAXNAME - 1);
            s->kind = S_GLOBAL;
            s->type = t;
            s->addr = data_alloc(cc, t->size ? t->size : 4, t->align);
            s->next = cc->locals;
            cc->locals = s;
            if (accept(cc, '=')) global_init(cc, s->addr, t);
        } else {
            Sym *s;
            if (is_punct(cc, '=')) {
                /* an array with no size takes it from the initializer; the
                 * slot must be allocated after that is known */
                if (t->kind == T_ARRAY && t->nelem < 0) {
                    Token *p = lex_peek(cc);
                    if (p->kind == TK_STR) { t->nelem = p->slen + 1; t->size = t->nelem * t->to->size; }
                }
            }
            s = add_local(cc, name, t);
            if (accept(cc, '=')) local_init(cc, s, t, s->offset);
        }
        if (accept(cc, ',')) continue;
        break;
    }
    expect(cc, ';', "';'");
}

static void block(CC *cc)
{
    Sym *save_locals = cc->locals;
    int save_frame = cc->frame;
    expect(cc, '{', "'{'");
    while (!is_punct(cc, '}')) {
        if (cc->tok.kind == TK_EOF) cc_error(cc, "unexpected end of file in a block");
        statement(cc);
    }
    lex_next(cc);
    cc->locals = save_locals;
    cc->frame = save_frame;
}

static void statement(CC *cc)
{
    if (is_punct(cc, '{')) { block(cc); return; }
    if (accept(cc, ';')) return;

    if (is_type_start(cc)) { local_decl(cc); return; }

    if (is_kw(cc, KW_IF)) {
        int bfalse, bend = -1;
        lex_next(cc);
        expect(cc, '(', "'(' after if");
        {
            Type *t = rvalue(cc, decay(cc, expr(cc)));
            to_truth_word(cc, t);
        }
        expect(cc, ')', "')'");
        em_cmp_imm(E, R0, 0);
        bfalse = em_bcc(E, CC_EQ, -1);
        statement(cc);
        if (is_kw(cc, KW_ELSE)) bend = em_b(E, -1);
        em_patch_branch(E, bfalse, emit_here(E));
        if (accept_kw(cc, KW_ELSE)) {
            statement(cc);
            em_patch_branch(E, bend, emit_here(E));
        }
        return;
    }

    if (is_kw(cc, KW_WHILE)) {
        int top_off, bexit, save_cont = cc->cont_target, save_base = cc->brk_base;
        lex_next(cc);
        top_off = emit_here(E);
        cc->cont_target = top_off;
        cc->brk_base = cc->nbrk;
        expect(cc, '(', "'(' after while");
        {
            Type *t = rvalue(cc, decay(cc, expr(cc)));
            to_truth_word(cc, t);
        }
        expect(cc, ')', "')'");
        em_cmp_imm(E, R0, 0);
        bexit = em_bcc(E, CC_EQ, -1);
        statement(cc);
        em_patch_branch(E, em_b(E, -1), top_off);
        em_patch_branch(E, bexit, emit_here(E));
        while (cc->nbrk > cc->brk_base)
            em_patch_branch(E, cc->brk[--cc->nbrk], emit_here(E));
        cc->cont_target = save_cont;
        cc->brk_base = save_base;
        return;
    }

    if (is_kw(cc, KW_DO)) {
        int top_off, save_cont = cc->cont_target, save_base = cc->brk_base;
        int cont_off;
        lex_next(cc);
        top_off = emit_here(E);
        cc->brk_base = cc->nbrk;
        statement(cc);
        cont_off = emit_here(E);
        cc->cont_target = cont_off;
        if (!accept_kw(cc, KW_WHILE)) cc_error(cc, "expected 'while' after do");
        expect(cc, '(', "'('");
        {
            Type *t = rvalue(cc, decay(cc, expr(cc)));
            to_truth_word(cc, t);
        }
        expect(cc, ')', "')'");
        expect(cc, ';', "';'");
        em_cmp_imm(E, R0, 0);
        em_patch_branch(E, em_bcc(E, CC_NE, -1), top_off);
        while (cc->nbrk > cc->brk_base)
            em_patch_branch(E, cc->brk[--cc->nbrk], emit_here(E));
        cc->cont_target = save_cont;
        cc->brk_base = save_base;
        return;
    }

    if (is_kw(cc, KW_FOR)) {
        int cond_off, bexit = -1, body_b, step_off, save_cont, save_base;
        Sym *save_locals = cc->locals;
        int save_frame = cc->frame;
        lex_next(cc);
        expect(cc, '(', "'(' after for");
        if (!is_punct(cc, ';')) {
            if (is_type_start(cc)) local_decl(cc);
            else { rvalue(cc, expr(cc)); expect(cc, ';', "';'"); }
        } else lex_next(cc);

        cond_off = emit_here(E);
        if (!is_punct(cc, ';')) {
            Type *t = rvalue(cc, decay(cc, expr(cc)));
            to_truth_word(cc, t);
            em_cmp_imm(E, R0, 0);
            bexit = em_bcc(E, CC_EQ, -1);
        }
        expect(cc, ';', "';'");

        body_b = em_b(E, -1);                 /* jump over the step */
        step_off = emit_here(E);
        if (!is_punct(cc, ')')) rvalue(cc, expr(cc));
        expect(cc, ')', "')'");
        em_patch_branch(E, em_b(E, -1), cond_off);

        em_patch_branch(E, body_b, emit_here(E));
        save_cont = cc->cont_target;
        save_base = cc->brk_base;
        cc->cont_target = step_off;
        cc->brk_base = cc->nbrk;
        statement(cc);
        em_patch_branch(E, em_b(E, -1), step_off);
        if (bexit >= 0) em_patch_branch(E, bexit, emit_here(E));
        while (cc->nbrk > cc->brk_base)
            em_patch_branch(E, cc->brk[--cc->nbrk], emit_here(E));
        cc->cont_target = save_cont;
        cc->brk_base = save_base;
        cc->locals = save_locals;
        cc->frame = save_frame;
        return;
    }

    if (is_kw(cc, KW_SWITCH)) {
        int cases[CC_MAXCASES], case_off[CC_MAXCASES], ncase = 0;
        int default_off = -1, bdispatch, i;
        int save_base = cc->brk_base, save_cont = cc->cont_target;
        Sym *tmp;
        lex_next(cc);
        expect(cc, '(', "'(' after switch");
        {
            Type *t = rvalue(cc, decay(cc, expr(cc)));
            to_truth_word(cc, t);
        }
        expect(cc, ')', "')'");
        tmp = add_local(cc, "@switch", ty_int);
        em_add_imm(E, R4, R7, tmp->offset);
        em_str(E, R0, R4, 0);
        bdispatch = em_b(E, -1);
        cc->brk_base = cc->nbrk;

        /* compile the body, recording where each label lands */
        expect(cc, '{', "'{' after switch");
        while (!is_punct(cc, '}')) {
            if (cc->tok.kind == TK_EOF) cc_error(cc, "unterminated switch");
            if (is_kw(cc, KW_CASE)) {
                lex_next(cc);
                if (ncase >= CC_MAXCASES) cc_error(cc, "too many case labels");
                cases[ncase] = (int)const_expr(cc);
                case_off[ncase] = emit_here(E);
                ncase++;
                expect(cc, ':', "':' after case");
                continue;
            }
            if (is_kw(cc, KW_DEFAULT)) {
                lex_next(cc);
                expect(cc, ':', "':' after default");
                default_off = emit_here(E);
                continue;
            }
            statement(cc);
        }
        lex_next(cc);
        {
            int bend = em_b(E, -1);
            em_patch_branch(E, bdispatch, emit_here(E));
            em_add_imm(E, R4, R7, tmp->offset);
            em_ldr(E, R0, R4, 0);
            for (i = 0; i < ncase; i++) {
                em_imm32(E, R2, (uint32_t)cases[i]);
                em_cmp(E, R0, R2);
                em_patch_branch(E, em_bcc(E, CC_EQ, -1), case_off[i]);
            }
            if (default_off >= 0) em_patch_branch(E, em_b(E, -1), default_off);
            em_patch_branch(E, bend, emit_here(E));
        }
        while (cc->nbrk > cc->brk_base)
            em_patch_branch(E, cc->brk[--cc->nbrk], emit_here(E));
        cc->brk_base = save_base;
        cc->cont_target = save_cont;
        return;
    }

    if (is_kw(cc, KW_BREAK)) {
        lex_next(cc);
        expect(cc, ';', "';'");
        if (cc->nbrk >= CC_MAXBREAK) cc_error(cc, "too many breaks");
        cc->brk[cc->nbrk++] = em_b(E, -1);
        return;
    }
    if (is_kw(cc, KW_CONTINUE)) {
        lex_next(cc);
        expect(cc, ';', "';'");
        if (cc->cont_target < 0) cc_error(cc, "'continue' outside a loop");
        em_patch_branch(E, em_b(E, -1), cc->cont_target);
        return;
    }
    if (is_kw(cc, KW_RETURN)) {
        lex_next(cc);
        if (!is_punct(cc, ';')) {
            Type *t = rvalue(cc, decay(cc, expr(cc)));
            Type *rt = cc->curfn->type->to;
            if (is_ll(rt) || is_ll(t)) convert(cc, t, rt);
            else if (is_dbl(rt) && !is_dbl(t)) to_double(cc, t);
            else if (!is_dbl(rt) && is_dbl(t) && rt->kind != T_VOID) {
                to_int_from_double(cc);
                narrow(cc, rt);
            }
        }
        expect(cc, ';', "';'");
        if (cc->nret >= CC_MAXBREAK) cc_error(cc, "too many return statements");
        cc->ret[cc->nret++] = em_b(E, -1);
        return;
    }
    if (is_kw(cc, KW_GOTO)) {
        Label *l;
        lex_next(cc);
        if (cc->tok.kind != TK_IDENT) cc_error(cc, "expected a label");
        l = find_label(cc, cc->tok.name);
        lex_next(cc);
        expect(cc, ';', "';'");
        if (l->off >= 0) em_patch_branch(E, em_b(E, -1), l->off);
        else {
            Fixup *f = (Fixup *)cc_alloc(cc, sizeof(Fixup));
            f->at = em_b(E, -1);
            f->next = l->pending;
            l->pending = f;
        }
        return;
    }
    /* label: */
    if (cc->tok.kind == TK_IDENT) {
        Token *p = lex_peek(cc);
        if (p->kind == TK_PUNCT && p->val == ':') {
            Label *l = find_label(cc, cc->tok.name);
            Fixup *f;
            lex_next(cc);
            lex_next(cc);
            l->off = emit_here(E);
            for (f = l->pending; f; f = f->next) em_patch_branch(E, f->at, l->off);
            l->pending = NULL;
            statement(cc);
            return;
        }
    }

    rvalue(cc, expr(cc));
    expect(cc, ';', "';'");
}

/* ------------------------------------------------------------------ */
/* functions                                                           */
/* ------------------------------------------------------------------ */

#define PROLOGUE_SAVE ((1u << R4) | (1u << R5) | (1u << R6) | (1u << R7) \
                     | (1u << R8) | (1u << LR))
#define EPILOGUE_REST ((1u << R4) | (1u << R5) | (1u << R6) | (1u << R7) \
                     | (1u << R8) | (1u << PC))

static void patch_frame(CC *cc, int at, int size)
{
    uint8_t *b = cc->code.buf;
    unsigned u = (unsigned)size & 0xFFF;
    unsigned i = (u >> 11) & 1, imm3 = (u >> 8) & 7, imm8 = u & 0xFF;
    unsigned h1 = (unsigned)(b[at] | (b[at + 1] << 8));
    unsigned op = h1 & 0xFBF0u;               /* keep addw/subw selector */
    h1 = op | (i << 10) | (unsigned)SP;
    b[at + 0] = (uint8_t)(h1 & 0xFF);
    b[at + 1] = (uint8_t)(h1 >> 8);
    b[at + 2] = (uint8_t)(((imm3 << 12) | ((unsigned)SP << 8) | imm8) & 0xFF);
    b[at + 3] = (uint8_t)((((imm3 << 12) | ((unsigned)SP << 8) | imm8) >> 8) & 0xFF);
}

static void fn_body(CC *cc, Sym *fn, Type *params[], char pnames[][CC_MAXNAME],
                    int nparams)
{
    int i, ncore = 0, stack_off = 0;
    int ret_chain;

    cc->curfn = fn;
    cc->frame = 0;
    cc->maxframe = 0;
    cc->locals = NULL;
    cc->nbrk = 0;
    cc->brk_base = 0;
    cc->cont_target = -1;
    labels = NULL;

    fn->code_off = emit_here(E);
    fn->defined = 1;

    em_push(E, PROLOGUE_SAVE);
    em_mov(E, R8, SP);
    cc->prologue_at = emit_here(E);
    em_subw_fixed(E, SP, SP, 0);              /* patched with the frame size */
    em_mov(E, R7, SP);

    /* give every parameter a slot, then copy the incoming values in */
    for (i = 0; i < nparams; i++) {
        Sym *s;
        Type *t = params[i];
        if (t->kind == T_ARRAY) t = ptr_to(cc, t->to);
        if (t->kind == T_FLOAT) t = ty_double;
        s = add_local(cc, pnames[i], t);
        if (is_dbl(t)) {
            if (ncore & 1) ncore++;
            if (ncore + 2 <= 4) {
                em_add_imm(E, R4, R7, s->offset);
                em_str(E, ncore, R4, 0);
                em_str(E, ncore + 1, R4, 4);
                ncore += 2;
            } else {
                stack_off = (stack_off + 7) & ~7;
                em_ldr(E, R2, R8, 24 + stack_off);
                em_add_imm(E, R4, R7, s->offset);
                em_str(E, R2, R4, 0);
                em_ldr(E, R2, R8, 24 + stack_off + 4);
                em_str(E, R2, R4, 4);
                stack_off += 8;
                ncore = 4;
            }
        } else {
            if (ncore < 4) {
                em_add_imm(E, R4, R7, s->offset);
                em_str(E, ncore, R4, 0);
                ncore++;
            } else {
                em_ldr(E, R2, R8, 24 + stack_off);
                em_add_imm(E, R4, R7, s->offset);
                em_str(E, R2, R4, 0);
                stack_off += 4;
            }
        }
    }

    cc->nret = 0;
    (void)ret_chain;

    block(cc);

    /* epilogue: every 'return' branches here */
    {
        int epi = emit_here(E);
        while (cc->nret > 0) em_patch_branch(E, cc->ret[--cc->nret], epi);
        cc->epilogue_at = emit_here(E);
        em_addw_fixed(E, SP, SP, 0);          /* patched with the frame size */
        em_pop(E, EPILOGUE_REST);
    }

    {
        int fsize = (cc->maxframe + 7) & ~7;
        patch_frame(cc, cc->prologue_at, fsize);
        patch_frame(cc, cc->epilogue_at, fsize);
    }

    /* resolve forward calls to this function */
    {
        Fixup *f;
        for (f = fn->fixups; f; f = f->next)
            patch_movw_movt(cc, f->at, f->reg, emit_addr(E, fn->code_off) | 1u);
        fn->fixups = NULL;
    }

    cc->curfn = NULL;
    cc->locals = NULL;
}

/* ------------------------------------------------------------------ */
/* top level                                                           */
/* ------------------------------------------------------------------ */

static void toplevel(CC *cc)
{
    int sc = 0;
    Type *base = declspec(cc, &sc);

    if (accept(cc, ';')) return;

    for (;;) {
        char name[CC_MAXNAME];
        Type *t = declarator(cc, base, name);

        if (is_punct(cc, '(')) {
            Type *params[CC_MAXARGS];
            char pnames[CC_MAXARGS][CC_MAXNAME];
            int nparams = 0, variadic = 0;
            Type *fnty = new_type(cc, T_FUNC, 4, 4);
            Sym *fn;
            fnty->to = t;
            lex_next(cc);
            if (!is_punct(cc, ')')) {
                for (;;) {
                    int psc = 0;
                    Type *pb, *pt;
                    if (is_punct(cc, P_ELLIPSIS)) { lex_next(cc); variadic = 1; break; }
                    pb = declspec(cc, &psc);
                    pt = declarator(cc, pb, pnames[nparams]);
                    if (pt->kind == T_VOID && nparams == 0 && is_punct(cc, ')')) break;
                    if (nparams >= CC_MAXARGS) cc_error(cc, "too many parameters");
                    params[nparams] = pt;
                    nparams++;
                    if (accept(cc, ',')) continue;
                    break;
                }
            }
            expect(cc, ')', "')' after parameters");

            fn = add_global(cc, name, fnty, S_FUNC);
            fn->type = fnty;
            if (!fn->defined) fn->code_off = -1;
            if (!fn->defined || nparams) {
                /* Record the signature. A later prototype refines whatever a
                 * runtime import declared, which is how sqrt(2) learns that
                 * its argument must be widened. */
                int k, alld = 1;
                fn->ptypes = (Type **)cc_alloc(cc,
                                   sizeof(Type *) * (size_t)(nparams ? nparams : 1));
                for (k = 0; k < nparams; k++) {
                    Type *pt = params[k];
                    if (pt->kind == T_ARRAY) pt = ptr_to(cc, pt->to);
                    if (pt->kind == T_FLOAT) pt = ty_double;
                    fn->ptypes[k] = pt;
                    if (!is_dbl(pt)) alld = 0;
                }
                fn->nparams = nparams;
                fn->param_dbl = nparams ? alld : 1;
                fn->ret_dbl = is_dbl(t);
                fn->variadic = variadic;
            }

            if (is_punct(cc, '{')) {
                fn_body(cc, fn, params, pnames, nparams);
                return;
            }
            expect(cc, ';', "';' after a declaration");
            return;
        }

        if (sc == KW_TYPEDEF) {
            Sym *s = (Sym *)cc_alloc(cc, sizeof(Sym));
            strncpy(s->name, name, CC_MAXNAME - 1);
            s->kind = S_TYPEDEF;
            s->type = t;
            s->next = cc->globals;
            cc->globals = s;
        } else if (name[0]) {
            Sym *s = add_global(cc, name, t, S_GLOBAL);
            if (is_punct(cc, '=')) {
                lex_next(cc);
                if (t->kind == T_ARRAY && t->nelem < 0) {
                    /* reserve after the size is known: peek the initializer */
                    Token *p = &cc->tok;
                    if (p->kind == TK_STR) { t->nelem = p->slen + 1; t->size = t->nelem; }
                }
                s->addr = data_alloc(cc, t->size ? t->size : 4, t->align);
                s->type = t;
                global_init(cc, s->addr, t);
            } else if (!s->addr) {
                s->addr = data_alloc(cc, t->size ? t->size : 4, t->align);
            }
        }
        if (accept(cc, ',')) continue;
        break;
    }
    expect(cc, ';', "';' after a declaration");
}

/* Declare the entry points the firmware exports.
 *
 * The signature is a compact string: the first character is the return type
 * and the rest are the parameters, with '.' marking varargs.
 *   'd' double   'i' int   'p' pointer   'v' void
 * So sin is "dd", pow is "ddd", printf is "ip." and plot_point is "viii".
 * Getting this right is what makes sqrt(2) widen its argument instead of
 * handing the callee an int where it expects a register pair.
 */
static Type *sigchar(int c)
{
    switch (c) {
    case 'd': return ty_double;
    case 'l': return ty_llong;
    case 'v': return ty_void;
    default:  return ty_int;          /* 'i' and 'p' are both one word */
    }
}

static void declare_runtime(CC *cc)
{
    int i;
    for (i = 0; i < cc->nrt; i++) {
        Sym *s;
        Type *fnty;
        const char *d = cc->rt[i].decl;
        int np = 0, k;
        if (!d || !*d) continue;
        s = add_global(cc, cc->rt[i].name, NULL, S_FUNC);
        fnty = new_type(cc, T_FUNC, 4, 4);
        fnty->to = sigchar(d[0]);
        s->type = fnty;
        s->addr = (uint32_t)(uintptr_t)cc->rt[i].fn;
        s->code_off = -1;
        s->defined = 1;
        s->ret_dbl = (d[0] == 'd');

        for (k = 1; d[k] && d[k] != '.'; k++) np++;
        if (np > CC_MAXARGS) np = CC_MAXARGS;
        s->variadic = (strchr(d, '.') != NULL);
        s->nparams = np;
        s->param_dbl = 1;
        s->ptypes = (Type **)cc_alloc(cc, sizeof(Type *) * (size_t)(np ? np : 1));
        for (k = 0; k < np; k++) {
            s->ptypes[k] = sigchar(d[k + 1]);
            if (d[k + 1] != 'd') s->param_dbl = 0;
        }
    }
}

int cc_compile(CC *cc, const char *src, const char *fname,
               uint8_t *code, int code_cap, uint32_t code_base,
               uint8_t *data, int data_cap, uint32_t data_base,
               const RtSym *rt, int nrt)
{
    Sym *s;

    cc->globals = NULL;
    cc->locals = NULL;
    cc->types = NULL;
    cc->tags = NULL;
    cc->macros = NULL;
    cc->curfn = NULL;
    cc->nerr = 0;
    cc->err[0] = 0;
    cc->data = data;
    cc->data_len = 0;
    cc->data_cap = data_cap;
    cc->data_base = data_base;
    cc->rt = rt;
    cc->nrt = nrt;
    cc->arena_used = 0;
    cc->ninc = 0;
    labels = NULL;
    lval = 0;

    /* Uninitialised globals and statics must read as zero, and the buffer
     * handed in is reused between compiles, so clear it up front. */
    if (data && data_cap > 0) memset(data, 0, (size_t)data_cap);

    emit_init(&cc->code, code, code_cap, code_base);

    if (setjmp(cc->jb)) return -1;

    init_types(cc);
    declare_runtime(cc);
    lex_init(cc, src, fname);

    while (cc->tok.kind != TK_EOF) toplevel(cc);

    for (s = cc->globals; s; s = s->next)
        if (s->kind == S_FUNC && s->fixups && !s->defined)
            cc_error(cc, "undefined function '%s'", s->name);

    if (cc->code.overflow) cc_error(cc, "program is too large");
    return 0;
}

/* Only functions with a compiled body: runtime imports are also S_FUNC and
 * marked defined, but they have no code offset of their own. */
Sym *cc_find_func(CC *cc, const char *name)
{
    Sym *s;
    for (s = cc->globals; s; s = s->next)
        if (s->kind == S_FUNC && s->defined && s->code_off >= 0
            && strcmp(s->name, name) == 0)
            return s;
    return NULL;
}
