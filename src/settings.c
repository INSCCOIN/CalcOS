/* Settings: defaults, parsing, and persistence to the SD card. */
#include "calc.h"
#include "settings.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

Settings g_set;

void settings_defaults(void)
{
    memset(&g_set, 0, sizeof g_set);
    g_set.cpu_mhz     = 150;        /* the part's own default: safe */
    g_set.splash      = 1;
    g_set.fg          = 0xC8E6C8u;
    g_set.bg          = 0x080A08u;
    g_set.font        = 1;
    g_set.backlight   = 220;
    /* Lit by default: if the two backlight registers turn out to be the
     * other way round on this unit, the keys light up instead of the screen,
     * which says so plainly rather than leaving a dark panel. */
    g_set.kbd_light   = 160;
    g_set.bl_swap     = 0;
    g_set.lcd_bits    = 16;
    g_set.lcd_madctl  = 0x48;
    g_set.lcd_invert  = 1;
    g_set.lcd_spi_mhz = 0;
    g_set.digits      = 15;
    g_set.ddigits     = 32;
    g_set.deg         = 0;
}

/* ------------------------------------------------------------------ */
/* the table drives parsing, listing and saving alike                  */
/* ------------------------------------------------------------------ */

typedef enum { V_INT, V_HEX, V_BOOL } VType;

typedef struct {
    const char *name;
    VType       type;
    int         lo, hi;
    size_t      off;
    const char *help;
} Entry;

#define OFF(f) ((size_t)&((Settings *)0)->f)

static const Entry table[] = {
    { "cpu",       V_INT,  100, 300,       OFF(cpu_mhz),
      "system clock in MHz, applied at the next boot" },
    { "splash",    V_BOOL, 0, 1,           OFF(splash),
      "show a display test pattern at startup" },
    { "fg",        V_HEX,  0, 0xFFFFFF,    OFF(fg),
      "text colour, 0xRRGGBB" },
    { "bg",        V_HEX,  0, 0xFFFFFF,    OFF(bg),
      "background colour, 0xRRGGBB" },
    { "font",      V_INT,  0, 1,           OFF(font),
      "0 regular, 1 bold" },
    { "backlight", V_INT,  0, 255,         OFF(backlight),
      "screen backlight" },
    { "kbdlight",  V_INT,  0, 255,         OFF(kbd_light),
      "keyboard backlight" },
    { "blswap",    V_BOOL, 0, 1,           OFF(bl_swap),
      "set to 1 if the keys light up instead of the screen" },
    { "lcdbits",   V_INT,  16, 18,         OFF(lcd_bits),
      "panel pixel format: 16 or 18" },
    { "lcdmadctl", V_HEX,  0, 0xFF,        OFF(lcd_madctl),
      "panel orientation byte, default 0x48" },
    { "lcdinvert", V_BOOL, 0, 1,           OFF(lcd_invert),
      "panel inversion, on for IPS" },
    { "lcdspi",    V_INT,  0, 150,         OFF(lcd_spi_mhz),
      "panel SPI clock in MHz, 0 means a quarter of the CPU" },
    { "digits",    V_INT,  1, 17,          OFF(digits),
      "significant digits shown" },
    { "ddigits",   V_INT,  1, 34,          OFF(ddigits),
      "digits shown for extended precision" },
    { "deg",       V_BOOL, 0, 1,           OFF(deg),
      "1 for degrees, 0 for radians" },
};

#define NENTRY ((int)(sizeof table / sizeof table[0]))

int settings_count(void) { return NENTRY; }
const char *settings_key(int i)
{
    return (i >= 0 && i < NENTRY) ? table[i].name : NULL;
}

static const Entry *find(const char *key)
{
    int i;
    for (i = 0; i < NENTRY; i++)
        if (strcmp(table[i].name, key) == 0) return &table[i];
    return NULL;
}

static int *slot(const Entry *e)
{
    return (int *)((char *)&g_set + e->off);
}

int settings_set(const char *key, const char *value)
{
    const Entry *e = find(key);
    long v;
    char *end;
    if (!e) return -1;
    if (!value || !*value) return -2;

    if (!strcmp(value, "on") || !strcmp(value, "yes") || !strcmp(value, "true"))
        v = 1;
    else if (!strcmp(value, "off") || !strcmp(value, "no") || !strcmp(value, "false"))
        v = 0;
    else if (!strcmp(value, "deg")) v = 1;
    else if (!strcmp(value, "rad")) v = 0;
    else {
        v = strtol(value, &end, 0);
        if (end == value) return -2;
    }
    if (v < e->lo || v > e->hi) return -2;
    *slot(e) = (int)v;
    return 0;
}

int settings_get(const char *key, char *out, int cap)
{
    const Entry *e = find(key);
    if (!e) return -1;
    if (e->type == V_HEX)
        snprintf(out, (size_t)cap, e->hi > 0xFF ? "0x%06X" : "0x%02X",
                 (unsigned)*slot(e));
    else snprintf(out, (size_t)cap, "%d", *slot(e));
    return 0;
}

const char *settings_help(const char *key)
{
    const Entry *e = find(key);
    return e ? e->help : "";
}

/* ------------------------------------------------------------------ */
/* persistence                                                         */
/* ------------------------------------------------------------------ */

static char *trimp(char *s)
{
    char *e;
    while (*s == ' ' || *s == '\t') s++;
    e = s + strlen(s);
    while (e > s && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t'))
        *--e = 0;
    return s;
}

int settings_load(void)
{
    void *f = plat_fopen(SETTINGS_PATH, "r");
    char line[128];
    if (!f) return 0;
    while (plat_fgets(f, line, sizeof line)) {
        char *s = trimp(line), *eq;
        if (!*s || *s == '#') continue;
        eq = strchr(s, '=');
        if (!eq) continue;
        *eq = 0;
        settings_set(trimp(s), trimp(eq + 1));
    }
    plat_fclose(f);
    return 1;
}

int settings_save(void)
{
    char buf[768];
    int n = 0, i;
    n += snprintf(buf + n, sizeof buf - (size_t)n,
                  "# CalcOS settings. Edit on a PC or with 'set KEY VALUE'.\n");
    for (i = 0; i < NENTRY && n < (int)sizeof buf - 64; i++) {
        char v[32];
        settings_get(table[i].name, v, sizeof v);
        n += snprintf(buf + n, sizeof buf - (size_t)n, "%-10s = %s\n",
                      table[i].name, v);
    }
    return plat_write_file(SETTINGS_PATH, buf, n);
}
