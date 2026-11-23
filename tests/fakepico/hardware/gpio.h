#ifndef FAKE_GPIO_H
#define FAKE_GPIO_H
#include <stdint.h>
typedef enum { GPIO_FUNC_SPI = 1, GPIO_FUNC_I2C = 3, GPIO_FUNC_PWM = 4 } gpio_function_t;
#define GPIO_OUT 1
#define GPIO_IN  0
void gpio_init(unsigned pin);
void gpio_set_dir(unsigned pin, int out);
void gpio_put(unsigned pin, int value);
void gpio_set_function(unsigned pin, gpio_function_t f);
void gpio_pull_up(unsigned pin);
#endif
