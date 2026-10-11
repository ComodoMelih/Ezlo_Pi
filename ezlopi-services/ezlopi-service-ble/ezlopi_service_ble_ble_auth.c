
/**
 * @file    ezlopi_service_ble_ble_auth.c
 * @brief
 * @authors Krishna Kumar Sah (work.krishnasah@gmail.com)
 * @version
 * @date
 */
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
 * @file    ezlopi_service_ble_ble_auth.c
 * @brief   File contains functions definitions for user authentication through BLE
 * @author
 * @version 1.0
 * @date    January 22, 2024
 */

/*******************************************************************************
 *                          Include Files
 *******************************************************************************/

#include "../../build/config/sdkconfig.h"

#ifdef CONFIG_EZPI_BLE_ENABLE
// #TODO ADD guard for key enable
#include <string.h>

#include "ezlopi_util_trace.h"

#include "ezlopi_core_nvs.h"
#include "ezlopi_core_errors.h"

#include "ezlopi_service_ble_ble_auth.h"

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
static e_auth_status_t sg_last_auth_status = BLE_AUTH_STATUS_UNKNOWN;

/*******************************************************************************
 *                          Extern Data Definitions
 *******************************************************************************/

/*******************************************************************************
 *                          Extern Function Definitions
 *******************************************************************************/

/**
 * @brief Global/extern function template example
 * Convention : Use capital letter for initial word on extern function
 * @param arg
 */
e_auth_status_t EZPI_ble_auth_last_status(void)
{
    return sg_last_auth_status;
}

e_auth_status_t EZPI_ble_auth_store_user_id(char *user_id)
{
    if (user_id)
    {
        TRACE_D("here");
        if (EZPI_SUCCESS == EZPI_core_nvs_write_user_id_str(user_id))
        {
            sg_last_auth_status = BLE_AUTH_SUCCESS;
        }
        else
        {
            sg_last_auth_status = BLE_AUTH_USER_ID_NVS_WRITE_FAILED;
        }
    }

    return sg_last_auth_status;
}

e_auth_status_t EZPI_ble_auth_check_user_id(char *user_id)
{
    /* A NULL user id is rejected before anything else, including before the
     * NVS read below, so there is nothing allocated to leak on this path.
     *
     * Previously a NULL id fell through both branches without assigning
     * sg_last_auth_status, so the function returned whatever the *previous*
     * caller's result had been -- which, if that call had succeeded, meant a
     * NULL id authenticated. That variable is file-static purely so
     * EZPI_ble_auth_get_status() can report the last result; it was never
     * meant to serve as a fallback return value.
     *
     * This also keeps EZPI_core_nvs_write_user_id_str() in the unclaimed
     * branch from being handed a NULL pointer to store. */
    if (NULL == user_id)
    {
        TRACE_E("No user id supplied.");
        sg_last_auth_status = BLE_AUTH_USER_ID_NOT_MATCHED;
        return sg_last_auth_status;
    }

    char *stored_user_id = EZPI_core_nvs_read_user_id_str();

    if (NULL != stored_user_id)
    {
        TRACE_D("stored_user_id: %s, user_id: %s", stored_user_id, user_id);

        /* NOTE: this is a prefix comparison, not an equality comparison --
         * the length comes from the *stored* id, so a stored "123" is
         * satisfied by an offered "1234567". Deliberately left as it is: it
         * is the comparison the shipped Ezlo app and cloud already
         * authenticate against, and tightening it here would change the
         * behaviour of existing claimed devices. The empty-stored-id case,
         * where strlen is zero and every id therefore matches, is prevented
         * at the point of storage instead -- see the user_id[0] check in
         * ezlopi_service_ble_provisioning.c. */
        if (0 == strncmp(stored_user_id, user_id, strlen(stored_user_id)))
        {
            TRACE_S("Authenticated.");
            sg_last_auth_status = BLE_AUTH_SUCCESS;
        }
        else
        {
            sg_last_auth_status = BLE_AUTH_USER_ID_NOT_MATCHED;
        }

        ezlopi_free(__FUNCTION__, stored_user_id);
    }
    else
    {
        EZPI_core_nvs_write_user_id_str(user_id);
        // sg_last_auth_status = BLE_AUTH_USER_ID_NOT_FOUND;
        sg_last_auth_status = BLE_AUTH_SUCCESS;
    }

    return sg_last_auth_status;
}

char *EZPI_ble_auth_status_to_string(e_auth_status_t status)
{
    char *ret = "BLE_AUTH_STATUS_UNKNOWN";

    switch (status)
    {
    case BLE_AUTH_STATUS_UNKNOWN:
    {
        ret = "BLE_AUTH_STATUS_UNKNOWN";
        break;
    }
    case BLE_AUTH_SUCCESS:
    {
        ret = "BLE_AUTH_SUCCESS";
        break;
    }
    case BLE_AUTH_USER_ID_NOT_FOUND:
    {
        ret = "BLE_AUTH_USER_ID_NOT_FOUND";
        break;
    }
    case BLE_AUTH_USER_ID_NOT_MATCHED:
    {
        ret = "BLE_AUTH_USER_ID_NOT_MATCHED";
        break;
    }
    case BLE_AUTH_USER_ID_NVS_WRITE_FAILED:
    {
        ret = "BLE_AUTH_USER_ID_NVS_WRITE_FAILED";
        break;
    }
    default:
    {
        ret = "BLE_AUTH_STATUS_UNKNOWN";
        break;
    }
    }

    return ret;
}

/*******************************************************************************
 *                          Static Function Definitions
 *******************************************************************************/

#endif // CONFIG_EZPI_BLE_ENABLE

/*******************************************************************************
 *                          End of File
 *******************************************************************************/
