#ifndef FAKE_SPI_H
#define FAKE_SPI_H
#include <stdint.h>
#include <stddef.h>
typedef struct spi_inst spi_inst_t;
extern spi_inst_t *spi0, *spi1;
typedef enum { SPI_CPOL_0 = 0, SPI_CPOL_1 = 1 } spi_cpol_t;
typedef enum { SPI_CPHA_0 = 0, SPI_CPHA_1 = 1 } spi_cpha_t;
typedef enum { SPI_LSB_FIRST = 0, SPI_MSB_FIRST = 1 } spi_order_t;
unsigned spi_init(spi_inst_t *spi, unsigned hz);
unsigned spi_set_baudrate(spi_inst_t *spi, unsigned hz);
void spi_set_format(spi_inst_t *spi, unsigned bits, spi_cpol_t, spi_cpha_t, spi_order_t);
int  spi_write_blocking(spi_inst_t *spi, const uint8_t *src, size_t len);
int  spi_write16_blocking(spi_inst_t *spi, const uint16_t *src, size_t len);
#endif
