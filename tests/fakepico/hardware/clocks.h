#ifndef FAKE_CLOCKS_H
#define FAKE_CLOCKS_H
#include <stdint.h>
typedef enum { clk_sys = 0 } clock_handle_t;
uint32_t clock_get_hz(clock_handle_t c);
#endif
