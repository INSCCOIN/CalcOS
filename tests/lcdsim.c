/* A model of the ILI9488 as seen from the SPI bus, so the real LCD driver
 * can be exercised without the panel.
 *
 * This exists because of a bug that cost a boot: the driver was raising chip
 * select between a command and its parameters, so the controller discarded
 * every init parameter and stayed on its power-on defaults. Nothing in the
 * source looked wrong. Modelling the controller's own rules - CS frames a
 * transaction, parameters belong to the command that opened it - catches
 * that class of mistake on a workstation.
 */
#include "picocalc.h"
#include "settings.h"
#include "pico/stdlib.h"
#include "hardware/spi.h"
#include "hardware/gpio.h"
#include "hardware/clocks.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ------------------------------------------------------------------ */
/* the fake bus                                                        */
/* ------------------------------------------------------------------ */

struct spi_inst { int dummy; };
static struct spi_inst s0, s1;
spi_inst_t *spi0 = &s0, *spi1 = &s1;

Settings g_set;

static int pin_cs = 1, pin_dc = 0, pin_rst = 0;
static unsigned spi_bits = 8;
static unsigned spi_hz;

void sleep_ms(uint32_t ms) { (void)ms; }
uint32_t clock_get_hz(clock_handle_t c) { (void)c; return 150000000u; }
void gpio_init(unsigned pin) { (void)pin; }
void gpio_set_dir(unsigned pin, int out) { (void)pin; (void)out; }
void gpio_set_function(unsigned pin, gpio_function_t f) { (void)pin; (void)f; }
void gpio_pull_up(unsigned pin) { (void)pin; }
unsigned spi_init(spi_inst_t *spi, unsigned hz) { (void)spi; spi_hz = hz; return hz; }
unsigned spi_set_baudrate(spi_inst_t *spi, unsigned hz) { (void)spi; spi_hz = hz; return hz; }
void spi_set_format(spi_inst_t *spi, unsigned bits, spi_cpol_t a, spi_cpha_t b,
                    spi_order_t c)
{ (void)spi; (void)a; (void)b; (void)c; spi_bits = bits; }

/* ------------------------------------------------------------------ */
/* the controller model                                                */
/* ------------------------------------------------------------------ */

#define W 320
#define H 480                 /* the controller's own GRAM height */

typedef struct {
    uint16_t fb[H][W];
    int      x0, x1, y0, y1;      /* active window           */
    int      cx, cy;              /* RAM write cursor        */
    int      cmd;                 /* command in progress     */
    int      nparam;
    uint8_t  param[32];
    int      writing;             /* inside a 0x2C RAM write */
    int      pixfmt;              /* 0x55 or 0x66            */
    int      madctl;
    int      inverted;
    int      sleeping;
    int      display_on;
    int      pending18;           /* bytes buffered in 18-bit mode */
    uint8_t  buf18[3];
    /* faults */
    int      err_param_outside_transaction;
    int      err_data_no_command;
    int      err_write_outside_window;
    int      pixels_written;
} Panel;

static Panel p;

static void panel_reset(void)
{
    memset(&p, 0, sizeof p);
    /* power-on defaults: 18-bit pixels, no inversion, asleep, display off.
     * These are what the panel keeps if the init parameters never land. */
    p.pixfmt = 0x66;
    p.madctl = 0x00;
    p.sleeping = 1;
    p.x1 = W - 1;
    p.y1 = H - 1;
}

static void put_pixel(uint16_t c)
{
    if (p.cx < 0 || p.cx >= W || p.cy < 0 || p.cy >= H) {
        p.err_write_outside_window++;
        return;
    }
    p.fb[p.cy][p.cx] = c;
    p.pixels_written++;
    if (++p.cx > p.x1) {
        p.cx = p.x0;
        if (++p.cy > p.y1) p.cy = p.y0;
    }
}

static void finish_command(void)
{
    switch (p.cmd) {
    case 0x2A:
        if (p.nparam >= 4) {
            p.x0 = (p.param[0] << 8) | p.param[1];
            p.x1 = (p.param[2] << 8) | p.param[3];
        }
        break;
    case 0x2B:
        if (p.nparam >= 4) {
            p.y0 = (p.param[0] << 8) | p.param[1];
            p.y1 = (p.param[2] << 8) | p.param[3];
        }
        break;
    case 0x3A: if (p.nparam >= 1) p.pixfmt = p.param[0]; break;
    case 0x36: if (p.nparam >= 1) p.madctl = p.param[0]; break;
    default: break;
    }
}

static void bus_byte(uint8_t v)
{
    if (pin_cs) return;                       /* not selected: ignored */

    if (!pin_dc) {                            /* a command */
        finish_command();
        p.cmd = v;
        p.nparam = 0;
        p.writing = 0;
        p.pending18 = 0;
        switch (v) {
        case 0x2C:
            p.writing = 1;
            p.cx = p.x0;
            p.cy = p.y0;
            break;
        case 0x20: p.inverted = 0; break;
        case 0x21: p.inverted = 1; break;
        case 0x11: p.sleeping = 0; break;
        case 0x28: p.display_on = 0; break;
        case 0x29: p.display_on = 1; break;
        default: break;
        }
        return;
    }

    /* data */
    if (p.writing) {
        if (p.pixfmt == 0x66) {
            p.buf18[p.pending18++] = v;
            if (p.pending18 == 3) {
                uint16_t c = (uint16_t)(((p.buf18[0] & 0xF8) << 8)
                                      | ((p.buf18[1] & 0xFC) << 3)
                                      | (p.buf18[2] >> 3));
                put_pixel(c);
                p.pending18 = 0;
            }
        } else {
            static int hi = -1;
            if (hi < 0) hi = v;
            else { put_pixel((uint16_t)((hi << 8) | v)); hi = -1; }
        }
        return;
    }

    if (p.cmd == 0) { p.err_data_no_command++; return; }
    if (p.nparam < (int)sizeof p.param) p.param[p.nparam++] = v;
}

void gpio_put(unsigned pin, int value)
{
    if (pin == 13) {                          /* chip select */
        if (!pin_cs && value) {
            /* rising edge ends the transaction: a real controller forgets
             * the command in progress here */
            finish_command();
            p.cmd = 0;
            p.writing = 0;
            p.nparam = 0;
        }
        pin_cs = value;
        return;
    }
    if (pin == 14) { pin_dc = value; return; }
    if (pin == 15) { pin_rst = value; if (!value) panel_reset(); return; }
}

int spi_write_blocking(spi_inst_t *spi, const uint8_t *src, size_t len)
{
    size_t i;
    (void)spi;
    for (i = 0; i < len; i++) bus_byte(src[i]);
    return (int)len;
}

int spi_write16_blocking(spi_inst_t *spi, const uint16_t *src, size_t len)
{
    size_t i;
    (void)spi;
    if (spi_bits != 16) {
        printf("FAIL: 16-bit write while the bus is in %u-bit mode\n", spi_bits);
        exit(1);
    }
    for (i = 0; i < len; i++) {
        bus_byte((uint8_t)(src[i] >> 8));     /* MSB first */
        bus_byte((uint8_t)src[i]);
    }
    return (int)len;
}

/* ------------------------------------------------------------------ */
/* the tests                                                           */
/* ------------------------------------------------------------------ */

extern const unsigned char calcos_font_bold[95][CELL_H];

static int fails;

static void check(int cond, const char *what)
{
    if (!cond) { printf("FAIL: %s\n", what); fails++; }
}

static void settings_for_test(int bits)
{
    memset(&g_set, 0, sizeof g_set);
    g_set.fg = 0xC8E6C8u;
    g_set.bg = 0x080A08u;
    g_set.lcd_bits = bits;
    g_set.lcd_madctl = 0x48;
    g_set.lcd_invert = 1;
    g_set.lcd_spi_mhz = 0;
}

static void test_init(int bits)
{
    settings_for_test(bits);
    panel_reset();
    lcd_init();

    check(p.pixfmt == (bits == 18 ? 0x66 : 0x55),
          "pixel format reached the controller");
    check(p.madctl == 0x48, "MADCTL reached the controller");
    check(p.inverted == 1, "inversion was applied");
    check(p.sleeping == 0, "sleep out was applied");
    check(p.display_on == 1, "display on was applied");
    check(p.err_data_no_command == 0,
          "no parameters were sent outside a command");
    check(pin_cs == 1, "chip select is released when idle");
}

static void test_fill(void)
{
    int x, y, bad = 0;
    settings_for_test(16);
    panel_reset();
    lcd_init();
    lcd_fill(0, 0, LCD_W, LCD_H, 0xF800);
    for (y = 0; y < LCD_H; y++)
        for (x = 0; x < LCD_W; x++)
            if (p.fb[y][x] != 0xF800) bad++;
    check(bad == 0, "a full-screen fill covers every pixel");
    check(p.err_write_outside_window == 0, "fill stayed inside its window");
}

static void test_window_blit(void)
{
    static uint16_t line[LCD_W * CELL_H];
    int x, y, bad = 0;
    settings_for_test(16);
    panel_reset();
    lcd_init();
    for (y = 0; y < CELL_H; y++)
        for (x = 0; x < LCD_W; x++)
            line[y * LCD_W + x] = (uint16_t)(x + y);

    p.pixels_written = 0;
    lcd_window(0, 5 * CELL_H, LCD_W, CELL_H);
    lcd_blit(line, LCD_W * CELL_H);

    check(p.pixels_written == LCD_W * CELL_H,
          "every pixel of a row blit reaches the panel");
    for (y = 0; y < CELL_H; y++)
        for (x = 0; x < LCD_W; x++)
            if (p.fb[5 * CELL_H + y][x] != (uint16_t)(x + y)) bad++;
    check(bad == 0, "a row blit lands at the right place, in order");
}

/* Render a line of text exactly as the console does, and read it back. */
static void test_text(void)
{
    static uint16_t line[LCD_W * CELL_H];
    const char *msg = "CalcOS 1.2";
    uint16_t fg = rgb565(g_set.fg), bg = rgb565(g_set.bg);
    int c, x, y, ink = 0, wrong = 0;

    settings_for_test(16);
    panel_reset();
    lcd_init();
    fg = rgb565(g_set.fg);
    bg = rgb565(g_set.bg);

    for (y = 0; y < CELL_H; y++) {
        uint16_t *out = &line[y * LCD_W];
        for (c = 0; c < TERM_COLS; c++) {
            int ch = (c < (int)strlen(msg)) ? msg[c] : ' ';
            const unsigned char *g = calcos_font_bold[ch - 32];
            for (x = 0; x < CELL_W; x++)
                *out++ = (g[y] >> x) & 1 ? fg : bg;
        }
    }
    lcd_window(0, 0, LCD_W, CELL_H);
    lcd_blit(line, LCD_W * CELL_H);

    for (y = 0; y < CELL_H; y++)
        for (c = 0; c < (int)strlen(msg); c++)
            for (x = 0; x < CELL_W; x++) {
                int want = (calcos_font_bold[msg[c] - 32][y] >> x) & 1;
                uint16_t got = p.fb[y][c * CELL_W + x];
                if (got != (want ? fg : bg)) wrong++;
                if (want) ink++;
            }
    check(ink > 40, "the text actually has ink in it");
    check(wrong == 0, "every glyph pixel matches the font");
}

/* The failure that shipped: if CS is raised between a command and its
 * parameters, the controller keeps its defaults. Prove the model notices. */
static void test_model_catches_cs_bug(void)
{
    panel_reset();
    gpio_put(13, 0);
    gpio_put(14, 0);
    { uint8_t c = 0x3A; spi_write_blocking(spi1, &c, 1); }
    gpio_put(13, 1);                 /* the bug: transaction ends here */
    gpio_put(13, 0);
    gpio_put(14, 1);
    { uint8_t v = 0x55; spi_write_blocking(spi1, &v, 1); }
    gpio_put(13, 1);
    check(p.pixfmt == 0x66 && p.err_data_no_command > 0,
          "the model notices parameters orphaned by a CS pulse");
}

int main(void)
{
    printf("-- panel model\n");
    test_model_catches_cs_bug();
    test_init(16);
    test_init(18);
    test_fill();
    test_window_blit();
    test_text();
    if (fails == 0) printf("   driver drives the panel correctly (16 and 18 bit)\n");
    return fails ? 1 : 0;
}
