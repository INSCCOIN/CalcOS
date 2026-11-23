/* ILI9488/ST7365P driver for the PicoCalc's 320x320 panel.
 *
 * Two rules here are not optional, and getting either wrong produces a panel
 * that looks dead or draws a thin stripe of pixels:
 *
 *  1. Chip select stays LOW across a whole transaction. A command and its
 *     parameters are one transaction; so are the window registers and the
 *     pixel data that follow them. Raising CS in between makes the controller
 *     discard the parameters, which leaves it on its power-on defaults.
 *
 *  2. Pixel buffers go out with blocking SPI, not DMA. Pacing a DMA channel
 *     against the SPI TX FIFO is easy to get subtly wrong, and when it is
 *     wrong only the FIFO's worth of pixels arrives before the transfer
 *     stalls - which looks like a dotted line at the edge of the screen.
 *
 * The register sequence matches the PicoCalc drivers that are known to work
 * on this hardware.
 */
#include "picocalc.h"
#include "settings.h"
#include "pico/stdlib.h"
#include "hardware/spi.h"
#include "hardware/gpio.h"
#include "hardware/clocks.h"
#include <string.h>

#define LCD_SPI   spi1
#define LCD_SCK   10
#define LCD_TX    11
#define LCD_RX    12
#define LCD_CS    13
#define LCD_DC    14
#define LCD_RST   15

static int bits18;                 /* follows the lcdbits setting */

/* Other PicoCalc firmware drives these panels at about 105 MHz. A quarter of
 * the system clock is well inside that; 'set lcdspi N' overrides it. */
static uint32_t lcd_spi_hz(void)
{
    if (g_set.lcd_spi_mhz > 0) return (uint32_t)g_set.lcd_spi_mhz * 1000000u;
    return clock_get_hz(clk_sys) / 4u;
}

static inline void cs_lo(void) { gpio_put(LCD_CS, 0); }
static inline void cs_hi(void) { gpio_put(LCD_CS, 1); }

/* These assume CS is already low and never touch it. */
static void wr_cmd(uint8_t c)
{
    gpio_put(LCD_DC, 0);
    spi_write_blocking(LCD_SPI, &c, 1);
}

static void wr_dat(const uint8_t *d, size_t n)
{
    if (!n) return;
    gpio_put(LCD_DC, 1);
    spi_write_blocking(LCD_SPI, d, n);
}

static void reg(uint8_t c, const uint8_t *d, size_t n)
{
    wr_cmd(c);
    wr_dat(d, n);
}

#define REG(c, ...) do { \
    static const uint8_t _d[] = { __VA_ARGS__ }; \
    reg((c), _d, sizeof _d); \
} while (0)

/* Opens a transaction: CS is left LOW, ready for pixel data. */
void lcd_window(int x, int y, int w, int h)
{
    uint8_t b[4];
    int x1 = x + w - 1, y1 = y + h - 1;
    cs_lo();
    b[0] = (uint8_t)(x >> 8);  b[1] = (uint8_t)x;
    b[2] = (uint8_t)(x1 >> 8); b[3] = (uint8_t)x1;
    reg(0x2A, b, 4);
    b[0] = (uint8_t)(y >> 8);  b[1] = (uint8_t)y;
    b[2] = (uint8_t)(y1 >> 8); b[3] = (uint8_t)y1;
    reg(0x2B, b, 4);
    wr_cmd(0x2C);
}

/* Send pixels with CS held low and leave it low. */
static void blit_raw(const uint16_t *px, int count)
{
    if (count <= 0) return;
    gpio_put(LCD_DC, 1);

    if (bits18) {
        /* RGB666: three bytes a pixel, so the SPI stays in 8-bit mode */
        static uint8_t tmp[768];
        int i = 0;
        while (i < count) {
            int n = 0;
            while (i < count && n + 3 <= (int)sizeof tmp) {
                uint16_t p = px[i++];
                tmp[n++] = (uint8_t)((p >> 8) & 0xF8);
                tmp[n++] = (uint8_t)((p >> 3) & 0xFC);
                tmp[n++] = (uint8_t)((p << 3) & 0xF8);
            }
            spi_write_blocking(LCD_SPI, tmp, (size_t)n);
        }
        return;
    }

    spi_set_format(LCD_SPI, 16, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    spi_write16_blocking(LCD_SPI, px, (size_t)count);
    spi_set_format(LCD_SPI, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
}

/* Send pixels and close the transaction. */
void lcd_blit(const uint16_t *px, int count)
{
    blit_raw(px, count);
    cs_hi();
}

void lcd_fill(int x, int y, int w, int h, uint16_t color)
{
    static uint16_t buf[LCD_W];
    int i, row;
    if (w <= 0 || h <= 0) return;
    if (w > LCD_W) w = LCD_W;
    for (i = 0; i < w; i++) buf[i] = color;
    lcd_window(x, y, w, h);
    for (row = 0; row < h; row++) blit_raw(buf, w);
    cs_hi();
}

/* Colour bars, then white, then black: enough to tell a dead panel from one
 * whose colour order or orientation is merely wrong. */
void lcd_test_pattern(void)
{
    static const uint16_t bars[8] = {
        0xFFFF, 0xFFE0, 0x07FF, 0x07E0, 0xF81F, 0xF800, 0x001F, 0x0000
    };
    int i;
    for (i = 0; i < 8; i++)
        lcd_fill(i * (LCD_W / 8), 0, LCD_W / 8, LCD_H / 2, bars[i]);
    lcd_fill(0, LCD_H / 2, LCD_W, LCD_H / 4, 0xFFFF);
    lcd_fill(0, LCD_H * 3 / 4, LCD_W, LCD_H / 4, 0x0000);
}

void lcd_init(void)
{
    uint8_t pixfmt, madctl;

    bits18 = (g_set.lcd_bits == 18);
    pixfmt = bits18 ? 0x66 : 0x55;
    madctl = (uint8_t)g_set.lcd_madctl;

    spi_init(LCD_SPI, lcd_spi_hz());
    spi_set_format(LCD_SPI, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    gpio_set_function(LCD_SCK, GPIO_FUNC_SPI);
    gpio_set_function(LCD_TX, GPIO_FUNC_SPI);
    gpio_set_function(LCD_RX, GPIO_FUNC_SPI);

    gpio_init(LCD_CS);  gpio_set_dir(LCD_CS, GPIO_OUT);
    gpio_init(LCD_DC);  gpio_set_dir(LCD_DC, GPIO_OUT);
    gpio_init(LCD_RST); gpio_set_dir(LCD_RST, GPIO_OUT);
    cs_hi();
    gpio_put(LCD_DC, 0);

    gpio_put(LCD_RST, 1); sleep_ms(10);
    gpio_put(LCD_RST, 0); sleep_ms(10);
    gpio_put(LCD_RST, 1); sleep_ms(200);

    /* One transaction for the whole init: CS low throughout, so every
     * parameter reaches the controller with its command. */
    cs_lo();
    REG(0xE0, 0x00, 0x03, 0x09, 0x08, 0x16, 0x0A, 0x3F, 0x78,
              0x4C, 0x09, 0x0A, 0x08, 0x16, 0x1A, 0x0F);
    REG(0xE1, 0x00, 0x16, 0x19, 0x03, 0x0F, 0x05, 0x32, 0x45,
              0x46, 0x04, 0x0E, 0x0D, 0x35, 0x37, 0x0F);
    REG(0xC0, 0x17, 0x15);              /* power control 1                 */
    REG(0xC1, 0x41);                    /* power control 2                 */
    REG(0xC5, 0x00, 0x12, 0x80);        /* VCOM                            */
    reg(0x36, &madctl, 1);              /* orientation, 'set lcdmadctl'    */
    reg(0x3A, &pixfmt, 1);              /* pixel format, 'set lcdbits'     */
    REG(0xB0, 0x00);                    /* interface mode                  */
    REG(0xB1, 0xD0, 0x11);              /* 60 Hz                           */
    wr_cmd(g_set.lcd_invert ? 0x21 : 0x20);   /* 'set lcdinvert'           */
    REG(0xB4, 0x02);
    REG(0xB6, 0x02, 0x02, 0x3B);        /* display function control        */
    REG(0xB7, 0xC6);
    REG(0xE9, 0x00);
    REG(0xF7, 0xA9, 0x51, 0x2C, 0x82);
    wr_cmd(0x11);                       /* sleep out                       */
    cs_hi();
    sleep_ms(120);

    cs_lo();
    wr_cmd(0x29);                       /* display on                      */
    cs_hi();
    sleep_ms(20);

    lcd_fill(0, 0, LCD_W, LCD_H, rgb565(g_set.bg));
}

/* Blank the panel and put the controller to sleep. Used on shutdown: with
 * the backlight already off this is what stops the panel drawing current. */
void lcd_sleep(void)
{
    cs_lo();
    wr_cmd(0x28);                       /* display off                     */
    wr_cmd(0x10);                       /* sleep in                        */
    cs_hi();
    sleep_ms(20);
}
