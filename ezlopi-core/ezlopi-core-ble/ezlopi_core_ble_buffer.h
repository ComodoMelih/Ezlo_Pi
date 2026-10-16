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
 * @file    ezlopi_core_ble_buffer.h
 * @brief   perform some function on ble-operations
 * @author  Krishna Kumar Sah (work.krishnasah@gmail.com)
 * @version 0.1
 * @date    December 7th, 2022 10:22 PM
 */
#ifndef _EZLOPI_CORE_BLE_BUFFER_H_
#define _EZLOPI_CORE_BLE_BUFFER_H_

/*******************************************************************************
 *                          Include Files
 *******************************************************************************/

#include "sdkconfig.h"

#ifdef CONFIG_EZPI_BLE_ENABLE

/*******************************************************************************
 *                          Include Files
 *******************************************************************************/
#include <stdbool.h>
#include <stdint.h>

#include "esp_gatts_api.h"

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
    typedef struct s_linked_buffer
    {
        struct s_linked_buffer *next;
        uint32_t len;
        uint8_t *buffer;
    } s_linked_buffer_t;
    /*******************************************************************************
     *                          Extern Data Declarations
     *******************************************************************************/

    /*******************************************************************************
     *                          Extern Function Prototypes
     *******************************************************************************/
    /**
     * @brief This function creates and return new ble-buffer
     *
     * @param param Pointer to ble-gatts info struct
     * @return s_linked_buffer_t *
     */
    s_linked_buffer_t *EZPI_core_ble_buffer_create(esp_ble_gatts_cb_param_t *param);
    /**
     * @brief This function adds gatt params to ll-buffer
     *
     * Returns false if the chunk could not be stored, which used to be
     * unreportable: this returned void, and the only caller that could have
     * noticed was the one that needed to. A chunk silently dropped in the
     * middle of a transfer is worse than a refused transfer, because the
     * remaining chunks still arrive, the length test at the end still passes,
     * and what gets base64-decoded and written to flash is a config with a
     * hole in it.
     *
     * @param buffer Destination buffer to append incoming chunk
     * @param param Source of data
     * @return bool true if the chunk was appended
     */
    bool EZPI_core_ble_buffer_add_to_buffer(s_linked_buffer_t *buffer, esp_ble_gatts_cb_param_t *param);
    /**
     * @brief Total number of payload bytes held across every node in the list
     *
     * Callers accumulating chunks off the air need this to bound what they are
     * willing to hold: neither the node count nor the total was limited by
     * anything, and a client that keeps writing chunks which never satisfy its
     * own declared total_len could grow this list until the heap ran out --
     * before authenticating, since the BLE characteristics are open.
     *
     * @param l_buffer Head of the list, may be NULL
     * @return uint32_t Sum of every node's len, 0 for NULL
     */
    uint32_t EZPI_core_ble_buffer_total_len(const s_linked_buffer_t *l_buffer);
    /**
     * @brief This function frees a ble-buffer-node
     *
     * @param l_buffer Target node to free
     */
    void EZPI_core_ble_buffer_free_buffer(s_linked_buffer_t *l_buffer);
    /**
     * @brief This function combines all chunked data scattered in the linked list
     *
     * @param l_buffer The node in ll to start combining from.
     */
    void EZPI_core_ble_buffer_accumulate_to_start(s_linked_buffer_t *l_buffer);

#ifdef __cplusplus
}
#endif

#endif // CONFIG_EZPI_BLE_ENABLE

#endif //  _EZLOPI_CORE_BLE_BUFFER_H_

/*******************************************************************************
 *                          End of File
 *******************************************************************************/
