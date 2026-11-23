/* Conway's life on the panel.
 *
 * Shows the pieces an interactive program needs: a drawing loop, a frame
 * timer, and a key check that does not block.
 *
 *   cc /life.c      run
 *   any key stops it
 */

#define W 64
#define H 64
#define CELL 5

static char a[W * H];
static char b[W * H];

static int at(char *g, int x, int y)
{
    if (x < 0) x += W;
    if (x >= W) x -= W;
    if (y < 0) y += H;
    if (y >= H) y -= H;
    return g[y * W + x];
}

static void step(void)
{
    int x, y;
    for (y = 0; y < H; y++) {
        for (x = 0; x < W; x++) {
            int n = at(a, x-1, y-1) + at(a, x, y-1) + at(a, x+1, y-1)
                  + at(a, x-1, y)               + at(a, x+1, y)
                  + at(a, x-1, y+1) + at(a, x, y+1) + at(a, x+1, y+1);
            b[y * W + x] = (n == 3 || (n == 2 && a[y * W + x])) ? 1 : 0;
        }
    }
    for (x = 0; x < W * H; x++) a[x] = b[x];
}

static void draw(void)
{
    int x, y, on = rgb(60, 230, 120), off = rgb(8, 10, 8);
    plot_clear();
    for (y = 0; y < H; y++)
        for (x = 0; x < W; x++)
            if (a[y * W + x])
                plot_fill_rect(x * CELL, y * CELL, CELL - 1, CELL - 1, on);
    (void)off;
    plot_show();
}

int main(void)
{
    int i, gen = 0, seed = millis();

    /* a cheap scatter, seeded from the clock so each run differs */
    for (i = 0; i < W * H; i++) {
        seed = seed * 1103515245 + 12345;
        a[i] = ((seed >> 16) & 7) == 0 ? 1 : 0;
    }

    while (!kbhit()) {
        int t = millis();
        draw();
        step();
        gen++;
        t = 80 - (millis() - t);     /* aim for about 12 frames a second */
        if (t > 0) delay(t);
    }
    getkey();
    plot_hide();
    printf("%d generations\n", gen);
    return 0;
}
