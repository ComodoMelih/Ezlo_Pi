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
 * @file    ezlopi_cloud_devcfg_port.c
 * @brief   The ESP-IDF side of the config API's six dependencies
 * @version 1.0
 * @date    13th OCT 2025
 *
 * Six forwarding bodies. This file exists so that the file with the decisions
 * in it does not include ezlopi_core_factory_info.h, which pulls in
 * <esp_partition.h>, or ezlopi_core_devices_list.h, which pulls in
 * ezlopi_core_devices.h and with it the whole HAL. See the header for the
 * argument.
 *
 * Nothing here is tested on a host, and that is the deal: the untestable part
 * of the feature is confined to this file, where it is short enough to read in
 * full and has no branch that could be wrong in an interesting way. The one
 * loop is the device-table walk, and it is the table's own documented
 * termination condition.
 */

/*******************************************************************************
 *                          Include Files
 *******************************************************************************/
#include <stdbool.h>
#include <stdint.h>

#include "EZLOPI_USER_CONFIG.h"

#include "cjext.h"

#include "ezlopi_core_devices.h"
#include "ezlopi_core_devices_list.h"
#include "ezlopi_core_factory_info.h"

#include "ezlopi_cloud_devcfg_port.h"

/*******************************************************************************
 *                          Extern Function Definitions
 *******************************************************************************/

char *EZPI_CLOUD_devcfg_port_read(void)
{
    /* Returns a fresh allocation every call -- unlike the SSL shared key next
    ** to it in that file, which caches into a static and must not be freed.
    ** That difference is the reason this port names the release explicitly. */
    return EZPI_core_factory_info_v3_get_ezlopi_config();
}

void EZPI_CLOUD_devcfg_port_free(char *str)
{
    if (str)
    {
        EZPI_core_factory_info_v3_free(str);
    }
}

bool EZPI_CLOUD_devcfg_port_write(cJSON *root)
{
    /* The int this returns is 1 on a completed write and 0 otherwise. Compared
    ** against 0 rather than assigned to a bool so that a future non-zero
    ** failure code could not read as success. */
    return (0 != EZPI_core_factory_info_v3_set_ezlopi_config(root));
}

const char *EZPI_CLOUD_devcfg_port_chipset(void)
{
    return EZPI_core_device_expected_chipset_name();
}

uint32_t EZPI_CLOUD_devcfg_port_region_size(void)
{
    /* The field's own size. Note that the storer serialises into a hardcoded
    ** 4 * 1024 buffer rather than into this constant; the two are equal today
    ** (0x1000), and if they ever stop being equal the smaller one is the real
    ** limit. Returning the field size is right either way: a document that
    ** fits the field but not the storer's buffer is refused by the storer,
    ** which reports failure rather than truncating. */
    return (uint32_t)EZLOPI_FINFO_LEN_EZLOPI_CONFIG_JSON;
}

bool EZPI_CLOUD_devcfg_port_driver_exists(uint32_t id_item)
{
    s_ezlopi_device_v3_t *list = EZPI_core_devices_list_get_list_v3();
    bool found = false;

    if (list)
    {
        int idx = 0;

        /* The same termination condition the boot parser uses: the table ends
        ** at the first row with no function pointer. Stops at the first match
        ** where the parser keeps walking -- it has to, because it calls every
        ** matching row, and here one is enough to answer the question. */
        while ((false == found) && (NULL != list[idx].func))
        {
            if (id_item == list[idx].id)
            {
                found = true;
            }

            idx++;
        }
    }

    return found;
}

/*******************************************************************************
 *                          End of File
 *******************************************************************************/
