/* 64-bit integers, function pointers and a static local.
 *
 *   cc /bigint.c    run
 * and from the prompt afterwards:  fib(90)
 */
#include <stdio.h>

/* 2^53 is where a double starts skipping odd numbers; these do not. */
long long fact(int n)
{
    long long r = 1;
    int i;
    for (i = 2; i <= n; i++) r = r * i;
    return r;
}

long long fib_ll(int n)
{
    long long a = 0, b = 1, t;
    int i;
    for (i = 0; i < n; i++) { t = a + b; a = b; b = t; }
    return a;
}

/* callable from the prompt, which passes and returns doubles */
double fib(double n) { return (double)fib_ll((int)n); }

/* a static local: the count survives between calls */
int called(void)
{
    static int n = 0;
    n++;
    return n;
}

/* a function pointer, and a table of them */
static long long apply(long long (*f)(int), int n) { return f(n); }

int main(void)
{
    long long (*table[2])(int);
    int i;

    table[0] = fact;
    table[1] = fib_ll;

    printf("20! = %lld\n", fact(20));
    printf("fib(90) = %lld\n", fib_ll(90));
    printf("2^53+1 = %lld\n", (1LL << 53) + 1LL);

    for (i = 0; i < 2; i++)
        printf("table[%d](25) = %lld\n", i, apply(table[i], 25));

    called(); called();
    printf("called %d times\n", called());
    return 0;
}
