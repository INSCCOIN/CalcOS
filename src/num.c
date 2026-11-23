/* CalcOS numeric layer: complex, double-double, interval, special functions.
 * Everything here is self-contained and depends only on libm.
 */
#include "calc.h"
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <float.h>

/* ================================================================== */
/* complex                                                            */
/* ================================================================== */

cx cx_mk(double re, double im) { cx z; z.re = re; z.im = im; return z; }
cx cx_add(cx a, cx b) { return cx_mk(a.re + b.re, a.im + b.im); }
cx cx_sub(cx a, cx b) { return cx_mk(a.re - b.re, a.im - b.im); }
cx cx_neg(cx a)       { return cx_mk(-a.re, -a.im); }
cx cx_conj(cx a)      { return cx_mk(a.re, -a.im); }

cx cx_mul(cx a, cx b)
{
    if (a.im == 0.0 && b.im == 0.0) return cx_mk(a.re * b.re, 0.0);
    return cx_mk(a.re * b.re - a.im * b.im, a.re * b.im + a.im * b.re);
}

/* Smith's algorithm: avoids spurious overflow in the denominator */
cx cx_div(cx a, cx b)
{
    double r, den;
    /* a real quotient must stay real: 0/0 in the imaginary slot would
     * otherwise turn 1/0 into inf + nan*i */
    if (b.im == 0.0)
        return cx_mk(a.re / b.re, a.im == 0.0 ? 0.0 : a.im / b.re);
    if (fabs(b.re) >= fabs(b.im)) {
        r = b.im / b.re; den = b.re + b.im * r;
        return cx_mk((a.re + a.im * r) / den, (a.im - a.re * r) / den);
    }
    r = b.re / b.im; den = b.re * r + b.im;
    return cx_mk((a.re * r + a.im) / den, (a.im * r - a.re) / den);
}

double cx_abs(cx a) { return hypot(a.re, a.im); }
double cx_arg(cx a) { return atan2(a.im, a.re); }
int    cx_isreal(cx a) { return a.im == 0.0; }

cx cx_exp(cx a)
{
    double e = exp(a.re);
    if (a.im == 0.0) return cx_mk(e, 0.0);
    return cx_mk(e * cos(a.im), e * sin(a.im));
}

cx cx_log(cx a)
{
    if (a.im == 0.0 && a.re > 0.0) return cx_mk(log(a.re), 0.0);
    return cx_mk(log(cx_abs(a)), cx_arg(a));
}

cx cx_sqrt(cx a)
{
    double r, t;
    if (a.re == 0.0 && a.im == 0.0) return cx_mk(0.0, 0.0);
    if (a.im == 0.0) {
        if (a.re > 0.0) return cx_mk(sqrt(a.re), 0.0);
        return cx_mk(0.0, sqrt(-a.re));
    }
    r = cx_abs(a);
    t = sqrt(0.5 * (r + fabs(a.re)));
    if (a.re >= 0.0) return cx_mk(t, 0.5 * a.im / t);
    return cx_mk(0.5 * fabs(a.im) / t, a.im >= 0 ? t : -t);
}

cx cx_pow(cx a, cx b)
{
    if (a.re == 0.0 && a.im == 0.0) {
        if (b.re == 0.0 && b.im == 0.0) return cx_mk(1.0, 0.0);
        return cx_mk(0.0, 0.0);
    }
    /* real base, integral real exponent: use exact repeated squaring */
    if (a.im == 0.0 && b.im == 0.0 && b.re == floor(b.re) && fabs(b.re) < 1024) {
        double r = 1.0, base = a.re;
        long n = (long)fabs(b.re), i;
        for (i = 0; i < n; i++) r *= base;
        return cx_mk(b.re < 0 ? 1.0 / r : r, 0.0);
    }
    if (a.im == 0.0 && a.re > 0.0 && b.im == 0.0)
        return cx_mk(pow(a.re, b.re), 0.0);
    return cx_exp(cx_mul(b, cx_log(a)));
}

cx cx_sin(cx a)
{
    if (a.im == 0.0) return cx_mk(sin(a.re), 0.0);
    return cx_mk(sin(a.re) * cosh(a.im), cos(a.re) * sinh(a.im));
}
cx cx_cos(cx a)
{
    if (a.im == 0.0) return cx_mk(cos(a.re), 0.0);
    return cx_mk(cos(a.re) * cosh(a.im), -sin(a.re) * sinh(a.im));
}
cx cx_tan(cx a) { return cx_div(cx_sin(a), cx_cos(a)); }

cx cx_sinh(cx a)
{
    if (a.im == 0.0) return cx_mk(sinh(a.re), 0.0);
    return cx_mk(sinh(a.re) * cos(a.im), cosh(a.re) * sin(a.im));
}
cx cx_cosh(cx a)
{
    if (a.im == 0.0) return cx_mk(cosh(a.re), 0.0);
    return cx_mk(cosh(a.re) * cos(a.im), sinh(a.re) * sin(a.im));
}
cx cx_tanh(cx a) { return cx_div(cx_sinh(a), cx_cosh(a)); }

cx cx_asin(cx a)
{
    cx i, t;
    if (a.im == 0.0 && fabs(a.re) <= 1.0) return cx_mk(asin(a.re), 0.0);
    i = cx_mk(0.0, 1.0);
    /* -i*log(i*a + sqrt(1-a^2)) */
    t = cx_sqrt(cx_sub(cx_mk(1.0, 0.0), cx_mul(a, a)));
    t = cx_log(cx_add(cx_mul(i, a), t));
    return cx_mul(cx_mk(0.0, -1.0), t);
}

cx cx_acos(cx a)
{
    if (a.im == 0.0 && fabs(a.re) <= 1.0) return cx_mk(acos(a.re), 0.0);
    return cx_sub(cx_mk(M_PI / 2, 0.0), cx_asin(a));
}

cx cx_atan(cx a)
{
    cx i = cx_mk(0.0, 1.0), num, den;
    if (a.im == 0.0) return cx_mk(atan(a.re), 0.0);
    num = cx_add(i, a);
    den = cx_sub(i, a);
    return cx_mul(cx_mk(0.0, 0.5), cx_log(cx_div(num, den)));
}

/* ================================================================== */
/* double-double (QD-style error-free transformations)                */
/* ================================================================== */

static double quick_two_sum(double a, double b, double *err)
{
    double s = a + b;
    *err = b - (s - a);
    return s;
}

static double two_sum(double a, double b, double *err)
{
    double s = a + b;
    double bb = s - a;
    *err = (a - (s - bb)) + (b - bb);
    return s;
}

static void split_d(double a, double *hi, double *lo)
{
    double t = 134217729.0 * a;      /* 2^27 + 1 */
    *hi = t - (t - a);
    *lo = a - *hi;
}

static double two_prod(double a, double b, double *err)
{
#ifdef FP_FAST_FMA
    double p = a * b;
    *err = fma(a, b, -p);
    return p;
#else
    double p = a * b, ahi, alo, bhi, blo;
    split_d(a, &ahi, &alo);
    split_d(b, &bhi, &blo);
    *err = ((ahi * bhi - p) + ahi * blo + alo * bhi) + alo * blo;
    return p;
#endif
}

dd DD_PI  = { 3.141592653589793116e+00, 1.224646799147353207e-16 };
dd DD_E   = { 2.718281828459045091e+00, 1.445646891729250158e-16 };
dd DD_LN2 = { 6.931471805599452862e-01, 2.319046813846299558e-17 };
dd DD_PHI = { 1.618033988749894848e+00, 0.0 };

dd dd_mk(double hi, double lo) { dd r; r.hi = hi; r.lo = lo; return r; }
dd dd_from(double x) { return dd_mk(x, 0.0); }
double dd_to(dd a) { return a.hi + a.lo; }
dd dd_neg(dd a) { return dd_mk(-a.hi, -a.lo); }

dd dd_add(dd a, dd b)
{
    double s1, s2, t1, t2;
    s1 = two_sum(a.hi, b.hi, &s2);
    t1 = two_sum(a.lo, b.lo, &t2);
    s2 += t1;
    s1 = quick_two_sum(s1, s2, &s2);
    s2 += t2;
    s1 = quick_two_sum(s1, s2, &s2);
    return dd_mk(s1, s2);
}

dd dd_sub(dd a, dd b) { return dd_add(a, dd_neg(b)); }

dd dd_mul(dd a, dd b)
{
    double p1, p2;
    p1 = two_prod(a.hi, b.hi, &p2);
    p2 += a.hi * b.lo + a.lo * b.hi;
    p1 = quick_two_sum(p1, p2, &p2);
    return dd_mk(p1, p2);
}

dd dd_div(dd a, dd b)
{
    double q1, q2, q3, p1, p2, s, e;
    dd r;
    q1 = a.hi / b.hi;
    /* r = a - q1*b */
    p1 = two_prod(q1, b.hi, &p2);
    s  = two_sum(a.hi, -p1, &e);
    e += a.lo;
    e -= p2;
    e -= q1 * b.lo;
    q2 = (s + e) / b.hi;
    /* note: quick_two_sum writes q3, so it must be sequenced before the
     * struct is built - do not fold this into a single expression. */
    q1 = quick_two_sum(q1, q2, &q3);
    r.hi = q1;
    r.lo = q3;
    return r;
}

int dd_cmp(dd a, dd b)
{
    if (a.hi < b.hi) return -1;
    if (a.hi > b.hi) return 1;
    if (a.lo < b.lo) return -1;
    if (a.lo > b.lo) return 1;
    return 0;
}

dd dd_sqrt(dd a)
{
    double x, ax;
    dd t;
    if (a.hi == 0.0 && a.lo == 0.0) return dd_from(0.0);
    if (a.hi < 0.0) return dd_from(NAN);
    /* Karp's trick: one Newton step in double precision */
    x  = 1.0 / sqrt(a.hi);
    ax = a.hi * x;
    t  = dd_sub(a, dd_mul(dd_from(ax), dd_from(ax)));
    return dd_add(dd_from(ax), dd_from(dd_to(t) * x * 0.5));
}

dd dd_powi(dd a, int n)
{
    dd r = dd_from(1.0), b = a;
    int k = n < 0 ? -n : n;
    while (k) {
        if (k & 1) r = dd_mul(r, b);
        b = dd_mul(b, b);
        k >>= 1;
    }
    if (n < 0) return dd_div(dd_from(1.0), r);
    return r;
}

dd dd_exp(dd a)
{
    /* a = k*ln2 + r, |r| <= ln2/2; then exp(r) by Taylor after 9 halvings */
    const int halvings = 9;
    double k;
    dd r, s, p, t;
    int i, j;
    if (a.hi <= -745.0) return dd_from(0.0);
    if (a.hi >= 710.0)  return dd_from(INFINITY);
    if (a.hi == 0.0 && a.lo == 0.0) return dd_from(1.0);

    k = floor(a.hi / DD_LN2.hi + 0.5);
    r = dd_sub(a, dd_mul(dd_from(k), DD_LN2));
    r = dd_div(r, dd_from((double)(1 << halvings)));

    /* Taylor series of exp(r) - 1 */
    s = r;
    p = r;
    for (i = 2; i < 20; i++) {
        p = dd_div(dd_mul(p, r), dd_from((double)i));
        s = dd_add(s, p);
        if (fabs(p.hi) < 1e-40 * fabs(s.hi)) break;
    }
    /* undo halvings: (1+s)^2 repeatedly, written as s = 2s + s^2 */
    for (j = 0; j < halvings; j++)
        s = dd_add(dd_mul(dd_from(2.0), s), dd_mul(s, s));
    t = dd_add(s, dd_from(1.0));
    /* multiply by 2^k */
    return dd_mul(t, dd_from(ldexp(1.0, (int)k)));
}

dd dd_log(dd a)
{
    /* Newton on exp: x_{n+1} = x_n + a*exp(-x_n) - 1 */
    dd x, e;
    if (a.hi <= 0.0) return dd_from(a.hi == 0.0 ? -INFINITY : NAN);
    x = dd_from(log(a.hi));
    e = dd_mul(a, dd_exp(dd_neg(x)));
    x = dd_add(x, dd_sub(e, dd_from(1.0)));
    e = dd_mul(a, dd_exp(dd_neg(x)));
    x = dd_add(x, dd_sub(e, dd_from(1.0)));
    return x;
}

static dd dd_sin_small(dd r)
{
    /* |r| small: Taylor */
    dd s = r, p = r, r2 = dd_mul(r, r);
    int i;
    for (i = 3; i < 30; i += 2) {
        p = dd_div(dd_mul(p, r2), dd_from((double)(i * (i - 1))));
        p = dd_neg(p);
        s = dd_add(s, p);
        if (fabs(p.hi) < 1e-40 * (fabs(s.hi) + 1e-300)) break;
    }
    return s;
}

static dd dd_reduce_2pi(dd a)
{
    dd two_pi = dd_mul(dd_from(2.0), DD_PI);
    double k = floor(dd_to(dd_div(a, two_pi)) + 0.5);
    return dd_sub(a, dd_mul(dd_from(k), two_pi));
}

dd dd_sin(dd a)
{
    /* reduce mod 2pi, then triple-angle thrice so |x| <= pi/27 */
    dd x, s;
    int i;
    x = dd_reduce_2pi(a);
    x = dd_div(x, dd_from(27.0));
    s = dd_sin_small(x);
    for (i = 0; i < 3; i++)   /* sin(3u) = 3 sin u - 4 sin^3 u */
        s = dd_sub(dd_mul(dd_from(3.0), s),
                   dd_mul(dd_from(4.0), dd_mul(s, dd_mul(s, s))));
    return s;
}

dd dd_cos(dd a)
{
    return dd_sin(dd_sub(dd_div(DD_PI, dd_from(2.0)), a));
}

void dd_init_consts(void)
{
    DD_PHI = dd_div(dd_add(dd_from(1.0), dd_sqrt(dd_from(5.0))), dd_from(2.0));
}

/* decimal conversion: digit extraction in double-double */
void dd_str(dd a, int digits, char *out, size_t n)
{
    char buf[64];
    int e = 0, i, neg = 0, len = 0;
    dd x;
    if (digits < 1) digits = 1;
    if (digits > 34) digits = 34;
    if (isnan(a.hi)) { snprintf(out, n, "nan"); return; }
    if (isinf(a.hi)) { snprintf(out, n, a.hi > 0 ? "inf" : "-inf"); return; }
    if (a.hi == 0.0) { snprintf(out, n, "0.0"); return; }
    x = a;
    if (x.hi < 0) { neg = 1; x = dd_neg(x); }

    e = (int)floor(log10(x.hi));
    /* scale into [1,10) */
    if (e != 0) {
        dd p = dd_powi(dd_from(10.0), e < 0 ? -e : e);
        x = (e > 0) ? dd_div(x, p) : dd_mul(x, p);
    }
    /* guard against scaling error */
    if (dd_to(x) >= 10.0) { x = dd_div(x, dd_from(10.0)); e++; }
    if (dd_to(x) < 1.0)   { x = dd_mul(x, dd_from(10.0)); e--; }

    for (i = 0; i < digits && len < (int)sizeof(buf) - 2; i++) {
        int d = (int)x.hi;
        if (d < 0) d = 0;
        if (d > 9) d = 9;
        buf[len++] = (char)('0' + d);
        x = dd_mul(dd_sub(x, dd_from((double)d)), dd_from(10.0));
    }
    buf[len] = 0;
    /* round last digit */
    if (x.hi >= 5.0) {
        for (i = len - 1; i >= 0; i--) {
            if (buf[i] != '9') { buf[i]++; break; }
            buf[i] = '0';
            if (i == 0) { memmove(buf + 1, buf, (size_t)len + 1); buf[0] = '1'; e++; }
        }
    }
    snprintf(out, n, "%s%c.%se%+d", neg ? "-" : "", buf[0], buf + 1, e);
}

/* ================================================================== */
/* interval arithmetic (outward rounding by one ulp)                  */
/* ================================================================== */

static double dn(double x) { return nextafter(x, -INFINITY); }
static double up(double x) { return nextafter(x,  INFINITY); }

iv iv_mk(double lo, double hi) { iv r; r.lo = lo < hi ? lo : hi; r.hi = lo < hi ? hi : lo; return r; }
iv iv_point(double x) { return iv_mk(x, x); }
double iv_mid(iv a) { return 0.5 * (a.lo + a.hi); }
double iv_wid(iv a) { return up(a.hi - a.lo); }

iv iv_add(iv a, iv b) { return iv_mk(dn(a.lo + b.lo), up(a.hi + b.hi)); }
iv iv_sub(iv a, iv b) { return iv_mk(dn(a.lo - b.hi), up(a.hi - b.lo)); }
iv iv_neg(iv a)       { return iv_mk(-a.hi, -a.lo); }

iv iv_mul(iv a, iv b)
{
    double p[4], lo, hi;
    int i;
    p[0] = a.lo * b.lo; p[1] = a.lo * b.hi;
    p[2] = a.hi * b.lo; p[3] = a.hi * b.hi;
    lo = hi = p[0];
    for (i = 1; i < 4; i++) { if (p[i] < lo) lo = p[i]; if (p[i] > hi) hi = p[i]; }
    return iv_mk(dn(lo), up(hi));
}

iv iv_div(iv a, iv b)
{
    if (b.lo <= 0.0 && b.hi >= 0.0) return iv_mk(-INFINITY, INFINITY);
    return iv_mul(a, iv_mk(dn(1.0 / b.hi), up(1.0 / b.lo)));
}

iv iv_sqrt(iv a)
{
    if (a.hi < 0.0) return iv_mk(NAN, NAN);
    return iv_mk(dn(sqrt(a.lo < 0 ? 0 : a.lo)), up(sqrt(a.hi)));
}
iv iv_exp(iv a) { return iv_mk(dn(exp(a.lo)), up(exp(a.hi))); }
iv iv_log(iv a)
{
    if (a.hi <= 0.0) return iv_mk(NAN, NAN);
    return iv_mk(dn(log(a.lo <= 0 ? DBL_MIN : a.lo)), up(log(a.hi)));
}

iv iv_powi(iv a, int n)
{
    iv r = iv_point(1.0);
    int k = n < 0 ? -n : n, i;
    for (i = 0; i < k; i++) r = iv_mul(r, a);
    if (n < 0) return iv_div(iv_point(1.0), r);
    return r;
}

/* monotone-piece scan: sample the extremes plus any critical points */
static iv iv_trig(iv a, int is_cos)
{
    double lo, hi, k, x;
    double f0 = is_cos ? cos(a.lo) : sin(a.lo);
    double f1 = is_cos ? cos(a.hi) : sin(a.hi);
    lo = f0 < f1 ? f0 : f1;
    hi = f0 > f1 ? f0 : f1;
    if (iv_wid(a) >= 2.0 * M_PI) return iv_mk(-1.0, 1.0);
    /* critical points at x = (k + c) * pi */
    for (k = floor(a.lo / M_PI) - 1; k <= ceil(a.hi / M_PI) + 1; k += 1.0) {
        x = is_cos ? k * M_PI : (k + 0.5) * M_PI;
        if (x >= a.lo && x <= a.hi) {
            double f = is_cos ? cos(x) : sin(x);
            if (f < lo) lo = f;
            if (f > hi) hi = f;
        }
    }
    if (lo < -1.0) lo = -1.0;
    if (hi >  1.0) hi =  1.0;
    return iv_mk(dn(lo), up(hi));
}

iv iv_sin(iv a) { return iv_trig(a, 0); }
iv iv_cos(iv a) { return iv_trig(a, 1); }

/* ================================================================== */
/* IEEE-754 inspection                                                */
/* ================================================================== */

double cos_ulp(double x)
{
    double a = fabs(x);
    if (isnan(x) || isinf(x)) return NAN;
    return nextafter(a, INFINITY) - a;
}

void cos_bits(double x, char *out, size_t n)
{
    union { double d; uint64_t u; } u;
    uint64_t m;
    int s, e;
    u.d = x;
    s = (int)(u.u >> 63);
    e = (int)((u.u >> 52) & 0x7ff);
    m = u.u & 0xfffffffffffffULL;
    /* Three short lines: the console is 40 columns, and a caller that wants
     * one line folds the newlines back to spaces. */
    snprintf(out, n, "sign=%d exp=%d (%d)\nfrac=0x%013llx\n%a",
             s, e, e ? e - 1023 : -1022, (unsigned long long)m, x);
}

double cos_relerr(double a, double b)
{
    if (a == b) return 0.0;
    if (b == 0.0) return fabs(a);
    return fabs((a - b) / b);
}

double cos_ulpdiff(double a, double b)
{
    double u;
    if (a == b) return 0.0;
    if (isnan(a) || isnan(b)) return NAN;
    u = cos_ulp(fabs(a) > fabs(b) ? a : b);
    if (u == 0.0 || isnan(u)) return INFINITY;
    return (a - b) / u;
}

/* ================================================================== */
/* compensated primitives                                             */
/* ================================================================== */

double cos_nsum(const double *v, int n)
{
    /* Neumaier / Kahan-Babuska compensated summation */
    double s = 0.0, c = 0.0, t;
    int i;
    for (i = 0; i < n; i++) {
        t = s + v[i];
        if (fabs(s) >= fabs(v[i])) c += (s - t) + v[i];
        else                       c += (v[i] - t) + s;
        s = t;
    }
    return s + c;
}

double cos_ndot(const double *a, const double *b, int n)
{
    /* exact products accumulated in double-double */
    dd acc = dd_from(0.0);
    int i;
    for (i = 0; i < n; i++) {
        double e, p = two_prod(a[i], b[i], &e);
        acc = dd_add(acc, dd_mk(p, e));
    }
    return dd_to(acc);
}

double cos_horner(const double *p, int n, double x)
{
    double r;
    int i;
    if (n <= 0) return 0.0;
    r = p[0];
    for (i = 1; i < n; i++) r = fma(r, x, p[i]);
    return r;
}

/* ================================================================== */
/* special functions                                                  */
/* ================================================================== */

static const double lanczos_g = 7.0;
static const double lanczos_c[9] = {
    0.99999999999980993, 676.5203681218851, -1259.1392167224028,
    771.32342877765313, -176.61502916214059, 12.507343278686905,
    -0.13857109526572012, 9.9843695780195716e-6, 1.5056327351493116e-7
};

double cos_gamma(double x)
{
    double a, t, y;
    int i;
    if (isnan(x)) return x;
    if (x == floor(x) && x <= 0.0) return NAN;
    if (x < 0.5)
        return M_PI / (sin(M_PI * x) * cos_gamma(1.0 - x));
    x -= 1.0;
    a = lanczos_c[0];
    t = x + lanczos_g + 0.5;
    for (i = 1; i < 9; i++) a += lanczos_c[i] / (x + (double)i);
    y = sqrt(2.0 * M_PI) * pow(t, x + 0.5) * exp(-t) * a;
    return y;
}

double cos_lgamma(double x)
{
    double a, t;
    int i;
    if (x < 0.5)
        return log(M_PI / fabs(sin(M_PI * x))) - cos_lgamma(1.0 - x);
    x -= 1.0;
    a = lanczos_c[0];
    t = x + lanczos_g + 0.5;
    for (i = 1; i < 9; i++) a += lanczos_c[i] / (x + (double)i);
    return 0.5 * log(2.0 * M_PI) + (x + 0.5) * log(t) - t + log(a);
}

double cos_digamma(double x)
{
    double r = 0.0, f, x2;
    if (x <= 0.0 && x == floor(x)) return NAN;
    if (x < 0.0)   /* reflection */
        return cos_digamma(1.0 - x) - M_PI / tan(M_PI * x);
    while (x < 20.0) { r -= 1.0 / x; x += 1.0; }
    f = 1.0 / (x * x);
    x2 = log(x) - 0.5 / x;
    /* asymptotic series with Bernoulli numbers B2..B10 */
    x2 -= f * (1.0 / 12.0 - f * (1.0 / 120.0 - f * (1.0 / 252.0
          - f * (1.0 / 240.0 - f * (1.0 / 132.0)))));
    return r + x2;
}

double cos_erf(double x)
{
    double t, s, term;
    int i;
    if (fabs(x) > 4.0) return x > 0 ? 1.0 - cos_erfc(x) : cos_erfc(-x) - 1.0;
    /* Maclaurin series: 2/sqrt(pi) * sum (-1)^n x^(2n+1) / (n! (2n+1)) */
    s = term = x;
    for (i = 1; i < 120; i++) {
        term *= -x * x / (double)i;
        t = term / (double)(2 * i + 1);
        s += t;
        if (fabs(t) < 1e-18 * fabs(s)) break;
    }
    return 2.0 / sqrt(M_PI) * s;
}

double cos_erfc(double x)
{
    /* continued fraction (Lentz) for x > 0; reflection otherwise */
    double f, c, d, an, b, del;
    int i;
    if (x < 0.0) return 2.0 - cos_erfc(-x);
    if (x < 4.0) return 1.0 - cos_erf(x);
    b = x * x + 0.5;
    c = 1e300;
    d = 1.0 / b;
    f = d;
    for (i = 1; i < 300; i++) {
        an = -(double)i * ((double)i - 0.5);
        b += 2.0;
        d = an * d + b; if (fabs(d) < 1e-300) d = 1e-300;
        c = b + an / c; if (fabs(c) < 1e-300) c = 1e-300;
        d = 1.0 / d;
        del = d * c;
        f *= del;
        if (fabs(del - 1.0) < 1e-17) break;
    }
    return exp(-x * x) * x / sqrt(M_PI) * f;
}

double cos_beta(double a, double b)
{
    return exp(cos_lgamma(a) + cos_lgamma(b) - cos_lgamma(a + b));
}

double cos_zeta(double s)
{
    /* Euler-Maclaurin:
     *   zeta(s) = sum_{k<N} k^-s + N^-s/2 + N^(1-s)/(s-1)
     *           + sum_j B_2j/(2j)! * (s)_(2j-1) * N^(-s-2j+1)
     * with the functional equation used for s < 1/2.
     */
    const int N = 24, M = 9;
    static const double b2k[] = {
        1.0 / 6, -1.0 / 30, 1.0 / 42, -1.0 / 30, 5.0 / 66,
        -691.0 / 2730, 7.0 / 6, -3617.0 / 510, 43867.0 / 798
    };
    double sum = 0.0, nps, rising, fact, npow;
    int k, j, i;

    if (s == 1.0) return INFINITY;
    if (s < 0.5)
        return pow(2.0, s) * pow(M_PI, s - 1.0) * sin(M_PI * s / 2.0)
             * cos_gamma(1.0 - s) * cos_zeta(1.0 - s);

    for (k = 1; k < N; k++) sum += pow((double)k, -s);
    nps = pow((double)N, -s);
    sum += 0.5 * nps + nps * (double)N / (s - 1.0);

    for (j = 1; j <= M; j++) {
        rising = 1.0;
        for (i = 0; i <= 2 * j - 2; i++) rising *= (s + (double)i);
        fact = 1.0;
        for (i = 1; i <= 2 * j; i++) fact *= (double)i;
        npow = pow((double)N, -s - 2.0 * j + 1.0);
        {
            double term = b2k[j - 1] / fact * rising * npow;
            sum += term;
            if (fabs(term) < 1e-18 * fabs(sum)) break;
        }
    }
    return sum;
}

/* Bessel functions.
 *
 * The ascending power series is mathematically exact but catastrophically
 * cancelling: at x = 16 the largest term is ~1e12 while J0 is ~0.1, so a
 * double-precision evaluation loses ~13 digits.  CalcOS evaluates the series
 * in double-double instead, which leaves ~19 good digits at the crossover,
 * and switches to the Hankel asymptotic expansion (with optimal truncation)
 * beyond it, where that expansion is good to better than 1e-16.
 */
#define BESSEL_XSWITCH 18.0

static void bessel_asym(double x, int nu, double *p_out, double *q_out)
{
    double mu = 4.0 * (double)nu * (double)nu, z = 8.0 * x;
    double term = 1.0, p = 1.0, q = 0.0, prev = 1e300;
    int k;
    for (k = 1; k <= 20; k++) {
        double f = (mu - (2.0 * k - 1.0) * (2.0 * k - 1.0));
        term *= f / ((double)k * z);
        if (fabs(term) > prev) break;      /* optimal truncation */
        prev = fabs(term);
        switch (k & 3) {
        case 0: p += term; break;
        case 1: q += term; break;
        case 2: p -= term; break;
        default: q -= term; break;
        }
        if (prev < 1e-19) break;
    }
    *p_out = p;
    *q_out = q;
}

/* series for J_nu(x) evaluated in double-double */
static double bessel_series(double x, int nu)
{
    dd term, sum, h, xx;
    int k;
    xx = dd_from(x);
    h = dd_div(dd_mul(xx, xx), dd_from(4.0));
    term = dd_from(1.0);
    for (k = 1; k <= nu; k++)
        term = dd_div(dd_mul(term, dd_div(xx, dd_from(2.0))), dd_from((double)k));
    sum = term;
    for (k = 1; k < 400; k++) {
        term = dd_div(dd_mul(term, dd_neg(h)),
                      dd_from((double)k * (double)(k + nu)));
        sum = dd_add(sum, term);
        if (fabs(term.hi) < 1e-34 * fabs(sum.hi)) break;
    }
    return dd_to(sum);
}

double cos_j0(double x)
{
    double p, q, chi;
    x = fabs(x);
    if (x < BESSEL_XSWITCH) return bessel_series(x, 0);
    bessel_asym(x, 0, &p, &q);
    chi = x - 0.25 * M_PI;
    return sqrt(2.0 / (M_PI * x)) * (p * cos(chi) - q * sin(chi));
}

double cos_j1(double x)
{
    double p, q, chi, s = x < 0 ? -1.0 : 1.0;
    x = fabs(x);
    if (x < BESSEL_XSWITCH) return s * bessel_series(x, 1);
    bessel_asym(x, 1, &p, &q);
    chi = x - 0.75 * M_PI;
    return s * sqrt(2.0 / (M_PI * x)) * (p * cos(chi) - q * sin(chi));
}

#define EUL_GAMMA 0.57721566490153286061

double cos_y0(double x)
{
    /* A&S 9.1.13 with the series taken in double-double, Hankel beyond. */
    dd sum, term, h, hk;
    int k;
    if (x <= 0.0) return NAN;
    if (x >= BESSEL_XSWITCH) {
        double p, q, chi = x - 0.25 * M_PI;
        bessel_asym(x, 0, &p, &q);
        return sqrt(2.0 / (M_PI * x)) * (p * sin(chi) + q * cos(chi));
    }
    h = dd_div(dd_mul(dd_from(x), dd_from(x)), dd_from(4.0));
    term = dd_from(1.0);
    sum = dd_from(0.0);
    hk = dd_from(0.0);
    for (k = 1; k < 400; k++) {
        term = dd_div(dd_mul(term, dd_neg(h)), dd_from((double)k * (double)k));
        hk = dd_add(hk, dd_div(dd_from(1.0), dd_from((double)k)));
        sum = dd_sub(sum, dd_mul(term, hk));
        if (fabs(term.hi * hk.hi) < 1e-34 * (fabs(sum.hi) + 1e-300)) break;
    }
    {
        dd j0 = dd_from(bessel_series(x, 0));
        dd lg = dd_add(dd_log(dd_div(dd_from(x), dd_from(2.0))),
                       dd_from(EUL_GAMMA));
        dd r  = dd_add(dd_mul(lg, j0), sum);
        return dd_to(dd_mul(dd_div(dd_from(2.0), DD_PI), r));
    }
}

double cos_y1(double x)
{
    /* A&S 9.1.11: Y1 = 2/pi ln(x/2) J1 - 2/(pi x)
     *                  - x/(2 pi) sum (psi(k+1)+psi(k+2)) (-x^2/4)^k/(k!(k+1)!)
     */
    dd sum, term, h, psi, j1, lg, a, b, c;
    int k;
    if (x <= 0.0) return NAN;
    if (x >= BESSEL_XSWITCH) {
        double p, q, chi = x - 0.75 * M_PI;
        bessel_asym(x, 1, &p, &q);
        return sqrt(2.0 / (M_PI * x)) * (p * sin(chi) + q * cos(chi));
    }
    h = dd_div(dd_mul(dd_from(x), dd_from(x)), dd_from(4.0));
    term = dd_from(1.0);                      /* (-x^2/4)^k / (k!(k+1)!) */
    psi = dd_from(-2.0 * EUL_GAMMA + 1.0);    /* psi(1)+psi(2) = -2g + 1 */
    sum = dd_mul(term, psi);
    for (k = 1; k < 400; k++) {
        term = dd_div(dd_mul(term, dd_neg(h)),
                      dd_from((double)k * (double)(k + 1)));
        /* psi(k+1)+psi(k+2) = -2g + H_k + H_{k+1} */
        psi = dd_add(psi, dd_add(dd_div(dd_from(1.0), dd_from((double)k)),
                                 dd_div(dd_from(1.0), dd_from((double)(k + 1)))));
        sum = dd_add(sum, dd_mul(term, psi));
        if (fabs(term.hi * psi.hi) < 1e-34 * (fabs(sum.hi) + 1e-300)) break;
    }
    j1 = dd_from(bessel_series(x, 1));
    lg = dd_log(dd_div(dd_from(x), dd_from(2.0)));
    a = dd_mul(dd_div(dd_from(2.0), DD_PI), dd_mul(lg, j1));
    b = dd_div(dd_from(2.0), dd_mul(DD_PI, dd_from(x)));
    c = dd_mul(dd_div(dd_from(x), dd_mul(dd_from(2.0), DD_PI)), sum);
    return dd_to(dd_sub(dd_sub(a, b), c));
}

double cos_ei(double x)
{
    const double EUL = 0.57721566490153286061;
    double sum, term;
    int k;
    if (x == 0.0) return -INFINITY;
    if (x < -40.0) return -exp(x) / x * (1.0 - 1.0 / x + 2.0 / (x * x));
    if (fabs(x) <= 40.0) {
        sum = EUL + log(fabs(x));
        term = 1.0;
        for (k = 1; k < 300; k++) {
            term *= x / (double)k;
            sum += term / (double)k;
            if (fabs(term / (double)k) < 1e-18 * fabs(sum)) break;
        }
        return sum;
    }
    /* large positive x: asymptotic */
    sum = 1.0;
    term = 1.0;
    for (k = 1; k < 40; k++) {
        double prev = fabs(term);
        term *= (double)k / x;
        if (fabs(term) > prev) break;
        sum += term;
    }
    return exp(x) / x * sum;
}

double cos_li(double x)
{
    if (x <= 0.0) return NAN;
    if (x == 1.0) return -INFINITY;
    return cos_ei(log(x));
}

/* ================================================================== */
/* xoshiro256++ PRNG                                                  */
/* ================================================================== */

static uint64_t rng_s[4] = {
    0x853c49e6748fea9bULL, 0xda3e39cb94b95bdbULL,
    0x9e3779b97f4a7c15ULL, 0xbf58476d1ce4e5b9ULL
};

static uint64_t rotl64(uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }

void cos_srand(uint64_t seed)
{
    int i;
    for (i = 0; i < 4; i++) {
        seed += 0x9e3779b97f4a7c15ULL;
        uint64_t z = seed;
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
        rng_s[i] = z ^ (z >> 31);
    }
}

double cos_prng(void)
{
    uint64_t r = rotl64(rng_s[0] + rng_s[3], 23) + rng_s[0];
    uint64_t t = rng_s[1] << 17;
    rng_s[2] ^= rng_s[0];
    rng_s[3] ^= rng_s[1];
    rng_s[1] ^= rng_s[2];
    rng_s[0] ^= rng_s[3];
    rng_s[2] ^= t;
    rng_s[3] = rotl64(rng_s[3], 45);
    return (double)(r >> 11) * 0x1.0p-53;
}
