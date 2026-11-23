/* CalcOS value model: arena memory, promotion rules, formatting. */
#include "calc.h"
#include "bignum.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ================================================================== */
/* arena                                                              */
/* ================================================================== */

struct Block {
    Block *next;
    size_t cap, used;
    char  *data;
};

#define BLOCK_MIN 4096

void *arena_alloc(Arena *a, size_t n)
{
    Block *b;
    size_t cap;
    void *p;
    n = (n + 15u) & ~(size_t)15u;          /* 16-byte alignment */
    for (b = a->head; b; b = b->next)
        if (b->cap - b->used >= n) break;
    if (!b) {
        cap = n > BLOCK_MIN ? n : BLOCK_MIN;
        b = (Block *)malloc(sizeof(Block));
        if (!b) return NULL;
        b->data = (char *)malloc(cap);
        if (!b->data) { free(b); return NULL; }
        b->cap = cap;
        b->used = 0;
        b->next = a->head;
        a->head = b;
        a->total += cap;
        if (a->total > a->peak) a->peak = a->total;
    }
    p = b->data + b->used;
    b->used += n;
    memset(p, 0, n);
    return p;
}

void arena_reset(Arena *a)
{
    Block *b = a->head, *nx;
    /* keep the first block, release the rest: typical lines reuse one block */
    if (!b) return;
    for (nx = b->next; nx; ) {
        Block *t = nx->next;
        a->total -= nx->cap;
        free(nx->data);
        free(nx);
        nx = t;
    }
    b->next = NULL;
    b->used = 0;
}

void arena_free(Arena *a)
{
    Block *b = a->head, *nx;
    while (b) { nx = b->next; free(b->data); free(b); b = nx; }
    a->head = NULL;
    a->total = a->peak = 0;
}

/* ================================================================== */
/* interpreter lifecycle                                              */
/* ================================================================== */

void cos_err(Interp *in, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(in->err, sizeof(in->err), fmt, ap);
    va_end(ap);
    if (in->jb_set) longjmp(in->jb, 1);
    /* no handler installed: fail loudly rather than silently continuing */
    plat_puts("fatal: ");
    plat_puts(in->err);
    plat_puts("\n");
    abort();
}

void *cos_alloc(Interp *in, size_t n)
{
    void *p = arena_alloc(&in->arena, n);
    if (!p) cos_err(in, "out of memory (%u bytes)", (unsigned)n);
    return p;
}

char *cos_strdup_arena(Interp *in, const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = (char *)cos_alloc(in, n);
    memcpy(p, s, n);
    return p;
}

void cos_init(Interp *in)
{
    memset(in, 0, sizeof(*in));
    in->digits = 15;
    in->ddigits = 32;
    dd_init_consts();
    cos_setvar(in, "pi",  v_real(M_PI));
    cos_setvar(in, "e",   v_real(M_E));
    cos_setvar(in, "phi", v_real(1.6180339887498948482));
    cos_setvar(in, "i",   v_num(cx_mk(0.0, 1.0)));
    cos_setvar(in, "eps", v_real(2.220446049250313e-16));
    cos_setvar(in, "inf", v_real(INFINITY));
    cos_setvar(in, "nan", v_real(NAN));
    cos_setvar(in, "ans", v_real(0.0));
}

void cos_shutdown(Interp *in)
{
    Var *v = in->globals, *nx;
    FuncDef *f = in->funcs, *fn;
    while (v) { nx = v->next; v_perm_free(&v->v); free(v); v = nx; }
    while (f) { fn = f->next; cos_fn_free(f); f = fn; }
    in->globals = NULL;
    in->funcs = NULL;
    cos_bigs_release(in);
    arena_free(&in->arena);
}

/* ================================================================== */
/* constructors                                                       */
/* ================================================================== */

Value v_nil(void)        { Value v; memset(&v, 0, sizeof v); v.k = K_NIL; return v; }
Value v_num(cx z)        { Value v = v_nil(); v.k = K_NUM; v.n = z; return v; }
Value v_real(double x)   { return v_num(cx_mk(x, 0.0)); }
Value v_dd(dd d)         { Value v = v_nil(); v.k = K_DD; v.d = d; return v; }
Value v_iv(iv i)         { Value v = v_nil(); v.k = K_IV; v.v = i; return v; }
Value v_fun(FuncDef *f)  { Value v = v_nil(); v.k = K_FUN; v.f = f; return v; }

/* Big integers are malloc'd, so every temporary made while evaluating a
 * line is chained here and released when the line's arena is. A value that
 * is kept — stored in a variable — is deep-copied by v_perm_dup first, so
 * releasing the temporaries never pulls the ground from under it. */
typedef struct BigNode { struct Big *b; struct BigNode *next; } BigNode;

Value v_big(Interp *in, struct Big *b)
{
    Value v = v_nil();
    BigNode *n;
    if (!b) cos_err(in, "the number is too large to work with");
    n = (BigNode *)malloc(sizeof *n);
    if (!n) { big_free(b); cos_err(in, "out of memory"); }
    n->b = b;
    n->next = (BigNode *)in->bigs;
    in->bigs = n;
    v.k = K_BIG;
    v.big = b;
    return v;
}

void cos_bigs_release(Interp *in)
{
    BigNode *n = (BigNode *)in->bigs, *nx;
    while (n) { nx = n->next; big_free(n->b); free(n); n = nx; }
    in->bigs = NULL;
}

Value v_str(Interp *in, const char *s)
{
    Value v = v_nil();
    v.k = K_STR;
    v.s = cos_strdup_arena(in, s);
    return v;
}

Value v_mat(Interp *in, int rows, int cols)
{
    Value v = v_nil();
    if (rows < 0 || cols < 0 || (long)rows * cols > COS_MAXELEM)
        cos_err(in, "matrix too large (%dx%d, limit %d elements)",
                rows, cols, COS_MAXELEM);
    v.k = K_MAT;
    v.rows = rows;
    v.cols = cols;
    v.m = (cx *)cos_alloc(in, (size_t)rows * (size_t)cols * sizeof(cx));
    return v;
}

/* ================================================================== */
/* coercion                                                           */
/* ================================================================== */

cx v_scalar(Interp *in, Value a)
{
    if (a.k == K_BIG) return cx_mk(big_to_double(a.big), 0.0);
    switch (a.k) {
    case K_NUM: return a.n;
    case K_DD:  return cx_mk(dd_to(a.d), 0.0);
    case K_IV:  return cx_mk(iv_mid(a.v), 0.0);
    case K_MAT:
        if (a.rows == 1 && a.cols == 1) return a.m[0];
        cos_err(in, "expected a scalar, got a %dx%d matrix", a.rows, a.cols);
        break;
    default:
        cos_err(in, "expected a number");
    }
    return cx_mk(0, 0);
}

double v_realnum(Interp *in, Value a)
{
    cx z = v_scalar(in, a);
    if (z.im != 0.0) cos_err(in, "expected a real number, got a complex value");
    return z.re;
}

int v_truth(Interp *in, Value a)
{
    if (a.k == K_MAT) {
        int i, n = a.rows * a.cols;
        for (i = 0; i < n; i++)
            if (a.m[i].re == 0.0 && a.m[i].im == 0.0) return 0;
        return n > 0;
    }
    { cx z = v_scalar(in, a); return !(z.re == 0.0 && z.im == 0.0); }
}

/* ================================================================== */
/* arithmetic                                                         */
/* ================================================================== */

static int is_matrix(Value a) { return a.k == K_MAT; }

static Value mat_from_scalar(Interp *in, Value s, int rows, int cols)
{
    Value r = v_mat(in, rows, cols);
    cx z = v_scalar(in, s);
    int i, n = rows * cols;
    for (i = 0; i < n; i++) r.m[i] = z;
    return r;
}

static Value mat_elementwise(Interp *in, int op, Value a, Value b)
{
    Value r;
    int i, n;
    if (a.rows != b.rows || a.cols != b.cols)
        cos_err(in, "dimension mismatch: %dx%d vs %dx%d",
                a.rows, a.cols, b.rows, b.cols);
    r = v_mat(in, a.rows, a.cols);
    n = a.rows * a.cols;
    for (i = 0; i < n; i++) {
        switch (op) {
        case OP_ADD:  r.m[i] = cx_add(a.m[i], b.m[i]); break;
        case OP_SUB:  r.m[i] = cx_sub(a.m[i], b.m[i]); break;
        case OP_EMUL: r.m[i] = cx_mul(a.m[i], b.m[i]); break;
        case OP_EDIV: r.m[i] = cx_div(a.m[i], b.m[i]); break;
        case OP_EPOW: r.m[i] = cx_pow(a.m[i], b.m[i]); break;
        case OP_MUL:  r.m[i] = cx_mul(a.m[i], b.m[i]); break;
        case OP_DIV:  r.m[i] = cx_div(a.m[i], b.m[i]); break;
        default: cos_err(in, "operator not defined for matrices");
        }
    }
    return r;
}

/* One row of a product. Rows are independent and nothing here allocates,
 * which is exactly what makes the work safe to split between the cores. */
typedef struct { const cx *a, *b; cx *r; int ak, bc, rc; } MulJob;

static void mul_row(int i, void *vj)
{
    MulJob *j = (MulJob *)vj;
    int c, k;
    for (c = 0; c < j->bc; c++) {
        /* accumulate the real and imaginary parts compensated */
        dd sr = dd_from(0.0), si = dd_from(0.0);
        for (k = 0; k < j->ak; k++) {
            cx p = cx_mul(j->a[i * j->ak + k], j->b[k * j->bc + c]);
            sr = dd_add(sr, dd_from(p.re));
            si = dd_add(si, dd_from(p.im));
        }
        j->r[i * j->rc + c] = cx_mk(dd_to(sr), dd_to(si));
    }
}

static Value mat_mul(Interp *in, Value a, Value b)
{
    Value r;
    MulJob job;
    if (a.cols != b.rows)
        cos_err(in, "cannot multiply %dx%d by %dx%d",
                a.rows, a.cols, b.rows, b.cols);
    r = v_mat(in, a.rows, b.cols);
    job.a = a.m; job.b = b.m; job.r = r.m;
    job.ak = a.cols; job.bc = b.cols; job.rc = r.cols;
    /* Small products are not worth waking the other core for: the handover
     * costs more than the arithmetic saves. */
    if ((long)a.rows * b.cols * a.cols >= 4096)
        plat_par_for(a.rows, mul_row, &job);
    else {
        int i;
        for (i = 0; i < a.rows; i++) mul_row(i, &job);
    }
    return r;
}

static Value mat_pow(Interp *in, Value a, int p)
{
    Value r;
    int i;
    if (a.rows != a.cols) cos_err(in, "matrix power needs a square matrix");
    if (p < 0) cos_err(in, "negative matrix power: use inv()");
    r = v_mat(in, a.rows, a.cols);
    for (i = 0; i < a.rows; i++) r.m[i * r.cols + i] = cx_mk(1.0, 0.0);
    for (i = 0; i < p; i++) r = mat_mul(in, r, a);
    return r;
}

static Value cmp_op(Interp *in, int op, Value a, Value b)
{
    double x, y;
    int t = 0;
    x = v_scalar(in, a).re;
    y = v_scalar(in, b).re;
    switch (op) {
    case OP_EQ: t = (x == y); break;
    case OP_NE: t = (x != y); break;
    case OP_LT: t = (x <  y); break;
    case OP_GT: t = (x >  y); break;
    case OP_LE: t = (x <= y); break;
    case OP_GE: t = (x >= y); break;
    }
    return v_real(t ? 1.0 : 0.0);
}

static int dd_compatible(Value a) { return a.k == K_DD || (a.k == K_NUM && a.n.im == 0.0); }
static dd  to_dd(Value a) { return a.k == K_DD ? a.d : dd_from(a.n.re); }
static iv  to_iv(Interp *in, Value a)
{
    if (a.k == K_IV) return a.v;
    if (a.k == K_DD) return iv_mk(nextafter(dd_to(a.d), -INFINITY),
                                  nextafter(dd_to(a.d),  INFINITY));
    return iv_point(v_realnum(in, a));
}

/* Work out what unit the result carries, and complain about the one case
 * that is always a mistake: adding quantities of different kinds. The
 * numbers themselves are untouched, because everything is already in base
 * SI units. */
static Unit binop_unit(Interp *in, int op, Value a, Value b)
{
    int au = !unit_is_none(a.u), bu = !unit_is_none(b.u);
    if (!au && !bu) return unit_none();

    switch (op) {
    case OP_ADD: case OP_SUB:
        if (!unit_same(a.u, b.u)) {
            char ua[32], ub[32];
            unit_str(a.u, ua, sizeof ua);
            unit_str(b.u, ub, sizeof ub);
            cos_err(in, "cannot %s %s and %s",
                    op == OP_ADD ? "add" : "subtract",
                    au ? ua : "a plain number", bu ? ub : "a plain number");
        }
        return a.u;
    case OP_MUL: case OP_EMUL:
        return unit_mul(a.u, b.u);
    case OP_DIV: case OP_EDIV:
        return unit_div(a.u, b.u);
    case OP_MOD:
        return a.u;
    case OP_POW: case OP_EPOW: {
        double p;
        if (bu) cos_err(in, "an exponent cannot have a unit");
        if (!au) return unit_none();
        p = b.n.re;
        if (p != floor(p))
            cos_err(in, "a quantity with a unit needs a whole-number power");
        return unit_pow(a.u, (int)p);
    }
    default:
        return a.u;
    }
}

/* Is this value a whole number a big integer can hold exactly? */
static int is_whole(Value v)
{
    return v.k == K_NUM && v.n.im == 0.0 && v.n.re == floor(v.n.re)
        && v.n.re > -9.007199254740992e15 && v.n.re < 9.007199254740992e15;
}

static struct Big *as_big(Interp *in, Value v)
{
    struct Big *b;
    if (v.k == K_BIG) return v.big;
    if (!is_whole(v))
        cos_err(in, "a big integer can only be combined with a whole number");
    b = big_from_double(v.n.re);
    if (!b) cos_err(in, "that number cannot be made exact");
    v_big(in, b);                    /* so the temporary is released later */
    return b;
}

/* Exact integer arithmetic. Anything that cannot stay exact — division
 * that does not divide, a fractional power — is answered as a double
 * instead, which is what the person asking almost always wants. */
static Value big_binop(Interp *in, int op, Value av, Value bv)
{
    struct Big *a = as_big(in, av), *b = as_big(in, bv), *r = NULL;

    switch (op) {
    case OP_ADD: r = big_add(a, b); break;
    case OP_SUB: r = big_sub(a, b); break;
    case OP_MUL: case OP_EMUL: r = big_mul(a, b); break;
    case OP_DIV: case OP_EDIV: {
        struct Big *q = NULL, *rem = NULL;
        if (big_is_zero(b)) cos_err(in, "division by zero");
        if (!big_divmod(a, b, &q, &rem)) cos_err(in, "the division failed");
        if (big_is_zero(rem)) { big_free(rem); r = q; }
        else {                      /* not exact: fall back to a double */
            double x = big_to_double(a) / big_to_double(b);
            big_free(q);
            big_free(rem);
            return v_real(x);
        }
        break;
    }
    case OP_MOD: r = big_is_zero(b) ? NULL : big_mod(a, b);
                 if (!r) cos_err(in, "division by zero");
                 break;
    case OP_POW: case OP_EPOW: {
        double e = big_to_double(b);
        if (e < 0 || e != floor(e) || e > 1e6)
            return v_real(pow(big_to_double(a), e));
        r = big_pow(a, (long long)e);
        break;
    }
    default: {
        int c = big_cmp(a, b);
        switch (op) {
        case OP_EQ: return v_real(c == 0);
        case OP_NE: return v_real(c != 0);
        case OP_LT: return v_real(c < 0);
        case OP_LE: return v_real(c <= 0);
        case OP_GT: return v_real(c > 0);
        case OP_GE: return v_real(c >= 0);
        default: cos_err(in, "that operator does not apply to big integers");
        }
    }
    }
    if (!r) cos_err(in, "the result is too large");
    return v_big(in, r);
}

Value v_binop(Interp *in, int op, Value a, Value b)
{
    Unit ru;

    if (a.k == K_BIG || b.k == K_BIG) return big_binop(in, op, a, b);

    if (op >= OP_EQ && op <= OP_GE) {
        if (!unit_same(a.u, b.u) && !unit_is_none(a.u) && !unit_is_none(b.u))
            cos_err(in, "cannot compare quantities of different kinds");
        return cmp_op(in, op, a, b);
    }
    ru = binop_unit(in, op, a, b);
    if (!unit_is_none(ru)) {
        Value r;
        a.u = b.u = unit_none();
        r = v_binop(in, op, a, b);
        r.u = ru;
        return r;
    }
    a.u = b.u = unit_none();
    if (op == OP_AND) return v_real(v_truth(in, a) && v_truth(in, b) ? 1 : 0);
    if (op == OP_OR)  return v_real(v_truth(in, a) || v_truth(in, b) ? 1 : 0);

    /* --- matrices --- */
    if (is_matrix(a) || is_matrix(b)) {
        if (op == OP_POW) {
            double p = v_realnum(in, b);
            if (p != floor(p)) cos_err(in, "matrix power must be an integer");
            return mat_pow(in, a, (int)p);
        }
        if (!is_matrix(a)) {
            if (op == OP_DIV || op == OP_EDIV)
                a = mat_from_scalar(in, a, b.rows, b.cols);
            else if (op == OP_MUL || op == OP_EMUL)
                return mat_elementwise(in, OP_EMUL,
                                       mat_from_scalar(in, a, b.rows, b.cols), b);
            else
                a = mat_from_scalar(in, a, b.rows, b.cols);
        } else if (!is_matrix(b)) {
            if (op == OP_MUL || op == OP_DIV || op == OP_EMUL || op == OP_EDIV
                || op == OP_EPOW)
                b = mat_from_scalar(in, b, a.rows, a.cols);
            else
                b = mat_from_scalar(in, b, a.rows, a.cols);
        } else if (op == OP_MUL) {
            return mat_mul(in, a, b);
        } else if (op == OP_DIV) {
            cos_err(in, "matrix / matrix is ambiguous: use solve() or inv()");
        }
        return mat_elementwise(in, op == OP_MUL ? OP_EMUL : op, a, b);
    }

    /* --- intervals dominate --- */
    if (a.k == K_IV || b.k == K_IV) {
        iv x = to_iv(in, a), y = to_iv(in, b);
        switch (op) {
        case OP_ADD: return v_iv(iv_add(x, y));
        case OP_SUB: return v_iv(iv_sub(x, y));
        case OP_MUL: case OP_EMUL: return v_iv(iv_mul(x, y));
        case OP_DIV: case OP_EDIV: return v_iv(iv_div(x, y));
        case OP_POW: case OP_EPOW: {
            double p = v_realnum(in, b);
            if (p != floor(p)) cos_err(in, "interval power must be an integer");
            return v_iv(iv_powi(x, (int)p));
        }
        default: cos_err(in, "operator not defined for intervals");
        }
    }

    /* --- double-double --- */
    if ((a.k == K_DD || b.k == K_DD) && dd_compatible(a) && dd_compatible(b)) {
        dd x = to_dd(a), y = to_dd(b);
        switch (op) {
        case OP_ADD: return v_dd(dd_add(x, y));
        case OP_SUB: return v_dd(dd_sub(x, y));
        case OP_MUL: case OP_EMUL: return v_dd(dd_mul(x, y));
        case OP_DIV: case OP_EDIV: return v_dd(dd_div(x, y));
        case OP_POW: case OP_EPOW: {
            double p = dd_to(y);
            if (p == floor(p) && fabs(p) < 4096)
                return v_dd(dd_powi(x, (int)p));
            return v_dd(dd_exp(dd_mul(y, dd_log(x))));
        }
        case OP_MOD: return v_dd(dd_from(fmod(dd_to(x), dd_to(y))));
        default: cos_err(in, "operator not defined for extended precision");
        }
    }

    /* --- complex double --- */
    {
        cx x = v_scalar(in, a), y = v_scalar(in, b);
        switch (op) {
        case OP_ADD: return v_num(cx_add(x, y));
        case OP_SUB: return v_num(cx_sub(x, y));
        case OP_MUL: case OP_EMUL: return v_num(cx_mul(x, y));
        case OP_DIV: case OP_EDIV: return v_num(cx_div(x, y));
        case OP_POW: case OP_EPOW: return v_num(cx_pow(x, y));
        case OP_MOD:
            if (x.im || y.im) cos_err(in, "mod needs real arguments");
            return v_real(fmod(x.re, y.re));
        default: cos_err(in, "unknown operator");
        }
    }
    return v_nil();
}

Value v_unop(Interp *in, int op, Value a)
{
    /* Negation keeps the unit; the others either reduce to a plain number
     * or are meaningless on a quantity. */
    if (!unit_is_none(a.u)) {
        Unit u = a.u;
        a.u = unit_none();
        if (op == OP_NEG) {
            Value r = v_unop(in, op, a);
            r.u = u;
            return r;
        }
        if (op == OP_FACT) {
            char us[32];
            unit_str(u, us, sizeof us);
            cos_err(in, "factorial of a quantity in %s has no meaning", us);
        }
    }
    switch (op) {
    case OP_NEG:
        if (a.k == K_MAT) {
            Value r = v_mat(in, a.rows, a.cols);
            int i, n = a.rows * a.cols;
            for (i = 0; i < n; i++) r.m[i] = cx_neg(a.m[i]);
            return r;
        }
        if (a.k == K_DD) return v_dd(dd_neg(a.d));
        if (a.k == K_IV) return v_iv(iv_neg(a.v));
        return v_num(cx_neg(v_scalar(in, a)));
    case OP_NOT:
        return v_real(v_truth(in, a) ? 0.0 : 1.0);
    case OP_FACT: {
        double x = v_realnum(in, a);
        if (x < 0 && x == floor(x)) cos_err(in, "factorial of a negative integer");
        return v_real(cos_gamma(x + 1.0));
    }
    case OP_TRANS: {
        Value r;
        int i, j;
        if (a.k != K_MAT) {
            cx z = v_scalar(in, a);
            return v_num(cx_conj(z));
        }
        r = v_mat(in, a.cols, a.rows);
        for (i = 0; i < a.rows; i++)
            for (j = 0; j < a.cols; j++)
                r.m[j * r.cols + i] = cx_conj(a.m[i * a.cols + j]);
        return r;
    }
    default:
        cos_err(in, "unknown unary operator");
    }
    return v_nil();
}

/* ================================================================== */
/* persistent copies (variables survive the per-line arena reset)      */
/* ================================================================== */

Value v_perm_dup(Value a)
{
    Value r = a;
    if (a.k == K_MAT && a.m) {
        size_t n = (size_t)a.rows * (size_t)a.cols * sizeof(cx);
        r.m = (cx *)malloc(n ? n : 1);
        if (r.m) memcpy(r.m, a.m, n);
    } else if (a.k == K_STR && a.s) {
        size_t n = strlen(a.s) + 1;
        r.s = (char *)malloc(n);
        if (r.s) memcpy(r.s, a.s, n);
    } else if (a.k == K_FUN && a.f) {
        r.f = cos_fn_clone(a.f);
    } else if (a.k == K_BIG && a.big) {
        /* a stored big integer owns its own copy, so the temporary it came
         * from can be released at the end of the statement */
        r.big = big_copy(a.big);
    }
    return r;
}

void v_perm_free(Value *a)
{
    if (!a) return;
    if (a->k == K_MAT && a->m) free(a->m);
    else if (a->k == K_STR && a->s) free(a->s);
    else if (a->k == K_FUN && a->f && a->f->perm) cos_fn_free(a->f);
    else if (a->k == K_BIG && a->big) big_free(a->big);
    memset(a, 0, sizeof(*a));
}

/* ================================================================== */
/* formatting                                                         */
/* ================================================================== */

static void fmt_real(Interp *in, double x, char *out, size_t n)
{
    int d = in->digits;
    if (isnan(x)) { snprintf(out, n, "nan"); return; }
    if (isinf(x)) { snprintf(out, n, x > 0 ? "inf" : "-inf"); return; }
    if (x == 0.0) { snprintf(out, n, "0"); return; }
    if (d < 1) d = 1;
    if (d > 17) d = 17;
    snprintf(out, n, "%.*g", d, x);
}

static void fmt_cx(Interp *in, cx z, char *out, size_t n)
{
    char a[48], b[48];
    if (z.im == 0.0) { fmt_real(in, z.re, out, n); return; }
    fmt_real(in, z.re, a, sizeof a);
    fmt_real(in, fabs(z.im), b, sizeof b);
    if (z.re == 0.0)
        snprintf(out, n, "%s%si", z.im < 0 ? "-" : "", b);
    else
        snprintf(out, n, "%s %c %si", a, z.im < 0 ? '-' : '+', b);
}

void v_format(Interp *in, Value a, char *out, size_t n)
{
    if (a.k == K_BIG) {
        /* a value with hundreds of digits is printed whole: it was asked
         * for exactly, and rounding it would defeat the point */
        if (!big_str(a.big, out, (int)n))
            snprintf(out, n, "<%d digits>", big_digits(a.big));
        return;
    }
    if (!unit_is_none(a.u)) {
        char num[200], us[40];
        Value plain = a;
        plain.u = unit_none();
        v_format(in, plain, num, sizeof num);
        unit_str(a.u, us, sizeof us);
        snprintf(out, n, "%s %s", num, us);
        return;
    }
    switch (a.k) {
    case K_NIL: if (n) out[0] = 0; break;
    case K_NUM: fmt_cx(in, a.n, out, n); break;
    case K_DD:  dd_str(a.d, in->ddigits, out, n); break;
    case K_IV: {
        char lo[48], hi[48];
        fmt_real(in, a.v.lo, lo, sizeof lo);
        fmt_real(in, a.v.hi, hi, sizeof hi);
        snprintf(out, n, "[%s, %s]", lo, hi);
        break;
    }
    case K_STR: snprintf(out, n, "%s", a.s ? a.s : ""); break;
    case K_FUN:
        snprintf(out, n, "<function %s/%d>",
                 a.f && a.f->name[0] ? a.f->name : "anonymous",
                 a.f ? a.f->nparams : 0);
        break;
    case K_MAT: snprintf(out, n, "<%dx%d matrix>", a.rows, a.cols); break;
    }
}

void v_print(Interp *in, Value a)
{
    char buf[256];
    if (a.k == K_NIL) return;
    if (a.k == K_BIG) {
        /* However many digits it has, they were asked for: the buffer is
         * taken from the line's arena and sized to the number, rather than
         * the fixed one below. The console wraps it across as many rows as
         * it needs. */
        int need = big_digits(a.big) + 3;
        char *big = (char *)cos_alloc(in, (size_t)need);
        if (big_str(a.big, big, need)) {
            plat_puts(big);
            plat_puts("\n");
        } else {
            cos_printf("<%d digits>\n", big_digits(a.big));
        }
        return;
    }
    if (a.k == K_MAT) {
        int i, j, w = 0, cols = plat_cols();
        char cell[64];
        /* column width pass */
        for (i = 0; i < a.rows * a.cols; i++) {
            fmt_cx(in, a.m[i], cell, sizeof cell);
            if ((int)strlen(cell) > w) w = (int)strlen(cell);
        }
        if (w > 20) w = 20;
        for (i = 0; i < a.rows; i++) {
            int used = 0;
            plat_puts(" ");
            for (j = 0; j < a.cols; j++) {
                fmt_cx(in, a.m[i * a.cols + j], cell, sizeof cell);
                snprintf(buf, sizeof buf, "%*s ", w, cell);
                used += (int)strlen(buf);
                if (used > cols - 2) { plat_puts("..."); break; }
                plat_puts(buf);
            }
            plat_puts("\n");
        }
        return;
    }
    v_format(in, a, buf, sizeof buf);
    plat_puts(buf);
    plat_puts("\n");
    if (in->verbose_fp && a.k == K_NUM && a.n.im == 0.0) {
        char bits[96];
        cos_bits(a.n.re, bits, sizeof bits);
        cos_wrap(2, bits);
    }
}

void cos_printf(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    plat_puts(buf);
}
