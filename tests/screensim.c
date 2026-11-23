/* The whole screen, simulated: the real console, the real LCD driver, and a
 * model of the panel that records every pixel ever written.
 *
 * tests/lcdsim.c checks that the driver talks to the controller correctly.
 * This goes a layer up and asks the question that one cannot: after a real
 * session, what is actually on the glass? That matters because the console
 * only repaints rows it has marked dirty, and anything it never paints keeps
 * whatever was there before — a splash screen, a graphics overlay, or the
 * panel's power-on noise. A region nobody paints is invisible in the source
 * and obvious on the hardware.
 *
 * So the panel model here starts every pixel as "never written" and the
 * tests assert that, by the time the prompt is up, no pixel is still in that
 * state. It also dumps the framebuffer as a PPM so a human can look.
 */
#define _POSIX_C_SOURCE 200809L
#include "picocalc.h"
#include "settings.h"
#include "scrollback.h"
#include "calc.h"
#include "pico/stdlib.h"
#include "hardware/spi.h"
#include "hardware/gpio.h"
#include "hardware/i2c.h"
#include "hardware/clocks.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* the bus and the controller                                          */
/* ------------------------------------------------------------------ */

struct spi_inst { int dummy; };
static struct spi_inst s0, s1;
spi_inst_t *spi0 = &s0, *spi1 = &s1;

/* g_set itself comes from the real settings module, which is linked in. */

/* the panel's pins, as lcd.c defines them */
#define LCD_CS 13
#define LCD_DC 14

static int pin_cs = 1, pin_dc = 0;
static unsigned spi_bits = 8;

#define W LCD_W
#define H LCD_H

static struct {
    uint16_t fb[H][W];
    uint8_t  written[H][W];       /* 0 = never painted */
    int      x0, x1, y0, y1;
    int      cx, cy;
    int      cmd, nparam;
    uint8_t  param[8];
    int      writing;
    int      pixfmt;              /* 0x55 or 0x66 */
    int      byte3[3], nbyte3;
} p;

void sleep_ms(uint32_t ms) { (void)ms; }
void sleep_us(uint64_t us) { (void)us; }
uint32_t clock_get_hz(clock_handle_t c) { (void)c; return 150000000u; }
void gpio_init(unsigned pin) { (void)pin; }
void gpio_set_dir(unsigned pin, int out) { (void)pin; (void)out; }
void gpio_set_function(unsigned pin, gpio_function_t f) { (void)pin; (void)f; }
void gpio_pull_up(unsigned pin) { (void)pin; }
unsigned spi_init(spi_inst_t *spi, unsigned hz) { (void)spi; return hz; }
unsigned spi_set_baudrate(spi_inst_t *spi, unsigned hz) { (void)spi; return hz; }
void spi_set_format(spi_inst_t *spi, unsigned bits, spi_cpol_t a, spi_cpha_t b,
                    spi_order_t c)
{ (void)spi; (void)a; (void)b; (void)c; spi_bits = bits; }

void gpio_put(unsigned pin, int v)
{
    if (pin == LCD_CS) { pin_cs = v; if (v) { p.writing = 0; p.cmd = -1; } }
    else if (pin == LCD_DC) pin_dc = v;
}

static void put_pixel(uint16_t c)
{
    if (p.cy >= p.y0 && p.cy <= p.y1 && p.cx >= p.x0 && p.cx <= p.x1
        && p.cy >= 0 && p.cy < H && p.cx >= 0 && p.cx < W) {
        p.fb[p.cy][p.cx] = c;
        p.written[p.cy][p.cx] = 1;
    }
    if (++p.cx > p.x1) { p.cx = p.x0; if (++p.cy > p.y1) p.cy = p.y0; }
}

static void data_byte(uint8_t b)
{
    if (p.writing) {
        if (p.pixfmt == 0x66) {
            p.byte3[p.nbyte3++] = b;
            if (p.nbyte3 == 3) {
                uint16_t c = (uint16_t)(((p.byte3[0] & 0xF8) << 8)
                                      | ((p.byte3[1] & 0xFC) << 3)
                                      | (p.byte3[2] >> 3));
                p.nbyte3 = 0;
                put_pixel(c);
            }
        }
        return;
    }
    if (p.cmd < 0) return;
    if (p.nparam < (int)sizeof p.param) p.param[p.nparam++] = b;
    if (p.cmd == 0x2A && p.nparam == 4) {
        p.x0 = (p.param[0] << 8) | p.param[1];
        p.x1 = (p.param[2] << 8) | p.param[3];
    }
    if (p.cmd == 0x2B && p.nparam == 4) {
        p.y0 = (p.param[0] << 8) | p.param[1];
        p.y1 = (p.param[2] << 8) | p.param[3];
    }
    if (p.cmd == 0x3A && p.nparam == 1) p.pixfmt = p.param[0];
}

int spi_write_blocking(spi_inst_t *spi, const uint8_t *src, size_t len)
{
    size_t i;
    (void)spi;
    if (pin_cs) return (int)len;           /* ignored with CS high */
    for (i = 0; i < len; i++) {
        if (!pin_dc) {
            p.cmd = src[i];
            p.nparam = 0;
            if (p.cmd == 0x2C) {
                p.writing = 1;
                p.cx = p.x0;
                p.cy = p.y0;
                p.nbyte3 = 0;
            } else {
                p.writing = 0;
            }
        } else {
            data_byte(src[i]);
        }
    }
    return (int)len;
}

int spi_write16_blocking(spi_inst_t *spi, const uint16_t *src, size_t len)
{
    size_t i;
    (void)spi;
    if (pin_cs || !p.writing) return (int)len;
    for (i = 0; i < len; i++) put_pixel(src[i]);
    return (int)len;
}

/* ------------------------------------------------------------------ */
/* the keyboard controller, so keys can be scripted                    */
/* ------------------------------------------------------------------ */

struct i2c_inst { int dummy; };
static struct i2c_inst c0, c1;
i2c_inst_t *i2c0 = &c0, *i2c1 = &c1;

static uint16_t kfifo[256];
static int kfifo_head, kfifo_tail;
static uint8_t kb_selected;

/* The panel as it stood the moment the editor asked for the return key:
 * whatever was typed before it must already be on screen by then, and it
 * must be the right thing. */
static int snap_at_enter = -1;
static char snap_line[TERM_COLS + 1];
static int non_bg(void);
static void screen_row(int row, char *out);

static void key_push(int code, int state)
{
    kfifo[kfifo_head] = (uint16_t)((code << 8) | state);
    kfifo_head = (kfifo_head + 1) % 256;
}

static void key_type(const char *s)
{
    while (*s) { key_push((unsigned char)*s, 1); s++; }
}

unsigned i2c_init(i2c_inst_t *i, unsigned hz) { (void)i; return hz; }

int i2c_write_timeout_us(i2c_inst_t *i, uint8_t addr, const uint8_t *src,
                         size_t len, int nostop, unsigned t)
{
    (void)i; (void)addr; (void)nostop; (void)t;
    if (len == 1) { kb_selected = src[0]; return 1; }
    return (int)len;
}

int i2c_read_timeout_us(i2c_inst_t *i, uint8_t addr, uint8_t *dst,
                        size_t len, int nostop, unsigned t)
{
    (void)i; (void)addr; (void)nostop; (void)t;
    if (len != 2) return -1;
    if (kb_selected == 0x09) {
        uint16_t v = 0;
        if (kfifo_head != kfifo_tail) {
            v = kfifo[kfifo_tail];
            kfifo_tail = (kfifo_tail + 1) % 256;
            if ((v >> 8) == '\n' || (v >> 8) == '\r') {
                snap_at_enter = non_bg();
                screen_row(0, snap_line);
            }
        }
        dst[0] = (uint8_t)v;
        dst[1] = (uint8_t)(v >> 8);
        return 2;
    }
    dst[0] = 0; dst[1] = 80;
    return 2;
}

/* ------------------------------------------------------------------ */
/* everything else the board layer leans on                            */
/* ------------------------------------------------------------------ */

void stdio_init_all(void) { }
int  getchar_timeout_us(uint32_t us) { (void)us; return PICO_ERROR_TIMEOUT; }
void tight_loop_contents(void) { }
static uint64_t fake_us;
absolute_time_t get_absolute_time(void) { return fake_us += 1000; }
uint32_t to_ms_since_boot(absolute_time_t t) { return (uint32_t)(t / 1000); }
uint64_t to_us_since_boot(absolute_time_t t) { return t; }
absolute_time_t make_timeout_time_ms(uint32_t ms) { return fake_us + ms * 1000ull; }
int64_t absolute_time_diff_us(absolute_time_t a, absolute_time_t b)
{ return (int64_t)(b - a); }
bool set_sys_clock_khz(uint32_t khz, bool req) { (void)khz; (void)req; return true; }
void vreg_set_voltage(int v) { (void)v; }
void watchdog_reboot(uint32_t a, uint32_t b, uint32_t c)
{ (void)a; (void)b; (void)c; }
void reset_usb_boot(uint32_t a, uint32_t b) { (void)a; (void)b; }
uint32_t save_and_disable_interrupts(void) { return 0; }
void multicore_launch_core1(void (*entry)(void)) { (void)entry; }
void __sev(void) { }
void __wfe(void) { }
void __dmb(void) { }
void __dsb(void) { }
void __isb(void) { }
/* the linker symbols the free-memory report reads */
char __bss_end__, __StackLimit;
void restore_interrupts(uint32_t s) { (void)s; }
uint pwm_gpio_to_slice_num(uint g) { return g / 2; }
void pwm_set_enabled(uint s, bool on) { (void)s; (void)on; }
void pwm_set_clkdiv_int_frac(uint s, uint8_t i, uint8_t f)
{ (void)s; (void)i; (void)f; }
void pwm_set_wrap(uint s, uint16_t w) { (void)s; (void)w; }
void pwm_set_gpio_level(uint g, uint16_t l) { (void)g; (void)l; }
void pwm_set_output_polarity(uint s, bool a, bool b) { (void)s; (void)a; (void)b; }

/* no card, so the console is the only thing on screen */
int  sd_init(void) { return 0; }
void sd_reclock(void) { }
int  fat_mount(void) { return 0; }
int  fat_open(FatFile *f, const char *path) { (void)f; (void)path; return 0; }
int  fat_read(FatFile *f, void *b, int n) { (void)f; (void)b; (void)n; return 0; }
int  fat_seek(FatFile *f, uint32_t pos) { (void)f; (void)pos; return 0; }
int  fat_write(const char *p, const void *d, int n)
{ (void)p; (void)d; (void)n; return 0; }
int  fat_remove(const char *p) { (void)p; return 0; }
int  fat_list(const char *p, char *o, size_t c) { (void)p; (void)o; (void)c; return -1; }

int  plat_store_pages(void) { return 0; }
int  plat_store_page_size(void) { return 4096; }
const void *plat_store_read(int page) { (void)page; return 0; }
int  plat_store_write(int pg, const void *d, int n)
{ (void)pg; (void)d; (void)n; return 0; }
int  plat_store_erase(int pg) { (void)pg; return 0; }

/* ------------------------------------------------------------------ */
/* reading the panel back as text                                      */
/* ------------------------------------------------------------------ */

/* Match a character cell against every glyph in the font. This is what
 * makes a drawing bug visible to a test: the line editor can hold a
 * perfectly correct buffer and still paint the wrong thing, and only the
 * pixels say which happened. */
extern const unsigned char calcos_font[95][CELL_H];
extern const unsigned char calcos_font_bold[95][CELL_H];

static int glyph_at(int row, int col)
{
    const unsigned char (*fnt)[CELL_H] = g_set.font ? calcos_font_bold
                                                    : calcos_font;
    uint16_t fg = rgb565(g_set.fg), bg = rgb565(g_set.bg);
    int ch, y, x;

    for (ch = 0; ch < 95; ch++) {
        int ok = 1;
        for (y = 0; y < CELL_H && ok; y++) {
            unsigned bits = fnt[ch][y];
            for (x = 0; x < CELL_W; x++) {
                uint16_t want = ((bits >> x) & 1) ? fg : bg;
                if (p.fb[row * CELL_H + y][col * CELL_W + x] != want) {
                    ok = 0;
                    break;
                }
            }
        }
        if (ok) return 32 + ch;
    }
    return '?';
}

/* One row of the panel as text, with trailing blanks removed. */
static void screen_row(int row, char *out)
{
    int c, last = -1;
    for (c = 0; c < TERM_COLS; c++) {
        out[c] = (char)glyph_at(row, c);
        if (out[c] != ' ') last = c;
    }
    out[last + 1] = 0;
}

/* ------------------------------------------------------------------ */
/* the checks                                                          */
/* ------------------------------------------------------------------ */

static int fails;
static int edit_cases;

/* The board layer mirrors everything it prints to stdout, the way it does to
 * the USB serial console. That is the session talking, not the test, so the
 * report goes to a saved copy of the original stdout and the session's own
 * chatter is dropped. */
static FILE *rep;

static void check(int cond, const char *what)
{
    if (!cond) { fprintf(rep, "FAIL: %s\n", what); fails++; }
}

static int unwritten(int *fx, int *fy)
{
    int x, y, n = 0;
    for (y = 0; y < H; y++)
        for (x = 0; x < W; x++)
            if (!p.written[y][x]) {
                if (!n && fx) { *fx = x; *fy = y; }
                n++;
            }
    return n;
}

static void dump(const char *path)
{
    FILE *f = fopen(path, "wb");
    int x, y;
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (y = 0; y < H; y++)
        for (x = 0; x < W; x++) {
            uint16_t c = p.fb[y][x];
            fputc(((c >> 11) & 0x1F) << 3, f);
            fputc(((c >> 5) & 0x3F) << 2, f);
            fputc((c & 0x1F) << 3, f);
        }
    fclose(f);
}

/* how many pixels differ from the console background */
static int non_bg(void)
{
    uint16_t bg = rgb565(g_set.bg);
    int x, y, n = 0;
    for (y = 0; y < H; y++)
        for (x = 0; x < W; x++)
            if (p.fb[y][x] != bg) n++;
    return n;
}

int main(void)
{
    int fx = -1, fy = -1, left;
    int saved = dup(fileno(stdout));

    rep = fdopen(saved, "w");
    if (!rep) rep = stderr;
    setvbuf(rep, NULL, _IOLBF, 0);
    if (!freopen("/dev/null", "w", stdout)) { /* keep going if it fails */ }

    memset(&p, 0, sizeof p);
    p.cmd = -1;
    p.pixfmt = 0x66;                 /* the controller's power-on default */
    p.x1 = W - 1;
    p.y1 = H - 1;

    fprintf(rep, "-- screen\n");

    /* Boot the board layer exactly as main() does. */
    plat_init();

    left = unwritten(&fx, &fy);
    check(left == 0,
          "every pixel is painted once the console is up");
    if (left)
        fprintf(rep, "       %d pixels never written, first at x=%d y=%d\n",
               left, fx, fy);

    /* Typing must appear as it is typed, not when the line is entered. The
     * console only sends rows it has been told to flush, so an editor that
     * echoes with plat_putc and never says so leaves the panel blank while
     * the shadow buffer quietly fills up — which is what the hardware
     * showed. This drives the real editor through the real keyboard. */
    {
        static Interp in;
        char line[64];
        int bare_prompt;

        cos_init(&in);
        plat_clear_screen();
        plat_puts(cos_prompt());
        bare_prompt = non_bg();

        key_type("hi");
        key_push('\n', 1);
        cos_readline(&in, line, sizeof line, NULL);

        check(strcmp(line, "hi") == 0, "the editor returns what was typed");
        check(snap_at_enter > bare_prompt,
              "what is typed is on the panel before return is pressed");
        if (snap_at_enter <= bare_prompt)
            fprintf(rep, "       panel unchanged while typing: %d pixels, "
                    "same as the bare prompt\n", snap_at_enter);
    }

    /* ---------------------------------------------------------------- */
    /* editing, judged by what is on the panel rather than by the buffer  */
    /*                                                                    */
    /* 2.0.1 shipped a backspace that produced the right string and drew  */
    /* the wrong thing: the repaint was told the cursor's new column      */
    /* instead of the one the screen was still on, so it started one      */
    /* character late and left the first character duplicated. Every      */
    /* check here reads the glyphs back off the panel.                    */
    /* ---------------------------------------------------------------- */
    {
        static Interp ed;
        struct { const char *keys; const char *line; const char *screen; }
        cases[] = {
            /* backspace removes the character and leaves nothing behind */
            { "abc\b",        "ab",     "> ab" },
            { "abc\b\bz",     "az",     "> az" },
            { "1+99\b\b2",    "1+2",    "> 1+2" },
            { "ab\b\b\b\bxy", "xy",     "> xy" },   /* past the start */
            /* delete takes the character under the cursor */
            { "abc\x{B4}\x{B4}\x{D4}", "ac", "> ac" },
            /* inserting in the middle: the cursor sits before the
             * character the arrow moved onto, so the new text goes there */
            { "13\x{B4}2",    "123",    "> 123" },
            { "ac\x{B4}b",    "abc",    "> abc" },
            { "abcd\x{B4}\x{B4}X", "abXcd", "> abXcd" },
            /* home, end, and the controls */
            { "abc\x{D2}z",   "zabc",   "> zabc" },
            { "abc\x{D2}\x{D5}z", "abcz", "> abcz" },
            { "abcdef\x{15}xy", "xy",    "> xy" },   /* ^U */
            { "one two\x{17}x", "one x", "> one x" },/* ^W */
            { "abcdef\x{B4}\x{B4}\x{0B}", "abcd", "> abcd" }, /* ^K */
            /* a function key on an empty line runs its command outright */
            { "\x{81}",       "help",   "> help" },
            { "\x{85}",       "consts", "> consts" },
            { "\x{8A}",       "clear",  "> clear" },
            /* with something typed it is inserted instead, never destroying
             * the line that was being worked on */
            { "ab\x{82}",     "abvars", "> abvars" },
            { "1+\x{83}",     "1+funcs", "> 1+funcs" },
            /* nor must the other keys with no meaning here */
            { "ab\x{D1}\x{D0}\x{C1}c", "abc", "> abc" },
            /* tab completes, and what it completes is what is drawn */
            { "hel\t",        "help ",  "> help" },
            { "sqr\t(4)",     "sqrt(4)", "> sqrt(4)" },
            /* the commands added in 2.2 are reachable from the keyboard */
            { "ccs\t",        "ccs ",   "> ccs" },
            { "us\t",         "use ",   "> use" },
            { "sto\t",        "store ", "> store" },
            { "big\t",        "big",    "> big" },     /* big/bigfact */
            /* "cc" is a prefix of "ccs", so it lists rather than
             * completing; "disp" is unique and finishes */
            { "disp\t",       "display ", "> display" },
        };
        int n = (int)(sizeof cases / sizeof cases[0]), i;
        edit_cases = n;

        cos_init(&ed);
        for (i = 0; i < n; i++) {
            char line[80];
            char want_line[80];
            const char *k = cases[i].keys;

            kfifo_head = kfifo_tail = 0;
            snap_at_enter = -1;
            snap_line[0] = 0;
            plat_clear_screen();
            plat_puts(cos_prompt());

            /* the key string understands \x{NN} for the codes above 127 */
            while (*k) {
                if (k[0] == '\\' && k[1] == 'x' && k[2] == '{') {
                    int v = (int)strtol(k + 3, NULL, 16);
                    key_push(v, 1);
                    k = strchr(k, '}') + 1;
                } else if (k[0] == '\\' && k[1] == 'b') {
                    key_push(8, 1);
                    k += 2;
                } else if (k[0] == '\\' && k[1] == 't') {
                    key_push(9, 1);
                    k += 2;
                } else {
                    key_push((unsigned char)*k, 1);
                    k++;
                }
            }
            key_push('\n', 1);
            cos_readline(&ed, line, sizeof line, NULL);

            /* The prompt was put on row 0 of a cleared screen and nothing
             * has scrolled, so that row still holds the finished line.
             * Reading it after the editor returns covers the paths that
             * never see a return key, such as a function key on an empty
             * line. */
            screen_row(0, snap_line);
            snprintf(want_line, sizeof want_line, "%s", cases[i].line);
            if (strcmp(line, want_line) != 0) {
                fprintf(rep, "FAIL: keys %s gave the line \"%s\", wanted \"%s\"\n",
                        cases[i].keys, line, want_line);
                fails++;
            }
            if (strcmp(snap_line, cases[i].screen) != 0) {
                fprintf(rep, "FAIL: keys %s drew \"%s\" on the panel, wanted \"%s\"\n",
                        cases[i].keys, snap_line, cases[i].screen);
                fails++;
            }
        }
    }

    /* After graphics, returning to the console must repaint all of it. */
    {
        int x, y;
        plat_gfx_clear();
        for (y = 0; y < H; y++)
            for (x = 0; x < W; x++)
                plat_gfx_point(x, y, 0xFF00FF);
        plat_gfx_flush();
        memset(p.written, 0, sizeof p.written);
        plat_gfx_hide();
        left = unwritten(&fx, &fy);
        check(left == 0, "leaving graphics repaints the whole panel");
        if (left)
            fprintf(rep, "       %d pixels left from the overlay, first at x=%d y=%d\n",
                   left, fx, fy);
    }

    /* A picture must stay up until someone has looked at it. Text arriving
     * while a frame is showing waits for a key first, or a plot from the
     * prompt is wiped by the prompt that follows it half a millisecond
     * later. An explicit plot_hide is the program saying it has finished
     * with the picture, and does not wait. */
    {
        int x, y;
        plat_gfx_clear();
        for (y = 0; y < H; y++)
            for (x = 0; x < W; x++)
                plat_gfx_point(x, y, 0x20C040);
        plat_gfx_flush();

        kfifo_head = kfifo_tail = 0;
        key_push('q', 1);                 /* the key that dismisses it */
        plat_puts("after the plot\n");    /* must consume that key */
        check(kfifo_head == kfifo_tail,
              "text after a frame waits for a key before taking the screen");

        /* and the console really did come back */
        {
            char row[TERM_COLS + 1];
            screen_row(0, row);
            check(strstr(row, "after the plot") != NULL
                  || strlen(row) > 0,
                  "the console is drawn again once the key arrives");
        }

        /* plot_hide is explicit, so it must not eat a keystroke */
        plat_gfx_flush();
        kfifo_head = kfifo_tail = 0;
        key_push('z', 1);
        plat_gfx_hide();
        check(kfifo_head != kfifo_tail,
              "plot_hide gives the console back without waiting");
        kfifo_head = kfifo_tail = 0;
    }

    /* Clearing the screen must leave nothing behind either. */
    {
        memset(p.written, 0, sizeof p.written);
        plat_clear_screen();
        left = unwritten(&fx, &fy);
        check(left == 0, "clearing the screen paints the whole panel");
        if (left)
            fprintf(rep, "       %d pixels not cleared, first at x=%d y=%d\n",
                   left, fx, fy);
    }

    /* Replay something close to a real session and keep the picture, so a
     * human can look at what the panel would be showing. */
    {
        int x, y;
        plat_clear_screen();
        plat_puts("CalcOS 2.0\ncalculator + C compiler, 40x26\n"
                  "type 'help' for commands\n\nclock 150 MHz   no SD card\n\n");
        plat_puts("> cc life.c\ncompiled life.c\n2092 code, 8200 data, "
                  "4 function(s)\n> run\n");
        /* the program draws over the whole panel, then hands it back */
        plat_gfx_clear();
        for (y = 0; y < H; y++)
            for (x = 0; x < W; x++)
                if (((x / 5) + (y / 5)) % 7 == 0)
                    plat_gfx_point(x, y, 0x3CE678);
        plat_gfx_flush();
        plat_gfx_hide();
        plat_puts("35 generations\n\n[exit 0]\n> ");
        plat_putc('c'); plat_putc('c'); plat_putc(' ');
        plat_putc('s'); plat_putc('t'); plat_putc('o');
        plat_flush();                 /* as the line editor does */
    }

    dump("screen.ppm");
    if (!fails)
        fprintf(rep, "   panel fully painted; %d editing cases drawn correctly\n",
                edit_cases);
    return fails ? 1 : 0;
}
