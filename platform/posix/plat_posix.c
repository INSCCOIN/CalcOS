/* Host platform: develops and tests the whole system on a workstation.
 * Graphics render into an RGB buffer that is written out as a PNM, so plots
 * can be checked for real rather than assumed. */
#define _POSIX_C_SOURCE 200809L
#include "calc.h"
#include "cprog.h"
#include "settings.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <time.h>

#define GLYPH_H 12
extern const unsigned char calcos_font_bold[95][GLYPH_H];

#define GW 320
#define GH 320

static unsigned char fb[GH][GW][3];
static int           fb_dirty;
static const char   *fb_path = "plot.pnm";

void plat_init(void) { setvbuf(stdout, NULL, _IOLBF, 0); }
void plat_putc(char c) { fputc(c, stdout); }
void plat_puts(const char *s) { fputs(s, stdout); }
int  plat_getc(void) { return fgetc(stdin); }
void plat_poll(void) { }

int plat_cols(void)
{
    const char *c = getenv("COLUMNS");
    int n = c ? atoi(c) : 0;
    return n > 20 ? n : 40;
}
int plat_rows(void) { return 26; }
size_t plat_free_mem(void) { return 0; }

int plat_readline(char *buf, size_t n, const char *prompt)
{
    if (prompt) { fputs(prompt, stdout); fflush(stdout); }
    if (!fgets(buf, (int)n, stdin)) return -1;
    return (int)strlen(buf);
}

/* On the device "/" is the SD card root. On a workstation, map it to the
 * working directory so host runs do not write to the real filesystem root. */
static const char *hostpath(const char *p)
{
    while (*p == '/') p++;
    return *p ? p : ".";
}

void *plat_fopen(const char *path, const char *mode)
{
    return fopen(hostpath(path), mode);
}
int   plat_fgets(void *f, char *b, size_t n) { return fgets(b, (int)n, (FILE *)f) ? 1 : 0; }
int   plat_fputs(void *f, const char *s) { return fputs(s, (FILE *)f); }
void  plat_fclose(void *f) { fclose((FILE *)f); }
int   plat_fread(void *f, void *b, int n)
{ return n <= 0 ? 0 : (int)fread(b, 1, (size_t)n, (FILE *)f); }
int   plat_fwrite(void *f, const void *b, int n)
{ return n <= 0 ? 0 : (int)fwrite(b, 1, (size_t)n, (FILE *)f); }
int   plat_fseek(void *f, long off, int whence) { return fseek((FILE *)f, off, whence); }
long  plat_ftell(void *f) { return ftell((FILE *)f); }
long  plat_fsize(void *f)
{
    long here = ftell((FILE *)f), end;
    if (fseek((FILE *)f, 0, SEEK_END)) return -1;
    end = ftell((FILE *)f);
    fseek((FILE *)f, here, SEEK_SET);
    return end;
}
int   plat_feof(void *f) { return feof((FILE *)f) != 0; }
int   plat_remove(const char *path) { return remove(hostpath(path)) == 0; }

/* ------------------------------------------------------------------ */
/* time, keys, sound and power                                         */
/* ------------------------------------------------------------------ */

static uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)(ts.tv_nsec / 1000);
}

static uint64_t t0;

uint32_t plat_millis(void) { if (!t0) t0 = now_us(); return (uint32_t)((now_us() - t0) / 1000); }
uint64_t plat_micros(void) { if (!t0) t0 = now_us(); return now_us() - t0; }

void plat_delay_ms(int ms)
{
    struct timespec ts;
    if (ms <= 0) return;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

/* stdin on the host is a pipe or a terminal in line mode, so a key is
 * "ready" only once a whole line has been typed. That is enough for the
 * tests, which drive compiled programs from a script. */
static int peeked = -2;

int plat_getkey(void)
{
    int c;
    if (peeked != -2) { c = peeked; peeked = -2; return c; }
    c = fgetc(stdin);
    return c == EOF ? -1 : c;
}

int plat_key_ready(void)
{
    if (peeked != -2) return peeked >= 0;
    peeked = plat_getkey();
    return peeked >= 0;
}

/* The host has no buzzer; record the request so tests can check it. */
int cos_host_tone_hz, cos_host_tone_ms;
void plat_tone(int hz, int ms) { cos_host_tone_hz = hz; cos_host_tone_ms = ms; }
void plat_tone_off(void) { cos_host_tone_hz = 0; }

void plat_reboot(void)   { fputs("\n[reboot]\n", stdout); exit(0); }
void plat_poweroff(void) { fputs("\n[poweroff]\n", stdout); exit(0); }

int plat_listdir(const char *path, char *out, size_t n)
{
    DIR *d = opendir(path && *path ? hostpath(path) : ".");
    struct dirent *e;
    size_t used = 0;
    if (!d) return -1;
    while ((e = readdir(d)) != NULL) {
        size_t l = strlen(e->d_name);
        if (e->d_name[0] == '.') continue;
        if (used + l + 2 >= n) break;
        memcpy(out + used, e->d_name, l);
        used += l;
        out[used++] = '\n';
    }
    out[used] = 0;
    closedir(d);
    return 0;
}

/* ------------------------------------------------------------------ */
/* graphics                                                            */
/* ------------------------------------------------------------------ */

int plat_gfx_width(void)  { return GW; }
int plat_gfx_height(void) { return GH; }

/* The host has no XIP flash, so the store is a file. It behaves the same
 * way from a program's point of view, which is what the tests check; the
 * difference is that a read here is a copy rather than a pointer into
 * memory-mapped flash. */
#define HOST_STORE_PAGES 128
#define HOST_STORE_PGSZ  4096
static uint8_t host_store[HOST_STORE_PAGES][HOST_STORE_PGSZ];
static int     host_store_ready;

static void host_store_init(void)
{
    FILE *f;
    if (host_store_ready) return;
    memset(host_store, 0xFF, sizeof host_store);
    f = fopen("store.bin", "rb");
    if (f) { fread(host_store, 1, sizeof host_store, f); fclose(f); }
    host_store_ready = 1;
}

static void host_store_save(void)
{
    FILE *f = fopen("store.bin", "wb");
    if (!f) return;
    fwrite(host_store, 1, sizeof host_store, f);
    fclose(f);
}

int plat_store_pages(void) { return HOST_STORE_PAGES; }
int plat_store_page_size(void) { return HOST_STORE_PGSZ; }

const void *plat_store_read(int page)
{
    host_store_init();
    if (page < 0 || page >= HOST_STORE_PAGES) return NULL;
    return host_store[page];
}

int plat_store_write(int page, const void *data, int len)
{
    host_store_init();
    if (page < 0 || page >= HOST_STORE_PAGES) return 0;
    if (len < 0 || len > HOST_STORE_PGSZ) return 0;
    memset(host_store[page], 0xFF, HOST_STORE_PGSZ);
    if (len) memcpy(host_store[page], data, (size_t)len);
    host_store_save();
    return 1;
}

int plat_store_erase(int page)
{
    host_store_init();
    if (page < 0 || page >= HOST_STORE_PAGES) return 0;
    memset(host_store[page], 0xFF, HOST_STORE_PGSZ);
    host_store_save();
    return 1;
}

/* The host console is whatever terminal is attached: it has its own
 * scrollback, and clearing is the terminal's business. */
void plat_flush(void) { fflush(stdout); }

/* The host runs the loop on one core. The point of testing it here is that
 * the *work* is correct and index-independent, which is the property that
 * makes it safe to split on the device. */
static int host_par_active;
int  plat_par_cores(void) { return 1; }
int  plat_par_active(void) { return host_par_active; }
void plat_par_for(int n, void (*fn)(int, void *), void *ctx)
{
    int i;
    if (n <= 0 || !fn) return;
    host_par_active = 1;
    for (i = 0; i < n; i++) fn(i, ctx);
    host_par_active = 0;
}
void plat_scrollback(int screens) { (void)screens; }
void plat_clear_screen(void) { fputs("\033[H\033[2J", stdout); }

int plat_gfx_colours(void) { return 1 << 24; }
void plat_gfx_hide(void) { }

int plat_gfx_get(int x, int y)
{
    if (x < 0 || y < 0 || x >= GW || y >= GH) return -1;
    return (fb[y][x][0] << 16) | (fb[y][x][1] << 8) | fb[y][x][2];
}

void plat_gfx_clear(void)
{
    memset(fb, 0, sizeof fb);
    fb_dirty = 1;
}

void plat_gfx_raw_point(int x, int y, unsigned rgb)
{
    if (x < 0 || y < 0 || x >= GW || y >= GH) return;
    fb[y][x][0] = (unsigned char)(rgb >> 16);
    fb[y][x][1] = (unsigned char)(rgb >> 8);
    fb[y][x][2] = (unsigned char)rgb;
    fb_dirty = 1;
}

void plat_gfx_text(int x, int y, const char *s, unsigned rgb)
{
    for (; *s; s++, x += 8) {
        int c = (unsigned char)*s, r, col;
        if (c < 32 || c > 126) continue;
        for (r = 0; r < GLYPH_H; r++)
            for (col = 0; col < 8; col++)
                if ((calcos_font_bold[c - 32][r] >> col) & 1)
                    plat_gfx_point(x + col, y + r, rgb);
    }
}

void plat_gfx_flush(void)
{
    FILE *f;
    if (!fb_dirty) return;
    f = fopen(fb_path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", GW, GH);
    fwrite(fb, 1, sizeof fb, f);
    fclose(f);
    fb_dirty = 0;
}

void plat_sync_icache(const void *addr, size_t len) { (void)addr; (void)len; }

int plat_write_file(const char *path, const void *data, int len)
{
    FILE *f = fopen(hostpath(path), "wb");
    size_t n;
    if (!f) return 0;
    n = fwrite(data, 1, (size_t)len, f);
    fclose(f);
    return n == (size_t)len;
}

/* No panel on a workstation: the colour and font settings still take effect
 * in rendered plots, and the panel registers have nothing to apply to. */
void plat_apply_display(void) { }
void plat_display_test(void) { plat_puts("no panel on this host\n"); }

/* ------------------------------------------------------------------ */

/* Host tools that only need the HAL (ccrun) link this file with
 * PLAT_NO_MAIN so they can keep their own entry point. */
#ifndef PLAT_NO_MAIN
int main(int argc, char **argv)
{
    static Interp in;
    int i;
    plat_init();
    settings_defaults();
    cos_init(&in);

    /* The device runs the card's startup files at boot; here the working
     * directory stands in for the card, so a test can put one there. */
    if (!getenv("CALCOS_NO_STARTUP")) {
        cos_load_boot_prog();
        cos_run_startup(&in);
    }

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-o") && i + 1 < argc) { fb_path = argv[++i]; continue; }
        if (!strcmp(argv[i], "-e")) {
            for (i++; i < argc; i++) {
                char buf[1024];
                snprintf(buf, sizeof buf, "%s", argv[i]);
                cos_shell_line(&in, buf);
            }
            break;
        }
        if (!strcmp(argv[i], "-f") && i + 1 < argc) {
            FILE *f = fopen(argv[++i], "r");
            char line[1024];
            if (!f) { perror(argv[i]); return 1; }
            while (fgets(line, sizeof line, f)) cos_shell_line(&in, line);
            fclose(f);
            break;
        }
    }
    if (argc == 1) cos_shell_run(&in);
    cos_shutdown(&in);
    return 0;
}
#endif /* PLAT_NO_MAIN */
