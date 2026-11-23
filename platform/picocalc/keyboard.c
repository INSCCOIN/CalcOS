/* PicoCalc keyboard: an STM32 controller on I2C1 at 0x1F.
 *
 * Protocol: write the register number, then read two bytes. The FIFO
 * register returns a little-endian word whose high byte is the key code and
 * whose low byte is the state. The write ends with a STOP rather than a
 * repeated start; this controller does not answer a repeated start reliably.
 *
 * Writing a register is (reg | 0x80, value).
 *
 * The backlights are the awkward part: the two PicoCalc projects that
 * document these registers disagree about which of 0x05 and 0x0A drives the
 * screen and which drives the keys. Rather than pick one and risk shipping a
 * dark panel, both are set at startup and 'set blswap' exchanges them.
 */
#include "picocalc.h"
#include "settings.h"
#include "pico/stdlib.h"
#include "hardware/i2c.h"
#include "hardware/gpio.h"

#define KBD_I2C   i2c1
#define KBD_SDA   6
#define KBD_SCL   7
#define KBD_ADDR  0x1F
#define KBD_HZ    20000

enum {
    REG_VER = 0x01, REG_CFG = 0x02, REG_INT = 0x03, REG_KEY = 0x04,
    REG_BL_A = 0x05, REG_DEB = 0x06, REG_FRQ = 0x07, REG_RST = 0x08,
    REG_FIF = 0x09, REG_BL_B = 0x0A, REG_BAT = 0x0B
};

enum { ST_IDLE = 0, ST_PRESSED = 1, ST_HOLD = 2, ST_RELEASED = 3 };

#define KEY_ALT_CODE    0xA1
#define KEY_LSHIFT_CODE 0xA2
#define KEY_RSHIFT_CODE 0xA3
#define KEY_CTRL_CODE   0xA5

static int modifiers;

static int kbd_cmd_read(uint8_t regno, uint8_t *out, int n)
{
    if (i2c_write_timeout_us(KBD_I2C, KBD_ADDR, &regno, 1, false, 4000) != 1)
        return 0;
    if (i2c_read_timeout_us(KBD_I2C, KBD_ADDR, out, (size_t)n, false, 4000) != n)
        return 0;
    return 1;
}

static int kbd_write_reg(uint8_t regno, uint8_t value)
{
    uint8_t b[2];
    b[0] = (uint8_t)(regno | 0x80);
    b[1] = value;
    return i2c_write_timeout_us(KBD_I2C, KBD_ADDR, b, 2, false, 4000) == 2;
}

static int read_fifo(void)
{
    uint8_t b[2];
    if (!kbd_cmd_read(REG_FIF, b, 2)) return 0;
    return ((int)b[1] << 8) | b[0];        /* code << 8 | state */
}

void kbd_init(void)
{
    int guard = 0;
    i2c_init(KBD_I2C, KBD_HZ);
    gpio_set_function(KBD_SDA, GPIO_FUNC_I2C);
    gpio_set_function(KBD_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(KBD_SDA);
    gpio_pull_up(KBD_SCL);
    modifiers = 0;
    while (read_fifo() != 0 && ++guard < 64) { }   /* drain */
}

int kbd_modifiers(void) { return modifiers; }

/* The screen backlight is driven by the keyboard controller, not the
 * RP2350, so it has to be turned on explicitly: a working panel looks dead
 * if it is left at zero. */
void kbd_set_backlight(int level)
{
    kbd_write_reg(g_set.bl_swap ? REG_BL_A : REG_BL_B, (uint8_t)level);
}

void kbd_set_kbd_backlight(int level)
{
    kbd_write_reg(g_set.bl_swap ? REG_BL_B : REG_BL_A, (uint8_t)level);
}

/* Register 0x0E is the controller's shutdown request: the PicoCalc's power
 * rail is switched by the keyboard MCU, not by the RP2350, so this is the
 * only way to actually cut power. Boards with older controller firmware
 * ignore it, which is why the caller must be able to park instead. */
int kbd_poweroff(void)
{
    return kbd_write_reg(0x0E, 0x01);
}

int kbd_battery(void)
{
    uint8_t b[2];
    if (!kbd_cmd_read(REG_BAT, b, 2)) return -1;
    return b[1] & 0x7F;
}

static void update_modifiers(int code, int state)
{
    int bit = 0;
    switch (code) {
    case KEY_CTRL_CODE:   bit = MOD_CTRL; break;
    case KEY_ALT_CODE:    bit = MOD_ALT; break;
    case KEY_LSHIFT_CODE: bit = MOD_SHIFT_L; break;
    case KEY_RSHIFT_CODE: bit = MOD_SHIFT_R; break;
    default: return;
    }
    if (state == ST_PRESSED) modifiers |= bit;
    else if (state == ST_RELEASED) modifiers &= ~bit;
}

/* Returns a key code on press or auto-repeat, 0 when nothing is pending.
 * Ctrl+letter is folded into a control character so the shell sees ^C. */
int kbd_poll(void)
{
    int v = read_fifo();
    int code, state;
    if (!v) return 0;
    code = (v >> 8) & 0xFF;
    state = v & 0xFF;
    update_modifiers(code, state);
    if (state != ST_PRESSED && state != ST_HOLD) return 0;
    if (code == KEY_CTRL_CODE || code == KEY_ALT_CODE
        || code == KEY_LSHIFT_CODE || code == KEY_RSHIFT_CODE) return 0;
    if ((modifiers & MOD_CTRL) && code >= 'a' && code <= 'z')
        return code - 'a' + 1;
    if ((modifiers & MOD_CTRL) && code >= 'A' && code <= 'Z')
        return code - 'A' + 1;
    return code;
}
