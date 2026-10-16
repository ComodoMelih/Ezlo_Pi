
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
 * @file    ezlopi_hal_i2c_master.c
 * @brief   perform some function on I2C
 * @author  Krishna Kumar Sah (work.krishnasah@gmail.com)
 * @version 1.0
 * @date    October 13th, 2022 7:24 PM
 */

/*******************************************************************************
 *                          Include Files
 *******************************************************************************/
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "ezlopi_util_trace.h"
#include "ezlopi_core_errors.h"

#include "ezlopi_hal_i2c_master.h"

/*******************************************************************************
 *                          Extern Data Declarations
 *******************************************************************************/

/*******************************************************************************
 *                          Extern Function Declarations
 *******************************************************************************/

/*******************************************************************************
 *                          Type & Macro Definitions
 *******************************************************************************/

/*******************************************************************************
 *                          Static Function Prototypes
 *******************************************************************************/

/*******************************************************************************
 *                          Static Data Definitions
 *******************************************************************************/

/*******************************************************************************
 *                          Extern Data Definitions
 *******************************************************************************/
/* One initialiser, not two. I2C_NUM_MAX follows SOC_I2C_NUM, which is 2 on
** esp32/esp32s2/esp32s3 but 1 on esp32c3 -- so the previous `{NULL, NULL}`
** produced "excess elements in array initializer" on every c3 build. The
** remaining elements of a static array are zero-initialised anyway, so this is
** correct for any channel count and stops asserting a 2-channel layout that is
** not universal. */
static s_ezlopi_i2c_master_t *i2c_master_conf_ptr[I2C_NUM_MAX] = {NULL};

static SemaphoreHandle_t i2c_semaphore = NULL;

/*******************************************************************************
 *                          Extern Function Definitions
 *******************************************************************************/

ezlopi_error_t EZPI_hal_i2c_master_init(s_ezlopi_i2c_master_t *i2c_master_conf)
{
    ezlopi_error_t ret = EZPI_ERR_HAL_INIT_FAILED;
    if (NULL != i2c_master_conf)
    {
        if (true == i2c_master_conf->enable)
        {
            if (NULL == i2c_master_conf_ptr[i2c_master_conf->channel])
            {
                i2c_config_t i2c_config = {
                    .mode = I2C_MODE_MASTER,
                    .sda_io_num = i2c_master_conf->sda,
                    .scl_io_num = i2c_master_conf->scl,
                    .sda_pullup_en = GPIO_PULLUP_ENABLE,
                    .scl_pullup_en = GPIO_PULLUP_ENABLE,
                    .master.clk_speed = i2c_master_conf->clock_speed,
                };

                // TRACE_W("Frequency : %d", i2c_master_conf->clock_speed);
                // TRACE_W("Pin SDA : %d", i2c_master_conf->sda);
                // TRACE_W("Pin SCL : %d", i2c_master_conf->scl);

                ESP_ERROR_CHECK(i2c_param_config(i2c_master_conf->channel, &i2c_config));
                ESP_ERROR_CHECK(i2c_driver_install(i2c_master_conf->channel, I2C_MODE_MASTER, 0, 0, 0));
                i2c_master_conf_ptr[i2c_master_conf->channel] = i2c_master_conf;
                ret = EZPI_SUCCESS;
            }
            else
            {
                /* The channel is already installed because another device
                ** brought it up. For I2C that is the ordinary case and not an
                ** error: every sensor on a bus shares SDA and SCL and is
                ** selected by address at transfer time, which is why the guard
                ** above deliberately installs only once.
                **
                ** This branch did not exist, so ret kept its initial
                ** EZPI_ERR_HAL_INIT_FAILED. Eight of the nine callers test
                ** `EZPI_SUCCESS ==` and abandon the sensor on anything else,
                ** and every I2C driver hardcodes channel I2C_NUM_0 rather than
                ** reading it from the configuration -- so the *second* I2C
                ** sensor in any configuration was always reported as failed
                ** init, no matter which pins it named. Two sensors on one bus
                ** is how I2C is normally used and it did not work.
                **
                ** All this branch has to establish is that the bus already
                ** running is the one this device asked for. */
                const s_ezlopi_i2c_master_t *installed =
                    i2c_master_conf_ptr[i2c_master_conf->channel];

                if ((installed->sda == i2c_master_conf->sda) &&
                    (installed->scl == i2c_master_conf->scl))
                {
                    ret = EZPI_SUCCESS;
                }
                else
                {
                    /* One channel cannot drive two different pin pairs. Handing
                    ** this device the pins the first one installed would put a
                    ** sensor in the device list that can never respond, so name
                    ** the clash instead of continuing quietly. */
                    TRACE_E("I2C channel %u already on SDA %d/SCL %d, this device asks for SDA %d/SCL %d - refusing",
                            (unsigned)i2c_master_conf->channel,
                            installed->sda, installed->scl,
                            i2c_master_conf->sda, i2c_master_conf->scl);
                }
            }
        }

        if (NULL == i2c_semaphore)
        {
            i2c_semaphore = xSemaphoreCreateBinary();
            if (i2c_semaphore)
            {
                TRACE_E("Created I2C semaphore");
                xSemaphoreGive(i2c_semaphore);
            }
        }
    }
    return ret;
}

esp_err_t EZPI_hal_i2c_master_read_from_device(s_ezlopi_i2c_master_t *i2c_master_conf, uint8_t *read_buffer, uint32_t read_len)
{
    int ret = EZPI_FAILED;
    if (i2c_semaphore)
    {
        if (pdTRUE == xSemaphoreTake(i2c_semaphore, portMAX_DELAY))
        {
            esp_err_t error = i2c_master_read_from_device(i2c_master_conf->channel, i2c_master_conf->address, read_buffer, read_len, 200);
            xSemaphoreGive(i2c_semaphore);
            if (ESP_OK == error)
            {
                ret = EZPI_SUCCESS;
            }
        }
    }
    return ret;
}

esp_err_t EZPI_hal_i2c_master_write_to_device(s_ezlopi_i2c_master_t *i2c_master_conf, uint8_t *write_buffer, uint32_t write_len)
{
    int ret = EZPI_FAILED;
    if (i2c_semaphore)
    {
        if (pdTRUE == xSemaphoreTake(i2c_semaphore, portMAX_DELAY))
        {
            esp_err_t error = i2c_master_write_to_device(i2c_master_conf->channel, i2c_master_conf->address, write_buffer, write_len, 200);
            xSemaphoreGive(i2c_semaphore);
            if (ESP_OK == error)
            {
                ret = EZPI_SUCCESS;
            }
        }
    }
    return ret;
}

void EZPI_hal_i2c_master_deinit(s_ezlopi_i2c_master_t *i2c_master_conf)
{
    if (NULL != i2c_master_conf)
    {
        if (1 == i2c_master_conf_ptr[i2c_master_conf->channel]->channel)
        {
            i2c_driver_delete(i2c_master_conf->channel);
            i2c_master_conf_ptr[i2c_master_conf->channel] = NULL;
        }
    }
    if (NULL != i2c_semaphore)
    {
        vSemaphoreDelete(i2c_semaphore);
        i2c_semaphore = NULL;
    }
}

/*******************************************************************************
 *                         Static Function Definitions
 *******************************************************************************/

// static int ezlopi_i2c_check_channel(s_ezlopi_i2c_master_t *i2c_master_conf)
// {
//     int ret = 0;

//     for (int idx = 0; idx < I2C_NUM_MAX; idx++)
//     {
//         if (NULL != i2c_master_conf_ptr)
//         {
//         }
//     }

//     return ret;
// }

/*******************************************************************************
 *                          End of File
 *******************************************************************************/
