/* ===========================================================================
** Copyright (C) 2025 Ezlo Innovation Inc
**
** Under EZLO AVAILABLE SOURCE LICENSE (EASL) AGREEMENT
** ===========================================================================
*/

/**
 * @file    ezlopi_core_pin_reserved.c
 * @brief   Which pins the chip is using for flash/PSRAM, asked of the chip.
 *
 * WHY THIS IS NOT A TABLE
 *
 * The pins are in efuse, not in the part number. soc/spi_pins.h for the esp32
 * carries two sets -- SPI_IOMUX_PIN_NUM_* (6,7,8,9,10,11) and
 * SPI_D2WD_PIN_NUM_* (17,8,6,16,7,11) -- because parts with embedded flash
 * remap them, and the bootloader picks between them by reading efuse
 * (bootloader_flash_config_esp32.c:88). A table saying "6..11 on esp32" would
 * therefore be wrong on a PICO-D4 or D2WD in the direction that costs most:
 * refusing GPIO 16 and 17, which on such a part are flash, while allowing 9 and
 * 10, which are not.
 *
 * So the question is put to esp_mspi_get_io(), which is the function ESP-IDF
 * itself uses for it (spi_flash/flash_ops.c:964). It handles the efuse decode,
 * the esp32 WP pad -- which is in the image header rather than efuse and so
 * needs bootloader_flash_get_wp_pin() -- and the PSRAM chip select.
 *
 * THE PRIVATE HEADER
 *
 * esp_mspi_get_io() is declared in spi_flash/include/esp_private/spi_flash_os.h.
 * "esp_private" means IDF promises nothing about it across versions. That cost
 * is accepted here, and the alternative was weighed: decoding
 * esp_rom_efuse_get_flash_gpio_info() by hand needs the 6-bits-per-pad layout,
 * the 0-means-unset convention, the 0x3f WP convention and the esp32 WP special
 * case reimplemented -- which is a copy of this function that cannot be
 * corrected by an IDF update, rather than a dependency that can. If a future
 * IDF removes it the build breaks loudly at the call, which is the failure mode
 * to prefer over a table that silently describes the wrong chip.
 *
 * TWO THINGS DELIBERATELY NOT REPORTED
 *
 *   - A decoded pin number of 0. ROM uses 0 as "unset" for every pad except
 *     WP, which flash_ops.c:985 states outright: "rom code take 0x3f as invalid
 *     wp pad num, but take 0 as other invalid mspi pads num". Treating a
 *     returned 0 as GPIO 0 would reserve the pin that is the boot strap and the
 *     user button on most of these boards. No part uses GPIO 0 for flash, so
 *     dropping 0 cannot hide a real reservation.
 *
 *   - The octal pads, ESP_MSPI_IO_DQS and ESP_MSPI_IO_D4..D7. They exist only
 *     where SOC_SPI_MEM_SUPPORT_OPI_MODE is set, and on an esp32s3 with quad
 *     flash -- which is what sdkconfig.defaults.esp32s3 configures -- their
 *     default pin numbers 33..37 are free for a user to wire a sensor to.
 *     Reporting them would refuse working configurations, which is the one
 *     error this area must not make. A board actually running octal flash has
 *     them in efuse, and this file does not claim to cover that case; it is
 *     named here so the limit is visible rather than discovered.
 *
 * PSRAM is included (ESP_MSPI_IO_CS1) but exists in the enum only under
 * CONFIG_SPIRAM, which is off in all four of this project's defaults. The
 * conditional is kept so that enabling PSRAM extends the answer rather than
 * silently leaving its chip select unprotected.
 */

/*******************************************************************************
 *                          Include Files
 *******************************************************************************/

#include <stdint.h>

#include "sdkconfig.h"
#include "driver/gpio.h"
#include "esp_private/spi_flash_os.h"

#include "ezlopi_core_pin_reserved.h"

/*******************************************************************************
 *                          Static Data
 *******************************************************************************/

/* Built once. esp_mspi_get_io() reads efuse and, on the esp32, the image
** header; none of that changes while the firmware runs, and the 144 call sites
** that reach this through EZPI_core_cjson_get_value_gpio() would otherwise
** repeat it per pin per device. uint64_t because GPIO_NUM_MAX is 49 on the
** esp32s3. */
static uint64_t s_mspi_pin_mask;
static bool s_mspi_pin_mask_valid;

/*******************************************************************************
 *                          Public Functions
 *******************************************************************************/

bool EZPI_core_pin_is_mspi(int gpio_num)
{
    if (false == s_mspi_pin_mask_valid)
    {
        static const esp_mspi_io_t pads[] = {
            ESP_MSPI_IO_CLK,
            ESP_MSPI_IO_Q,
            ESP_MSPI_IO_D,
            ESP_MSPI_IO_CS0,
            ESP_MSPI_IO_HD,
            ESP_MSPI_IO_WP,
#if CONFIG_SPIRAM
            ESP_MSPI_IO_CS1,
#endif
        };

        uint64_t mask = 0;

        for (size_t i = 0; i < (sizeof(pads) / sizeof(pads[0])); i++)
        {
            const uint8_t pin = esp_mspi_get_io(pads[i]);

            /* 0 is ROM's "unset", not GPIO 0 -- see the note at the top. The
            ** upper test keeps the shift count in range: a 0x3f decoded out of
            ** a blank efuse field is 63, which is a defined shift for a
            ** uint64_t but is not a pin, and on the esp32c3 (GPIO_NUM_MAX 22)
            ** would otherwise set a bit no pin can ever match. */
            if ((0 != pin) && (pin < (uint8_t)GPIO_NUM_MAX))
            {
                mask |= (1ULL << pin);
            }
        }

        s_mspi_pin_mask = mask;
        s_mspi_pin_mask_valid = true;
    }

    if ((gpio_num < 0) || (gpio_num >= (int)GPIO_NUM_MAX))
    {
        return false;
    }

    return (0 != (s_mspi_pin_mask & (1ULL << gpio_num)));
}
