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
 * @file    ezlopi_service_system_temperature_sensor.h
 * @brief   Contains function declarations related to system temperature sensor
 * @authors
 * @version 1.0
 * @date    September 9th, 2024 7:15 PM
 */

#include "sdkconfig.h"
#if defined(CONFIG_EZPI_ENABLE_SYSTEM_TEMPERATURE)

#ifndef __EZLOPI_SERVICE_SYSTEM_TEMPERATURE_SENSOR_H__
#define __EZLOPI_SERVICE_SYSTEM_TEMPERATURE_SENSOR_H__

/*******************************************************************************
 *                          Include Files
 *******************************************************************************/
/* For ezlopi_error_t in the prototype below. This header still takes
** e_ezlopi_actions_t and l_ezlopi_item_t from whatever includes it, which it
** always has; adding the include for the type this change introduces is as
** far as that goes here rather than reordering a header nobody asked about. */
#include "ezlopi_core_errors.h"

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

    /*******************************************************************************
     *                          Extern Data Declarations
     *******************************************************************************/

    /*******************************************************************************
     *                          Extern Function Prototypes
     *******************************************************************************/

    /**
     * @brief Function that handles different actions related to temperatore sensor
     *
     * @param action Action to process
     * @param item Pointer to item linked to temperature sensor
     * @param arg Action related argument
     * @param user_arg User argument
     * @return ezlopi_error_t EZPI_SUCCESS, or EZPI_FAILED if the action was
     *         EZLOPI_ACTION_PREPARE and the device or its item could not be
     *         added. Every other action reports EZPI_SUCCESS, including the
     *         ones that do nothing.
     */
    ezlopi_error_t EZPI_system_temperature_device(e_ezlopi_actions_t action, l_ezlopi_item_t *item, void *arg, void *user_arg);

#ifdef __cplusplus
}
#endif

#endif // __EZLOPI_SERVICE_SYSTEM_TEMPERATURE_SENSOR_H__

#endif // CONFIG_EZPI_ENABLE_SYSTEM_TEMPERATURE

/*******************************************************************************
 *                          End of File
 *******************************************************************************/
