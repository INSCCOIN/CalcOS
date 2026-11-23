/* Arbitrary-precision integers. See src/bignum.c.
 *
 * Every call that returns a Big* returns a fresh one the caller frees, or
 * NULL if it could not be made. Nothing is shared and nothing is mutated
 * in place, which costs a little speed and removes a whole class of bug.
 */
#ifndef BIGNUM_H
#define BIGNUM_H

#include <stdint.h>

/* Ceilings, so a runaway expression fails rather than eating the heap.
 * The part has about 100 KB of heap: 1024 limbs is 4 KB of number and
 * nearly ten thousand decimal digits, which is far past anything anyone
 * types at a calculator and still leaves room to print it. */
#define BIG_MAXLIMBS   1024          /* about 9 800 decimal digits */
#define BIG_MAXDIGITS  10000
#define BIG_MAXFACT    3000

typedef struct Big Big;

Big   *big_from_ll(long long v);
Big   *big_from_double(double v);
Big   *big_parse(const char *s);      /* decimal, or 0x... hex */
Big   *big_copy(const Big *a);
void   big_free(Big *a);

Big   *big_add(const Big *a, const Big *b);
Big   *big_sub(const Big *a, const Big *b);
Big   *big_mul(const Big *a, const Big *b);
Big   *big_div(const Big *a, const Big *b);   /* truncating */
Big   *big_mod(const Big *a, const Big *b);   /* sign of the dividend */
int    big_divmod(const Big *a, const Big *b, Big **q, Big **r);
Big   *big_neg(const Big *a);
Big   *big_shl(const Big *a, int k);
Big   *big_shr(const Big *a, int k);
Big   *big_pow(const Big *a, long long e);
Big   *big_powmod(const Big *a, const Big *e, const Big *m);
Big   *big_gcd(const Big *a, const Big *b);
Big   *big_fact(long long n);

int    big_cmp(const Big *a, const Big *b);
int    big_sign(const Big *a);
int    big_is_zero(const Big *a);
int    big_bits(const Big *a);
int    big_digits(const Big *a);
double big_to_double(const Big *a);
int    big_str(const Big *a, char *out, int cap);

#endif
