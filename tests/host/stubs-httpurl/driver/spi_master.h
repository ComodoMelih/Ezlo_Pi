/* Host stub for driver/spi_master.h.
 *
 * One typedef, because that is all ezlopi_hal_spi_master.h needs to declare
 * its prototypes, and that header is pulled in transitively by the scenes
 * then-methods translation unit that test_httpurl compiles.
 *
 * The real type is an opaque handle -- `typedef struct spi_device_t
 * *spi_device_handle_t` in the IDF -- so `void *` is equivalent for every use
 * a host build can make of it: nothing here dereferences it, and any source
 * that tried would not link because no SPI function is declared.
 */
#ifndef _HOST_STUB_DRIVER_SPI_MASTER_H_
#define _HOST_STUB_DRIVER_SPI_MASTER_H_

typedef void *spi_device_handle_t;

#endif /* _HOST_STUB_DRIVER_SPI_MASTER_H_ */
