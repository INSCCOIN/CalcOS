/* A library that lives in flash.
 *
 * Compile it into the store once and it is there after a reboot, with no
 * card in the machine:
 *
 *     ccs numlib /numlib.c      compile and keep it in flash
 *     use numlib                load it (not needed if you name it "boot")
 *     rms([1,2,3,4])            no — these take plain numbers; see below
 *
 * Every function here takes and returns doubles, which is what the prompt
 * can call:
 *
 *     use numlib
 *     nroot(2, 5)               the fifth root of two
 *     bessel_j0(1.5)
 *     mandel(-0.5, 0.6, 200)    how fast that point escapes
 *
 * Name it "boot" instead of "numlib" and it loads at every startup.
 */

/* An nth root by Newton's method, which keeps full precision where
 * pow(x, 1.0/n) loses a digit or two to the division. */
double nroot(double x, double n)
{
    double g, prev;
    int i;
    if (n == 0.0) return 1.0;
    if (x == 0.0) return 0.0;
    if (x < 0.0 && ((int)n & 1) == 0) return 0.0 / 0.0;   /* NaN */
    g = pow(x < 0 ? -x : x, 1.0 / n);
    for (i = 0; i < 12; i++) {
        prev = g;
        g = ((n - 1.0) * g + x / pow(g, n - 1.0)) / n;
        if (g == prev) break;
    }
    return (x < 0) ? -g : g;
}

/* Bessel J0 by its series near zero and the asymptotic form far out. */
double bessel_j0(double x)
{
    double ax = x < 0 ? -x : x;
    if (ax < 8.0) {
        double t = x * x / 4.0, term = 1.0, sum = 1.0;
        int k;
        for (k = 1; k < 30; k++) {
            term = -term * t / ((double)k * (double)k);
            sum = sum + term;
            if (term < 1e-18 && term > -1e-18) break;
        }
        return sum;
    }
    {
        double z = 8.0 / ax, y = z * z;
        double p = 1.0 - 0.1098628627e-2 * y + 0.2734510407e-4 * y * y;
        double q = -0.1562499995e-1 * z + 0.1430488765e-3 * z * y;
        double xx = ax - 0.785398164;
        return sqrt(0.636619772 / ax) * (cos(xx) * p - sin(xx) * q);
    }
}

/* How many iterations a point of the Mandelbrot set survives. Useful from
 * the prompt for exploring before you draw anything. */
double mandel(double cr, double ci, double limit)
{
    double zr = 0.0, zi = 0.0;
    int n = 0, max = (int)limit;
    while (n < max && zr * zr + zi * zi < 4.0) {
        double t = zr * zr - zi * zi + cr;
        zi = 2.0 * zr * zi + ci;
        zr = t;
        n++;
    }
    return (double)n;
}

/* The logistic map, iterated: a one-line chaos demonstration that is
 * tedious to type at the prompt every time. */
double logistic(double r, double x0, double steps)
{
    double x = x0;
    int i, n = (int)steps;
    for (i = 0; i < n; i++) x = r * x * (1.0 - x);
    return x;
}

/* A 1024-point power spectrum peak, using the firmware's own FFT. Returns
 * the bin with the most energy for a sampled sine, which is the thing you
 * actually want to know. */
static double re[256];
static double im[256];

double peak_bin(double freq, double rate)
{
    int i, best = 0;
    double bestv = -1.0;
    for (i = 0; i < 256; i++) {
        re[i] = sin(2.0 * 3.14159265358979 * freq * (double)i / rate);
        im[i] = 0.0;
    }
    if (!fft(re, im, 256, -1)) return -1.0;
    for (i = 1; i < 128; i++) {
        double p = re[i] * re[i] + im[i] * im[i];
        if (p > bestv) { bestv = p; best = i; }
    }
    return (double)best;
}

int main(void)
{
    printf("numlib: nroot bessel_j0 mandel logistic peak_bin\n");
    printf("call them from the prompt\n");
    return 0;
}
