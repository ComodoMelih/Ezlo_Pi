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
 * @file    pms5003_parse.c
 * @brief   PMS5003 frame parsing, separated from the driver's hardware setup.
 *
 * Both functions here were static in pms5003.c and moved verbatim. The split
 * is what lets tests/host compile them: pms5003.c drives GPIOs and creates an
 * esp_timer, so it cannot be built by a host compiler, while nothing below
 * touches a peripheral. See pms5003_parse.h for why the dependency direction
 * matters.
 */

/*******************************************************************************
 *                          Include Files
 *******************************************************************************/

#include <string.h>

#include "esp_log.h"
#include "ezlopi_util_trace.h"

#include "pms5003_parse.h"

/*******************************************************************************
 *                          Extern Function Definitions
 *******************************************************************************/

esp_err_t pms_read_upcall(uint8_t *buffer, uint32_t output_len, PM25_AQI_Data *data)
{
  uint16_t sum = 0;

  /* Reject anything shorter than a frame before reading any of it. buffer is a
     heap block sized to the bytes that actually arrived on the line, and this
     function reads a fixed 32 bytes out of it regardless -- so a UART event
     carrying a partial frame, which is the ordinary way a 32-byte frame
     arrives on a busy line rather than a malformed-input case, was read past
     the end of the allocation. output_len was already a parameter of the
     calling upcall and was simply never passed down or consulted. */
  if ((NULL == buffer) || (NULL == data) || (output_len < PMS_MIN_FRAME_LEN))
  {
    return ESP_FAIL;
  }

  memset(data, 0, sizeof(PM25_AQI_Data));
  /* Check that start byte is correct! */
  if (buffer[0] != 0x42)
  {
    return ESP_FAIL;
  }

  /* Get checksum ready */
  for (uint8_t i = 0; i < 30; i++)
  {
    sum += buffer[i];
  }

  /* The data comes in endian'd, this solves it so it works on all platforms */
  uint16_t buffer_u16[15];
  for (uint8_t i = 0; i < 15; i++)
  {
    buffer_u16[i] = buffer[2 + i * 2 + 1];
    buffer_u16[i] += (buffer[2 + i * 2] << 8);
  }

  /* put it into a nice struct :) */
  memcpy((void *)data, (void *)buffer_u16, 30);

  if (sum != data->checksum)
  {
    ESP_LOGE(__FILENAME__, "CheckSum Failed");
    return ESP_FAIL;
  }
  /* success! */
  return ESP_OK;
}

void EZPI_pms5003_uart_upcall(uint8_t *buffer, uint32_t output_len, s_ezlopi_uart_object_handle_t uart_object_handle)
{
  /* Check the handle before reading through it; this runs on the UART event
     task, where a fault takes down that task rather than the caller. */
  if (NULL == uart_object_handle)
  {
    return;
  }

  PM25_AQI_Data *data = (PM25_AQI_Data *)uart_object_handle->arg;
  pms_read_upcall(buffer, output_len, data);
}

/*******************************************************************************
 *                          End of File
 *******************************************************************************/
