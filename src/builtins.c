/* CalcOS builtin function library. */
#include "calc.h"
#include "bignum.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define RA(i) v_realnum(in, a[i])
#define CA(i) v_scalar(in, a[i])

/* ================================================================== */
/* helpers                                                            */
/* ================================================================== */

typedef struct {
    cx     (*c)(cx);
    dd     (*d)(dd);
    iv     (*v)(iv);
    double (*r)(double);
} UnFns;

static Value apply_un(Interp *in, Value x, const UnFns *f, const char *name)
{
    if (x.k == K_MAT) {
        Value r = v_mat(in, x.rows, x.cols);
        int i, n = x.rows * x.cols;
        for (i = 0; i < n; i++) {
            if (f->c) r.m[i] = f->c(x.m[i]);
            else if (f->r) {
                if (x.m[i].im != 0.0)
                    cos_err(in, "%s is real-only", name);
                r.m[i] = cx_mk(f->r(x.m[i].re), 0.0);
            } else cos_err(in, "%s does not apply to matrices", name);
        }
        return r;
    }
    if (x.k == K_DD) {
        if (f->d) return v_dd(f->d(x.d));
        if (f->r) return v_real(f->r(dd_to(x.d)));
        if (f->c) return v_num(f->c(cx_mk(dd_to(x.d), 0.0)));
    }
    if (x.k == K_IV) {
        if (f->v) return v_iv(f->v(x.v));
        cos_err(in, "%s has no verified interval form", name);
    }
    if (f->c) return v_num(f->c(v_scalar(in, x)));
    if (f->r) return v_real(f->r(v_realnum(in, x)));
    cos_err(in, "%s: unsupported argument", name);
    return v_nil();
}

#define UN_C(fname, cfn, dfn, vfn)                                        \
    static Value bi_##fname(Interp *in, Value *a, int n) {                \
        UnFns f; (void)n; f.c = cfn; f.d = dfn; f.v = vfn; f.r = NULL;    \
        return apply_un(in, a[0], &f, #fname); }

#define UN_R(fname, rfn)                                                  \
    static Value bi_##fname(Interp *in, Value *a, int n) {                \
        UnFns f; (void)n; f.c = NULL; f.d = NULL; f.v = NULL; f.r = rfn;  \
        return apply_un(in, a[0], &f, #fname); }

static Value need_mat(Interp *in, Value x, const char *who)
{
    if (x.k != K_MAT) cos_err(in, "%s expects a matrix or vector", who);
    return x;
}

static double *real_vec(Interp *in, Value x, int *n)
{
    double *p;
    int i;
    if (x.k != K_MAT) {
        p = (double *)cos_alloc(in, sizeof(double));
        p[0] = v_realnum(in, x);
        *n = 1;
        return p;
    }
    *n = x.rows * x.cols;
    p = (double *)cos_alloc(in, (size_t)(*n ? *n : 1) * sizeof(double));
    for (i = 0; i < *n; i++) {
        if (x.m[i].im != 0.0) cos_err(in, "expected real data");
        p[i] = x.m[i].re;
    }
    return p;
}

static double ang_in(Interp *in, double x)  { return in->deg ? x * M_PI / 180.0 : x; }
static double ang_out(Interp *in, double x) { return in->deg ? x * 180.0 / M_PI : x; }

/* ================================================================== */
/* elementary                                                         */
/* ================================================================== */

UN_C(sqrt, cx_sqrt, dd_sqrt, iv_sqrt)
UN_C(exp,  cx_exp,  dd_exp,  iv_exp)
UN_R(cbrt,  cbrt)
UN_R(floor, floor)
UN_R(ceil,  ceil)
UN_R(trunc, trunc)
UN_R(round, round)
UN_R(log2,  log2)
UN_R(log10, log10)
UN_R(log1p, log1p)
UN_R(expm1, expm1)
UN_R(asinh, asinh)
UN_R(acosh, acosh)
UN_R(atanh, atanh)

static Value bi_log(Interp *in, Value *a, int n)
{
    if (n == 2) {
        cx x = CA(0), b = CA(1);
        return v_num(cx_div(cx_log(x), cx_log(b)));
    }
    { UnFns f; f.c = cx_log; f.d = dd_log; f.v = iv_log; f.r = NULL;
      return apply_un(in, a[0], &f, "log"); }
}

static Value bi_abs(Interp *in, Value *a, int n)
{
    (void)n;
    if (a[0].k == K_MAT) {
        Value r = v_mat(in, a[0].rows, a[0].cols);
        int i, m = a[0].rows * a[0].cols;
        for (i = 0; i < m; i++) r.m[i] = cx_mk(cx_abs(a[0].m[i]), 0.0);
        return r;
    }
    if (a[0].k == K_DD) {
        dd d = a[0].d;
        return v_dd(d.hi < 0 ? dd_neg(d) : d);
    }
    if (a[0].k == K_IV) {
        iv v = a[0].v;
        if (v.lo >= 0) return v_iv(v);
        if (v.hi <= 0) return v_iv(iv_neg(v));
        return v_iv(iv_mk(0.0, fabs(v.lo) > v.hi ? fabs(v.lo) : v.hi));
    }
    return v_real(cx_abs(CA(0)));
}

#define TRIG(name, cfn, dfn, vfn)                                          \
    static Value bi_##name(Interp *in, Value *a, int n) {                  \
        (void)n;                                                           \
        if (in->deg && a[0].k != K_MAT) {                                  \
            cx z = CA(0);                                                  \
            if (z.im == 0.0) return v_num(cfn(cx_mk(ang_in(in, z.re), 0)));\
        }                                                                  \
        { UnFns f; f.c = cfn; f.d = dfn; f.v = vfn; f.r = NULL;            \
          return apply_un(in, a[0], &f, #name); } }

TRIG(sin, cx_sin, dd_sin, iv_sin)
TRIG(cos, cx_cos, dd_cos, iv_cos)
TRIG(tan, cx_tan, NULL, NULL)

#define ATRIG(name, cfn)                                                   \
    static Value bi_##name(Interp *in, Value *a, int n) {                  \
        cx z; (void)n; z = CA(0);                                          \
        { cx r = cfn(z);                                                   \
          if (r.im == 0.0) return v_real(ang_out(in, r.re));               \
          return v_num(r); } }

ATRIG(asin, cx_asin)
ATRIG(acos, cx_acos)
ATRIG(atan, cx_atan)

UN_C(sinh, cx_sinh, NULL, NULL)
UN_C(cosh, cx_cosh, NULL, NULL)
UN_C(tanh, cx_tanh, NULL, NULL)

static Value bi_atan2(Interp *in, Value *a, int n)
{ (void)n; return v_real(ang_out(in, atan2(RA(0), RA(1)))); }

static Value bi_hypot(Interp *in, Value *a, int n)
{ (void)n; return v_real(hypot(RA(0), RA(1))); }

static Value bi_mod(Interp *in, Value *a, int n)
{
    double x = RA(0), y = RA(1), r;
    (void)n;
    r = fmod(x, y);
    if (r != 0.0 && ((r < 0) != (y < 0))) r += y;   /* floored, like Python */
    return v_real(r);
}

static Value bi_rem(Interp *in, Value *a, int n)
{ (void)n; return v_real(fmod(RA(0), RA(1))); }

static Value bi_pow(Interp *in, Value *a, int n)
{ (void)n; return v_binop(in, OP_POW, a[0], a[1]); }

static Value bi_fma(Interp *in, Value *a, int n)
{ (void)n; return v_real(fma(RA(0), RA(1), RA(2))); }

static Value bi_sign(Interp *in, Value *a, int n)
{ double x = RA(0); (void)n; return v_real(x > 0 ? 1.0 : (x < 0 ? -1.0 : 0.0)); }

static Value bi_min(Interp *in, Value *a, int n)
{
    double best = INFINITY;
    int i, k, m;
    for (i = 0; i < n; i++) {
        if (a[i].k == K_MAT) {
            m = a[i].rows * a[i].cols;
            for (k = 0; k < m; k++) if (a[i].m[k].re < best) best = a[i].m[k].re;
        } else { double x = RA(i); if (x < best) best = x; }
    }
    return v_real(best);
}

static Value bi_max(Interp *in, Value *a, int n)
{
    double best = -INFINITY;
    int i, k, m;
    for (i = 0; i < n; i++) {
        if (a[i].k == K_MAT) {
            m = a[i].rows * a[i].cols;
            for (k = 0; k < m; k++) if (a[i].m[k].re > best) best = a[i].m[k].re;
        } else { double x = RA(i); if (x > best) best = x; }
    }
    return v_real(best);
}

/* ================================================================== */
/* complex parts                                                      */
/* ================================================================== */

static Value bi_re(Interp *in, Value *a, int n)   { (void)n; return v_real(CA(0).re); }
static Value bi_im(Interp *in, Value *a, int n)   { (void)n; return v_real(CA(0).im); }
static Value bi_conj(Interp *in, Value *a, int n) { (void)n; return v_num(cx_conj(CA(0))); }
static Value bi_arg(Interp *in, Value *a, int n)  { (void)n; return v_real(ang_out(in, cx_arg(CA(0)))); }
static Value bi_polar(Interp *in, Value *a, int n)
{
    double r = RA(0), t = ang_in(in, RA(1));
    (void)n;
    return v_num(cx_mk(r * cos(t), r * sin(t)));
}

/* ================================================================== */
/* special functions                                                  */
/* ================================================================== */

UN_R(gamma,    cos_gamma)
UN_R(lgamma,   cos_lgamma)
UN_R(digamma,  cos_digamma)
UN_R(erf,      cos_erf)
UN_R(erfc,     cos_erfc)
UN_R(zeta,     cos_zeta)
UN_R(besselj0, cos_j0)
UN_R(besselj1, cos_j1)
UN_R(bessely0, cos_y0)
UN_R(bessely1, cos_y1)
UN_R(ei,       cos_ei)
UN_R(li,       cos_li)

static Value bi_beta(Interp *in, Value *a, int n)
{ (void)n; return v_real(cos_beta(RA(0), RA(1))); }

static Value bi_factorial(Interp *in, Value *a, int n)
{ (void)n; return v_real(cos_gamma(RA(0) + 1.0)); }

static Value bi_binomial(Interp *in, Value *a, int n)
{
    double nn = RA(0), k = RA(1);
    (void)n;
    return v_real(floor(0.5 + exp(cos_lgamma(nn + 1.0) - cos_lgamma(k + 1.0)
                                  - cos_lgamma(nn - k + 1.0))));
}

/* ================================================================== */
/* floating-point laboratory                                          */
/* ================================================================== */

static Value bi_ulp(Interp *in, Value *a, int n)      { (void)n; return v_real(cos_ulp(RA(0))); }
static Value bi_nextup(Interp *in, Value *a, int n)   { (void)n; return v_real(nextafter(RA(0), INFINITY)); }
static Value bi_nextdown(Interp *in, Value *a, int n) { (void)n; return v_real(nextafter(RA(0), -INFINITY)); }
static Value bi_relerr(Interp *in, Value *a, int n)   { (void)n; return v_real(cos_relerr(RA(0), RA(1))); }
static Value bi_ulpdiff(Interp *in, Value *a, int n)  { (void)n; return v_real(cos_ulpdiff(RA(0), RA(1))); }
static Value bi_isnan(Interp *in, Value *a, int n)    { (void)n; return v_real(isnan(RA(0)) ? 1 : 0); }
static Value bi_isinf(Interp *in, Value *a, int n)    { (void)n; return v_real(isinf(RA(0)) ? 1 : 0); }

static Value bi_bits(Interp *in, Value *a, int n)
{
    char buf[96];
    (void)n;
    cos_bits(RA(0), buf, sizeof buf);
    { char *p; for (p = buf; *p; p++) if (*p == '\n') *p = ' '; }
    return v_str(in, buf);
}

static Value bi_hex(Interp *in, Value *a, int n)
{
    char buf[48];
    (void)n;
    snprintf(buf, sizeof buf, "%a", RA(0));
    return v_str(in, buf);
}

static Value bi_exponent(Interp *in, Value *a, int n)
{
    int e;
    (void)n;
    frexp(RA(0), &e);
    return v_real((double)(e - 1));
}

static Value bi_significand(Interp *in, Value *a, int n)
{
    int e;
    (void)n;
    return v_real(frexp(RA(0), &e) * 2.0);
}

/* ================================================================== */
/* extended precision and intervals                                   */
/* ================================================================== */

static dd dd_parse(const char *s)
{
    dd r = dd_from(0.0);
    int neg = 0, seen_dot = 0, exp10 = 0, i = 0, digits = 0;
    if (s[i] == '+' || s[i] == '-') { neg = (s[i] == '-'); i++; }
    for (; s[i]; i++) {
        if (s[i] >= '0' && s[i] <= '9') {
            r = dd_add(dd_mul(r, dd_from(10.0)), dd_from((double)(s[i] - '0')));
            if (seen_dot) exp10--;
            digits++;
        } else if (s[i] == '.' && !seen_dot) {
            seen_dot = 1;
        } else if (s[i] == 'e' || s[i] == 'E') {
            exp10 += atoi(s + i + 1);
            break;
        } else break;
    }
    (void)digits;
    if (exp10 > 0) r = dd_mul(r, dd_powi(dd_from(10.0), exp10));
    else if (exp10 < 0) r = dd_div(r, dd_powi(dd_from(10.0), -exp10));
    return neg ? dd_neg(r) : r;
}

static Value bi_dd(Interp *in, Value *a, int n)
{
    (void)n;
    if (a[0].k == K_STR) return v_dd(dd_parse(a[0].s));
    if (a[0].k == K_DD)  return a[0];
    return v_dd(dd_from(RA(0)));
}

static Value bi_hi(Interp *in, Value *a, int n)
{ (void)n; if (a[0].k != K_DD) return v_real(RA(0)); return v_real(a[0].d.hi); }

static Value bi_lo(Interp *in, Value *a, int n)
{ (void)n; (void)in; if (a[0].k != K_DD) return v_real(0.0); return v_real(a[0].d.lo); }

static Value bi_iv(Interp *in, Value *a, int n)
{
    if (n == 2) return v_iv(iv_mk(RA(0), RA(1)));
    if (a[0].k == K_IV) return a[0];
    {
        double x = RA(0);
        return v_iv(iv_mk(nextafter(x, -INFINITY), nextafter(x, INFINITY)));
    }
}

static iv as_iv(Interp *in, Value x)
{
    if (x.k == K_IV) return x.v;
    return iv_point(v_realnum(in, x));
}
static Value bi_inf(Interp *in, Value *a, int n) { (void)n; return v_real(as_iv(in, a[0]).lo); }
static Value bi_sup(Interp *in, Value *a, int n) { (void)n; return v_real(as_iv(in, a[0]).hi); }
static Value bi_wid(Interp *in, Value *a, int n) { (void)n; return v_real(iv_wid(as_iv(in, a[0]))); }
static Value bi_mid(Interp *in, Value *a, int n) { (void)n; return v_real(iv_mid(as_iv(in, a[0]))); }

/* ================================================================== */
/* construction                                                       */
/* ================================================================== */

static void dims(Interp *in, Value *a, int n, int *r, int *c)
{
    if (n == 0) { *r = *c = 1; return; }
    *r = (int)RA(0);
    *c = (n >= 2) ? (int)v_realnum(in, a[1]) : *r;
    if (*r < 0 || *c < 0) cos_err(in, "negative dimension");
}

static Value bi_zeros(Interp *in, Value *a, int n)
{ int r, c; dims(in, a, n, &r, &c); return v_mat(in, r, c); }

static Value bi_ones(Interp *in, Value *a, int n)
{
    int r, c, i;
    Value m;
    dims(in, a, n, &r, &c);
    m = v_mat(in, r, c);
    for (i = 0; i < r * c; i++) m.m[i] = cx_mk(1.0, 0.0);
    return m;
}

static Value bi_eye(Interp *in, Value *a, int n)
{
    int r, c, i;
    Value m;
    dims(in, a, n, &r, &c);
    m = v_mat(in, r, c);
    for (i = 0; i < (r < c ? r : c); i++) m.m[i * c + i] = cx_mk(1.0, 0.0);
    return m;
}

static Value bi_rand(Interp *in, Value *a, int n)
{
    int r, c, i;
    Value m;
    if (n == 0) return v_real(cos_prng());
    dims(in, a, n, &r, &c);
    m = v_mat(in, r, c);
    for (i = 0; i < r * c; i++) m.m[i] = cx_mk(cos_prng(), 0.0);
    return m;
}

static Value bi_randn(Interp *in, Value *a, int n)
{
    /* Box-Muller */
    double u1 = cos_prng(), u2 = cos_prng();
    (void)a; (void)n; (void)in;
    if (u1 < 1e-300) u1 = 1e-300;
    return v_real(sqrt(-2.0 * log(u1)) * cos(2.0 * M_PI * u2));
}

static Value bi_seed(Interp *in, Value *a, int n)
{ (void)n; cos_srand((uint64_t)RA(0)); return v_real(RA(0)); }

static Value bi_linspace(Interp *in, Value *a, int n)
{
    double lo = RA(0), hi = RA(1);
    int k = (n >= 3) ? (int)v_realnum(in, a[2]) : 50, i;
    Value m;
    if (k < 1) cos_err(in, "linspace needs at least one point");
    m = v_mat(in, 1, k);
    for (i = 0; i < k; i++)
        m.m[i] = cx_mk(k == 1 ? lo : lo + (hi - lo) * (double)i / (double)(k - 1), 0.0);
    return m;
}

static Value bi_size(Interp *in, Value *a, int n)
{
    Value m = v_mat(in, 1, 2);
    (void)n;
    if (a[0].k == K_MAT) {
        m.m[0] = cx_mk(a[0].rows, 0.0);
        m.m[1] = cx_mk(a[0].cols, 0.0);
    } else { m.m[0] = cx_mk(1, 0); m.m[1] = cx_mk(1, 0); }
    return m;
}

static Value bi_numel(Interp *in, Value *a, int n)
{ (void)n; (void)in; return v_real(a[0].k == K_MAT ? a[0].rows * a[0].cols : 1); }

static Value bi_reshape(Interp *in, Value *a, int n)
{
    Value x = need_mat(in, a[0], "reshape"), r;
    int rr = (int)RA(1), cc = (int)v_realnum(in, a[2]), i;
    (void)n;
    if (rr * cc != x.rows * x.cols)
        cos_err(in, "cannot reshape %d elements into %dx%d",
                x.rows * x.cols, rr, cc);
    r = v_mat(in, rr, cc);
    for (i = 0; i < rr * cc; i++) r.m[i] = x.m[i];
    return r;
}

static Value bi_transpose(Interp *in, Value *a, int n)
{ (void)n; return v_unop(in, OP_TRANS, a[0]); }

static Value bi_diag(Interp *in, Value *a, int n)
{
    Value x = a[0], r;
    int i, k;
    (void)n;
    if (x.k != K_MAT) return x;
    if (x.rows == 1 || x.cols == 1) {
        k = x.rows * x.cols;
        r = v_mat(in, k, k);
        for (i = 0; i < k; i++) r.m[i * k + i] = x.m[i];
        return r;
    }
    k = x.rows < x.cols ? x.rows : x.cols;
    r = v_mat(in, 1, k);
    for (i = 0; i < k; i++) r.m[i] = x.m[i * x.cols + i];
    return r;
}

/* ================================================================== */
/* linear algebra                                                     */
/* ================================================================== */

/* LU with partial pivoting; returns permutation sign, 0 if singular */
static int lu_decomp(Interp *in, cx *m, int n, int *piv)
{
    int i, j, k, sign = 1;
    for (i = 0; i < n; i++) piv[i] = i;
    for (k = 0; k < n; k++) {
        double best = -1.0;
        int bi = k;
        for (i = k; i < n; i++) {
            double v = cx_abs(m[i * n + k]);
            if (v > best) { best = v; bi = i; }
        }
        if (best <= 0.0) return 0;
        if (bi != k) {
            for (j = 0; j < n; j++) {
                cx t = m[k * n + j]; m[k * n + j] = m[bi * n + j]; m[bi * n + j] = t;
            }
            { int t = piv[k]; piv[k] = piv[bi]; piv[bi] = t; }
            sign = -sign;
        }
        for (i = k + 1; i < n; i++) {
            cx f = cx_div(m[i * n + k], m[k * n + k]);
            m[i * n + k] = f;
            for (j = k + 1; j < n; j++)
                m[i * n + j] = cx_sub(m[i * n + j], cx_mul(f, m[k * n + j]));
        }
    }
    (void)in;
    return sign;
}

static void lu_solve(cx *lu, int n, int *piv, cx *b, cx *x)
{
    int i, j;
    for (i = 0; i < n; i++) {
        cx s = b[piv[i]];
        for (j = 0; j < i; j++) s = cx_sub(s, cx_mul(lu[i * n + j], x[j]));
        x[i] = s;
    }
    for (i = n - 1; i >= 0; i--) {
        cx s = x[i];
        for (j = i + 1; j < n; j++) s = cx_sub(s, cx_mul(lu[i * n + j], x[j]));
        x[i] = cx_div(s, lu[i * n + i]);
    }
}

static cx *copy_square(Interp *in, Value m, int *n, const char *who)
{
    cx *p;
    need_mat(in, m, who);
    if (m.rows != m.cols) cos_err(in, "%s needs a square matrix", who);
    *n = m.rows;
    p = (cx *)cos_alloc(in, (size_t)(*n) * (size_t)(*n) * sizeof(cx));
    memcpy(p, m.m, (size_t)(*n) * (size_t)(*n) * sizeof(cx));
    return p;
}

static Value bi_det(Interp *in, Value *a, int n)
{
    int sz, *piv, i, sign;
    cx *lu, d;
    (void)n;
    lu = copy_square(in, a[0], &sz, "det");
    piv = (int *)cos_alloc(in, (size_t)sz * sizeof(int));
    sign = lu_decomp(in, lu, sz, piv);
    if (!sign) return v_real(0.0);
    d = cx_mk((double)sign, 0.0);
    for (i = 0; i < sz; i++) d = cx_mul(d, lu[i * sz + i]);
    return v_num(d);
}

static Value bi_inv(Interp *in, Value *a, int n)
{
    int sz, *piv, i, j;
    cx *lu, *col, *x;
    Value r;
    (void)n;
    lu = copy_square(in, a[0], &sz, "inv");
    piv = (int *)cos_alloc(in, (size_t)sz * sizeof(int));
    if (!lu_decomp(in, lu, sz, piv)) cos_err(in, "matrix is singular");
    col = (cx *)cos_alloc(in, (size_t)sz * sizeof(cx));
    x   = (cx *)cos_alloc(in, (size_t)sz * sizeof(cx));
    r = v_mat(in, sz, sz);
    for (j = 0; j < sz; j++) {
        for (i = 0; i < sz; i++) col[i] = cx_mk(i == j ? 1.0 : 0.0, 0.0);
        lu_solve(lu, sz, piv, col, x);
        for (i = 0; i < sz; i++) r.m[i * sz + j] = x[i];
    }
    return r;
}

static Value bi_solve(Interp *in, Value *a, int n)
{
    int sz, *piv, i, k;
    cx *lu, *b, *x, *orig;
    Value r, bv;
    (void)n;
    lu = copy_square(in, a[0], &sz, "solve");
    orig = (cx *)cos_alloc(in, (size_t)sz * (size_t)sz * sizeof(cx));
    memcpy(orig, lu, (size_t)sz * (size_t)sz * sizeof(cx));
    bv = need_mat(in, a[1], "solve");
    if (bv.rows * bv.cols != sz)
        cos_err(in, "right-hand side has %d entries, expected %d",
                bv.rows * bv.cols, sz);
    piv = (int *)cos_alloc(in, (size_t)sz * sizeof(int));
    if (!lu_decomp(in, lu, sz, piv)) cos_err(in, "matrix is singular");
    b = (cx *)cos_alloc(in, (size_t)sz * sizeof(cx));
    x = (cx *)cos_alloc(in, (size_t)sz * sizeof(cx));
    for (i = 0; i < sz; i++) b[i] = bv.m[i];
    lu_solve(lu, sz, piv, b, x);

    /* one step of iterative refinement with a double-double residual */
    {
        cx *res = (cx *)cos_alloc(in, (size_t)sz * sizeof(cx));
        cx *dx  = (cx *)cos_alloc(in, (size_t)sz * sizeof(cx));
        for (i = 0; i < sz; i++) {
            dd sr = dd_from(b[i].re), si = dd_from(b[i].im);
            for (k = 0; k < sz; k++) {
                cx p = cx_mul(orig[i * sz + k], x[k]);
                sr = dd_sub(sr, dd_from(p.re));
                si = dd_sub(si, dd_from(p.im));
            }
            res[i] = cx_mk(dd_to(sr), dd_to(si));
        }
        lu_solve(lu, sz, piv, res, dx);
        for (i = 0; i < sz; i++) x[i] = cx_add(x[i], dx[i]);
    }
    r = v_mat(in, sz, 1);
    for (i = 0; i < sz; i++) r.m[i] = x[i];
    return r;
}

static Value bi_trace(Interp *in, Value *a, int n)
{
    int sz, i;
    cx s = cx_mk(0, 0);
    Value m = need_mat(in, a[0], "trace");
    (void)n;
    if (m.rows != m.cols) cos_err(in, "trace needs a square matrix");
    sz = m.rows;
    for (i = 0; i < sz; i++) s = cx_add(s, m.m[i * sz + i]);
    return v_num(s);
}

static double norm_of(Interp *in, Value m, double p)
{
    int i, j, nel;
    double s = 0.0;
    if (m.k != K_MAT) return cx_abs(v_scalar(in, m));
    nel = m.rows * m.cols;
    if (m.rows == 1 || m.cols == 1) {
        if (isinf(p)) {
            for (i = 0; i < nel; i++) { double v = cx_abs(m.m[i]); if (v > s) s = v; }
            return s;
        }
        if (p == 1.0) { for (i = 0; i < nel; i++) s += cx_abs(m.m[i]); return s; }
        for (i = 0; i < nel; i++) s += pow(cx_abs(m.m[i]), p);
        return pow(s, 1.0 / p);
    }
    if (p == 1.0) {                     /* max column sum */
        for (j = 0; j < m.cols; j++) {
            double c = 0;
            for (i = 0; i < m.rows; i++) c += cx_abs(m.m[i * m.cols + j]);
            if (c > s) s = c;
        }
        return s;
    }
    if (isinf(p)) {                     /* max row sum */
        for (i = 0; i < m.rows; i++) {
            double c = 0;
            for (j = 0; j < m.cols; j++) c += cx_abs(m.m[i * m.cols + j]);
            if (c > s) s = c;
        }
        return s;
    }
    for (i = 0; i < nel; i++) s += cx_abs(m.m[i]) * cx_abs(m.m[i]);
    return sqrt(s);                     /* Frobenius */
}

static Value bi_norm(Interp *in, Value *a, int n)
{
    double p = (n >= 2) ? v_realnum(in, a[1]) : 2.0;
    return v_real(norm_of(in, a[0], p));
}

static Value bi_cond(Interp *in, Value *a, int n)
{
    Value iv1;
    double n1, n2;
    (void)n;
    n1 = norm_of(in, a[0], 1.0);
    iv1 = bi_inv(in, a, 1);
    n2 = norm_of(in, iv1, 1.0);
    return v_real(n1 * n2);
}

static Value bi_dot(Interp *in, Value *a, int n)
{
    int na, nb;
    double *x = real_vec(in, a[0], &na), *y = real_vec(in, a[1], &nb);
    (void)n;
    if (na != nb) cos_err(in, "dot: lengths %d and %d differ", na, nb);
    return v_real(cos_ndot(x, y, na));   /* exact products, dd accumulation */
}

static Value bi_cross(Interp *in, Value *a, int n)
{
    int na, nb;
    double *x = real_vec(in, a[0], &na), *y = real_vec(in, a[1], &nb);
    Value r;
    (void)n;
    if (na != 3 || nb != 3) cos_err(in, "cross needs two 3-vectors");
    r = v_mat(in, 1, 3);
    r.m[0] = cx_mk(x[1] * y[2] - x[2] * y[1], 0);
    r.m[1] = cx_mk(x[2] * y[0] - x[0] * y[2], 0);
    r.m[2] = cx_mk(x[0] * y[1] - x[1] * y[0], 0);
    return r;
}

/* ================================================================== */
/* statistics                                                         */
/* ================================================================== */

static int dcmp(const void *p, const void *q)
{
    double a = *(const double *)p, b = *(const double *)q;
    return a < b ? -1 : (a > b ? 1 : 0);
}

/* ------------------------------------------------------------------ */
/* big integers                                                        */
/* ------------------------------------------------------------------ */

/* big(x): from a whole number, or from a string for anything a double
 * could not have held in the first place. */
static Value bi_big(Interp *in, Value *a, int nargs)
{
    (void)nargs;
    if (a[0].k == K_BIG) return a[0];
    if (a[0].k == K_STR) {
        Big *b = big_parse(a[0].s);
        if (!b) cos_err(in, "'%s' is not a whole number", a[0].s);
        return v_big(in, b);
    }
    {
        double x = v_realnum(in, a[0]);
        Big *b;
        if (x != floor(x)) cos_err(in, "big() needs a whole number");
        b = big_from_double(x);
        if (!b) cos_err(in, "that number cannot be made exact");
        return v_big(in, b);
    }
}

static Big *arg_big(Interp *in, Value v)
{
    Big *b;
    if (v.k == K_BIG) return v.big;
    if (v.k == K_STR) {
        b = big_parse(v.s);
        if (!b) cos_err(in, "'%s' is not a whole number", v.s);
    } else {
        double x = v_realnum(in, v);
        if (x != floor(x)) cos_err(in, "a whole number is needed here");
        b = big_from_double(x);
        if (!b) cos_err(in, "that number cannot be made exact");
    }
    v_big(in, b);                     /* owned by the line */
    return b;
}

static Value bi_bigfact(Interp *in, Value *a, int nargs)
{
    double n = v_realnum(in, a[0]);
    Big *r;
    (void)nargs;
    if (n < 0 || n != floor(n)) cos_err(in, "factorial needs a whole number");
    if (n > BIG_MAXFACT) cos_err(in, "that factorial is beyond the limit");
    r = big_fact((long long)n);
    if (!r) cos_err(in, "the result is too large");
    return v_big(in, r);
}

static Value bi_powmod(Interp *in, Value *a, int nargs)
{
    Big *r = big_powmod(arg_big(in, a[0]), arg_big(in, a[1]),
                        arg_big(in, a[2]));
    (void)nargs;
    if (!r) cos_err(in, "powmod needs whole numbers and a non-zero modulus");
    return v_big(in, r);
}

static Value bi_gcd(Interp *in, Value *a, int nargs)
{
    Big *r = big_gcd(arg_big(in, a[0]), arg_big(in, a[1]));
    (void)nargs;
    if (!r) cos_err(in, "gcd failed");
    return v_big(in, r);
}

static Value bi_lcm(Interp *in, Value *a, int nargs)
{
    Big *x = arg_big(in, a[0]), *y = arg_big(in, a[1]);
    Big *g = big_gcd(x, y), *p, *r;
    (void)nargs;
    if (!g) cos_err(in, "lcm failed");
    if (big_is_zero(g)) { big_free(g); return v_real(0); }
    p = big_mul(x, y);
    r = p ? big_div(p, g) : NULL;
    big_free(p);
    big_free(g);
    if (!r) cos_err(in, "lcm failed");
    if (big_sign(r) < 0) { Big *t = big_neg(r); big_free(r); r = t; }
    return v_big(in, r);
}

static Value bi_digits_of(Interp *in, Value *a, int nargs)
{
    (void)nargs;
    return v_real(big_digits(arg_big(in, a[0])));
}

static Value bi_tostr(Interp *in, Value *a, int nargs)
{
    (void)nargs;
    if (a[0].k == K_BIG) {
        /* the buffer comes from the line's arena, sized to the number, so
         * a thousand-digit value costs nothing when nobody asks for one */
        int need = big_digits(a[0].big) + 3;
        char *buf = (char *)cos_alloc(in, (size_t)need);
        if (!big_str(a[0].big, buf, need))
            cos_err(in, "that number has too many digits to show");
        return v_str(in, buf);
    }
    {
        char buf[256];
        v_format(in, a[0], buf, sizeof buf);
        return v_str(in, buf);
    }
}

/* ------------------------------------------------------------------ */
/* units and physical constants                                        */
/* ------------------------------------------------------------------ */

/* conv(x, "from", "to"): a plain number converted between two units.
 * This is the form that works without ever writing a quantity, which is
 * what most conversions actually are: "how many km is 5 miles". */
static Value bi_conv(Interp *in, Value *a, int nargs)
{
    double x = v_realnum(in, a[0]), ff, tf;
    Unit fu, tu;
    (void)nargs;
    if (a[1].k != K_STR || a[2].k != K_STR)
        cos_err(in, "conv takes the unit names as strings");
    if (!unit_parse(a[1].s, &ff, &fu)) cos_err(in, "unknown unit '%s'", a[1].s);
    if (!unit_parse(a[2].s, &tf, &tu)) cos_err(in, "unknown unit '%s'", a[2].s);
    if (!unit_same(fu, tu)) {
        char f1[32], f2[32];
        unit_str(fu, f1, sizeof f1);
        unit_str(tu, f2, sizeof f2);
        cos_err(in, "cannot convert %s to %s", f1, f2);
    }
    return v_real(x * ff / tf);
}

/* to(q, "unit"): a quantity expressed in the unit asked for, as a plain
 * number. 3000 m in km is 3. */
static Value bi_to(Interp *in, Value *a, int nargs)
{
    double x = v_realnum(in, a[0]), tf;
    Unit tu;
    (void)nargs;
    if (a[1].k != K_STR) cos_err(in, "to takes the unit name as a string");
    if (!unit_parse(a[1].s, &tf, &tu)) cos_err(in, "unknown unit '%s'", a[1].s);
    /* the unit was stripped before the call, and handed back after, so it
     * is checked here against what the caller asked for */
    return v_real(x / tf);
}

/* unit("km") as a value: 1 km, so "3 * unit(\"km\")" is three kilometres */
static Value bi_unit(Interp *in, Value *a, int nargs)
{
    double f;
    Unit u;
    Value r;
    (void)nargs;
    if (a[0].k != K_STR) cos_err(in, "unit takes a name as a string");
    if (!unit_parse(a[0].s, &f, &u)) cos_err(in, "unknown unit '%s'", a[0].s);
    r = v_real(f);
    r.u = u;
    return r;
}

static Value bi_const(Interp *in, Value *a, int nargs)
{
    double v;
    Unit u;
    Value r;
    (void)nargs;
    if (a[0].k != K_STR) cos_err(in, "const takes a name as a string");
    if (!const_lookup(a[0].s, &v, &u)) cos_err(in, "unknown constant '%s'", a[0].s);
    r = v_real(v);
    r.u = u;
    return r;
}

/* ------------------------------------------------------------------ */
/* statistics                                                          */
/* ------------------------------------------------------------------ */

/* Least squares through (x, y): returns [slope, intercept, r].
 *
 * The sums are compensated and the slope uses the centred form
 * sum((x-xbar)(y-ybar)) / sum((x-xbar)^2) rather than the textbook
 * (n*Sxy - Sx*Sy) one, which loses most of its significant digits when
 * the x values are large and close together: fitting a line to a set of
 * timestamps is the usual way to meet that. */
static Value bi_fit(Interp *in, Value *a, int nargs)
{
    int nx, ny, i;
    double *x = real_vec(in, a[0], &nx);
    double *y = real_vec(in, a[1], &ny);
    double xbar = 0, ybar = 0, sxx = 0, sxy = 0, syy = 0, slope, icept, r;
    Value out;
    (void)nargs;

    if (nx != ny) cos_err(in, "fit needs two lists of the same length");
    if (nx < 2) cos_err(in, "fit needs at least two points");

    xbar = cos_nsum(x, nx) / nx;
    ybar = cos_nsum(y, ny) / ny;
    for (i = 0; i < nx; i++) {
        double dx = x[i] - xbar, dy = y[i] - ybar;
        sxx += dx * dx;
        sxy += dx * dy;
        syy += dy * dy;
    }
    if (sxx == 0.0) cos_err(in, "fit needs x values that are not all equal");
    slope = sxy / sxx;
    icept = ybar - slope * xbar;
    r = (syy > 0.0) ? sxy / sqrt(sxx * syy) : 1.0;

    out = v_mat(in, 1, 3);
    out.m[0] = cx_mk(slope, 0.0);
    out.m[1] = cx_mk(icept, 0.0);
    out.m[2] = cx_mk(r, 0.0);
    return out;
}

/* Discrete convolution: the output is len(a)+len(b)-1 long. Written
 * directly rather than through the FFT because on lists of the size a
 * calculator handles, the direct sum is quicker and exact to the last
 * bit, and it accumulates in double-double so a long kernel does not
 * lose the small terms. */
static Value bi_convolve(Interp *in, Value *a, int nargs)
{
    int na, nb, i, j;
    double *x = real_vec(in, a[0], &na);
    double *y = real_vec(in, a[1], &nb);
    Value out;
    (void)nargs;
    if (na < 1 || nb < 1) cos_err(in, "convolve needs two non-empty lists");
    if ((long)na + nb - 1 > COS_MAXELEM) cos_err(in, "the result is too large");
    out = v_mat(in, 1, na + nb - 1);
    for (i = 0; i < na + nb - 1; i++) {
        dd acc = dd_from(0.0);
        for (j = 0; j < na; j++) {
            int k = i - j;
            if (k < 0 || k >= nb) continue;
            acc = dd_add(acc, dd_mul(dd_from(x[j]), dd_from(y[k])));
        }
        out.m[i] = cx_mk(dd_to(acc), 0.0);
    }
    return out;
}

/* Cross-correlation, which is the same sum with one side not reversed. */
static Value bi_xcorr(Interp *in, Value *a, int nargs)
{
    int na, nb, lag, j;
    double *x = real_vec(in, a[0], &na);
    double *y = real_vec(in, a[1], &nb);
    Value out;
    (void)nargs;
    if (na < 1 || nb < 1) cos_err(in, "xcorr needs two non-empty lists");
    out = v_mat(in, 1, na + nb - 1);
    for (lag = -(nb - 1); lag <= na - 1; lag++) {
        dd acc = dd_from(0.0);
        for (j = 0; j < na; j++) {
            int k = j - lag;
            if (k < 0 || k >= nb) continue;
            acc = dd_add(acc, dd_mul(dd_from(x[j]), dd_from(y[k])));
        }
        out.m[lag + nb - 1] = cx_mk(dd_to(acc), 0.0);
    }
    return out;
}

static Value bi_corr(Interp *in, Value *a, int nargs)
{
    int nx, ny, i;
    double *x = real_vec(in, a[0], &nx);
    double *y = real_vec(in, a[1], &ny);
    double xbar, ybar, sxx = 0, sxy = 0, syy = 0;
    (void)nargs;
    if (nx != ny) cos_err(in, "corr needs two lists of the same length");
    if (nx < 2) cos_err(in, "corr needs at least two points");
    xbar = cos_nsum(x, nx) / nx;
    ybar = cos_nsum(y, ny) / ny;
    for (i = 0; i < nx; i++) {
        double dx = x[i] - xbar, dy = y[i] - ybar;
        sxx += dx * dx; sxy += dx * dy; syy += dy * dy;
    }
    if (sxx == 0.0 || syy == 0.0) cos_err(in, "corr needs values that vary");
    return v_real(sxy / sqrt(sxx * syy));
}

/* quantile(v, p) with linear interpolation between order statistics. */
static Value bi_quantile(Interp *in, Value *a, int nargs)
{
    int k, i, j;
    double *v = real_vec(in, a[0], &k);
    double p = v_realnum(in, a[1]), pos, frac;
    (void)nargs;
    if (k < 1) cos_err(in, "quantile needs at least one value");
    if (p < 0.0 || p > 1.0) cos_err(in, "quantile needs p between 0 and 1");
    for (i = 1; i < k; i++) {            /* insertion sort: lists are short */
        double t = v[i];
        for (j = i - 1; j >= 0 && v[j] > t; j--) v[j + 1] = v[j];
        v[j + 1] = t;
    }
    pos = p * (k - 1);
    i = (int)pos;
    frac = pos - i;
    if (i >= k - 1) return v_real(v[k - 1]);
    return v_real(v[i] + frac * (v[i + 1] - v[i]));
}

static Value bi_mode(Interp *in, Value *a, int nargs)
{
    int k, i, j, best = 0, bestn = 0;
    double *v = real_vec(in, a[0], &k);
    (void)nargs;
    if (k < 1) cos_err(in, "mode needs at least one value");
    for (i = 0; i < k; i++) {
        int c = 0;
        for (j = 0; j < k; j++) if (v[j] == v[i]) c++;
        if (c > bestn) { bestn = c; best = i; }
    }
    return v_real(v[best]);
}

static Value bi_sum(Interp *in, Value *a, int n)
{
    int k;
    double *v = real_vec(in, a[0], &k);
    (void)n;
    return v_real(cos_nsum(v, k));       /* compensated */
}

static Value bi_prod(Interp *in, Value *a, int n)
{
    int k, i;
    double *v = real_vec(in, a[0], &k);
    dd p = dd_from(1.0);
    (void)n;
    for (i = 0; i < k; i++) p = dd_mul(p, dd_from(v[i]));
    return v_real(dd_to(p));
}

static Value bi_mean(Interp *in, Value *a, int n)
{
    int k;
    double *v = real_vec(in, a[0], &k);
    (void)n;
    if (!k) cos_err(in, "mean of an empty vector");
    return v_real(cos_nsum(v, k) / (double)k);
}

static Value bi_var(Interp *in, Value *a, int n)
{
    int k, i;
    double *v = real_vec(in, a[0], &k), m, s = 0.0;
    (void)n;
    if (k < 2) cos_err(in, "variance needs at least two samples");
    m = cos_nsum(v, k) / (double)k;
    /* two-pass, which is backward stable unlike the textbook formula */
    for (i = 0; i < k; i++) s += (v[i] - m) * (v[i] - m);
    return v_real(s / (double)(k - 1));
}

static Value bi_std(Interp *in, Value *a, int n)
{ return v_real(sqrt(v_realnum(in, bi_var(in, a, n)))); }

static Value bi_median(Interp *in, Value *a, int n)
{
    int k;
    double *v = real_vec(in, a[0], &k);
    (void)n;
    if (!k) cos_err(in, "median of an empty vector");
    qsort(v, (size_t)k, sizeof(double), dcmp);
    return v_real(k & 1 ? v[k / 2] : 0.5 * (v[k / 2 - 1] + v[k / 2]));
}

static Value bi_sort(Interp *in, Value *a, int n)
{
    int k, i;
    double *v = real_vec(in, a[0], &k);
    Value r;
    (void)n;
    qsort(v, (size_t)k, sizeof(double), dcmp);
    r = v_mat(in, 1, k);
    for (i = 0; i < k; i++) r.m[i] = cx_mk(v[i], 0);
    return r;
}

static Value bi_cumsum(Interp *in, Value *a, int n)
{
    int k, i;
    double *v = real_vec(in, a[0], &k);
    dd acc = dd_from(0.0);
    Value r;
    (void)n;
    r = v_mat(in, 1, k);
    for (i = 0; i < k; i++) {
        acc = dd_add(acc, dd_from(v[i]));
        r.m[i] = cx_mk(dd_to(acc), 0);
    }
    return r;
}

/* ================================================================== */
/* numerical methods                                                  */
/* ================================================================== */

static Value bi_polyval(Interp *in, Value *a, int n)
{
    int k;
    double *p = real_vec(in, a[0], &k);
    (void)n;
    return v_real(cos_horner(p, k, RA(1)));
}

/* tanh-sinh (double exponential) quadrature.
 *
 * x(t) = tanh(pi/2 sinh t),  w(t) = (pi/2) cosh t / cosh^2(pi/2 sinh t)
 * The substitution makes every derivative vanish at the endpoints, so
 * endpoint singularities (1/sqrt(x), log x) integrate to full precision.
 * Halving the step reuses every previous node: S_m = S_{m-1}/2 + h_m T_m.
 */
static double ts_halfsum(Interp *in, Value f, double lo, double hi, double d,
                         double h, int odd_only)
{
    double s = 0.0;
    int k = odd_only ? 1 : 0;
    int step = odd_only ? 2 : 1;
    for (; ; k += step) {
        double t = (double)k * h;
        double u = (M_PI / 2.0) * sinh(t);
        double ch = cosh(u);
        double w = (M_PI / 2.0) * cosh(t) / (ch * ch);
        double omx, fv;
        if (!isfinite(w) || w < 1e-300) break;
        if (k == 0) {
            fv = cos_call_real(in, f, lo + d);        /* the midpoint */
            if (isfinite(fv)) s += w * fv;
            if (step == 1) continue;
            break;
        }
        /* 1 - tanh(u), formed directly so that points close to an endpoint
         * keep full relative accuracy - this is what lets the rule handle
         * endpoint singularities. */
        omx = 2.0 / (1.0 + exp(2.0 * u));
        if (omx * d == 0.0) break;                    /* node has collapsed */
        fv = cos_call_real(in, f, lo + d * omx);
        if (isfinite(fv)) s += w * fv;
        fv = cos_call_real(in, f, hi - d * omx);
        if (isfinite(fv)) s += w * fv;
        if (t > 7.0) break;
    }
    return s;
}

static Value bi_integrate(Interp *in, Value *a, int n)
{
    double lo = RA(1), hi = RA(2), d, h = 1.0, s, prev;
    int level, sign = 1;
    (void)n;
    if (hi == lo) return v_real(0.0);
    if (hi < lo) { double t = lo; lo = hi; hi = t; sign = -1; }

    d = 0.5 * (hi - lo);

    s = ts_halfsum(in, a[0], lo, hi, d, h, 0);  /* level 0, step 1 */
    prev = s;
    for (level = 1; level <= 11; level++) {
        h *= 0.5;
        s = 0.5 * s + h * ts_halfsum(in, a[0], lo, hi, d, h, 1);
        if (level >= 3 && fabs(s - prev) < 1e-15 * (fabs(s) + 1e-30)) break;
        prev = s;
    }
    return v_real((double)sign * d * s);
}

/* Brent's method */
static Value bi_root(Interp *in, Value *a, int n)
{
    double lo = RA(1), hi = RA(2);
    double fa = cos_call_real(in, a[0], lo), fb = cos_call_real(in, a[0], hi);
    double c = lo, fc = fa, d = hi - lo, e = d;
    int it;
    (void)n;
    if (fa == 0.0) return v_real(lo);
    if (fb == 0.0) return v_real(hi);
    if (fa * fb > 0.0)
        cos_err(in, "root: f(%g) and f(%g) have the same sign", lo, hi);
    for (it = 0; it < 200; it++) {
        double tol, m, s, p, q, r;
        if (fb * fc > 0.0) { c = lo; fc = fa; d = e = hi - lo; }
        if (fabs(fc) < fabs(fb)) {
            lo = hi; hi = c; c = lo;
            fa = fb; fb = fc; fc = fa;
        }
        tol = 2.0 * 2.22e-16 * fabs(hi) + 1e-300;
        m = 0.5 * (c - hi);
        if (fabs(m) <= tol || fb == 0.0) return v_real(hi);
        if (fabs(e) < tol || fabs(fa) <= fabs(fb)) {
            d = e = m;
        } else {
            s = fb / fa;
            if (lo == c) { p = 2.0 * m * s; q = 1.0 - s; }
            else {
                q = fa / fc; r = fb / fc;
                p = s * (2.0 * m * q * (q - r) - (hi - lo) * (r - 1.0));
                q = (q - 1.0) * (r - 1.0) * (s - 1.0);
            }
            if (p > 0.0) q = -q; else p = -p;
            if (2.0 * p < 3.0 * m * q - fabs(tol * q) && p < fabs(0.5 * e * q)) {
                e = d; d = p / q;
            } else { d = e = m; }
        }
        lo = hi; fa = fb;
        hi += (fabs(d) > tol) ? d : (m > 0 ? tol : -tol);
        fb = cos_call_real(in, a[0], hi);
    }
    cos_err(in, "root: no convergence in 200 iterations");
    return v_nil();
}

static Value bi_newton(Interp *in, Value *a, int n)
{
    double x = RA(1), fx, dfx, h, step;
    int it;
    (void)n;
    for (it = 0; it < 80; it++) {
        fx = cos_call_real(in, a[0], x);
        if (fx == 0.0) return v_real(x);
        h = cbrt(2.22e-16) * (fabs(x) > 1.0 ? fabs(x) : 1.0);
        dfx = (cos_call_real(in, a[0], x + h) - cos_call_real(in, a[0], x - h))
              / (2.0 * h);
        if (dfx == 0.0) cos_err(in, "newton: zero derivative at %g", x);
        step = fx / dfx;
        x -= step;
        if (fabs(step) <= 1e-15 * (fabs(x) + 1e-30)) return v_real(x);
    }
    cos_err(in, "newton: no convergence in 80 iterations");
    return v_nil();
}

/* Richardson-extrapolated central difference */
static Value bi_diff(Interp *in, Value *a, int n)
{
    double x = RA(1);
    int order = (n >= 3) ? (int)v_realnum(in, a[2]) : 1;
    double tab[8][8];
    double h = 0.25 * (fabs(x) > 1.0 ? fabs(x) : 1.0);
    int i, j, levels = 6;   /* tuned: deeper tables amplify rounding noise */
    if (order < 1 || order > 2) cos_err(in, "diff supports order 1 or 2");
    for (i = 0; i < levels; i++) {
        if (order == 1)
            tab[i][0] = (cos_call_real(in, a[0], x + h)
                       - cos_call_real(in, a[0], x - h)) / (2.0 * h);
        else
            tab[i][0] = (cos_call_real(in, a[0], x + h)
                       - 2.0 * cos_call_real(in, a[0], x)
                       + cos_call_real(in, a[0], x - h)) / (h * h);
        for (j = 1; j <= i; j++) {
            double f = pow(4.0, (double)j);
            tab[i][j] = (f * tab[i][j - 1] - tab[i - 1][j - 1]) / (f - 1.0);
        }
        h *= 0.5;
    }
    return v_real(tab[levels - 1][levels - 1]);
}

/* golden-section minimisation */
static Value bi_minimize(Interp *in, Value *a, int n)
{
    const double gr = 0.6180339887498948482;
    double lo = RA(1), hi = RA(2);
    double c = hi - gr * (hi - lo), d = lo + gr * (hi - lo);
    double fc = cos_call_real(in, a[0], c), fd = cos_call_real(in, a[0], d);
    int it;
    (void)n;
    for (it = 0; it < 200 && fabs(hi - lo) > 1e-12 * (fabs(lo) + fabs(hi) + 1e-30); it++) {
        if (fc < fd) {
            hi = d; d = c; fd = fc;
            c = hi - gr * (hi - lo);
            fc = cos_call_real(in, a[0], c);
        } else {
            lo = c; c = d; fc = fd;
            d = lo + gr * (hi - lo);
            fd = cos_call_real(in, a[0], d);
        }
    }
    return v_real(0.5 * (lo + hi));
}

/* Dormand-Prince RK45 with adaptive steps; scalar y, f(t, y) */
static Value bi_ode(Interp *in, Value *a, int n)
{
    double t = RA(1), y = RA(2), tend = RA(3);
    double tol = (n >= 5) ? v_realnum(in, a[4]) : 1e-10;
    double h = (tend - t) * 0.01;
    int steps = 0;
    Value args[2];
    if (h == 0.0) return v_real(y);
    while ((tend - t) * (h > 0 ? 1 : -1) > 0 && steps++ < 200000) {
        double k1, k2, k3, k4, k5, k6, k7, y5, y4, errv, sc;
        if (fabs(h) > fabs(tend - t)) h = tend - t;
#define F(tt, yy) (args[0] = v_real(tt), args[1] = v_real(yy), \
                   v_realnum(in, cos_call_value(in, a[0], args, 2)))
        k1 = F(t, y);
        k2 = F(t + h / 5.0, y + h * k1 / 5.0);
        k3 = F(t + 3.0 * h / 10.0, y + h * (3.0 * k1 + 9.0 * k2) / 40.0);
        k4 = F(t + 4.0 * h / 5.0,
               y + h * (44.0 * k1 / 45.0 - 56.0 * k2 / 15.0 + 32.0 * k3 / 9.0));
        k5 = F(t + 8.0 * h / 9.0,
               y + h * (19372.0 * k1 / 6561.0 - 25360.0 * k2 / 2187.0
                      + 64448.0 * k3 / 6561.0 - 212.0 * k4 / 729.0));
        k6 = F(t + h,
               y + h * (9017.0 * k1 / 3168.0 - 355.0 * k2 / 33.0
                      + 46732.0 * k3 / 5247.0 + 49.0 * k4 / 176.0
                      - 5103.0 * k5 / 18656.0));
        y5 = y + h * (35.0 * k1 / 384.0 + 500.0 * k3 / 1113.0
                    + 125.0 * k4 / 192.0 - 2187.0 * k5 / 6784.0
                    + 11.0 * k6 / 84.0);
        k7 = F(t + h, y5);               /* FSAL stage, needed by the 4th-order pair */
        y4 = y + h * (5179.0 * k1 / 57600.0 + 7571.0 * k3 / 16695.0
                    + 393.0 * k4 / 640.0 - 92097.0 * k5 / 339200.0
                    + 187.0 * k6 / 2100.0 + k7 / 40.0);
#undef F
        errv = fabs(y5 - y4);
        sc = tol * (1.0 + fabs(y));
        if (errv <= sc || fabs(h) < 1e-14) {
            t += h;
            y = y5;
        }
        {
            double fac = (errv > 0.0) ? 0.9 * pow(sc / errv, 0.2) : 4.0;
            if (fac < 0.2) fac = 0.2;
            if (fac > 4.0) fac = 4.0;
            h *= fac;
        }
    }
    return v_real(y);
}

/* compensated summation of a series f(k), k = lo..hi */
static Value bi_nsum(Interp *in, Value *a, int n)
{
    double lo = RA(1), hi = RA(2), k;
    dd acc = dd_from(0.0);
    long count = 0;
    (void)n;
    for (k = lo; k <= hi; k += 1.0) {
        acc = dd_add(acc, dd_from(cos_call_real(in, a[0], k)));
        if (++count > 2000000) cos_err(in, "nsum: too many terms");
    }
    return v_real(dd_to(acc));
}

/* iterative radix-2 FFT */
static Value bi_fft_core(Interp *in, Value x, int inverse)
{
    int n = x.rows * x.cols, i, j, len;
    Value r;
    if (n < 2 || (n & (n - 1)))
        cos_err(in, "fft needs a power-of-two length (got %d)", n);
    r = v_mat(in, 1, n);
    for (i = 0; i < n; i++) r.m[i] = x.m[i];
    for (i = 1, j = 0; i < n; i++) {           /* bit reversal */
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { cx t = r.m[i]; r.m[i] = r.m[j]; r.m[j] = t; }
    }
    for (len = 2; len <= n; len <<= 1) {
        double ang = 2.0 * M_PI / (double)len * (inverse ? 1.0 : -1.0);
        for (i = 0; i < n; i += len) {
            for (j = 0; j < len / 2; j++) {
                double th = ang * (double)j;
                cx w = cx_mk(cos(th), sin(th));
                cx u = r.m[i + j], v = cx_mul(r.m[i + j + len / 2], w);
                r.m[i + j] = cx_add(u, v);
                r.m[i + j + len / 2] = cx_sub(u, v);
            }
        }
    }
    if (inverse)
        for (i = 0; i < n; i++) r.m[i] = cx_div(r.m[i], cx_mk((double)n, 0.0));
    return r;
}

static Value bi_fft(Interp *in, Value *a, int n)
{ (void)n; return bi_fft_core(in, need_mat(in, a[0], "fft"), 0); }
static Value bi_ifft(Interp *in, Value *a, int n)
{ (void)n; return bi_fft_core(in, need_mat(in, a[0], "ifft"), 1); }

/* ================================================================== */
/* misc                                                               */
/* ================================================================== */

/* ================================================================== */
/* graphing                                                           */
/* ================================================================== */

#define PLOT_FG   0x30E060u
#define PLOT_AXIS 0x606060u
#define PLOT_BG   0x000000u

static Value bi_plot(Interp *in, Value *a, int n)
{
    int w = plat_gfx_width(), h = plat_gfx_height();
    double x0 = RA(1), x1 = RA(2);
    double lo, hi, span;
    double *ys;
    int *py;
    int i, first = 1, prev_x = 0, prev_y = 0;
    char lbl[32];

    if (w < 16 || h < 16) cos_err(in, "no display to plot on");
    if (x1 == x0) cos_err(in, "plot range is empty");
    if (x1 < x0) { double t = x0; x0 = x1; x1 = t; }

    ys = (double *)cos_alloc(in, (size_t)w * sizeof(double));
    py = (int *)cos_alloc(in, (size_t)w * sizeof(int));

    /* sample */
    for (i = 0; i < w; i++) {
        double x = x0 + (x1 - x0) * (double)i / (double)(w - 1);
        double v;
        if (a[0].k == K_FUN) v = cos_call_real(in, a[0], x);
        else {
            Value arg = v_real(x);
            Value r = cos_call_value(in, a[0], &arg, 1);
            v = v_realnum(in, r);
        }
        ys[i] = v;
    }

    if (n >= 5) { lo = RA(3); hi = v_realnum(in, a[4]); }
    else {
        lo = 1e308; hi = -1e308;
        for (i = 0; i < w; i++) {
            if (!isfinite(ys[i])) continue;
            if (ys[i] < lo) lo = ys[i];
            if (ys[i] > hi) hi = ys[i];
        }
        if (lo > hi) { lo = -1.0; hi = 1.0; }
        if (hi - lo < 1e-12) { lo -= 1.0; hi += 1.0; }
        span = hi - lo;
        lo -= 0.05 * span;
        hi += 0.05 * span;
    }
    span = hi - lo;
    if (span <= 0) span = 1.0;

    plat_gfx_clear();

    /* axes, where they fall inside the view */
    if (lo < 0.0 && hi > 0.0) {
        int yz = (int)((hi - 0.0) / span * (double)(h - 1));
        plat_gfx_line(0, yz, w - 1, yz, PLOT_AXIS);
    }
    if (x0 < 0.0 && x1 > 0.0) {
        int xz = (int)((0.0 - x0) / (x1 - x0) * (double)(w - 1));
        plat_gfx_line(xz, 0, xz, h - 1, PLOT_AXIS);
    }

    for (i = 0; i < w; i++) {
        if (!isfinite(ys[i])) { py[i] = -1000000; continue; }
        py[i] = (int)((hi - ys[i]) / span * (double)(h - 1));
    }

    for (i = 0; i < w; i++) {
        int y = py[i];
        if (y < -h || y > 2 * h) { first = 1; continue; }
        if (!first) plat_gfx_line(prev_x, prev_y, i, y, PLOT_FG);
        else plat_gfx_point(i, y, PLOT_FG);
        prev_x = i;
        prev_y = y;
        first = 0;
    }

    snprintf(lbl, sizeof lbl, "%.4g", hi);
    plat_gfx_text(2, 1, lbl, PLOT_AXIS);
    snprintf(lbl, sizeof lbl, "%.4g", lo);
    plat_gfx_text(2, h - 10, lbl, PLOT_AXIS);
    snprintf(lbl, sizeof lbl, "%.4g", x0);
    plat_gfx_text(2, h - 20, lbl, PLOT_AXIS);
    snprintf(lbl, sizeof lbl, "%.4g", x1);
    plat_gfx_text(w - 40, h - 20, lbl, PLOT_AXIS);

    plat_gfx_flush();
    return v_nil();
}

static Value bi_typeof(Interp *in, Value *a, int n)
{
    const char *s = "unknown";
    (void)n;
    switch (a[0].k) {
    case K_NUM: s = a[0].n.im == 0.0 ? "real" : "complex"; break;
    case K_DD:  s = "extended"; break;
    case K_IV:  s = "interval"; break;
    case K_MAT: s = "matrix"; break;
    case K_STR: s = "string"; break;
    case K_FUN: s = "function"; break;
    default:    s = "nil"; break;
    }
    return v_str(in, s);
}

static Value bi_print(Interp *in, Value *a, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        if (i) plat_puts(" ");
        if (a[i].k == K_STR) plat_puts(a[i].s);
        else { char b[192]; v_format(in, a[i], b, sizeof b); plat_puts(b); }
    }
    plat_puts("\n");
    return v_nil();
}

/* ================================================================== */
/* table                                                              */
/* ================================================================== */

static const Builtin table[] = {
 {"sqrt",1,1,bi_sqrt,"square root (complex, extended and interval aware)"},
 {"cbrt",1,1,bi_cbrt,"real cube root"},
 {"exp",1,1,bi_exp,"e^x"},
 {"log",1,2,bi_log,"natural log, or log(x, base)"},
 {"log2",1,1,bi_log2,"base-2 log"},
 {"log10",1,1,bi_log10,"base-10 log"},
 {"log1p",1,1,bi_log1p,"log(1+x), accurate for tiny x"},
 {"expm1",1,1,bi_expm1,"exp(x)-1, accurate for tiny x"},
 {"abs",1,1,bi_abs,"absolute value / modulus"},
 {"sign",1,1,bi_sign,"-1, 0 or 1"},
 {"floor",1,1,bi_floor,"round toward -inf"},
 {"ceil",1,1,bi_ceil,"round toward +inf"},
 {"round",1,1,bi_round,"round half away from zero"},
 {"trunc",1,1,bi_trunc,"round toward zero"},
 {"sin",1,1,bi_sin,"sine"},
 {"cos",1,1,bi_cos,"cosine"},
 {"tan",1,1,bi_tan,"tangent"},
 {"asin",1,1,bi_asin,"inverse sine"},
 {"acos",1,1,bi_acos,"inverse cosine"},
 {"atan",1,1,bi_atan,"inverse tangent"},
 {"atan2",2,2,bi_atan2,"atan2(y, x)"},
 {"sinh",1,1,bi_sinh,"hyperbolic sine"},
 {"cosh",1,1,bi_cosh,"hyperbolic cosine"},
 {"tanh",1,1,bi_tanh,"hyperbolic tangent"},
 {"asinh",1,1,bi_asinh,"inverse hyperbolic sine"},
 {"acosh",1,1,bi_acosh,"inverse hyperbolic cosine"},
 {"atanh",1,1,bi_atanh,"inverse hyperbolic tangent"},
 {"hypot",2,2,bi_hypot,"sqrt(x^2+y^2) without overflow"},
 {"mod",2,2,bi_mod,"floored modulus"},
 {"rem",2,2,bi_rem,"truncated remainder"},
 {"pow",2,2,bi_pow,"x^y"},
 {"fma",3,3,bi_fma,"fused multiply-add, one rounding"},
 {"min",1,COS_MAXARGS,bi_min,"smallest value"},
 {"max",1,COS_MAXARGS,bi_max,"largest value"},

 {"re",1,1,bi_re,"real part"},
 {"im",1,1,bi_im,"imaginary part"},
 {"conj",1,1,bi_conj,"complex conjugate"},
 {"arg",1,1,bi_arg,"argument (phase)"},
 {"polar",2,2,bi_polar,"polar(r, theta) -> complex"},

 {"gamma",1,1,bi_gamma,"gamma function"},
 {"lgamma",1,1,bi_lgamma,"log gamma"},
 {"digamma",1,1,bi_digamma,"psi(x)"},
 {"beta",2,2,bi_beta,"beta function"},
 {"erf",1,1,bi_erf,"error function"},
 {"erfc",1,1,bi_erfc,"complementary error function"},
 {"zeta",1,1,bi_zeta,"Riemann zeta"},
 {"besselj0",1,1,bi_besselj0,"Bessel J0"},
 {"besselj1",1,1,bi_besselj1,"Bessel J1"},
 {"bessely0",1,1,bi_bessely0,"Bessel Y0"},
 {"bessely1",1,1,bi_bessely1,"Bessel Y1"},
 {"ei",1,1,bi_ei,"exponential integral"},
 {"li",1,1,bi_li,"logarithmic integral"},
 {"factorial",1,1,bi_factorial,"x! via gamma"},
 {"binomial",2,2,bi_binomial,"n choose k"},

 {"ulp",1,1,bi_ulp,"spacing of doubles at x"},
 {"nextup",1,1,bi_nextup,"next double toward +inf"},
 {"nextdown",1,1,bi_nextdown,"next double toward -inf"},
 {"relerr",2,2,bi_relerr,"relative error of a against b"},
 {"ulpdiff",2,2,bi_ulpdiff,"signed difference in ulps"},
 {"bits",1,1,bi_bits,"IEEE-754 fields of x"},
 {"hex",1,1,bi_hex,"exact hex float form"},
 {"exponent",1,1,bi_exponent,"binary exponent"},
 {"significand",1,1,bi_significand,"mantissa in [1,2)"},
 {"isnan",1,1,bi_isnan,"1 if NaN"},
 {"isinf",1,1,bi_isinf,"1 if infinite"},

 {"dd",1,1,bi_dd,"to extended precision; dd(\"...\") parses digits"},
 {"hi",1,1,bi_hi,"high limb of an extended value"},
 {"lo",1,1,bi_lo,"low limb of an extended value"},
 {"iv",1,2,bi_iv,"interval: iv(a,b) or iv(x)"},
 {"inf",1,1,bi_inf,"interval lower bound"},
 {"sup",1,1,bi_sup,"interval upper bound"},
 {"wid",1,1,bi_wid,"interval width"},
 {"mid",1,1,bi_mid,"interval midpoint"},

 {"zeros",0,2,bi_zeros,"zero matrix"},
 {"ones",0,2,bi_ones,"matrix of ones"},
 {"eye",0,2,bi_eye,"identity matrix"},
 {"rand",0,2,bi_rand,"uniform random"},
 {"randn",0,0,bi_randn,"standard normal sample"},
 {"seed",1,1,bi_seed,"seed the generator"},
 {"linspace",2,3,bi_linspace,"evenly spaced row vector"},
 {"size",1,1,bi_size,"[rows, cols]"},
 {"numel",1,1,bi_numel,"element count"},
 {"reshape",3,3,bi_reshape,"reshape(m, rows, cols)"},
 {"transpose",1,1,bi_transpose,"conjugate transpose"},
 {"diag",1,1,bi_diag,"diagonal to vector, or vector to diagonal"},

 {"det",1,1,bi_det,"determinant (LU)"},
 {"inv",1,1,bi_inv,"matrix inverse"},
 {"solve",2,2,bi_solve,"solve A x = b with iterative refinement"},
 {"trace",1,1,bi_trace,"sum of the diagonal"},
 {"norm",1,2,bi_norm,"norm(x) or norm(x, p)"},
 {"cond",1,1,bi_cond,"1-norm condition number"},
 {"dot",2,2,bi_dot,"exact-product dot product"},
 {"cross",2,2,bi_cross,"3-vector cross product"},

 {"sum",1,1,bi_sum,"compensated sum"},
 {"prod",1,1,bi_prod,"product"},
 {"mean",1,1,bi_mean,"arithmetic mean"},
 {"var",1,1,bi_var,"sample variance (two-pass)"},
 {"std",1,1,bi_std,"sample standard deviation"},
 {"median",1,1,bi_median,"median"},
 {"mode",1,1,bi_mode,"most frequent value in a list"},
 {"quantile",2,2,bi_quantile,"quantile of a list, p from 0 to 1"},
 {"corr",2,2,bi_corr,"correlation coefficient of two lists"},
 {"convolve",2,2,bi_convolve,"discrete convolution of two lists"},
 {"xcorr",2,2,bi_xcorr,"cross-correlation of two lists"},
 {"fit",2,2,bi_fit,"least-squares line through two lists: [slope, intercept, r]"},
 {"big",1,1,bi_big,"an exact integer of any size: big(2)^100, big(\"123...\")"},
 {"bigfact",1,1,bi_bigfact,"exact factorial"},
 {"powmod",3,3,bi_powmod,"a^b mod m, exactly"},
 {"gcd",2,2,bi_gcd,"greatest common divisor, exactly"},
 {"lcm",2,2,bi_lcm,"least common multiple, exactly"},
 {"ndigits",1,1,bi_digits_of,"how many decimal digits a whole number has"},
 {"str",1,1,bi_tostr,"a value as a string"},
 {"conv",3,3,bi_conv,"convert a number between units: conv(5,\"mile\",\"km\")"},
 {"to",2,2,bi_to,"a quantity expressed in another unit, as a plain number"},
 {"unit",1,1,bi_unit,"one of the named unit, as a quantity"},
 {"const",1,1,bi_const,"a physical constant by name, with its unit"},
 {"sort",1,1,bi_sort,"ascending sort"},
 {"cumsum",1,1,bi_cumsum,"running compensated sum"},

 {"polyval",2,2,bi_polyval,"Horner with fma; highest power first"},
 {"integrate",3,3,bi_integrate,"integrate(f, a, b) by tanh-sinh"},
 {"root",3,3,bi_root,"root(f, a, b) by Brent"},
 {"newton",2,2,bi_newton,"newton(f, x0)"},
 {"diff",2,3,bi_diff,"diff(f, x [, order]) by Richardson"},
 {"minimize",3,3,bi_minimize,"minimize(f, a, b) by golden section"},
 {"ode",4,5,bi_ode,"ode(f, t0, y0, t1 [, tol]) RK45"},
 {"nsum",3,3,bi_nsum,"nsum(f, lo, hi) compensated series sum"},
 {"fft",1,1,bi_fft,"radix-2 FFT"},
 {"ifft",1,1,bi_ifft,"inverse FFT"},

 {"plot",3,5,bi_plot,"plot(f, x0, x1 [, y0, y1]) graph a function"},
 {"typeof",1,1,bi_typeof,"name of a value's type"},
 {"print",1,COS_MAXARGS,bi_print,"print values"},
 {NULL,0,0,NULL,NULL}
};

const Builtin *cos_builtin(const char *name)
{
    int i;
    for (i = 0; table[i].name; i++)
        if (strcmp(table[i].name, name) == 0) return &table[i];
    return NULL;
}

const Builtin *cos_builtin_table(int *count)
{
    int i = 0;
    while (table[i].name) i++;
    if (count) *count = i;
    return table;
}

