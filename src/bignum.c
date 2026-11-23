/* Arbitrary-precision integers.
 *
 * The calculator already has a double-double type good for 32 digits, and
 * intervals for rigorous bounds. What neither can do is be exact without
 * limit: 2^500, or 100!, or modular arithmetic on numbers that do not fit a
 * double. Everyone who needs that ends up writing a digit array by hand,
 * which is the thing this is here to stop.
 *
 * A number is a sign and an array of 32-bit limbs, least significant first,
 * with no leading zero limbs and zero represented by no limbs at all. The
 * algorithms are the schoolbook ones: they are O(n^2), which on numbers of
 * a few hundred digits is far quicker than anything cleverer would be once
 * its constant factor is paid, and they are short enough to read.
 *
 * Division is Knuth's algorithm D. It is the one piece here that is easy to
 * get subtly wrong, so it is written out in full rather than compressed,
 * and the tests check it against Python's integers over a few thousand
 * random cases.
 */
#include "calc.h"
#include "bignum.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>

#define LIMB_BITS 32
typedef uint32_t limb;
typedef uint64_t dlimb;

struct Big {
    int    sign;      /* -1, 0, +1; zero has n == 0 */
    int    n;         /* limbs in use */
    int    cap;
    limb  *d;
};

/* ------------------------------------------------------------------ */
/* allocation                                                          */
/* ------------------------------------------------------------------ */

static Big *big_alloc(int cap)
{
    Big *b;
    if (cap < 1) cap = 1;
    if (cap > BIG_MAXLIMBS) return NULL;
    b = (Big *)malloc(sizeof *b);
    if (!b) return NULL;
    b->d = (limb *)calloc((size_t)cap, sizeof(limb));
    if (!b->d) { free(b); return NULL; }
    b->cap = cap;
    b->n = 0;
    b->sign = 0;
    return b;
}

void big_free(Big *b)
{
    if (!b) return;
    free(b->d);
    free(b);
}

static int big_grow(Big *b, int cap)
{
    limb *d;
    if (cap <= b->cap) return 1;
    if (cap > BIG_MAXLIMBS) return 0;
    d = (limb *)realloc(b->d, (size_t)cap * sizeof(limb));
    if (!d) return 0;
    memset(d + b->cap, 0, (size_t)(cap - b->cap) * sizeof(limb));
    b->d = d;
    b->cap = cap;
    return 1;
}

static void trim(Big *b)
{
    while (b->n > 0 && b->d[b->n - 1] == 0) b->n--;
    if (b->n == 0) b->sign = 0;
}

Big *big_copy(const Big *a)
{
    Big *r;
    if (!a) return NULL;
    r = big_alloc(a->n ? a->n : 1);
    if (!r) return NULL;
    memcpy(r->d, a->d, (size_t)a->n * sizeof(limb));
    r->n = a->n;
    r->sign = a->sign;
    return r;
}

Big *big_from_ll(long long v)
{
    Big *r = big_alloc(2);
    unsigned long long u;
    if (!r) return NULL;
    if (v == 0) return r;
    if (v < 0) { r->sign = -1; u = (unsigned long long)(-(v + 1)) + 1ull; }
    else { r->sign = 1; u = (unsigned long long)v; }
    r->d[0] = (limb)(u & 0xFFFFFFFFull);
    r->d[1] = (limb)(u >> 32);
    r->n = 2;
    trim(r);
    return r;
}

int big_is_zero(const Big *a) { return !a || a->n == 0; }
int big_sign(const Big *a) { return a ? a->sign : 0; }

/* ------------------------------------------------------------------ */
/* comparison                                                          */
/* ------------------------------------------------------------------ */

/* magnitudes only */
static int cmp_mag(const Big *a, const Big *b)
{
    int i;
    if (a->n != b->n) return a->n < b->n ? -1 : 1;
    for (i = a->n - 1; i >= 0; i--)
        if (a->d[i] != b->d[i]) return a->d[i] < b->d[i] ? -1 : 1;
    return 0;
}

int big_cmp(const Big *a, const Big *b)
{
    if (!a || !b) return 0;
    if (a->sign != b->sign) return a->sign < b->sign ? -1 : 1;
    if (a->sign == 0) return 0;
    return a->sign > 0 ? cmp_mag(a, b) : -cmp_mag(a, b);
}

/* ------------------------------------------------------------------ */
/* addition and subtraction                                            */
/* ------------------------------------------------------------------ */

static Big *add_mag(const Big *a, const Big *b)
{
    int i, n = (a->n > b->n ? a->n : b->n) + 1;
    Big *r = big_alloc(n);
    dlimb carry = 0;
    if (!r) return NULL;
    for (i = 0; i < n; i++) {
        dlimb s = carry;
        if (i < a->n) s += a->d[i];
        if (i < b->n) s += b->d[i];
        r->d[i] = (limb)s;
        carry = s >> LIMB_BITS;
    }
    r->n = n;
    trim(r);
    return r;
}

/* |a| - |b|, requiring |a| >= |b| */
static Big *sub_mag(const Big *a, const Big *b)
{
    int i;
    Big *r = big_alloc(a->n ? a->n : 1);
    long long borrow = 0;
    if (!r) return NULL;
    for (i = 0; i < a->n; i++) {
        long long s = (long long)a->d[i] - borrow - (i < b->n ? b->d[i] : 0);
        if (s < 0) { s += 1ll << LIMB_BITS; borrow = 1; } else borrow = 0;
        r->d[i] = (limb)s;
    }
    r->n = a->n;
    trim(r);
    return r;
}

Big *big_add(const Big *a, const Big *b)
{
    Big *r;
    if (!a || !b) return NULL;
    if (a->sign == 0) return big_copy(b);
    if (b->sign == 0) return big_copy(a);
    if (a->sign == b->sign) {
        r = add_mag(a, b);
        if (r && r->n) r->sign = a->sign;
        return r;
    }
    {
        int c = cmp_mag(a, b);
        if (c == 0) return big_alloc(1);        /* zero */
        r = (c > 0) ? sub_mag(a, b) : sub_mag(b, a);
        if (r && r->n) r->sign = (c > 0) ? a->sign : b->sign;
        return r;
    }
}

Big *big_neg(const Big *a)
{
    Big *r = big_copy(a);
    if (r) r->sign = -r->sign;
    return r;
}

Big *big_sub(const Big *a, const Big *b)
{
    Big *nb = big_neg(b), *r;
    if (!nb) return NULL;
    r = big_add(a, nb);
    big_free(nb);
    return r;
}

/* ------------------------------------------------------------------ */
/* multiplication                                                      */
/* ------------------------------------------------------------------ */

Big *big_mul(const Big *a, const Big *b)
{
    int i, j;
    Big *r;
    if (!a || !b) return NULL;
    if (a->sign == 0 || b->sign == 0) return big_alloc(1);
    r = big_alloc(a->n + b->n);
    if (!r) return NULL;
    for (i = 0; i < a->n; i++) {
        dlimb carry = 0;
        if (a->d[i] == 0) continue;
        for (j = 0; j < b->n; j++) {
            dlimb t = (dlimb)a->d[i] * b->d[j] + r->d[i + j] + carry;
            r->d[i + j] = (limb)t;
            carry = t >> LIMB_BITS;
        }
        {   /* the carry can ripple past the immediate limb */
            int k = i + b->n;
            while (carry) {
                dlimb t = (dlimb)r->d[k] + carry;
                r->d[k] = (limb)t;
                carry = t >> LIMB_BITS;
                k++;
            }
        }
    }
    r->n = a->n + b->n;
    r->sign = a->sign * b->sign;
    trim(r);
    return r;
}

/* ------------------------------------------------------------------ */
/* shifts, needed by division                                          */
/* ------------------------------------------------------------------ */

static Big *shl_bits(const Big *a, int k)
{
    int limbs = k / LIMB_BITS, bits = k % LIMB_BITS, i;
    Big *r;
    if (a->n == 0) return big_alloc(1);
    r = big_alloc(a->n + limbs + 1);
    if (!r) return NULL;
    for (i = 0; i < a->n; i++) {
        dlimb v = (dlimb)a->d[i] << bits;
        r->d[i + limbs] |= (limb)v;
        r->d[i + limbs + 1] |= (limb)(v >> LIMB_BITS);
    }
    r->n = a->n + limbs + 1;
    r->sign = a->sign;
    trim(r);
    return r;
}

static Big *shr_bits(const Big *a, int k)
{
    int limbs = k / LIMB_BITS, bits = k % LIMB_BITS, i;
    Big *r;
    if (limbs >= a->n) return big_alloc(1);
    r = big_alloc(a->n - limbs);
    if (!r) return NULL;
    for (i = 0; i + limbs < a->n; i++) {
        dlimb v = a->d[i + limbs] >> bits;
        if (bits && i + limbs + 1 < a->n)
            v |= (dlimb)a->d[i + limbs + 1] << (LIMB_BITS - bits);
        r->d[i] = (limb)v;
    }
    r->n = a->n - limbs;
    r->sign = a->sign;
    trim(r);
    return r;
}

Big *big_shl(const Big *a, int k)
{
    if (!a) return NULL;
    if (k < 0) return big_shr(a, -k);
    return shl_bits(a, k);
}

Big *big_shr(const Big *a, int k)
{
    if (!a) return NULL;
    if (k < 0) return big_shl(a, -k);
    return shr_bits(a, k);
}

static int bit_length(const Big *a)
{
    limb top;
    int n;
    if (a->n == 0) return 0;
    top = a->d[a->n - 1];
    n = (a->n - 1) * LIMB_BITS;
    while (top) { n++; top >>= 1; }
    return n;
}

int big_bits(const Big *a) { return a ? bit_length(a) : 0; }

/* ------------------------------------------------------------------ */
/* division: Knuth algorithm D                                         */
/* ------------------------------------------------------------------ */

/* Divide by a single limb, which is both the easy case and the one the
 * decimal printer leans on. */
static Big *divmod_small(const Big *a, limb v, limb *rem)
{
    int i;
    Big *q = big_alloc(a->n ? a->n : 1);
    dlimb r = 0;
    if (!q) return NULL;
    for (i = a->n - 1; i >= 0; i--) {
        dlimb cur = (r << LIMB_BITS) | a->d[i];
        q->d[i] = (limb)(cur / v);
        r = cur % v;
    }
    q->n = a->n;
    q->sign = a->sign;
    trim(q);
    if (rem) *rem = (limb)r;
    return q;
}

/* Truncating division: the quotient rounds toward zero and the remainder
 * takes the sign of the dividend, which is what C and every calculator
 * anyone has used agree on. */
int big_divmod(const Big *a, const Big *b, Big **qout, Big **rout)
{
    Big *u = NULL, *v = NULL, *q = NULL, *r = NULL;
    int shift, n, m, j, ok = 0;

    if (!a || !b || b->sign == 0) return 0;
    if (cmp_mag(a, b) < 0) {                 /* |a| < |b|: q = 0, r = a */
        if (qout) *qout = big_alloc(1);
        if (rout) *rout = big_copy(a);
        return 1;
    }
    if (b->n == 1) {
        limb rem = 0;
        q = divmod_small(a, b->d[0], &rem);
        if (!q) return 0;
        q->sign = (q->n ? a->sign * b->sign : 0);
        r = big_from_ll((long long)rem);
        if (!r) { big_free(q); return 0; }
        if (r->n) r->sign = a->sign;
        if (qout) *qout = q; else big_free(q);
        if (rout) *rout = r; else big_free(r);
        return 1;
    }

    /* normalise so the divisor's top limb has its high bit set */
    shift = LIMB_BITS - (bit_length(b) % LIMB_BITS);
    if (shift == LIMB_BITS) shift = 0;
    u = shl_bits(a, shift);
    v = shl_bits(b, shift);
    if (!u || !v) goto done;
    u->sign = v->sign = 1;

    n = v->n;
    m = u->n - n;
    if (!big_grow(u, u->n + 1)) goto done;
    u->d[u->n] = 0;
    q = big_alloc(m + 1);
    if (!q) goto done;

    for (j = m; j >= 0; j--) {
        dlimb num = ((dlimb)u->d[j + n] << LIMB_BITS) | u->d[j + n - 1];
        dlimb qhat = num / v->d[n - 1];
        dlimb rhat = num % v->d[n - 1];
        long long borrow = 0, carry = 0;
        int i;

        /* correct the estimate: it is at most two too large */
        while (qhat > 0xFFFFFFFFull
               || (dlimb)v->d[n - 2] * qhat
                  > ((rhat << LIMB_BITS) | u->d[j + n - 2])) {
            qhat--;
            rhat += v->d[n - 1];
            if (rhat > 0xFFFFFFFFull) break;
        }

        for (i = 0; i < n; i++) {             /* u -= qhat * v, shifted */
            dlimb p = qhat * v->d[i] + (dlimb)carry;
            long long t = (long long)u->d[i + j] - (long long)(limb)p - borrow;
            carry = (long long)(p >> LIMB_BITS);
            if (t < 0) { t += 1ll << LIMB_BITS; borrow = 1; } else borrow = 0;
            u->d[i + j] = (limb)t;
        }
        {
            long long t = (long long)u->d[j + n] - carry - borrow;
            if (t < 0) {                      /* estimate was one too big */
                t += 1ll << LIMB_BITS;
                u->d[j + n] = (limb)t;
                qhat--;
                carry = 0;
                for (i = 0; i < n; i++) {     /* add v back */
                    dlimb s = (dlimb)u->d[i + j] + v->d[i] + (dlimb)carry;
                    u->d[i + j] = (limb)s;
                    carry = (long long)(s >> LIMB_BITS);
                }
                u->d[j + n] = (limb)((dlimb)u->d[j + n] + (dlimb)carry);
            } else {
                u->d[j + n] = (limb)t;
            }
        }
        q->d[j] = (limb)qhat;
    }

    q->n = m + 1;
    q->sign = 1;
    trim(q);
    if (q->n) q->sign = a->sign * b->sign;

    u->n = n;
    trim(u);
    r = shr_bits(u, shift);
    if (!r) goto done;
    if (r->n) r->sign = a->sign;

    if (qout) { *qout = q; q = NULL; }
    if (rout) { *rout = r; r = NULL; }
    ok = 1;

done:
    big_free(u);
    big_free(v);
    big_free(q);
    big_free(r);
    return ok;
}

Big *big_div(const Big *a, const Big *b)
{
    Big *q = NULL;
    if (!big_divmod(a, b, &q, NULL)) return NULL;
    return q;
}

Big *big_mod(const Big *a, const Big *b)
{
    Big *r = NULL;
    if (!big_divmod(a, b, NULL, &r)) return NULL;
    return r;
}

/* ------------------------------------------------------------------ */
/* powers and number theory                                            */
/* ------------------------------------------------------------------ */

Big *big_pow(const Big *a, long long e)
{
    Big *base, *acc, *t;
    if (!a || e < 0) return NULL;
    acc = big_from_ll(1);
    base = big_copy(a);
    if (!acc || !base) { big_free(acc); big_free(base); return NULL; }
    while (e > 0) {
        if (e & 1) {
            t = big_mul(acc, base);
            big_free(acc);
            if (!t) { big_free(base); return NULL; }
            acc = t;
        }
        e >>= 1;
        if (e) {
            t = big_mul(base, base);
            big_free(base);
            if (!t) { big_free(acc); return NULL; }
            base = t;
        }
    }
    big_free(base);
    return acc;
}

/* a^e mod m, by repeated squaring: the one operation that makes big
 * integers useful rather than merely large. */
Big *big_powmod(const Big *a, const Big *e, const Big *m)
{
    Big *base, *acc, *t, *r;
    int i, bits;

    if (!a || !e || !m || m->sign == 0 || e->sign < 0) return NULL;
    acc = big_from_ll(1);
    base = big_mod(a, m);
    if (!acc || !base) { big_free(acc); big_free(base); return NULL; }
    if (base->sign < 0) {                     /* keep it non-negative */
        t = big_add(base, m);
        big_free(base);
        base = t;
        if (!base) { big_free(acc); return NULL; }
    }
    bits = bit_length(e);
    for (i = 0; i < bits; i++) {
        if ((e->d[i / LIMB_BITS] >> (i % LIMB_BITS)) & 1) {
            t = big_mul(acc, base);
            if (!t) break;
            r = big_mod(t, m);
            big_free(t);
            big_free(acc);
            acc = r;
            if (!acc) break;
        }
        t = big_mul(base, base);
        if (!t) break;
        r = big_mod(t, m);
        big_free(t);
        big_free(base);
        base = r;
        if (!base) break;
    }
    big_free(base);
    return acc;
}

Big *big_gcd(const Big *a, const Big *b)
{
    Big *x = big_copy(a), *y = big_copy(b), *t;
    if (!x || !y) { big_free(x); big_free(y); return NULL; }
    x->sign = x->n ? 1 : 0;
    y->sign = y->n ? 1 : 0;
    while (!big_is_zero(y)) {
        t = big_mod(x, y);
        if (!t) { big_free(x); big_free(y); return NULL; }
        big_free(x);
        x = y;
        y = t;
    }
    big_free(y);
    return x;
}

Big *big_fact(long long n)
{
    Big *acc, *t, *k;
    long long i;
    if (n < 0 || n > BIG_MAXFACT) return NULL;
    acc = big_from_ll(1);
    if (!acc) return NULL;
    for (i = 2; i <= n; i++) {
        k = big_from_ll(i);
        if (!k) { big_free(acc); return NULL; }
        t = big_mul(acc, k);
        big_free(k);
        big_free(acc);
        if (!t) return NULL;
        acc = t;
    }
    return acc;
}

/* ------------------------------------------------------------------ */
/* text                                                                */
/* ------------------------------------------------------------------ */

Big *big_parse(const char *s)
{
    Big *acc = big_from_ll(0), *t, *ten = NULL, *dig = NULL;
    int neg = 0, any = 0, base = 10;

    if (!acc) return NULL;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '-') { neg = 1; s++; }
    else if (*s == '+') s++;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { base = 16; s += 2; }

    ten = big_from_ll(base);
    if (!ten) { big_free(acc); return NULL; }
    for (; *s; s++) {
        int v;
        if (*s == '_' || *s == '\'') continue;
        if (*s >= '0' && *s <= '9') v = *s - '0';
        else if (base == 16 && *s >= 'a' && *s <= 'f') v = *s - 'a' + 10;
        else if (base == 16 && *s >= 'A' && *s <= 'F') v = *s - 'A' + 10;
        else break;
        if (v >= base) break;
        t = big_mul(acc, ten);
        big_free(acc);
        acc = t;
        if (!acc) break;
        dig = big_from_ll(v);
        t = acc ? big_add(acc, dig) : NULL;
        big_free(dig);
        big_free(acc);
        acc = t;
        if (!acc) break;
        any = 1;
    }
    big_free(ten);
    if (!acc) return NULL;
    if (!any) { big_free(acc); return NULL; }
    if (neg && acc->n) acc->sign = -1;
    return acc;
}

/* Decimal, by repeatedly dividing by the largest power of ten a limb
 * holds: nine digits at a time rather than one. */
/* The digits are produced least significant first, so they are written
 * straight into the caller's buffer and reversed at the end. Nothing is
 * staged in a temporary: a scratch buffer big enough for the largest
 * number would be ten kilobytes, which is more stack than this part has
 * and more static memory than it can spare. */
int big_str(const Big *a, char *out, int cap)
{
    int n = 0, i;
    Big *cur;

    if (!a || !out || cap < 2) { if (out && cap > 0) out[0] = 0; return 0; }
    if (a->n == 0) { out[0] = '0'; out[1] = 0; return 1; }

    cur = big_copy(a);
    if (!cur) { out[0] = 0; return 0; }
    cur->sign = 1;

    while (cur->n > 0) {
        limb rem = 0;
        Big *q = divmod_small(cur, 1000000000u, &rem);
        big_free(cur);
        cur = q;
        if (!cur) { out[0] = 0; return 0; }
        if (cur->n == 0) {                     /* last group: no padding */
            while (rem) {
                if (n >= cap - 2) goto too_long;
                out[n++] = (char)('0' + rem % 10);
                rem /= 10;
            }
        } else {
            for (i = 0; i < 9; i++) {
                if (n >= cap - 2) goto too_long;
                out[n++] = (char)('0' + rem % 10);
                rem /= 10;
            }
        }
    }
    big_free(cur);
    if (n == 0) out[n++] = '0';
    if (a->sign < 0) out[n++] = '-';
    for (i = 0; i < n / 2; i++) {
        char t = out[i];
        out[i] = out[n - 1 - i];
        out[n - 1 - i] = t;
    }
    out[n] = 0;
    return n;

too_long:
    big_free(cur);
    snprintf(out, (size_t)cap, "<%d digits>", big_digits(a));
    return 0;
}

/* The exact number of decimal digits, counted by dividing rather than
 * estimated from the bit length: an estimate is off by one often enough
 * that it cannot be shown to anyone. */
int big_digits(const Big *a)
{
    Big *cur;
    int n = 0;

    if (!a || a->n == 0) return 1;
    cur = big_copy(a);
    if (!cur) return 1;
    cur->sign = 1;
    while (cur->n > 0) {
        limb rem = 0;
        Big *q = divmod_small(cur, 1000000000u, &rem);
        big_free(cur);
        cur = q;
        if (!cur) return n ? n : 1;
        if (cur->n == 0) {
            while (rem) { n++; rem /= 10; }
        } else {
            n += 9;
        }
    }
    big_free(cur);
    return n ? n : 1;
}

double big_to_double(const Big *a)
{
    double v = 0.0;
    int i;
    if (!a || a->n == 0) return 0.0;
    for (i = a->n - 1; i >= 0; i--) v = v * 4294967296.0 + (double)a->d[i];
    return a->sign < 0 ? -v : v;
}

Big *big_from_double(double v)
{
    Big *t;
    int neg = v < 0;
    if (v != v || v > 1e300 || v < -1e300) return NULL;
    if (neg) v = -v;
    v = floor(v);

    {   /* peel 32 bits at a time from the bottom */
        limb limbs[64];
        int n = 0;
        while (v >= 1.0 && n < 64) {
            double hi = floor(v / 4294967296.0);
            limbs[n++] = (limb)(v - hi * 4294967296.0);
            v = hi;
        }
        t = big_alloc(n ? n : 1);
        if (!t) return NULL;
        memcpy(t->d, limbs, (size_t)n * sizeof(limb));
        t->n = n;
        t->sign = n ? (neg ? -1 : 1) : 0;
        trim(t);
        return t;
    }
}
