/* Structs, arrays and a calculator-callable function.
 *
 *   cc /stats.c
 *   run                  runs main()
 *   variance(10)         calls straight into the compiled code
 */
int    printf(char *fmt, ...);
double sqrt(double x);

struct Sample {
    double x;
    double w;
};

static struct Sample data[64];
static int n;

static void fill(int count)
{
    int i;
    unsigned seed = 12345;
    n = count > 64 ? 64 : count;
    for (i = 0; i < n; i++) {
        seed = seed * 1103515245 + 12345;
        data[i].x = (double)((seed >> 16) & 0x7FFF) / 32768.0;
        data[i].w = 1.0;
    }
}

/* Two-pass variance: the textbook one-pass formula loses most of its
   significant digits when the mean is large compared with the spread. */
double variance(double count)
{
    int i;
    double mean = 0.0, s = 0.0;
    fill((int)count);
    if (n < 2) return 0.0;
    for (i = 0; i < n; i++) mean += data[i].x;
    mean = mean / n;
    for (i = 0; i < n; i++) s += (data[i].x - mean) * (data[i].x - mean);
    return s / (n - 1);
}

int main(void)
{
    double v = variance(64.0);
    printf("n = %d\n", n);
    printf("variance = %.10f\n", v);
    printf("sigma    = %.10f\n", sqrt(v));
    return 0;
}
