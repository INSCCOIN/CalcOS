#ifndef FAKE_I2C_H
#define FAKE_I2C_H
#include <stdint.h>
#include <stddef.h>
typedef struct i2c_inst i2c_inst_t;
extern i2c_inst_t *i2c0, *i2c1;
unsigned i2c_init(i2c_inst_t *i2c, unsigned hz);
int i2c_write_timeout_us(i2c_inst_t *i2c, uint8_t addr, const uint8_t *src,
                         size_t len, int nostop, unsigned timeout);
int i2c_read_timeout_us(i2c_inst_t *i2c, uint8_t addr, uint8_t *dst,
                        size_t len, int nostop, unsigned timeout);
#endif
