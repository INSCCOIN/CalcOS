/* The Mandelbrot set, in the sixteen colours the overlay has.
 *
 * Run it from the prompt with:   cc /mandel.c    then    run
 *
 * The picture stays up until a key is pressed: the console waits before it
 * takes the screen back, so nothing has to ask for that here.
 */

static int palette[16];

static void build_palette(void)
{
    int i;
    palette[0] = rgb(0, 0, 0);
    for (i = 1; i < 16; i++) {
        int t = i * 17;
        palette[i] = rgb(t / 2, t, 255 - t);
    }
}

int main(void)
{
    int w = plot_width(), h = plot_height();
    int px, py, t0;

    build_palette();
    plot_clear();
    t0 = millis();

    for (py = 0; py < h; py++) {
        for (px = 0; px < w; px++) {
            double cr = -2.2 + 3.0 * px / w;
            double ci = -1.5 + 3.0 * py / h;
            double zr = 0.0, zi = 0.0;
            int n = 0;
            while (n < 60 && zr * zr + zi * zi < 4.0) {
                double nr = zr * zr - zi * zi + cr;
                zi = 2.0 * zr * zi + ci;
                zr = nr;
                n++;
            }
            if (n < 60) plot_point(px, py, palette[n % 15 + 1]);
        }
        if (kbhit()) break;          /* let a key cut it short */
    }
    plot_show();
    /* this printf is what brings the console back, and it waits for a key
     * first, so the picture is not wiped the instant it is finished */
    printf("drawn in %d ms\n", millis() - t0);
    return 0;
}
