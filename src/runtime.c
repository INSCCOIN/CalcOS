/* The runtime that compiled C programs link against, and the bridge that
 * lets the calculator call into them.
 *
 * Compiled code is position-dependent machine code placed in RAM: every call
 * to a runtime entry point is a movw/movt of that function's real address
 * followed by blx, so the table below is the whole linker.
 */
#include "calc.h"
#include "cprog.h"
#include "store.h"
#include "bignum.h"
#include "cc.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

/* ------------------------------------------------------------------ */
/* console and formatting                                              */
/* ------------------------------------------------------------------ */

static int rt_printf(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    plat_puts(buf);
    return n;
}

static int rt_putchar(int c) { plat_putc((char)c); return c; }
static int rt_puts(const char *s) { plat_puts(s); plat_puts("\n"); return 0; }

/* ------------------------------------------------------------------ */
/* plotting                                                            */
/* ------------------------------------------------------------------ */

/* A program draws into the console's graphics overlay. Coordinates are
 * pixels with the origin at the top left of the 320x320 panel. */
static void maybe_show(void);

static void rt_plot_clear(void)                 { plat_gfx_clear(); }
static void rt_plot_point(int x, int y, int c)  { plat_gfx_point(x, y, (unsigned)c); maybe_show(); }
static int  rt_plot_get(int x, int y)           { return plat_gfx_get(x, y); }
static void rt_plot_line(int x0, int y0, int x1, int y1, int c)
{ plat_gfx_line(x0, y0, x1, y1, (unsigned)c); maybe_show(); }
static void rt_plot_rect(int x, int y, int w, int h, int c)
{ plat_gfx_rect(x, y, w, h, (unsigned)c); maybe_show(); }
static void rt_plot_fill_rect(int x, int y, int w, int h, int c)
{ plat_gfx_fill_rect(x, y, w, h, (unsigned)c); maybe_show(); }
static void rt_plot_circle(int x, int y, int r, int c)
{ plat_gfx_circle(x, y, r, (unsigned)c); maybe_show(); }
static void rt_plot_fill_circle(int x, int y, int r, int c)
{ plat_gfx_fill_circle(x, y, r, (unsigned)c); maybe_show(); }
static void rt_plot_clip(int x, int y, int w, int h) { plat_gfx_clip(x, y, w, h); }
static void rt_plot_noclip(void) { plat_gfx_noclip(); }

/* A program's own palette, so sprites and images mean the same colours
 * whatever order things were drawn in. */
static unsigned rt_pal[16] = {
    0x000000u, 0xFFFFFFu, 0xE02020u, 0x20C040u, 0x3060E0u, 0xE0E020u,
    0xE020E0u, 0x20E0E0u, 0x808080u, 0x404040u, 0xC08020u, 0x8020C0u,
    0x20A080u, 0xA02040u, 0x60A0E0u, 0xC0C0C0u
};

static void rt_plot_palette(int i, int rgb)
{
    if (i >= 0 && i < 16) rt_pal[i] = (unsigned)rgb & 0xFFFFFFu;
}

static void rt_plot_sprite(int x, int y, int w, int h, const void *d)
{ plat_gfx_sprite(x, y, w, h, d, rt_pal, 16); maybe_show(); }

static void rt_plot_blit(int x, int y, int w, int h, const void *d)
{ plat_gfx_blit(x, y, w, h, d, rt_pal, 16); maybe_show(); }

/* Draw an image object kept in the flash store, by name. It is read in
 * place, so a picture costs no RAM however large it is. */
static int rt_plot_image(int x, int y, const char *name)
{
    int len = 0, kind = 0;
    const void *d = store_find(name, &len, &kind);
    if (!d) return 0;
    return plat_gfx_image(x, y, d, len);
}

static int rt_plot_image_w(const char *name)
{
    int len = 0, kind = 0, w = 0, h = 0;
    const void *d = store_find(name, &len, &kind);
    if (!d || !plat_gfx_image_size(d, len, &w, &h)) return 0;
    return w;
}

static int rt_plot_image_h(const char *name)
{
    int len = 0, kind = 0, w = 0, h = 0;
    const void *d = store_find(name, &len, &kind);
    if (!d || !plat_gfx_image_size(d, len, &w, &h)) return 0;
    return h;
}

/* When on, every drawing call presents the frame. Convenient while working
 * something out; far slower than drawing a whole frame and calling
 * plot_show once, which is what the overlay is for. */
static int rt_auto_show;
static void rt_plot_auto(int on) { rt_auto_show = on ? 1 : 0; }
static void maybe_show(void) { if (rt_auto_show) plat_gfx_flush(); }

static void rt_plot_text(int x, int y, const char *s, int c)
{ plat_gfx_text(x, y, s, (unsigned)c); maybe_show(); }
static void rt_plot_flush(void)                 { plat_gfx_flush(); }
static void rt_plot_hide(void)                  { plat_gfx_hide(); }
static int  rt_plot_width(void)                 { return plat_gfx_width(); }
static int  rt_plot_height(void)                { return plat_gfx_height(); }
static int  rt_rgb(int r, int g, int b)
{
    if (r < 0) r = 0;
    if (r > 255) r = 255;
    if (g < 0) g = 0;
    if (g > 255) g = 255;
    if (b < 0) b = 0;
    if (b > 255) b = 255;
    return (r << 16) | (g << 8) | b;
}

/* ------------------------------------------------------------------ */
/* keyboard, time and sound                                            */
/* ------------------------------------------------------------------ */

/* getchar() blocks for a key, the way a program expects; kbhit() answers
 * without taking one, so a game loop can keep running. getkey() is the
 * non-blocking form, returning -1 when nothing is waiting. */
static int rt_getchar(void)   { return plat_getc(); }
static int rt_getkey(void)    { return plat_getkey(); }
static int rt_kbhit(void)     { return plat_key_ready(); }

static int rt_getline(char *buf, int cap)
{
    if (cap <= 0) return 0;
    return plat_readline(buf, (size_t)cap, NULL);
}

static int      rt_millis(void) { return (int)plat_millis(); }
static unsigned rt_micros(void) { return (unsigned)plat_micros(); }
static void     rt_delay(int ms) { plat_delay_ms(ms); }
static void     rt_tone(int hz, int ms) { plat_tone(hz, ms); }
static void     rt_notone(void) { plat_tone_off(); }

/* ------------------------------------------------------------------ */
/* files                                                               */
/* ------------------------------------------------------------------ */

/* A program's files live on the card and survive it; everything the program
 * allocates does not, which is what makes these the way to keep data. */
static void *rt_fopen(const char *path, const char *mode)
{ return plat_fopen(path, mode); }
static int  rt_fclose(void *f) { plat_fclose(f); return 0; }
static int  rt_fread(void *buf, int size, int n, void *f)
{
    int want = size * n, got;
    if (want <= 0) return 0;
    got = plat_fread(f, buf, want);
    return size ? got / size : 0;
}
static int  rt_fwrite(const void *buf, int size, int n, void *f)
{
    int want = size * n, put;
    if (want <= 0) return 0;
    put = plat_fwrite(f, buf, want);
    return size ? put / size : 0;
}
static int  rt_fgets_(char *buf, int cap, void *f)
{ return plat_fgets(f, buf, (size_t)cap); }
static int  rt_fputs_(const char *s, void *f) { return plat_fputs(f, s); }
static int  rt_fseek(void *f, int off, int whence) { return plat_fseek(f, off, whence); }
static int  rt_ftell(void *f) { return (int)plat_ftell(f); }
static int  rt_fsize(void *f) { return (int)plat_fsize(f); }
static int  rt_feof(void *f) { return plat_feof(f); }
static int  rt_remove(const char *p) { return plat_remove(p) ? 0 : -1; }

/* ------------------------------------------------------------------ */
/* math shims, so the names compiled code uses are stable              */
/* ------------------------------------------------------------------ */

static double rt_gamma(double x)  { return cos_gamma(x); }
static double rt_lgamma(double x) { return cos_lgamma(x); }
static double rt_erf(double x)    { return cos_erf(x); }
static double rt_erfc(double x)   { return cos_erfc(x); }
static double rt_zeta(double x)   { return cos_zeta(x); }
static double rt_j0(double x)     { return cos_j0(x); }
static double rt_j1(double x)     { return cos_j1(x); }
static double rt_digamma(double x){ return cos_digamma(x); }

/* AEABI soft-double helpers live in libgcc on the target; the compiler emits
 * direct calls to their addresses. On a host build they do not exist, so
 * stand-ins are provided purely so the table can be built: compiled Thumb-2
 * is never executed on the host. */
#if !defined(__arm__) && !defined(__thumb__)
double __aeabi_dadd(double a, double b) { return a + b; }
double __aeabi_dsub(double a, double b) { return a - b; }
double __aeabi_dmul(double a, double b) { return a * b; }
double __aeabi_ddiv(double a, double b) { return a / b; }
double __aeabi_i2d(int a) { return (double)a; }
double __aeabi_ui2d(unsigned a) { return (double)a; }
int    __aeabi_d2iz(double a) { return (int)a; }
double __aeabi_f2d(float a) { return (double)a; }
float  __aeabi_d2f(double a) { return (float)a; }
int    __aeabi_dcmplt(double a, double b) { return a < b; }
int    __aeabi_dcmple(double a, double b) { return a <= b; }
int    __aeabi_dcmpgt(double a, double b) { return a > b; }
int    __aeabi_dcmpge(double a, double b) { return a >= b; }
int    __aeabi_dcmpeq(double a, double b) { return a == b; }
#endif

extern double __aeabi_dadd(double, double);
extern double __aeabi_dsub(double, double);
extern double __aeabi_dmul(double, double);
extern double __aeabi_ddiv(double, double);
extern double __aeabi_i2d(int);
extern double __aeabi_ui2d(unsigned);
extern int    __aeabi_d2iz(double);
extern double __aeabi_f2d(float);
extern float  __aeabi_d2f(double);
extern int    __aeabi_dcmplt(double, double);
extern int    __aeabi_dcmple(double, double);
extern int    __aeabi_dcmpgt(double, double);
extern int    __aeabi_dcmpge(double, double);
extern int    __aeabi_dcmpeq(double, double);

/* ------------------------------------------------------------------ */
/* numeric helpers, for compiled programs                              */
/* ------------------------------------------------------------------ */

/* The transforms the prompt has, on plain arrays, so a C program does not
 * have to carry its own copy. These are the firmware's, not a library in
 * the store: they are small, everyone wants them, and keeping them here
 * means they cost a program nothing to use. */

/* In-place radix-2 FFT. re and im are n doubles each, n a power of two;
 * dir is -1 for the forward transform and +1 for the inverse (which is
 * scaled by 1/n). Returns 0 if n is not a power of two. */
static int rt_fft(double *re, double *im, int n, int dir)
{
    int i, j, len;
    if (n < 2 || (n & (n - 1)) || !re || !im) return 0;
    for (i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            double t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    for (len = 2; len <= n; len <<= 1) {
        double ang = 2.0 * 3.14159265358979323846 / (double)len
                   * (dir >= 0 ? 1.0 : -1.0);
        for (i = 0; i < n; i += len) {
            for (j = 0; j < len / 2; j++) {
                double th = ang * (double)j;
                double wr = cos(th), wi = sin(th);
                double ur = re[i + j], ui = im[i + j];
                double vr = re[i + j + len / 2] * wr - im[i + j + len / 2] * wi;
                double vi = re[i + j + len / 2] * wi + im[i + j + len / 2] * wr;
                re[i + j] = ur + vr;
                im[i + j] = ui + vi;
                re[i + j + len / 2] = ur - vr;
                im[i + j + len / 2] = ui - vi;
            }
        }
    }
    if (dir >= 0) {
        for (i = 0; i < n; i++) { re[i] /= n; im[i] /= n; }
    }
    return 1;
}

/* Discrete convolution of two arrays into out, which must hold na+nb-1. */
static void rt_convolve(const double *a, int na, const double *b, int nb,
                        double *out)
{
    int i, j;
    if (!a || !b || !out || na < 1 || nb < 1) return;
    for (i = 0; i < na + nb - 1; i++) out[i] = 0.0;
    for (i = 0; i < na; i++)
        for (j = 0; j < nb; j++)
            out[i + j] += a[i] * b[j];
}

/* Solve A x = b for an n by n matrix held row-major, by Gaussian
 * elimination with partial pivoting. A and b are overwritten; the answer
 * is left in b. Returns 0 if the matrix is singular. */
static int rt_solve(double *A, double *b, int n)
{
    int i, j, k;
    if (!A || !b || n < 1) return 0;
    for (k = 0; k < n; k++) {
        int piv = k;
        double best = fabs(A[k * n + k]);
        for (i = k + 1; i < n; i++) {
            double v = fabs(A[i * n + k]);
            if (v > best) { best = v; piv = i; }
        }
        if (best == 0.0) return 0;
        if (piv != k) {
            for (j = 0; j < n; j++) {
                double t = A[k * n + j]; A[k * n + j] = A[piv * n + j];
                A[piv * n + j] = t;
            }
            { double t = b[k]; b[k] = b[piv]; b[piv] = t; }
        }
        for (i = k + 1; i < n; i++) {
            double f = A[i * n + k] / A[k * n + k];
            if (f == 0.0) continue;
            for (j = k; j < n; j++) A[i * n + j] -= f * A[k * n + j];
            b[i] -= f * b[k];
        }
    }
    for (i = n - 1; i >= 0; i--) {
        double sum = b[i];
        for (j = i + 1; j < n; j++) sum -= A[i * n + j] * b[j];
        b[i] = sum / A[i * n + i];
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* the second core                                                     */
/* ------------------------------------------------------------------ */

/* par_for(n, fn, ctx) runs fn(i, ctx) for every i below n across both
 * cores. The body must not depend on the other indices and must not write
 * to the flash store — core 1 runs from flash, and a store write turns the
 * XIP window off underneath it. */
static void rt_par_for(int n, void (*fn)(int, void *), void *ctx)
{
    plat_par_for(n, fn, ctx);
}

static int rt_cores(void) { return plat_par_cores(); }

/* ------------------------------------------------------------------ */
/* big integers, for compiled programs                                 */
/* ------------------------------------------------------------------ */

/* The same arbitrary-precision integers the prompt uses, as handles a C
 * program can hold. A program owns every handle it makes and frees it with
 * bigfree; nothing here is garbage collected, because a program that runs
 * for a second should not pay for a collector it cannot see. */
static void *rt_bignew(int v)              { return big_from_ll(v); }
static void *rt_bigparse(const char *s)    { return big_parse(s); }
static void *rt_bigcopy(void *a)           { return big_copy((Big *)a); }
static void  rt_bigfree(void *a)           { big_free((Big *)a); }
static void *rt_bigadd(void *a, void *b)   { return big_add((Big *)a, (Big *)b); }
static void *rt_bigsub(void *a, void *b)   { return big_sub((Big *)a, (Big *)b); }
static void *rt_bigmul(void *a, void *b)   { return big_mul((Big *)a, (Big *)b); }
static void *rt_bigdiv(void *a, void *b)   { return big_div((Big *)a, (Big *)b); }
static void *rt_bigmod(void *a, void *b)   { return big_mod((Big *)a, (Big *)b); }
static void *rt_bigpow(void *a, int e)     { return big_pow((Big *)a, e); }
static void *rt_bigpowmod(void *a, void *b, void *m)
{ return big_powmod((Big *)a, (Big *)b, (Big *)m); }
static void *rt_biggcd(void *a, void *b)   { return big_gcd((Big *)a, (Big *)b); }
static void *rt_bigfact(int n)             { return big_fact(n); }
static int   rt_bigcmp(void *a, void *b)   { return big_cmp((Big *)a, (Big *)b); }
static int   rt_bigstr(void *a, char *o, int cap)
{ return big_str((Big *)a, o, cap); }
static int   rt_bigdigits(void *a)         { return big_digits((Big *)a); }
static double rt_bigdbl(void *a)           { return big_to_double((Big *)a); }

/* ------------------------------------------------------------------ */
/* the flash store                                                     */
/* ------------------------------------------------------------------ */

/* store_read gives a program a pointer straight into memory-mapped flash:
 * a table kept there occupies no RAM at all, which is the point of it on
 * a part with 520 KB. store_write erases a page, so it is for saving
 * between runs, not for working storage. */
static int   rt_store_pages(void)     { return plat_store_pages(); }
static int   rt_store_page_size(void) { return plat_store_page_size(); }
static void *rt_store_read(int page)  { return (void *)plat_store_read(page); }
/* Writing flash turns the XIP window off, so core 1 must not be running
 * code from flash at the time. A parallel loop is the only way it can be,
 * and refusing is far better than the hard fault. */
static int   rt_store_write(int page, const void *d, int len)
{
    if (plat_par_active()) return 0;
    return plat_store_write(page, d, len);
}
static int   rt_store_erase(int page)
{
    if (plat_par_active()) return 0;
    return plat_store_erase(page);
}

/* ------------------------------------------------------------------ */
/* 64-bit integers                                                     */
/* ------------------------------------------------------------------ */

/* The compiler keeps a long long in r0:r1 and passes it in an even-aligned
 * register pair, exactly as it does a double, so every one of these takes
 * and returns its operands in the places the generated code already uses.
 * They are written in C and compiled by GCC rather than emitted inline:
 * 64-bit multiply, divide and shift are easy to get subtly wrong, and a
 * wrong answer here would be silent. */
typedef long long          ll;
typedef unsigned long long ull;

static ll  rt_ladd(ll a, ll b)  { return (ll)((ull)a + (ull)b); }
static ll  rt_lsub(ll a, ll b)  { return (ll)((ull)a - (ull)b); }
static ll  rt_lmul(ll a, ll b)  { return (ll)((ull)a * (ull)b); }
static ll  rt_ldiv(ll a, ll b)  { return b ? a / b : 0; }
static ll  rt_lmod(ll a, ll b)  { return b ? a % b : 0; }
static ull rt_uldiv(ull a, ull b) { return b ? a / b : 0; }
static ull rt_ulmod(ull a, ull b) { return b ? a % b : 0; }
static ll  rt_land(ll a, ll b)  { return a & b; }
static ll  rt_lor(ll a, ll b)   { return a | b; }
static ll  rt_lxor(ll a, ll b)  { return a ^ b; }
static ll  rt_lshl(ll a, ll b)  { int n = (int)(b & 63); return (ll)((ull)a << n); }
static ll  rt_lshr(ll a, ll b)  { int n = (int)(b & 63); return a >> n; }
static ull rt_lushr(ull a, ull b) { int n = (int)(b & 63); return a >> n; }
static int rt_lcmp(ll a, ll b)  { return a < b ? -1 : a > b ? 1 : 0; }
static int rt_ulcmp(ull a, ull b) { return a < b ? -1 : a > b ? 1 : 0; }
static ll  rt_i2l(int a)        { return (ll)a; }
static ll  rt_u2l(unsigned a)   { return (ll)(ull)a; }
static double rt_l2d(ll a)      { return (double)a; }
static double rt_ul2d(ull a)    { return (double)(ull)a; }
static ll  rt_d2l(double a)     { return (ll)a; }

#define RT(name, fn, decl) { name, (void *)(fn), decl }

static const RtSym rt_table[] = {
    /* soft double */
    RT("__aeabi_dadd", __aeabi_dadd, "ddd"),
    RT("__aeabi_dsub", __aeabi_dsub, "ddd"),
    RT("__aeabi_dmul", __aeabi_dmul, "ddd"),
    RT("__aeabi_ddiv", __aeabi_ddiv, "ddd"),
    RT("__aeabi_i2d", __aeabi_i2d, "di"),
    RT("__aeabi_ui2d", __aeabi_ui2d, "di"),
    RT("__aeabi_d2iz", __aeabi_d2iz, "id"),
    RT("__aeabi_f2d", __aeabi_f2d, "di"),
    RT("__aeabi_d2f", __aeabi_d2f, "id"),
    RT("__aeabi_dcmplt", __aeabi_dcmplt, "idd"),
    RT("__aeabi_dcmple", __aeabi_dcmple, "idd"),
    RT("__aeabi_dcmpgt", __aeabi_dcmpgt, "idd"),
    RT("__aeabi_dcmpge", __aeabi_dcmpge, "idd"),
    RT("__aeabi_dcmpeq", __aeabi_dcmpeq, "idd"),

    /* console */
    RT("printf", rt_printf, "ip."),
    RT("putchar", rt_putchar, "ii"),
    RT("puts", rt_puts, "ip"),

    /* libm */
    RT("sin", sin, "dd"),     RT("cos", cos, "dd"),
    RT("tan", tan, "dd"),     RT("asin", asin, "dd"),
    RT("acos", acos, "dd"),   RT("atan", atan, "dd"),
    RT("atan2", atan2, "ddd"), RT("sinh", sinh, "dd"),
    RT("cosh", cosh, "dd"),   RT("tanh", tanh, "dd"),
    RT("exp", exp, "dd"),     RT("log", log, "dd"),
    RT("log10", log10, "dd"), RT("log2", log2, "dd"),
    RT("pow", pow, "ddd"),     RT("sqrt", sqrt, "dd"),
    RT("cbrt", cbrt, "dd"),   RT("fabs", fabs, "dd"),
    RT("floor", floor, "dd"), RT("ceil", ceil, "dd"),
    RT("round", round, "dd"), RT("trunc", trunc, "dd"),
    RT("fmod", fmod, "ddd"),   RT("hypot", hypot, "ddd"),
    RT("fmin", fmin, "ddd"),   RT("fmax", fmax, "ddd"),

    /* the calculator's own special functions */
    RT("gamma_fn", rt_gamma, "dd"),   RT("lgamma_fn", rt_lgamma, "dd"),
    RT("erf_fn", rt_erf, "dd"),       RT("erfc_fn", rt_erfc, "dd"),
    RT("zeta_fn", rt_zeta, "dd"),     RT("digamma_fn", rt_digamma, "dd"),
    RT("besselj0_fn", rt_j0, "dd"),   RT("besselj1_fn", rt_j1, "dd"),

    /* memory and strings */
    RT("malloc", malloc, "pi"),  RT("free", free, "vp"),
    RT("memset", memset, "ppii"),  RT("memcpy", memcpy, "pppi"),
    RT("strlen", strlen, "ip"),  RT("strcmp", strcmp, "ipp"),
    RT("strcpy", strcpy, "ppp"),  RT("strncpy", strncpy, "pppi"),
    RT("strcat", strcat, "ppp"),  RT("abs", abs, "ii"),

    /* 64-bit integers; 'l' is a long long in a register pair */
    RT("__cos_ladd", rt_ladd, "lll"),   RT("__cos_lsub", rt_lsub, "lll"),
    RT("__cos_lmul", rt_lmul, "lll"),   RT("__cos_ldiv", rt_ldiv, "lll"),
    RT("__cos_lmod", rt_lmod, "lll"),   RT("__cos_uldiv", rt_uldiv, "lll"),
    RT("__cos_ulmod", rt_ulmod, "lll"), RT("__cos_land", rt_land, "lll"),
    RT("__cos_lor", rt_lor, "lll"),     RT("__cos_lxor", rt_lxor, "lll"),
    RT("__cos_lshl", rt_lshl, "lll"),   RT("__cos_lshr", rt_lshr, "lll"),
    RT("__cos_lushr", rt_lushr, "lll"), RT("__cos_lcmp", rt_lcmp, "ill"),
    RT("__cos_ulcmp", rt_ulcmp, "ill"), RT("__cos_i2l", rt_i2l, "li"),
    RT("__cos_u2l", rt_u2l, "li"),      RT("__cos_l2d", rt_l2d, "dl"),
    RT("__cos_ul2d", rt_ul2d, "dl"),    RT("__cos_d2l", rt_d2l, "ld"),

    /* more libm */
    RT("exp2", exp2, "dd"),       RT("log1p", log1p, "dd"),
    RT("expm1", expm1, "dd"),     RT("asinh", asinh, "dd"),
    RT("acosh", acosh, "dd"),     RT("atanh", atanh, "dd"),
    RT("copysign", copysign, "ddd"), RT("ldexp", ldexp, "ddi"),
    RT("nearbyint", nearbyint, "dd"), RT("remainder", remainder, "ddd"),

    /* graphics */
    RT("plot_clear", rt_plot_clear, "v"),
    RT("plot_point", rt_plot_point, "viii"),
    RT("plot_get", rt_plot_get, "iii"),
    RT("plot_line", rt_plot_line, "viiiii"),
    RT("plot_rect", rt_plot_rect, "viiiii"),
    RT("plot_fill_rect", rt_plot_fill_rect, "viiiii"),
    RT("plot_circle", rt_plot_circle, "viiii"),
    RT("plot_fill_circle", rt_plot_fill_circle, "viiii"),
    RT("plot_text", rt_plot_text, "viipi"),
    RT("plot_clip", rt_plot_clip, "viiii"),
    RT("plot_noclip", rt_plot_noclip, "v"),
    RT("plot_palette", rt_plot_palette, "vii"),
    RT("plot_sprite", rt_plot_sprite, "viiiip"),
    RT("plot_blit", rt_plot_blit, "viiiip"),
    RT("plot_image", rt_plot_image, "iiip"),
    RT("plot_image_width", rt_plot_image_w, "ip"),
    RT("plot_image_height", rt_plot_image_h, "ip"),
    RT("plot_auto", rt_plot_auto, "vi"),
    RT("plot_flush", rt_plot_flush, "v"),
    RT("plot_show", rt_plot_flush, "v"),
    RT("plot_hide", rt_plot_hide, "v"),
    RT("plot_width", rt_plot_width, "i"),
    RT("plot_height", rt_plot_height, "i"),
    RT("rgb", rt_rgb, "iiii"),

    /* keyboard, time and sound */
    RT("getchar", rt_getchar, "i"),
    RT("getkey", rt_getkey, "i"),
    RT("kbhit", rt_kbhit, "i"),
    RT("getline_", rt_getline, "ipi"),
    RT("millis", rt_millis, "i"),
    RT("micros", rt_micros, "i"),
    RT("delay", rt_delay, "vi"),
    RT("tone", rt_tone, "vii"),
    RT("notone", rt_notone, "v"),

    /* numeric helpers */
    RT("fft", rt_fft, "ippii"),
    RT("convolve", rt_convolve, "vpipip"),
    RT("solve", rt_solve, "ippi"),

    /* the second core */
    RT("par_for", rt_par_for, "vipp"),
    RT("cores", rt_cores, "i"),

    /* big integers */
    RT("bignew", rt_bignew, "pi"),       RT("bigparse", rt_bigparse, "pp"),
    RT("bigcopy", rt_bigcopy, "pp"),     RT("bigfree", rt_bigfree, "vp"),
    RT("bigadd", rt_bigadd, "ppp"),      RT("bigsub", rt_bigsub, "ppp"),
    RT("bigmul", rt_bigmul, "ppp"),      RT("bigdiv", rt_bigdiv, "ppp"),
    RT("bigmod", rt_bigmod, "ppp"),      RT("bigpow", rt_bigpow, "ppi"),
    RT("bigpowmod", rt_bigpowmod, "pppp"), RT("biggcd", rt_biggcd, "ppp"),
    RT("bigfact", rt_bigfact, "pi"),     RT("bigcmp", rt_bigcmp, "ipp"),
    RT("bigstr", rt_bigstr, "ippi"),     RT("bigdigits", rt_bigdigits, "ip"),
    RT("bigdouble", rt_bigdbl, "dp"),

    /* the flash store, outside SRAM */
    RT("store_pages", rt_store_pages, "i"),
    RT("store_page_size", rt_store_page_size, "i"),
    RT("store_read", rt_store_read, "pi"),
    RT("store_write", rt_store_write, "iipi"),
    RT("store_erase", rt_store_erase, "ii"),

    /* files */
    RT("fopen", rt_fopen, "ppp"),
    RT("fclose", rt_fclose, "ip"),
    RT("fread", rt_fread, "ipiip"),
    RT("fwrite", rt_fwrite, "ipiip"),
    RT("fgets", rt_fgets_, "ipip"),
    RT("fputs", rt_fputs_, "ipp"),
    RT("fseek", rt_fseek, "ipii"),
    RT("ftell", rt_ftell, "ip"),
    RT("fsize", rt_fsize, "ip"),
    RT("feof", rt_feof, "ip"),
    RT("remove", rt_remove, "ip"),
};

#define NRT ((int)(sizeof rt_table / sizeof rt_table[0]))

const RtSym *cprog_rt_table(int *count)
{
    if (count) *count = NRT;
    return rt_table;
}

/* ------------------------------------------------------------------ */
/* the loaded program                                                  */
/* ------------------------------------------------------------------ */

/* The RP2350 has 520 KB. Measured at 1.4 the firmware's own statics came to
 * about 230 KB, so these three buffers can be considerably larger than the
 * first cut and still leave room for the heap and the stack; 'mem' prints
 * what is actually left.
 *
 * The arena is the compiler's own working memory, and it was over-provided:
 * the largest example needs 45 KB of it, so 2.2 moved 32 KB of it into the
 * code buffer, where it buys a bigger program for nothing. */
#ifndef CPROG_CODE_CAP
#define CPROG_CODE_CAP  (128 * 1024)
#endif
#ifndef CPROG_DATA_CAP
#define CPROG_DATA_CAP  (48 * 1024)
#endif
#ifndef CPROG_ARENA_CAP
#define CPROG_ARENA_CAP (96 * 1024)
#endif

/* What the firmware needs to run a function, independent of the compiler
 * state that produced it. Keeping this separate is what lets a program be
 * saved to flash and brought back after a reboot, with no compiler and no
 * card involved. */
typedef struct {
    char name[CC_MAXNAME];
    int  code_off;
    int  nparams;
    int  param_dbl;
    int  ret_dbl;
} ProgFn;

#define PROG_MAXFN 96

struct CProg {
    int    loaded;
    char   name[64];
    int    code_len, data_len;
    int    nfn;
    ProgFn fn[PROG_MAXFN];
};

static struct CProg prog;
static CC   cc_state;
static char errbuf[192];
static int  errline;

/* Buffers are static so the memory is reserved up front rather than
 * fragmenting the heap; on a 520 KB part that predictability matters. */
static uint8_t code_buf[CPROG_CODE_CAP] __attribute__((aligned(8)));
static uint8_t data_buf[CPROG_DATA_CAP] __attribute__((aligned(8)));
static uint8_t arena_buf[CPROG_ARENA_CAP] __attribute__((aligned(8)));

static void capture_funcs(void);

const char *cprog_error(void)   { return errbuf; }
int   cprog_errline(void)       { return errline; }
int   cprog_loaded(void)        { return prog.loaded; }
const char *cprog_name(void)    { return prog.name; }
int   cprog_code_size(void)     { return prog.code_len; }
int   cprog_data_size(void)     { return prog.data_len; }
void  cprog_unload(void)        { prog.loaded = 0; prog.name[0] = 0; }

/* #include "file.h" reads from the card. A bare name is looked for beside
 * the program, that is in the root, which is where cc puts its sources. */
static int rt_readfile(void *ud, const char *name, char *buf, int cap)
{
    char path[80];
    void *f;
    int got;
    (void)ud;
    if (name[0] == '/') snprintf(path, sizeof path, "%s", name);
    else snprintf(path, sizeof path, "/%s", name);
    f = plat_fopen(path, "r");
    if (!f) return -1;
    got = plat_fread(f, buf, cap);
    plat_fclose(f);
    return got < 0 ? 0 : got;
}

int cprog_compile(const char *src, const char *fname)
{
    int rc;
    prog.loaded = 0;
    errbuf[0] = 0;
    errline = 0;

    cc_state.arena = arena_buf;
    cc_state.arena_cap = sizeof arena_buf;
    cc_state.readfile = rt_readfile;
    cc_state.readfile_ud = NULL;

    rc = cc_compile(&cc_state, src, fname,
                    code_buf, sizeof code_buf, (uint32_t)(uintptr_t)code_buf,
                    data_buf, sizeof data_buf, (uint32_t)(uintptr_t)data_buf,
                    rt_table, NRT);
    if (rc != 0) {
        snprintf(errbuf, sizeof errbuf, "%s", cc_state.err);
        errline = cc_state.errline;
        return -1;
    }
    prog.code_len = cc_state.code.len;
    prog.data_len = cc_state.data_len;
    snprintf(prog.name, sizeof prog.name, "%s", fname ? fname : "program");
    capture_funcs();
    prog.loaded = 1;

    /* Code was written as data; make sure the core fetches the new bytes. */
    plat_sync_icache(code_buf, (size_t)prog.code_len);
    return 0;
}

/* ------------------------------------------------------------------ */
/* saving a compiled program                                           */
/* ------------------------------------------------------------------ */

/* Compiled code carries absolute addresses: every reference to a global or
 * a runtime function was assembled as a movw/movt pair holding a real
 * address in this firmware image. Saving the bytes and loading them back
 * into the same buffers works exactly as long as those buffers are in the
 * same place and the runtime table is the same — which is to say, as long
 * as it is the same firmware.
 *
 * So the snapshot records where the buffers were and which build wrote it,
 * and refuses to load if either has moved. The alternative is a program
 * that jumps into whatever the new firmware happens to have at that
 * address, which would be a hard fault at best. */
#define PROG_MAGIC "CPROG1\0"

typedef struct {
    char     magic[8];
    char     build[32];      /* version, so a rebuild invalidates it */
    uint32_t code_base;      /* where code_buf was */
    uint32_t data_base;
    uint32_t rt_count;       /* the runtime table it was compiled against */
    uint32_t code_len;
    uint32_t data_len;
    uint32_t nfn;
    char     name[64];
} ProgHdr;

static void build_stamp(char *out, size_t cap)
{
    /* The version is the coarse check; the buffer addresses below are the
     * precise one, since any change in the firmware's layout moves them. */
    snprintf(out, cap, "CalcOS %s", COS_VERSION);
}

int cprog_snapshot_size(void)
{
    if (!prog.loaded) return 0;
    return (int)sizeof(ProgHdr) + prog.nfn * (int)sizeof(ProgFn)
         + prog.code_len + prog.data_len;
}

int cprog_snapshot(void *out, int cap)
{
    ProgHdr h;
    char *p = (char *)out;
    int need = cprog_snapshot_size();

    if (!prog.loaded || need == 0 || cap < need) return 0;
    memset(&h, 0, sizeof h);
    memcpy(h.magic, PROG_MAGIC, 8);
    build_stamp(h.build, sizeof h.build);
    h.code_base = (uint32_t)(uintptr_t)code_buf;
    h.data_base = (uint32_t)(uintptr_t)data_buf;
    h.rt_count = (uint32_t)NRT;
    h.code_len = (uint32_t)prog.code_len;
    h.data_len = (uint32_t)prog.data_len;
    h.nfn = (uint32_t)prog.nfn;
    snprintf(h.name, sizeof h.name, "%s", prog.name);

    memcpy(p, &h, sizeof h);                    p += sizeof h;
    memcpy(p, prog.fn, (size_t)prog.nfn * sizeof(ProgFn));
    p += (size_t)prog.nfn * sizeof(ProgFn);
    memcpy(p, code_buf, (size_t)prog.code_len); p += prog.code_len;
    memcpy(p, data_buf, (size_t)prog.data_len);
    return need;
}

int cprog_restore(const void *in, int len)
{
    ProgHdr h;
    const char *p = (const char *)in;
    char stamp[32];
    int need;

    if (!in || len < (int)sizeof h) return CPROG_RESTORE_BAD;
    memcpy(&h, p, sizeof h);
    if (memcmp(h.magic, PROG_MAGIC, 8) != 0) return CPROG_RESTORE_BAD;

    build_stamp(stamp, sizeof stamp);
    if (strncmp(h.build, stamp, sizeof stamp) != 0) return CPROG_RESTORE_BUILD;
    if (h.code_base != (uint32_t)(uintptr_t)code_buf
        || h.data_base != (uint32_t)(uintptr_t)data_buf
        || h.rt_count != (uint32_t)NRT)
        return CPROG_RESTORE_BUILD;
    if (h.code_len > sizeof code_buf || h.data_len > sizeof data_buf
        || h.nfn > PROG_MAXFN)
        return CPROG_RESTORE_BAD;

    need = (int)sizeof h + (int)h.nfn * (int)sizeof(ProgFn)
         + (int)h.code_len + (int)h.data_len;
    if (len < need) return CPROG_RESTORE_BAD;

    p += sizeof h;
    prog.nfn = (int)h.nfn;
    memcpy(prog.fn, p, (size_t)prog.nfn * sizeof(ProgFn));
    p += (size_t)prog.nfn * sizeof(ProgFn);
    memcpy(code_buf, p, h.code_len);            p += h.code_len;
    memcpy(data_buf, p, h.data_len);
    prog.code_len = (int)h.code_len;
    prog.data_len = (int)h.data_len;
    snprintf(prog.name, sizeof prog.name, "%s", h.name);
    prog.loaded = 1;
    plat_sync_icache(code_buf, (size_t)prog.code_len);
    return CPROG_RESTORE_OK;
}

/* ------------------------------------------------------------------ */
/* calling into compiled code                                          */
/* ------------------------------------------------------------------ */

typedef int    (*fn_i0)(void);
typedef int    (*fn_i1i)(int);
typedef int    (*fn_i2i)(int, int);
typedef int    (*fn_i3i)(int, int, int);
typedef int    (*fn_i4i)(int, int, int, int);
typedef double (*fn_d0)(void);
typedef double (*fn_d1i)(int);
typedef double (*fn_d2i)(int, int);
typedef double (*fn_d3i)(int, int, int);
typedef double (*fn_d4i)(int, int, int, int);
typedef int    (*fn_i1d)(double);
typedef int    (*fn_i2d)(double, double);
typedef int    (*fn_i3d)(double, double, double);
typedef int    (*fn_i4d)(double, double, double, double);
typedef double (*fn_d1d)(double);
typedef double (*fn_d2d)(double, double);
typedef double (*fn_d3d)(double, double, double);
typedef double (*fn_d4d)(double, double, double, double);

static ProgFn *find_fn(const char *name)
{
    int i;
    if (!prog.loaded) return NULL;
    for (i = 0; i < prog.nfn; i++)
        if (strcmp(prog.fn[i].name, name) == 0) return &prog.fn[i];
    return NULL;
}

int cprog_nfuncs(void) { return prog.loaded ? prog.nfn : 0; }

const char *cprog_func_name(int i)
{
    return (prog.loaded && i >= 0 && i < prog.nfn) ? prog.fn[i].name : "";
}

int cprog_func_nargs(int i)
{
    return (prog.loaded && i >= 0 && i < prog.nfn) ? prog.fn[i].nparams : 0;
}

/* Copy the compiler's function symbols into the table above, which is the
 * only thing running a program depends on afterwards. */
static void capture_funcs(void)
{
    Sym *s;
    prog.nfn = 0;
    for (s = cc_state.globals; s; s = s->next) {
        ProgFn *f;
        if (s->kind != S_FUNC || !s->defined || s->code_off < 0) continue;
        if (prog.nfn >= PROG_MAXFN) break;
        f = &prog.fn[prog.nfn++];
        snprintf(f->name, sizeof f->name, "%s", s->name);
        f->code_off = s->code_off;
        f->nparams = s->nparams;
        f->param_dbl = s->param_dbl;
        f->ret_dbl = s->ret_dbl;
    }
}

/* Compiled code is Thumb-2, so it can only be executed on the target. On a
 * host build the compiler still runs (and is checked by the Unicorn
 * harness), but calling into the output would be nonsense. */
#if defined(__arm__) || defined(__thumb__)
#define CAN_EXECUTE 1
#else
#define CAN_EXECUTE 0
#endif

static void *fn_ptr(ProgFn *s)
{
    return (void *)((char *)code_buf + s->code_off + 1);  /* +1: Thumb bit */
}

int cprog_call(const char *name, const double *a, int nargs, double *result)
{
    ProgFn *s = find_fn(name);
    void *p;
    if (!s) return -1;
    if (s->nparams != nargs) return -3;
    if (nargs > 4) return -2;
    if (!CAN_EXECUTE) return -4;
    p = fn_ptr(s);

    if (s->param_dbl) {
        if (s->ret_dbl) {
            switch (nargs) {
            case 0: *result = ((fn_d0)p)(); return 0;
            case 1: *result = ((fn_d1d)p)(a[0]); return 0;
            case 2: *result = ((fn_d2d)p)(a[0], a[1]); return 0;
            case 3: *result = ((fn_d3d)p)(a[0], a[1], a[2]); return 0;
            case 4: *result = ((fn_d4d)p)(a[0], a[1], a[2], a[3]); return 0;
            }
        } else {
            switch (nargs) {
            case 0: *result = (double)((fn_i0)p)(); return 0;
            case 1: *result = (double)((fn_i1d)p)(a[0]); return 0;
            case 2: *result = (double)((fn_i2d)p)(a[0], a[1]); return 0;
            case 3: *result = (double)((fn_i3d)p)(a[0], a[1], a[2]); return 0;
            case 4: *result = (double)((fn_i4d)p)(a[0], a[1], a[2], a[3]); return 0;
            }
        }
    } else {
        int i0 = nargs > 0 ? (int)a[0] : 0, i1 = nargs > 1 ? (int)a[1] : 0;
        int i2 = nargs > 2 ? (int)a[2] : 0, i3 = nargs > 3 ? (int)a[3] : 0;
        if (s->ret_dbl) {
            switch (nargs) {
            case 0: *result = ((fn_d0)p)(); return 0;
            case 1: *result = ((fn_d1i)p)(i0); return 0;
            case 2: *result = ((fn_d2i)p)(i0, i1); return 0;
            case 3: *result = ((fn_d3i)p)(i0, i1, i2); return 0;
            case 4: *result = ((fn_d4i)p)(i0, i1, i2, i3); return 0;
            }
        } else {
            switch (nargs) {
            case 0: *result = (double)((fn_i0)p)(); return 0;
            case 1: *result = (double)((fn_i1i)p)(i0); return 0;
            case 2: *result = (double)((fn_i2i)p)(i0, i1); return 0;
            case 3: *result = (double)((fn_i3i)p)(i0, i1, i2); return 0;
            case 4: *result = (double)((fn_i4i)p)(i0, i1, i2, i3); return 0;
            }
        }
    }
    return -2;
}

int cprog_run_main(void)
{
    ProgFn *s = find_fn("main");
    if (!s) return 0;
    if (s->nparams != 0) return 0;
    if (!CAN_EXECUTE) { plat_puts("compiled code only runs on the device\n"); return -1; }
    if (s->ret_dbl) return (int)((fn_d0)fn_ptr(s))();
    return ((fn_i0)fn_ptr(s))();
}
