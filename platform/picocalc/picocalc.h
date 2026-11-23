/* Shared declarations for the PicoCalc board layer. */
#ifndef PICOCALC_H
#define PICOCALC_H

#include <stdint.h>
#include <stddef.h>
#include "calc.h"     /* key codes and the platform contract */

#define LCD_W 320
#define LCD_H 320

/* Console geometry: 8x12 cells over 320x320, giving 40 columns and 26 rows.
 * The same geometry MMBasic uses on this hardware: 40 columns is what a
 * 320-pixel line affords, and 12 rows of height is what makes it legible.
 * 26 rows leave 8 pixels at the bottom of the panel, painted as background. */
#define CELL_W 8
#define CELL_H 12
#define TERM_COLS (LCD_W / CELL_W)            /* 40 */
#define TERM_ROWS (LCD_H / CELL_H)            /* 26 */
#define TERM_SPARE (LCD_H - TERM_ROWS * CELL_H)  /* 8 */

/* display */
void lcd_init(void);
void lcd_window(int x, int y, int w, int h);
void lcd_blit(const uint16_t *px, int count);
void lcd_fill(int x, int y, int w, int h, uint16_t color);
void lcd_test_pattern(void);
void lcd_sleep(void);         /* display off and into sleep-in */

static inline uint16_t rgb565(unsigned rgb)
{
    return (uint16_t)(((rgb >> 8) & 0xF800) | ((rgb >> 5) & 0x07E0)
                    | ((rgb >> 3) & 0x001F));
}

/* keyboard */
void kbd_init(void);
int  kbd_poll(void);          /* a key code, or 0 if nothing is waiting */
int  kbd_modifiers(void);
void kbd_set_backlight(int level);
void kbd_set_kbd_backlight(int level);
int  kbd_battery(void);       /* percent, or -1 if unknown */
int  kbd_poweroff(void);      /* ask the controller to cut power */

/* SD card and a read-only FAT16/FAT32 reader */
int  sd_init(void);
int  sd_read_block(uint32_t lba, uint8_t *buf);
int  sd_write_block(uint32_t lba, const uint8_t *buf);
void sd_reclock(void);
int  fat_mount(void);
int  fat_mounted(void);

typedef struct {
    uint32_t cluster;
    uint32_t size;
    uint32_t pos;
    uint32_t cur_cluster;
    uint32_t cur_off;
    int      valid;
} FatFile;

int  fat_open(FatFile *f, const char *path);
int  fat_read(FatFile *f, void *buf, int len);
int  fat_list(const char *path, char *out, size_t cap);
/* whole-file write, 8.3 names only, into an existing directory */
int  fat_write(const char *path, const void *data, int len);
int  fat_remove(const char *path);
/* seek within an open file, for the stdio-alike in the platform layer */
int  fat_seek(FatFile *f, uint32_t pos);

#endif
