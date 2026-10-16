/* Host stub for driver/i2c.h.
 *
 * Enough of the IDF I2C master API for the *real*
 * ezlopi-hal/ezlopi-hal-i2c/ezlopi_hal_i2c_master.c to compile on a host. The
 * types and constants are here; the five functions are only declared, so the
 * test linking this must supply them. That split is deliberate and matches
 * stubs/driver/gpio.h: a header that quietly returned ESP_OK for
 * i2c_driver_install() would let the HAL appear to work while nothing checked
 * how often it was called, and "how often is the driver installed" is exactly
 * the question the shared-bus test asks.
 *
 * i2c_config_t carries the fields the HAL sets and nothing else. If the HAL
 * starts setting another field, this fails to compile, which is the intended
 * way to find out.
 */
#ifndef _HOST_STUB_DRIVER_I2C_H_
#define _HOST_STUB_DRIVER_I2C_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* The real header is `#define I2C_NUM_MAX (SOC_I2C_NUM)`, and in IDF v4.4.6
 * SOC_I2C_NUM is 2 on esp32, esp32s2 and esp32s3 but 1 on esp32c3. 2 is used
 * here because the HAL sizes its channel table with it, and a one-element table
 * would make a channel-indexing mistake unreachable by the test rather than
 * caught by it.
 *
 * The consequence to be aware of: the host test's second-channel case is not
 * reachable on esp32c3, where only channel 0 exists. No driver in the tree asks
 * for channel 1 today -- they all hardcode I2C_NUM_0 -- so that case tests the
 * table indexing, not a configuration that currently ships. */
#define I2C_NUM_0 0
#define I2C_NUM_1 1
#define I2C_NUM_MAX 2

typedef int i2c_port_t;

typedef enum
{
    I2C_MODE_SLAVE = 0,
    I2C_MODE_MASTER,
    I2C_MODE_MAX
} i2c_mode_t;

#define GPIO_PULLUP_DISABLE 0
#define GPIO_PULLUP_ENABLE 1

typedef struct
{
    i2c_mode_t mode;
    int sda_io_num;
    int scl_io_num;
    int sda_pullup_en;
    int scl_pullup_en;
    struct
    {
        uint32_t clk_speed;
    } master;
    uint32_t clk_flags;
} i2c_config_t;

esp_err_t i2c_param_config(i2c_port_t port, const i2c_config_t *conf);
esp_err_t i2c_driver_install(i2c_port_t port, i2c_mode_t mode,
                             size_t slv_rx_buf_len, size_t slv_tx_buf_len,
                             int intr_alloc_flags);
esp_err_t i2c_driver_delete(i2c_port_t port);
esp_err_t i2c_master_read_from_device(i2c_port_t port, uint8_t device_address,
                                      uint8_t *read_buffer, size_t read_size,
                                      int ticks_to_wait);
esp_err_t i2c_master_write_to_device(i2c_port_t port, uint8_t device_address,
                                     const uint8_t *write_buffer,
                                     size_t write_size, int ticks_to_wait);

#endif /* _HOST_STUB_DRIVER_I2C_H_ */
