/* ===========================================================================
** Copyright (C) 2025 Ezlo Innovation Inc
**
** Under EZLO AVAILABLE SOURCE LICENSE (EASL) AGREEMENT
**
** Redistribution and use in source and binary forms, with or without
** modification, are permitted provided that the following conditions are met:
**
** 1. Redistributions of source code must retain the above copyright notice,
**    this list of conditions and the following disclaimer.
** 2. Redistributions in binary form must reproduce the above copyright
**    notice, this list of conditions and the following disclaimer in the
**    documentation and/or other materials provided with the distribution.
** 3. Neither the name of the copyright holder nor the names of its
**    contributors may be used to endorse or promote products derived from
**    this software without specific prior written permission.
**
** THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
** AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
** IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
** ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
** LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
** CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
** SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
** INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
** CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
** ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
** POSSIBILITY OF SUCH DAMAGE.
** ===========================================================================
*/

/**
 * @file    pms5003_parse.h
 * @brief   PMS5003 frame parsing, separated from the driver's hardware setup.
 *
 * This header exists so the parser can be compiled without ESP-IDF. pms5003.h
 * pulls in driver/gpio.h and esp_log.h to get GPIO_NUM_* for its pin macros,
 * and pms5003.c drives GPIOs and creates an esp_timer, so neither can be built
 * by a host compiler. The bytes arriving from the part are attacker- or
 * noise-controlled and the parser reads a fixed 32 bytes out of a
 * variable-length heap block, so it is exactly the part worth testing under a
 * sanitizer.
 *
 * Nothing moved behaviourally: pms_read_upcall() and EZPI_pms5003_uart_upcall()
 * are the same functions that used to be static in pms5003.c, with the same
 * bodies. They are declared here only so that pms5003.c can still register the
 * upcall with the UART HAL and so that tests/host can reach them.
 *
 * The dependency direction is the point: pms5003.h includes this header, never
 * the reverse. Adding an ESP-IDF include below would put the parser back out of
 * reach of the host suite, so please do not.
 */

#ifndef _PMS5003_PARSE_H_
#define _PMS5003_PARSE_H_

/*******************************************************************************
 *                          Include Files
 *******************************************************************************/

#include <stdint.h>

#include "esp_err.h"
#include "ezlopi_hal_uart.h"

/*******************************************************************************
 *                          C++ Declaration Wrapper
 *******************************************************************************/
#ifdef __cplusplus
extern "C"
{
#endif

    /*******************************************************************************
     *                          Type & Macro Declarations
     *******************************************************************************/

    /* Highest buffer index pms_read_upcall() touches, plus one. The checksum
     * walk covers indices 0..29 and the endian fix-up reads index
     * 2 + 14 * 2 + 1 = 31, so a frame has to be at least 32 bytes before any of
     * it may be read. The part itself emits 32-byte frames. */
#define PMS_MIN_FRAME_LEN 32

    /**! Structure holding Plantower's standard packet **/
    typedef struct PMSAQIdata
    {
        uint16_t framelen;       ///< How long this data chunk is
        uint16_t pm10_standard,  ///< Standard PM1.0
            pm25_standard,       ///< Standard PM2.5
            pm100_standard;      ///< Standard PM10.0
        uint16_t pm10_env,       ///< Environmental PM1.0
            pm25_env,            ///< Environmental PM2.5
            pm100_env;           ///< Environmental PM10.0
        uint16_t particles_03um, ///< 0.3um Particle Count
            particles_05um,      ///< 0.5um Particle Count
            particles_10um,      ///< 1.0um Particle Count
            particles_25um,      ///< 2.5um Particle Count
            particles_50um,      ///< 5.0um Particle Count
            particles_100um;     ///< 10.0um Particle Count
        uint16_t unused;         ///< Unused
        uint16_t checksum;       ///< Packet checksum
        uint8_t available;
    } PM25_AQI_Data;

    /*******************************************************************************
     *                          Extern Function Prototypes
     *******************************************************************************/

    /**
     * @brief Parse one PMS5003 frame out of a received buffer.
     *
     * @param buffer      Bytes received on the UART. May be NULL.
     * @param output_len  How many bytes @p buffer actually holds. A frame
     *                    shorter than PMS_MIN_FRAME_LEN is rejected without
     *                    any of it being read.
     * @param data        Destination. Zeroed on entry, so a rejected frame
     *                    also clears any previous reading.
     *
     * @return ESP_OK when the frame's start byte and checksum both agree,
     *         ESP_FAIL otherwise.
     */
    esp_err_t pms_read_upcall(uint8_t *buffer, uint32_t output_len, PM25_AQI_Data *data);

    /**
     * @brief The callback the UART HAL invokes. Registered by pms_init().
     *
     * Takes its destination from @p uart_object_handle->arg, which pms_init()
     * sets to the driver's PM25_AQI_Data.
     */
    void EZPI_pms5003_uart_upcall(uint8_t *buffer, uint32_t output_len, s_ezlopi_uart_object_handle_t uart_object_handle);

#ifdef __cplusplus
}
#endif

#endif // _PMS5003_PARSE_H_

/*******************************************************************************
 *                          End of File
 *******************************************************************************/
