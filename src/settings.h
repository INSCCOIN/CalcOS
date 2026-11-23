/* System settings, stored on the SD card as /calcos.cfg.
 *
 * Display settings are deliberately runtime-changeable: if a panel does not
 * come up, the serial console is still live, and the pixel format, MADCTL,
 * inversion and SPI clock can all be changed and reapplied without
 * reflashing. That turns a black screen into something you can work out.
 */
#ifndef SETTINGS_H
#define SETTINGS_H

#define SETTINGS_PATH "/calcos.cfg"

typedef struct {
    /* system */
    int      cpu_mhz;      /* applied at boot                       */
    int      splash;       /* show a test pattern at startup        */

    /* console appearance */
    unsigned fg, bg;       /* 0xRRGGBB                              */
    int      font;         /* 0 regular, 1 bold                     */

    /* panel */
    int      backlight;    /* 0..255, via the keyboard controller   */
    int      kbd_light;    /* 0..255                                */
    int      bl_swap;      /* exchange the two backlight registers  */
    int      lcd_bits;     /* 16 or 18                              */
    int      lcd_madctl;   /* memory access control byte            */
    int      lcd_invert;   /* inversion on                          */
    int      lcd_spi_mhz;  /* 0 = a quarter of the system clock     */

    /* calculator */
    int      digits;
    int      ddigits;
    int      deg;
} Settings;

extern Settings g_set;

void settings_defaults(void);
int  settings_load(void);                 /* 1 if a file was read   */
int  settings_save(void);                 /* 1 on success           */

/* 0 ok, -1 unknown key, -2 value out of range */
int  settings_set(const char *key, const char *value);
int  settings_get(const char *key, char *out, int cap);

/* iterate for listing: returns the key name or NULL past the end */
const char *settings_key(int i);
int  settings_count(void);

#endif
