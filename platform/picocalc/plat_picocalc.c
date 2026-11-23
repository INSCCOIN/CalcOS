/* PicoCalc board layer: console, graphics overlay, files, and main().
 *
 * The console keeps a character shadow and marks rows dirty; a flush renders
 * each dirty row into one 320x12 line buffer and sends it in a single
 * transfer. The graphics overlay is a 4-bit-per-pixel bitmap with a sixteen
 * colour palette, which costs 50 KB instead of the 200 KB a full RGB
 * framebuffer would need.
 */
#include "calc.h"
#include "cprog.h"
#include "picocalc.h"
#include "settings.h"
#include "scrollback.h"

#include "pico/stdlib.h"
#include "pico/bootrom.h"
#include "hardware/clocks.h"
#include "hardware/vreg.h"
#include "hardware/watchdog.h"
#include "hardware/sync.h"
#include "hardware/pwm.h"
#include "hardware/gpio.h"
#include "pico/multicore.h"

#include <string.h>
#include <stdio.h>
#include <malloc.h>

extern const unsigned char calcos_font[95][CELL_H];
extern const unsigned char calcos_font_bold[95][CELL_H];

static const unsigned char (*font)[CELL_H] = calcos_font_bold;

/* ------------------------------------------------------------------ */
/* console                                                             */
/* ------------------------------------------------------------------ */

static char     scr[TERM_ROWS][TERM_COLS];
static uint8_t  row_dirty[TERM_ROWS];
static int      cur_r, cur_c;
static uint16_t linebuf[LCD_W * CELL_H];
static int      gfx_showing;

/* The scrollback ring itself is in platform/scrollback.c. Here we only
 * track how far the reader has scrolled up from the live screen. */
static int      sb_view;          /* 0 = live, >0 = scrolled up */

/* The text of a line in the combined scrollback-then-screen sequence. */
static const char *virt_row(int i)
{
    int n = sb_count();
    if (i < 0) return NULL;
    if (i < n) return sb_at(i);
    if (i < n + TERM_ROWS) return scr[i - n];
    return NULL;
}

static void console_render_row(int r)
{
    int c, y, x;
    uint16_t fg = rgb565(g_set.fg), bg = rgb565(g_set.bg);
    const char *text = virt_row(sb_count() - sb_view + r);
    if (!text) text = scr[r];
    for (y = 0; y < CELL_H; y++) {
        uint16_t *out = &linebuf[y * LCD_W];
        for (c = 0; c < TERM_COLS; c++) {
            int ch = (unsigned char)text[c];
            const unsigned char *g;
            unsigned bits;
            if (ch < 32 || ch > 126) ch = ' ';
            g = font[ch - 32];
            bits = g[y];
            for (x = 0; x < CELL_W; x++)
                *out++ = (bits >> x) & 1 ? fg : bg;
        }
    }
    lcd_window(0, r * CELL_H, LCD_W, CELL_H);
    lcd_blit(linebuf, LCD_W * CELL_H);
}

/* The character rows do not divide the panel exactly: 26 rows of 12 pixels
 * leave 8 rows over at the bottom. Nothing else paints them, so whatever was
 * there last — a splash screen, a graphics overlay, the panel's power-on
 * noise — stays visible as a band along the bottom edge. This flag makes the
 * console repaint that band whenever it takes the screen back. */
static int spare_dirty = 1;

static void console_flush(void)
{
    int r;
    for (r = 0; r < TERM_ROWS; r++) {
        if (!row_dirty[r]) continue;
        console_render_row(r);
        row_dirty[r] = 0;
    }
#if TERM_SPARE > 0
    if (spare_dirty) {
        lcd_fill(0, TERM_ROWS * CELL_H, LCD_W, TERM_SPARE, rgb565(g_set.bg));
        spare_dirty = 0;
    }
#endif
}

static void console_clear(void)
{
    int r;
    memset(scr, ' ', sizeof scr);
    for (r = 0; r < TERM_ROWS; r++) row_dirty[r] = 1;
    cur_r = cur_c = 0;
    spare_dirty = 1;
}

static void console_scroll(void)
{
    int r;
    sb_push(scr[0]);          /* keep the line that is about to be lost */
    memmove(&scr[0][0], &scr[1][0], (size_t)(TERM_ROWS - 1) * TERM_COLS);
    memset(&scr[TERM_ROWS - 1][0], ' ', TERM_COLS);
    for (r = 0; r < TERM_ROWS; r++) row_dirty[r] = 1;
    cur_r = TERM_ROWS - 1;
}

/* Set when a frame has been presented and not yet looked at. A picture is
 * worth nothing if the next line of text wipes it half a millisecond later,
 * which is what a plot from the prompt used to do: draw, and vanish as the
 * prompt came back. So the console waits for a key before taking the screen
 * back. A program that calls plot_hide is saying it has finished with the
 * picture itself, and that does not wait. */
static int gfx_unseen;

static void leave_gfx(void)
{
    int r;
    if (!gfx_showing) return;
    if (gfx_unseen) {
        gfx_unseen = 0;
        (void)plat_getc();        /* hold the picture until a key arrives */
    }
    gfx_showing = 0;
    for (r = 0; r < TERM_ROWS; r++) row_dirty[r] = 1;
    spare_dirty = 1;              /* the overlay covered the whole panel */
    console_flush();
}

/* Any output snaps the view back to the live screen: text appearing
 * somewhere the reader cannot see would be worse than losing their place. */
static void console_live(void)
{
    int r;
    if (!sb_view) return;
    sb_view = 0;
    for (r = 0; r < TERM_ROWS; r++) row_dirty[r] = 1;
}

void plat_scrollback(int screens)
{
    int r, want = sb_view + screens * (TERM_ROWS - 2);
    if (want < 0) want = 0;
    if (want > sb_count()) want = sb_count();
    if (want == sb_view) return;
    sb_view = want;
    for (r = 0; r < TERM_ROWS; r++) row_dirty[r] = 1;
    console_flush();
}

void plat_clear_screen(void)
{
    console_clear();
    console_flush();
}

static void console_putc(char ch)
{
    leave_gfx();
    console_live();
    if (ch == '\r') { cur_c = 0; return; }
    if (ch == '\n') {
        cur_c = 0;
        if (++cur_r >= TERM_ROWS) console_scroll();
        /* a finished line is worth showing: a program that prints with
         * putchar alone would otherwise stay invisible */
        console_flush();
        return;
    }
    if (ch == '\b') {
        /* Move the cursor left without erasing: the line editor repaints
         * what it wants and blanks the rest itself. At column zero it
         * steps up a row, which is what makes editing a line that has
         * wrapped behave. */
        if (cur_c > 0) cur_c--;
        else if (cur_r > 0) { cur_r--; cur_c = TERM_COLS - 1; }
        return;
    }
    if (ch == '\t') {
        int k = 4 - (cur_c % 4);
        while (k--) console_putc(' ');
        return;
    }
    if (ch < 32 || ch > 126) return;
    scr[cur_r][cur_c] = ch;
    row_dirty[cur_r] = 1;
    if (++cur_c >= TERM_COLS) {
        cur_c = 0;
        if (++cur_r >= TERM_ROWS) console_scroll();
    }
}

/* ------------------------------------------------------------------ */
/* HAL: console                                                        */
/* ------------------------------------------------------------------ */

void plat_putc(char c) { console_putc(c); putchar(c); }

/* Send whatever has been written but not yet drawn.
 *
 * plat_putc only marks its row dirty, because flushing per character would
 * blit a whole 320x12 row over SPI for every letter of every printf. The
 * line editor echoes with plat_putc, so it has to say when the screen should
 * catch up — once per keystroke, which is free at typing speed. */
void plat_flush(void)
{
    console_flush();
    fflush(stdout);
}

void plat_puts(const char *s)
{
    const char *p = s;
    while (*p) console_putc(*p++);
    console_flush();
    fputs(s, stdout);               /* mirror to USB serial */
}

int plat_cols(void) { return TERM_COLS; }
int plat_rows(void) { return TERM_ROWS; }
void plat_poll(void) { tight_loop_contents(); }

size_t plat_free_mem(void)
{
    extern char __StackLimit, __bss_end__;
    struct mallinfo mi = mallinfo();
    size_t arena = (size_t)(&__StackLimit - &__bss_end__);
    return arena > mi.uordblks ? arena - mi.uordblks : 0;
}

/* ------------------------------------------------------------------ */
/* HAL: time                                                           */
/* ------------------------------------------------------------------ */

uint32_t plat_millis(void) { return to_ms_since_boot(get_absolute_time()); }
uint64_t plat_micros(void) { return to_us_since_boot(get_absolute_time()); }

void plat_delay_ms(int ms)
{
    absolute_time_t end;
    if (ms <= 0) return;
    end = make_timeout_time_ms((uint32_t)ms);
    /* Sleeping in short steps keeps the USB stack serviced, so a long
     * delay in a compiled program does not drop the serial console. */
    while (absolute_time_diff_us(get_absolute_time(), end) > 0) {
        int64_t left = absolute_time_diff_us(get_absolute_time(), end);
        sleep_us(left > 2000 ? 2000 : (uint64_t)left);
    }
}

/* ------------------------------------------------------------------ */
/* HAL: non-blocking keyboard                                          */
/* ------------------------------------------------------------------ */

/* One key of lookahead, so key_ready can answer without losing it. */
static int key_peeked = -1;

int plat_getkey(void)
{
    int k;
    if (key_peeked >= 0) { k = key_peeked; key_peeked = -1; return k; }
    k = kbd_poll();
    if (k > 0) return k;
    k = getchar_timeout_us(0);
    if (k != PICO_ERROR_TIMEOUT) return k;
    return -1;
}

int plat_key_ready(void)
{
    if (key_peeked >= 0) return 1;
    key_peeked = plat_getkey();
    return key_peeked >= 0;
}

/* ------------------------------------------------------------------ */
/* HAL: sound                                                          */
/* ------------------------------------------------------------------ */

/* The PicoCalc wires the buzzer to GP26 and GP27 as a pair; driving both
 * slices in antiphase doubles the swing across the transducer. */
#define SND_L 26
#define SND_R 27

static int snd_ready;

static void snd_init(void)
{
    if (snd_ready) return;
    gpio_set_function(SND_L, GPIO_FUNC_PWM);
    gpio_set_function(SND_R, GPIO_FUNC_PWM);
    snd_ready = 1;
}

void plat_tone_off(void)
{
    if (!snd_ready) return;
    pwm_set_enabled(pwm_gpio_to_slice_num(SND_L), false);
    pwm_set_enabled(pwm_gpio_to_slice_num(SND_R), false);
}

/* Wait out a note, but stay answerable.
 *
 * A note is the one place the firmware blocks for a length of time chosen by
 * a program, so it is the one place a program can appear to lock the machine
 * up. The wait ends early if a key is waiting — the key is left in the queue
 * for whoever asked — and the length is capped, so no single call can hold
 * the board for longer than a few seconds however it was written. */
#define TONE_MAX_MS 5000

static void tone_wait(int ms)
{
    absolute_time_t end;
    if (ms <= 0) return;
    if (ms > TONE_MAX_MS) ms = TONE_MAX_MS;
    end = make_timeout_time_ms((uint32_t)ms);
    while (absolute_time_diff_us(get_absolute_time(), end) > 0) {
        if (plat_key_ready()) return;
        sleep_us(1000);
    }
}

void plat_tone(int hz, int ms)
{
    uint slice_l, slice_r;
    uint32_t clk, div_q4, top;

    if (hz <= 0) { plat_tone_off(); tone_wait(ms); return; }
    if (hz < 20) hz = 20;
    if (hz > 20000) hz = 20000;
    snd_init();

    /* Pick a divider that puts the period inside the 16-bit counter, then
     * set TOP for the exact frequency. The divider is 8.4 fixed point. */
    clk = (uint32_t)clock_get_hz(clk_sys);
    div_q4 = (uint32_t)(((uint64_t)clk * 16) / ((uint64_t)hz * 65536u)) + 1;
    if (div_q4 < 16) div_q4 = 16;
    if (div_q4 > 255 * 16 + 15) div_q4 = 255 * 16 + 15;
    top = (uint32_t)(((uint64_t)clk * 16) / ((uint64_t)hz * div_q4));
    if (top < 2) top = 2;
    if (top > 65535) top = 65535;

    slice_l = pwm_gpio_to_slice_num(SND_L);
    slice_r = pwm_gpio_to_slice_num(SND_R);
    pwm_set_clkdiv_int_frac(slice_l, div_q4 >> 4, div_q4 & 15);
    pwm_set_clkdiv_int_frac(slice_r, div_q4 >> 4, div_q4 & 15);
    pwm_set_wrap(slice_l, (uint16_t)top);
    pwm_set_wrap(slice_r, (uint16_t)top);
    pwm_set_gpio_level(SND_L, (uint16_t)(top / 2));
    pwm_set_gpio_level(SND_R, (uint16_t)(top / 2));
    pwm_set_output_polarity(slice_r, true, true);   /* antiphase */
    pwm_set_enabled(slice_l, true);
    pwm_set_enabled(slice_r, true);

    if (ms > 0) { tone_wait(ms); plat_tone_off(); }
}

/* ------------------------------------------------------------------ */
/* HAL: the second core                                                */
/* ------------------------------------------------------------------ */

/* Core 1 sits in a loop waiting for work. The indices are split between
 * the cores statically — core 0 takes the even ones, core 1 the odd — so
 * there is no shared counter to claim and therefore nothing to lock. For
 * the work this is used on, rows of a matrix or of a picture, alternating
 * indices also balances the load better than halving the range would.
 *
 * Starting core 1 is deferred until something actually asks for it: a
 * calculator that never runs a parallel loop should not pay for a second
 * core sitting in a wait loop. */
static struct {
    void (*fn)(int, void *);
    void *ctx;
    volatile int n;
    volatile int go;
    volatile int done;
} job;

static int core1_up;
static volatile int par_active;

static void core1_worker(void)
{
    for (;;) {
        while (!job.go) __wfe();
        __dmb();
        {
            int i;
            for (i = 1; i < job.n; i += 2) job.fn(i, job.ctx);
        }
        __dmb();
        job.done = 1;
        __sev();
        while (job.go) __wfe();      /* wait for core 0 to clear the job */
    }
}

int plat_par_cores(void) { return core1_up ? 2 : 1; }
int plat_par_active(void) { return par_active; }

void plat_par_for(int n, void (*fn)(int, void *), void *ctx)
{
    int i;

    if (n <= 0 || !fn) return;

    /* One index, or a nested call, stays on this core: nesting would need
     * a second worker and there is not one. */
    if (n == 1 || par_active) {
        for (i = 0; i < n; i++) fn(i, ctx);
        return;
    }

    if (!core1_up) {
        multicore_launch_core1(core1_worker);
        core1_up = 1;
        sleep_us(200);               /* let it reach the wait loop */
    }

    par_active = 1;
    job.fn = fn;
    job.ctx = ctx;
    job.n = n;
    job.done = 0;
    __dmb();
    job.go = 1;
    __sev();

    for (i = 0; i < n; i += 2) fn(i, ctx);

    while (!job.done) __wfe();
    __dmb();
    job.go = 0;
    __sev();
    par_active = 0;
}

/* ------------------------------------------------------------------ */
/* HAL: power                                                          */
/* ------------------------------------------------------------------ */

void plat_reboot(void)
{
    plat_tone_off();
    plat_puts("\nrestarting...\n");
    console_flush();
    sleep_ms(120);
    watchdog_reboot(0, 0, 0);
    for (;;) tight_loop_contents();
}

void plat_poweroff(void)
{
    plat_tone_off();
    plat_puts("\npowering down...\n");
    console_flush();
    sleep_ms(120);
    kbd_set_backlight(0);
    kbd_set_kbd_backlight(0);
    lcd_sleep();
    /* The keyboard controller owns the power rail; if it declines, the
     * best we can do is park the core with everything dark. */
    kbd_poweroff();
    for (;;) __wfi();
}

int plat_getc(void)
{
    for (;;) {
        int k = plat_getkey();
        if (k >= 0) return k;
        sleep_ms(4);
    }
}

/* Plain line input for a compiled program calling getline_: echo,
 * backspace, enter. The shell's own editor lives in src/lineedit.c. */
int plat_readline(char *buf, size_t n, const char *prompt)
{
    size_t len = 0;
    int blink = 0, cursor_on = 0;
    if (prompt) plat_puts(prompt);
    console_flush();
    for (;;) {
        int k = plat_getkey();
        if (k <= 0) {
            if (++blink > 40) {             /* a soft block cursor */
                blink = 0;
                cursor_on = !cursor_on;
                if (!gfx_showing) {
                    scr[cur_r][cur_c] = cursor_on ? '_' : ' ';
                    row_dirty[cur_r] = 1;
                    console_flush();
                }
            }
            sleep_ms(4);
            continue;
        }
        if (cursor_on) {
            scr[cur_r][cur_c] = ' ';
            row_dirty[cur_r] = 1;
            cursor_on = 0;
        }
        if (k == '\r' || k == '\n') {
            plat_putc('\n');
            console_flush();
            buf[len] = 0;
            return (int)len;
        }
        if (k == 8 || k == 127) {
            if (len) {
                len--;
                plat_puts("\b \b");
                console_flush();
            }
            continue;
        }
        if (k == 3) {                        /* ctrl-C clears the line */
            while (len) { len--; plat_puts("\b \b"); }
            console_flush();
            continue;
        }
        if (k >= 32 && k < 127 && len + 1 < n) {
            buf[len++] = (char)k;
            plat_putc((char)k);
            console_flush();
        }
    }
}

/* ------------------------------------------------------------------ */
/* HAL: graphics overlay (2 bits per pixel)                            */
/* ------------------------------------------------------------------ */

/* Four bits per pixel: 16 colours over 320x320 costs 50 KB, against the
 * 200 KB a full RGB framebuffer would need. Entry 0 is the background and
 * the rest are claimed on first use, so a program that draws in four
 * colours behaves exactly as before and one that wants sixteen can have
 * them. */
#define GFX_STRIDE (LCD_W / 2)            /* 2 pixels per byte */
#define GFX_NPAL   16
static uint8_t  gfx[LCD_H * GFX_STRIDE];
static unsigned gfx_pal[GFX_NPAL] = {
    0x080A08u, 0x30E060u, 0x707070u, 0xE0E0E0u
};
static int      gfx_used = 1;             /* palette entries claimed */

int plat_gfx_width(void)   { return LCD_W; }
int plat_gfx_height(void)  { return LCD_H; }
int plat_gfx_colours(void) { return GFX_NPAL; }

static int pal_index(unsigned rgb)
{
    int i;
    rgb &= 0xFFFFFFu;
    for (i = 0; i < gfx_used; i++)
        if (gfx_pal[i] == rgb) return i;
    if (gfx_used < GFX_NPAL) {
        gfx_pal[gfx_used] = rgb;
        return gfx_used++;
    }
    /* Palette full: take the nearest entry rather than a wrong one. */
    {
        int best = 1; long bd = -1;
        for (i = 1; i < GFX_NPAL; i++) {
            long dr = (long)((rgb >> 16) & 0xFF) - (long)((gfx_pal[i] >> 16) & 0xFF);
            long dg = (long)((rgb >> 8) & 0xFF) - (long)((gfx_pal[i] >> 8) & 0xFF);
            long db = (long)(rgb & 0xFF) - (long)(gfx_pal[i] & 0xFF);
            long d = dr * dr + dg * dg + db * db;
            if (bd < 0 || d < bd) { bd = d; best = i; }
        }
        return best;
    }
}

void plat_gfx_clear(void)
{
    memset(gfx, 0, sizeof gfx);
    gfx_pal[0] = g_set.bg;
    gfx_pal[1] = 0x30E060u;
    gfx_pal[2] = 0x707070u;
    gfx_pal[3] = 0xE0E0E0u;
    gfx_used = 4;
}

void plat_gfx_raw_point(int x, int y, unsigned rgb)
{
    int idx, sh;
    uint8_t *p;
    if (x < 0 || y < 0 || x >= LCD_W || y >= LCD_H) return;
    idx = pal_index(rgb);
    p = &gfx[y * GFX_STRIDE + (x >> 1)];
    sh = (x & 1) * 4;
    *p = (uint8_t)((*p & ~(0x0Fu << sh)) | ((unsigned)idx << sh));
}

int plat_gfx_get(int x, int y)
{
    const uint8_t *p;
    if (x < 0 || y < 0 || x >= LCD_W || y >= LCD_H) return -1;
    p = &gfx[y * GFX_STRIDE + (x >> 1)];
    return (int)gfx_pal[(*p >> ((x & 1) * 4)) & 0x0F];
}


void plat_gfx_text(int x, int y, const char *s, unsigned rgb)
{
    for (; *s; s++, x += CELL_W) {
        int c = (unsigned char)*s, r, col;
        if (c < 32 || c > 126) continue;
        for (r = 0; r < CELL_H; r++)
            for (col = 0; col < CELL_W; col++)
                if ((font[c - 32][r] >> col) & 1)
                    plat_gfx_point(x + col, y + r, rgb);
    }
}

void plat_gfx_flush(void)
{
    int y, x, band;
    uint16_t pal16[GFX_NPAL];
    for (x = 0; x < GFX_NPAL; x++) pal16[x] = rgb565(gfx_pal[x]);
    /* The panel height is not a whole number of cells, so the last band is
     * short; without the clamp this would read past the overlay and address
     * rows the panel does not have. */
    for (band = 0; band < LCD_H; band += CELL_H) {
        int hh = (band + CELL_H <= LCD_H) ? CELL_H : (LCD_H - band);
        uint16_t *out = linebuf;
        for (y = band; y < band + hh; y++) {
            const uint8_t *row = &gfx[y * GFX_STRIDE];
            for (x = 0; x < LCD_W; x += 2) {
                uint8_t b = row[x >> 1];
                *out++ = pal16[b & 0x0F];
                *out++ = pal16[b >> 4];
            }
        }
        lcd_window(0, band, LCD_W, hh);
        lcd_blit(linebuf, LCD_W * hh);
    }
    gfx_showing = 1;
    gfx_unseen = 1;
}

/* Put the console back on screen without disturbing the overlay, so a
 * program can alternate between drawing and printing. */
void plat_gfx_hide(void)
{
    int r;
    if (!gfx_showing) return;
    gfx_showing = 0;
    gfx_unseen = 0;               /* asked for explicitly: no waiting */
    for (r = 0; r < TERM_ROWS; r++) row_dirty[r] = 1;
    spare_dirty = 1;              /* the overlay covered the whole panel */
    console_flush();
}

void plat_sync_icache(const void *addr, size_t len)
{
    (void)addr;
    (void)len;
    __dsb();
    __isb();
}

/* ------------------------------------------------------------------ */
/* HAL: files                                                          */
/* ------------------------------------------------------------------ */

/* A small stdio-alike over the FAT driver.
 *
 * Reads stream straight off the card. Writes are different: the card writer
 * replaces a file whole, because a FAT append has to rewrite the cluster
 * chain and the directory entry, and a half-finished append on a calculator
 * that can lose power is how a card gets corrupted. So a file opened for
 * writing accumulates in RAM and is committed in one go by fclose, and a
 * write past the buffer fails rather than silently truncating.
 *
 * Long names are fine: the writer creates them, and an existing file keeps
 * whatever name it already had. */
#define PF_MAX     4
#define PF_WRBUF   (16 * 1024)

typedef struct {
    int      used;
    int      writing;
    int      failed;
    FatFile  rf;
    char     path[64];
    uint8_t *wbuf;
    int      wlen, wcap;
} PlatFile;

static PlatFile pfiles[PF_MAX];
static uint8_t  wrbuf[PF_WRBUF];
static int      wrbuf_taken;

void *plat_fopen(const char *path, const char *mode)
{
    int i, writing = mode && (*mode == 'w' || *mode == 'a');
    for (i = 0; i < PF_MAX; i++) if (!pfiles[i].used) break;
    if (i == PF_MAX) return NULL;
    memset(&pfiles[i], 0, sizeof pfiles[i]);

    if (writing) {
        if (wrbuf_taken) return NULL;       /* one writer at a time */
        wrbuf_taken = 1;
        pfiles[i].wbuf = wrbuf;
        pfiles[i].wcap = PF_WRBUF;
        pfiles[i].wlen = 0;
        pfiles[i].writing = 1;
        snprintf(pfiles[i].path, sizeof pfiles[i].path, "%s", path);
        if (*mode == 'a') {                 /* append: start from what is there */
            FatFile old;
            if (fat_open(&old, path)) {
                int got = fat_read(&old, wrbuf, PF_WRBUF);
                pfiles[i].wlen = got > 0 ? got : 0;
            }
        }
    } else {
        if (!fat_open(&pfiles[i].rf, path)) return NULL;
    }
    pfiles[i].used = 1;
    return &pfiles[i];
}

int plat_fread(void *vf, void *buf, int len)
{
    PlatFile *f = (PlatFile *)vf;
    if (!f || !f->used || f->writing || len <= 0) return 0;
    return fat_read(&f->rf, buf, len);
}

int plat_fwrite(void *vf, const void *buf, int len)
{
    PlatFile *f = (PlatFile *)vf;
    if (!f || !f->used || !f->writing || len <= 0) return 0;
    if (f->wlen + len > f->wcap) { f->failed = 1; return 0; }
    memcpy(f->wbuf + f->wlen, buf, (size_t)len);
    f->wlen += len;
    return len;
}

int plat_fgets(void *vf, char *buf, size_t n)
{
    PlatFile *f = (PlatFile *)vf;
    size_t k = 0;
    char c;
    if (!f || !f->used || f->writing) { if (n) buf[0] = 0; return 0; }
    while (k + 1 < n) {
        if (fat_read(&f->rf, &c, 1) != 1) break;
        buf[k++] = c;
        if (c == '\n') break;
    }
    buf[k] = 0;
    return k > 0;
}

int plat_fputs(void *vf, const char *s)
{
    int n = (int)strlen(s);
    return plat_fwrite(vf, s, n) == n ? n : -1;
}

int plat_fseek(void *vf, long off, int whence)
{
    PlatFile *f = (PlatFile *)vf;
    long base;
    if (!f || !f->used) return -1;
    if (f->writing) {
        base = (whence == 1) ? f->wlen : (whence == 2) ? f->wlen : 0;
        base += off;
        if (base < 0 || base > f->wcap) return -1;
        if (base > f->wlen) memset(f->wbuf + f->wlen, 0, (size_t)(base - f->wlen));
        f->wlen = (int)base;
        return 0;
    }
    base = (whence == 1) ? (long)f->rf.pos : (whence == 2) ? (long)f->rf.size : 0;
    base += off;
    if (base < 0) return -1;
    return fat_seek(&f->rf, (uint32_t)base) ? 0 : -1;
}

long plat_ftell(void *vf)
{
    PlatFile *f = (PlatFile *)vf;
    if (!f || !f->used) return -1;
    return f->writing ? f->wlen : (long)f->rf.pos;
}

long plat_fsize(void *vf)
{
    PlatFile *f = (PlatFile *)vf;
    if (!f || !f->used) return -1;
    return f->writing ? f->wlen : (long)f->rf.size;
}

int plat_feof(void *vf)
{
    PlatFile *f = (PlatFile *)vf;
    if (!f || !f->used) return 1;
    return f->writing ? 0 : (f->rf.pos >= f->rf.size);
}

void plat_fclose(void *vf)
{
    PlatFile *f = (PlatFile *)vf;
    if (!f || !f->used) return;
    if (f->writing) {
        if (!f->failed) fat_write(f->path, f->wbuf, f->wlen);
        wrbuf_taken = 0;
    }
    f->used = 0;
}

int plat_remove(const char *path) { return fat_remove(path); }

int plat_write_file(const char *path, const void *data, int len)
{
    return fat_write(path, data, len);
}

int plat_listdir(const char *path, char *out, size_t n)
{
    return fat_list(path, out, n);
}

/* ------------------------------------------------------------------ */
/* clock                                                               */
/* ------------------------------------------------------------------ */

static int actual_khz;

/* A staged boot log on USB serial. If the panel stays black this is the only
 * thing that says how far the machine got, so it goes out before the display
 * is touched and does not depend on it. */
static void boot_log(const char *stage)
{
    printf("[calcos] %s\n", stage);
    fflush(stdout);
}

/* The part runs at 150 MHz by default. Anything higher needs a little more
 * core voltage, and if the requested clock is refused we stay where we are
 * rather than risk a machine that will not start. Holding Escape at power-on
 * skips the change entirely, which is the way back from a bad setting. */
static int boot_override;        /* escape was held at power-on */

static void clock_setup(void)
{
    int i;

    kbd_init();
    for (i = 0; i < 60; i++) {
        int k = kbd_poll();
        if (k == KEY_ESC) { boot_override = 1; break; }
        sleep_ms(5);
    }

    actual_khz = (int)(clock_get_hz(clk_sys) / 1000);
    if (boot_override) { boot_log("escape held: keeping the default clock"); return; }
    if (g_set.cpu_mhz * 1000 == actual_khz) return;
    if (g_set.cpu_mhz > 150) {
        vreg_set_voltage(VREG_VOLTAGE_1_15);
        sleep_ms(10);
    }
    if (set_sys_clock_khz((uint)(g_set.cpu_mhz * 1000), false))
        actual_khz = g_set.cpu_mhz * 1000;
    else if (g_set.cpu_mhz > 150)
        vreg_set_voltage(VREG_VOLTAGE_DEFAULT);
    sleep_ms(2);
}

int plat_sysclk_khz(void) { return actual_khz; }

/* ------------------------------------------------------------------ */

/* Re-apply everything the display settings control, without rebooting. This
 * is what makes a blank panel recoverable from the serial console. */
void plat_apply_display(void)
{
    font = g_set.font ? calcos_font_bold : calcos_font;
    kbd_set_backlight(g_set.backlight);
    kbd_set_kbd_backlight(g_set.kbd_light);
    lcd_init();
    {
        int r;
        for (r = 0; r < TERM_ROWS; r++) row_dirty[r] = 1;
    }
    gfx_showing = 0;
    console_flush();
}

void plat_display_test(void)
{
    lcd_test_pattern();
    sleep_ms(1200);
    plat_apply_display();
}

static int sd_present;

void plat_init(void)
{
    /* USB first, so the boot log is available even if nothing else works. */
    stdio_init_all();
    sleep_ms(400);
    boot_log("boot");

    settings_defaults();

    /* The card is read before the panel, because the panel's own settings
     * live on it. */
    sd_present = sd_init() && fat_mount();
    boot_log(sd_present ? "sd card mounted" : "no sd card, using defaults");
    if (sd_present && settings_load()) boot_log("settings loaded");

    clock_setup();
    if (sd_present) sd_reclock();
    boot_log("clock set");

    kbd_init();                 /* again: the I2C divider follows clk_sys */
    kbd_set_backlight(g_set.backlight);
    kbd_set_kbd_backlight(g_set.kbd_light);
    boot_log("keyboard ready, backlight on");

    font = g_set.font ? calcos_font_bold : calcos_font;
    lcd_init();
    boot_log("panel initialised");

    if (g_set.splash) {
        lcd_test_pattern();
        sleep_ms(900);
        boot_log("test pattern shown");
    }

    console_clear();
    console_flush();
    boot_log("console ready");
}

/* The screen simulator links this file to drive the real console against a
 * model of the panel, and brings its own entry point. */
#ifndef PLAT_NO_MAIN
int main(void)
{
    static Interp in;
    plat_init();

    cos_init(&in);
    in.digits = g_set.digits;
    in.ddigits = g_set.ddigits;
    in.deg = g_set.deg;

    cos_banner();
    cos_printf("clock %d MHz   ", plat_sysclk_khz() / 1000);
    cos_printf(sd_present ? "SD card ready\n" : "no SD card\n");
    cos_printf("\n");

    /* escape at boot skipped the stored settings; it skips the startup
     * files and the stored program too, so a bad one can always be got past */
    if (!boot_override) {
        cos_load_boot_prog();          /* works with no card at all */
        if (sd_present) cos_run_startup(&in);
    }

    {
        char line[256];
        for (;;) {
            if (cos_readline(&in, line, sizeof line, cos_prompt()) < 0) break;
            if (cos_shell_line(&in, line)) break;
            console_flush();
        }
    }
    cos_shutdown(&in);
    for (;;) tight_loop_contents();
}
#endif /* PLAT_NO_MAIN */
