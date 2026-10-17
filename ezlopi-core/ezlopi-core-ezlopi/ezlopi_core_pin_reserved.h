/* ===========================================================================
** Copyright (C) 2025 Ezlo Innovation Inc
**
** Under EZLO AVAILABLE SOURCE LICENSE (EASL) AGREEMENT
** ===========================================================================
*/

/**
 * @file    ezlopi_core_pin_reserved.h
 * @brief   Is a pin number one the chip is currently using for flash or PSRAM?
 *
 * Deliberately free of ESP-IDF types, so that
 * ezlopi_core_cjson_macros.c -- which the host suite compiles with the system
 * compiler -- can call this without the host needing a stub for
 * esp_private/spi_flash_os.h. The implementation lives in
 * ezlopi_core_pin_reserved.c, which the host suite does not compile;
 * tests/host/test_cjsongpio.c supplies its own definition of this one function
 * instead, which is what lets the policy be tested against a chosen pin set.
 */

#ifndef _EZLOPI_CORE_PIN_RESERVED_H_
#define _EZLOPI_CORE_PIN_RESERVED_H_

#include <stdbool.h>

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief   True if gpio_num is a pin this chip is using for the main SPI
     *          flash or PSRAM right now.
     *
     * Answered at runtime rather than from a table, because the set is not a
     * property of the target: it is in efuse, and parts with embedded flash
     * remap it (the esp32 SoC header carries a whole second set of pin numbers,
     * SPI_D2WD_PIN_NUM_*, for that reason). A hard-coded "6..11 on esp32" list
     * would be wrong on those parts in the expensive direction -- refusing pins
     * that are free.
     *
     * Out-of-range and sentinel values return false. This answers one question
     * only; it is not a general "is this pin usable" test.
     */
    bool EZPI_core_pin_is_mspi(int gpio_num);

#ifdef __cplusplus
}
#endif

#endif /* _EZLOPI_CORE_PIN_RESERVED_H_ */
