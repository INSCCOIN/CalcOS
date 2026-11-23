/* CalcOS example: numeric integration, callable from the calculator. */
#include <stdio.h>

double f(double x) { return x * x * x - 2.0 * x; }

/* Simpson's rule over n panels. Call from the prompt: simpson(0, 2, 1000) */
double simpson(double a, double b, double n)
{
    int    k, N = (int)n;
    double h, s;
    if (N % 2) N++;
    h = (b - a) / N;
    s = f(a) + f(b);
    for (k = 1; k < N; k++)
        s += f(a + k * h) * ((k % 2) ? 4.0 : 2.0);
    return s * h / 3.0;
}

int main(void)
{
    printf("integral of x^3-2x over [0,2] = %.10f\n", simpson(0.0, 2.0, 1000));
    return 0;
}
