/* Graphing a function you wrote.
 *
 *     cc /grapher.c
 *     run
 *
 * The drawing calls could already put a line on the screen; what was
 * missing was anything that turns a function into a graph. Writing the
 * sampling loop, the scaling and the axes by hand is a dozen lines of
 * arithmetic with a different mistake in it every time, so plot_fn and its
 * relatives do it: give them the address of a function and a window in the
 * function's own units.
 *
 * Three shapes, which are the three a calculator draws:
 *
 *     plot_fn(f, steps, colour)                  y against x
 *     plot_param(fx, fy, t0, t1, steps, colour)  a pair against a parameter
 *     plot_polar(fr, a0, a1, steps, colour)      a radius against an angle
 *
 * and plot_surface for a function of two variables.
 *
 * Two things the sampler handles that a hand-written loop usually does not.
 * A function with a pole — tan, 1/x — returns something enormous near it,
 * and joining the sample before to the sample after would draw a vertical
 * line that is not part of the function; the line breaks instead. And a
 * function that returns no number at all for some x, like sqrt of a
 * negative, leaves a gap rather than a line across it.
 */
#include <stdio.h>

static double damped(double x) { return exp(-x * x / 8.0) * sin(x * 2.0); }
static double poles(double x)  { return tan(x); }
static double root(double x)   { return sqrt(x - 1.0); }

/* A parametric pair: a Lissajous figure, which is the thing that cannot be
 * drawn as y against x at all. */
static double lx(double t) { return 3.0 * sin(t * 3.0); }
static double ly(double t) { return 3.0 * sin(t * 4.0 + 0.7); }

/* A polar rose. */
static double rose(double a) { return 3.0 * cos(a * 2.5); }

/* A surface. */
static double hill(double x, double y)
{
    double r = sqrt(x * x + y * y) + 0.001;
    return 4.0 * sin(r) / r;
}

static void pause_for_key(const char *what)
{
    printf("%s - press a key\n", what);
    plot_show();
    getchar();
}

int main(void)
{
    plot_auto(0);

    /* y against x, with a function that decays: the window is chosen to
     * suit the function rather than the other way round. */
    plot_clear();
    plot_window(-8.0, -1.2, 8.0, 1.2);
    plot_axes(1.0, 0x404040);
    plot_fn(damped, 400, 0x40FF80);
    plot_text_at(4, 4, "exp(-x*x/8)*sin(2x)", 0xFFFFFF, 1, 0);
    pause_for_key("damped sine");

    /* A function with poles every pi. The breaks are the point. */
    plot_clear();
    plot_window(-6.0, -6.0, 6.0, 6.0);
    plot_axes(1.0, 0x404040);
    plot_fn(poles, 600, 0xFF6040);
    plot_text_at(4, 4, "tan x - note the breaks", 0xFFFFFF, 1, 0);
    pause_for_key("tangent");

    /* A function with a domain: nothing is drawn left of x = 1. */
    plot_clear();
    plot_window(-4.0, -1.0, 10.0, 4.0);
    plot_axes(1.0, 0x404040);
    plot_fn(root, 300, 0x6080FF);
    plot_text_at(4, 4, "sqrt(x-1), undefined below 1", 0xFFFFFF, 1, 0);
    pause_for_key("square root");

    /* Parametric. */
    plot_clear();
    plot_window(-4.0, -4.0, 4.0, 4.0);
    plot_axes(1.0, 0x303030);
    plot_param(lx, ly, 0.0, 6.2831853, 800, 0xFFFF40);
    plot_text_at(4, 4, "Lissajous 3:4", 0xFFFFFF, 1, 0);
    pause_for_key("parametric");

    /* Polar. */
    plot_clear();
    plot_window(-4.0, -4.0, 4.0, 4.0);
    plot_axes(1.0, 0x303030);
    plot_polar(rose, 0.0, 12.566371, 900, 0xFF40FF);
    plot_text_at(4, 4, "r = 3 cos(2.5a)", 0xFFFFFF, 1, 0);
    pause_for_key("polar rose");

    /* A surface, as a grid of lines drawn far rows first. */
    plot_clear();
    plot_window(-4.0, -2.0, 4.0, 5.0);
    plot_surface(hill, -8.0, -8.0, 8.0, 8.0, 32, 0x40FFC0);
    plot_text_at(4, 4, "4 sin(r)/r", 0xFFFFFF, 1, 0);
    pause_for_key("surface");

    plot_hide();
    printf("done\n");
    return 0;
}
