/* Minimal stand-ins for the pico-sdk headers the board layer uses, so the
 * real drivers and the real console can be compiled and exercised on a
 * workstation. */
#ifndef FAKE_PICO_STDLIB_H
#define FAKE_PICO_STDLIB_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>

typedef unsigned int uint;
typedef uint64_t absolute_time_t;

#define PICO_ERROR_TIMEOUT (-1)

void sleep_ms(uint32_t ms);
void sleep_us(uint64_t us);
void stdio_init_all(void);
int  getchar_timeout_us(uint32_t us);
void tight_loop_contents(void);

absolute_time_t get_absolute_time(void);
uint32_t to_ms_since_boot(absolute_time_t t);
uint64_t to_us_since_boot(absolute_time_t t);
absolute_time_t make_timeout_time_ms(uint32_t ms);
int64_t absolute_time_diff_us(absolute_time_t a, absolute_time_t b);

bool set_sys_clock_khz(uint32_t khz, bool required);

#define __wfi() ((void)0)

#endif
