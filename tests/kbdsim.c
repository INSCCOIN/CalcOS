/* A model of the PicoCalc keyboard controller, so the real driver can be
 * exercised without the hardware: FIFO decoding, modifier tracking, the
 * control-character folding the shell depends on, and the backlight writes
 * (which are the reason a working panel can look dead). */
#include "picocalc.h"
#include "settings.h"
#include "pico/stdlib.h"
#include "hardware/i2c.h"
#include "hardware/gpio.h"
#include <stdio.h>
#include <string.h>

struct i2c_inst { int dummy; };
static struct i2c_inst c0, c1;
i2c_inst_t *i2c0 = &c0, *i2c1 = &c1;

Settings g_set;

void sleep_ms(uint32_t ms) { (void)ms; }
void gpio_init(unsigned pin) { (void)pin; }
void gpio_set_dir(unsigned pin, int out) { (void)pin; (void)out; }
void gpio_put(unsigned pin, int v) { (void)pin; (void)v; }
void gpio_set_function(unsigned pin, gpio_function_t f) { (void)pin; (void)f; }
void gpio_pull_up(unsigned pin) { (void)pin; }
unsigned i2c_init(i2c_inst_t *i, unsigned hz) { (void)i; return hz; }

/* ------------------------------------------------------------------ */
/* the controller                                                      */
/* ------------------------------------------------------------------ */

#define FIFO_MAX 32

static struct {
    uint16_t fifo[FIFO_MAX];
    int      head, tail;
    uint8_t  selected;
    uint8_t  regs[16];
    int      bl_a_writes, bl_b_writes;     /* 0x05 and 0x0A */
    int      last_bl_a, last_bl_b;
    int      bad_repeated_start;
} kb;

static void push(uint8_t code, uint8_t state)
{
    if ((kb.head + 1) % FIFO_MAX == kb.tail) return;
    kb.fifo[kb.head] = (uint16_t)((code << 8) | state);
    kb.head = (kb.head + 1) % FIFO_MAX;
}

int i2c_write_timeout_us(i2c_inst_t *i, uint8_t addr, const uint8_t *src,
                         size_t len, int nostop, unsigned timeout)
{
    (void)i; (void)timeout;
    if (addr != 0x1F) return -1;
    /* this controller does not answer a repeated start */
    if (nostop) kb.bad_repeated_start++;
    if (len == 1) { kb.selected = src[0]; return 1; }
    if (len == 2) {
        uint8_t r = src[0] & 0x7F;
        if (!(src[0] & 0x80)) return -1;      /* writes need the 0x80 bit */
        kb.regs[r & 0x0F] = src[1];
        if (r == 0x05) { kb.bl_a_writes++; kb.last_bl_a = src[1]; }
        if (r == 0x0A) { kb.bl_b_writes++; kb.last_bl_b = src[1]; }
        return 2;
    }
    return -1;
}

int i2c_read_timeout_us(i2c_inst_t *i, uint8_t addr, uint8_t *dst,
                        size_t len, int nostop, unsigned timeout)
{
    (void)i; (void)nostop; (void)timeout;
    if (addr != 0x1F || len != 2) return -1;
    if (kb.selected == 0x09) {                /* FIFO */
        uint16_t v = 0;
        if (kb.head != kb.tail) {
            v = kb.fifo[kb.tail];
            kb.tail = (kb.tail + 1) % FIFO_MAX;
        }
        dst[0] = (uint8_t)v;                  /* little endian: state first */
        dst[1] = (uint8_t)(v >> 8);
        return 2;
    }
    if (kb.selected == 0x0B) { dst[0] = 0; dst[1] = 87; return 2; }
    dst[0] = dst[1] = 0;
    return 2;
}

/* ------------------------------------------------------------------ */

static int fails;
static void check(int cond, const char *what)
{
    if (!cond) { printf("FAIL: %s\n", what); fails++; }
}

#define PRESSED 1
#define HOLD 2
#define RELEASED 3

int main(void)
{
    memset(&kb, 0, sizeof kb);
    memset(&g_set, 0, sizeof g_set);
    kbd_init();

    printf("-- keyboard model\n");
    check(kb.bad_repeated_start == 0,
          "register reads end with a stop, not a repeated start");

    /* plain keys */
    push('a', PRESSED);
    check(kbd_poll() == 'a', "a plain key arrives");
    push('a', RELEASED);
    check(kbd_poll() == 0, "key release produces no character");
    push('Z', PRESSED);
    check(kbd_poll() == 'Z', "a shifted key arrives as sent");

    /* auto-repeat should type */
    push('x', HOLD);
    check(kbd_poll() == 'x', "auto-repeat types");

    /* modifiers are tracked and swallowed */
    push(0xA5, PRESSED);                       /* control down */
    check(kbd_poll() == 0, "the control key itself types nothing");
    check((kbd_modifiers() & MOD_CTRL) != 0, "control is recorded as held");
    push('c', PRESSED);
    check(kbd_poll() == 3, "ctrl+c folds to ^C");
    push('C', PRESSED);
    check(kbd_poll() == 3, "ctrl+shift+c folds to ^C as well");
    push(0xA5, RELEASED);
    kbd_poll();
    check((kbd_modifiers() & MOD_CTRL) == 0, "control is released");
    push('c', PRESSED);
    check(kbd_poll() == 'c', "a plain c is unaffected afterwards");

    /* navigation keys pass through as their codes */
    push(0xB5, PRESSED);
    check(kbd_poll() == KEY_UP, "arrow keys pass through");
    push(0xB1, PRESSED);
    check(kbd_poll() == KEY_ESC, "escape passes through, for the boot override");

    /* empty FIFO */
    check(kbd_poll() == 0, "an empty queue reports nothing");

    /* backlights: both registers must be driven, because the two published
     * pin maps disagree about which one is the screen */
    g_set.bl_swap = 0;
    kbd_set_backlight(200);
    kbd_set_kbd_backlight(100);
    check(kb.bl_b_writes == 1 && kb.last_bl_b == 200,
          "the screen backlight goes to 0x0A by default");
    check(kb.bl_a_writes == 1 && kb.last_bl_a == 100,
          "the keyboard backlight goes to 0x05 by default");

    g_set.bl_swap = 1;
    kbd_set_backlight(210);
    kbd_set_kbd_backlight(110);
    check(kb.last_bl_a == 210 && kb.last_bl_b == 110,
          "blswap exchanges the two backlight registers");

    check(kbd_battery() == 87, "the battery level is read");

    if (!fails) printf("   keyboard protocol, modifiers and backlights correct\n");
    return fails ? 1 : 0;
}
