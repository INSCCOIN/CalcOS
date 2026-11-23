#!/usr/bin/env python3
"""End-to-end checks of the C runtime: compile, then execute the machine code.

Every one of these compiles real C with the on-device compiler and runs the
Thumb-2 it emits under Unicorn, with the runtime entry points modelled in
Python. A function that is merely declared in the table but takes its
arguments in the wrong registers fails here, which is the whole point:
nothing in this file would catch a typo if it only compiled the source.
"""
import sys
import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ccemu import run_source                      # noqa: E402

fails = []
count_total = 0


def check(cond, what):
    global count_total
    count_total += 1
    if not cond:
        fails.append(what)
        print("FAIL: " + what)


def case(name, src, fn="main", args=(), expect=None, double=False, then=None):
    try:
        m = run_source(src)
        got = m.call(fn, args, want_double=double)
    except Exception as e:                          # noqa: BLE001
        check(False, "%s: %s" % (name, e))
        return None
    if expect is not None:
        check(got == expect, "%s: returned %r, expected %r" % (name, got, expect))
    if then:
        then(m)
    return m


# ---------------------------------------------------------------- graphics

def _gfx(m):
    check(m.fb.get((5, 5)) == 0xFF0000, "plot_point stores the colour it was given")
    check(m.fb.get((40, 10)) == 0x00FF00, "plot_fill_rect fills its interior")
    check((39, 10) not in m.fb, "plot_fill_rect stops at its left edge")
    check((10, 10) in m.fb and (10, 29) in m.fb, "plot_rect draws its outline")
    check((11, 11) not in m.fb or m.fb[(11, 11)] != 0x0000FF,
          "plot_rect leaves the interior alone")
    check(m.fb.get((200, 100)) == 0xFFFF00, "plot_circle reaches its rightmost point")
    check(m.fb.get((150, 100)) == 0x00FFFF, "plot_fill_circle fills the centre")
    check(m.shown == 1, "plot_show presents the frame exactly once")
    check(m.texts == [(4, 300, "ok")], "plot_text passes its string through")


case("graphics", """
int main(void) {
    plot_clear();
    plot_point(5, 5, rgb(255, 0, 0));
    plot_fill_rect(40, 10, 20, 20, rgb(0, 255, 0));
    plot_rect(10, 10, 20, 20, rgb(0, 0, 255));
    plot_circle(150, 100, 50, rgb(255, 255, 0));
    plot_fill_circle(150, 100, 10, rgb(0, 255, 255));
    plot_text(4, 300, "ok", rgb(255, 255, 255));
    plot_show();
    return plot_width() + plot_height();
}
""", expect=640, then=_gfx)

case("plot_get reads a pixel back", """
int main(void) {
    plot_clear();
    plot_point(7, 9, 0x123456);
    return plot_get(7, 9) == 0x123456;
}
""", expect=1)

# ------------------------------------------------------------------ timing

case("millis advances and delay moves the clock", """
int main(void) {
    int a = millis();
    delay(50);
    int b = millis();
    return b - a >= 50;
}
""", expect=1)

case("micros is finer than millis", """
int main(void) {
    unsigned a = micros();
    unsigned b = micros();
    return b > a;
}
""", expect=1)

# -------------------------------------------------------------------- keys


def _keys(m):
    check(m.text() == "abc", "getchar returns the keys in order")


mm = run_source("""
int main(void) {
    int n = 0;
    while (kbhit()) { putchar(getchar()); n++; }
    return n;
}
""")
mm.keys = [ord("a"), ord("b"), ord("c")]
check(mm.call("main") == 3, "kbhit stops the loop when the queue empties")
_keys(mm)

mm = run_source("int main(void) { return getkey(); }")
check(mm.call("main") == -1, "getkey returns -1 when no key is waiting")

mm = run_source("""
int main(void) { char b[32]; int n = getline_(b, 32); return n + b[0]; }
""")
mm.lines = ["hi"]
check(mm.call("main") == 2 + ord("h"), "getline_ fills the buffer and returns the length")

# ------------------------------------------------------------------- sound

mm = case("tone", """
int main(void) { tone(440, 100); tone(880, 50); notone(); return 0; }
""", expect=0)
check(mm.tones == [(440, 100), (880, 50), (0, 0)],
      "tone passes frequency and duration through unchanged")

# ------------------------------------------------------------------- files

mm = run_source("""
int main(void) {
    void *f = fopen("/data.bin", "w");
    if (!f) return -1;
    int v[4];
    v[0] = 10; v[1] = 20; v[2] = 30; v[3] = 40;
    fwrite(v, 4, 4, f);
    fclose(f);

    int w[4];
    w[0] = 0;
    f = fopen("/data.bin", "r");
    if (!f) return -2;
    int n = fread(w, 4, 4, f);
    int size = fsize(f);
    fclose(f);
    return n * 1000 + w[2] + size;
}
""")
check(mm.call("main") == 4 * 1000 + 30 + 16,
      "a program writes a file, closes it and reads the same bytes back")
check(mm.files.get("/data.bin") is not None, "the file survives the program")

mm = run_source("""
int main(void) {
    void *f = fopen("/log.txt", "w");
    fputs("hello\\n", f);
    fputs("world\\n", f);
    fclose(f);
    f = fopen("/log.txt", "r");
    char line[32];
    int lines = 0;
    while (fgets(line, 32, f)) lines++;
    fclose(f);
    return lines;
}
""")
check(mm.call("main") == 2, "fgets walks a written file line by line")

mm = run_source("""
int main(void) {
    void *f = fopen("/seek.bin", "w");
    char s[8];
    s[0]='a'; s[1]='b'; s[2]='c'; s[3]='d';
    fwrite(s, 1, 4, f);
    fclose(f);
    f = fopen("/seek.bin", "r");
    fseek(f, 2, 0);
    char c;
    fread(&c, 1, 1, f);
    int at = ftell(f);
    int e = feof(f);
    fclose(f);
    return c * 100 + at * 10 + e;
}
""")
check(mm.call("main") == ord("c") * 100 + 3 * 10 + 0,
      "fseek, ftell and feof agree on a position in the middle of a file")

mm = run_source("""
int main(void) { return fopen("/missing.txt", "r") == 0; }
""")
check(mm.call("main") == 1, "opening a file that is not there returns null")

# --------------------------------------------------------------- more math

case("the added libm entries are callable", """
double f(void) {
    return exp2(3.0) + atanh(0.0) + copysign(2.0, -1.0) + ldexp(1.0, 3);
}
""", fn="f", expect=14.0, double=True)

# ------------------------------------------------------- the flash store

mm = run_source("""
int main(void) {
    int t[16], i;
    for (i = 0; i < 16; i++) t[i] = i * i;
    store_write(3, t, 64);
    int *p = (int *)store_read(3);
    return p[5] + p[12];
}
""")
check(mm.call("main") == 25 + 144,
      "a table written to the store reads back through a plain pointer")

mm = run_source("""
int main(void) { return store_pages() * 1000 + store_page_size() / 1024; }
""")
check(mm.call("main") == 128 * 1000 + 4,
      "the store reports its size in whole pages")

mm = run_source("""
int main(void) {
    char s[4]; s[0]='o'; s[1]='k'; s[2]=0;
    store_write(0, s, 3);
    store_erase(0);
    unsigned char *p = (unsigned char *)store_read(0);
    return p[0];
}
""")
check(mm.call("main") == 0xFF, "erasing a page leaves it reading as 0xFF")

mm = run_source("int main(void) { return store_read(9999) == 0; }")
check(mm.call("main") == 1, "a page number out of range reads as null")

# ------------------------------------------------- graphics helpers (2.2)

mm = run_source("""
int main(void) {
    plot_clear();
    plot_palette(1, rgb(255, 0, 0));
    unsigned char sp[2];
    sp[0] = 0x01; sp[1] = 0x10;      /* red, gap, gap, red */
    plot_sprite(10, 10, 4, 1, sp);
    return 0;
}
""")
mm.call("main")
check(mm.fb.get((10, 10)) == 0xFF0000, "a sprite draws its first pixel")
check((11, 10) not in mm.fb, "colour 0 in a sprite is transparent")
check(mm.fb.get((13, 10)) == 0xFF0000, "and the high nibble is the next pixel")

mm = run_source("""
int main(void) {
    plot_clear();
    plot_clip(0, 0, 12, 12);
    plot_fill_rect(0, 0, 100, 100, rgb(0, 0, 255));
    plot_noclip();
    plot_point(50, 50, rgb(0, 255, 0));
    return 0;
}
""")
mm.call("main")
check(mm.fb.get((11, 11)) == 0x0000FF, "drawing inside the clip rectangle lands")
check((12, 12) not in mm.fb, "and outside it does not")
check(mm.fb.get((50, 50)) == 0x00FF00, "clearing the clip lets drawing through again")

# --------------------------------------------------- numeric helpers (2.2)

mm = run_source("""
int main(void) {
    double A[4], b[2];
    A[0]=2; A[1]=1; A[2]=1; A[3]=3;
    b[0]=5; b[1]=10;
    if (!solve(A, b, 2)) return -1;
    return (int)(b[0] * 10 + b[1]);
}
""")
check(mm.call("main") == 13, "solve answers a 2x2 system")

mm = run_source("""
int main(void) {
    double A[4], b[2];
    A[0]=1; A[1]=2; A[2]=2; A[3]=4;     /* singular */
    b[0]=1; b[1]=2;
    return solve(A, b, 2);
}
""")
check(mm.call("main") == 0, "a singular matrix is reported, not divided by")

mm = run_source("""
int main(void) {
    double a[3], b[2], o[4];
    a[0]=1; a[1]=2; a[2]=3; b[0]=1; b[1]=1;
    convolve(a, 3, b, 2, o);
    return (int)(o[0]*1000 + o[1]*100 + o[2]*10 + o[3]);
}
""")
check(mm.call("main") == 1353, "convolve matches the hand-computed answer")

mm = run_source("""
int main(void) {
    double re[4], im[4];
    int i;
    for (i = 0; i < 4; i++) { re[i] = 1.0; im[i] = 0.0; }
    if (!fft(re, im, 4, -1)) return -1;
    /* a constant transforms to all its energy in bin 0 */
    return (int)(re[0] + 0.5) * 10 + (int)(re[1] + 0.5);
}
""")
check(mm.call("main") == 40, "the FFT puts a constant signal entirely in bin 0")

mm = run_source("int main(void) { double r[3], i[3]; return fft(r, i, 3, -1); }")
check(mm.call("main") == 0, "a length that is not a power of two is refused")

# --------------------------------------------------- the second core (2.2)

mm = run_source("""
static int out[16];
static void row(int i, void *ctx) { out[i] = ((int *)ctx)[0] * i + i; }
int main(void) {
    int ctx[1], s = 0, i;
    ctx[0] = 10;
    par_for(16, row, ctx);
    for (i = 0; i < 16; i++) s += out[i];
    return s;
}
""")
check(mm.call("main") == sum(10 * i + i for i in range(16)),
      "par_for runs the body once for every index, with its context")

mm = run_source("int main(void) { return cores() >= 1; }")
check(mm.call("main") == 1, "cores() says how many are available")

# ------------------------------------------------------ big integers (2.2)

mm = run_source("""
#include <stdio.h>
int main(void) {
    void *a = bignew(2), *p = bigpow(a, 100);
    char buf[64];
    bigstr(p, buf, 64);
    printf("%s/%d", buf, bigdigits(p));
    bigfree(a); bigfree(p);
    return 0;
}
""")
mm.call("main")
check(mm.text() == "%d/31" % (2 ** 100),
      "a program computes 2^100 exactly and prints all of it")

mm = run_source("""
int main(void) {
    void *a = bigparse("123456789012345678901234567890");
    void *b = bignew(65537), *m = bigparse("1000000000000000003");
    void *r = bigpowmod(a, b, m);
    char buf[64];
    bigstr(r, buf, 64);
    int n = bigdigits(r);
    bigfree(a); bigfree(b); bigfree(m); bigfree(r);
    return n;
}
""")
check(mm.call("main") == len(str(pow(123456789012345678901234567890, 65537,
                                     1000000000000000003))),
      "modular exponentiation of an RSA-sized number works from C")

print("   %d runtime checks on real machine code" % (count_total - len(fails)))
sys.exit(1 if fails else 0)
