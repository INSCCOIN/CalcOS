#ifndef FAKE_HW_PWM_H
#define FAKE_HW_PWM_H
#include <stdint.h>
#include <stdbool.h>
typedef unsigned int uint;
uint pwm_gpio_to_slice_num(uint gpio);
void pwm_set_enabled(uint slice, bool on);
void pwm_set_clkdiv_int_frac(uint slice, uint8_t i, uint8_t f);
void pwm_set_wrap(uint slice, uint16_t wrap);
void pwm_set_gpio_level(uint gpio, uint16_t level);
void pwm_set_output_polarity(uint slice, bool a, bool b);
#endif
