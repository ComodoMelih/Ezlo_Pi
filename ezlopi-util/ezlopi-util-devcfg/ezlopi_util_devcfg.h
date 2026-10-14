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
 * @file    ezlopi_util_devcfg.h
 * @brief   What to do when the stored device configuration will not parse
 * @author
 * @version 1.0
 * @date    13th OCT 2025
 *
 * EZPI_device_prepare() reads the device configuration out of the factory-info
 * partition once per boot. When the parse fails it has to choose between three
 * things, and the choice is made here so that it can be tested on a host:
 * the function it belongs to touches NVS, cJSON and esp_restart().
 *
 * WHY THIS EXISTS. The previous behaviour was to stamp this chip's name into
 * the config, write it back and reboot, unconditionally, for every parse
 * failure. That is a correct repair for exactly one cause -- a config saved
 * without a "chipset" key, which is the migration this code was written for --
 * and an unrecoverable reboot loop for every other cause:
 *
 *   - cjext's cJSON_AddStringToObject() appends. It does not replace, and
 *     get_object_item() returns the *first* key of a given name. So when a
 *     "chipset" key is already there and is the reason the parse failed (wrong
 *     chip, or not a string), stamping adds a second key that is never read.
 *     The next boot fails identically, stamps again, and reboots again.
 *   - When the chipset is right and a device entry is what failed, stamping
 *     changes nothing relevant at all.
 *
 * The loop is not survivable from the outside. The reboot happens inside
 * EZPI_init(), which runs before EZPI_SERV_uart_init() and
 * EZPI_ble_service_init(), so no BLE, no UART and no websocket ever come up:
 * the hub cannot be reconfigured or factory-reset over any interface. With
 * CONFIG_FREERTOS_HZ=1000 the vTaskDelay(1000) ahead of the restart is one
 * second, so the cycle also rewrites the configuration to flash about once per
 * second for as long as the board has power.
 *
 * THE RULE. Only reboot when rebooting can change the answer, which is only
 * when the key being added was absent. Otherwise finish booting: the services
 * come up, and a reachable hub can be fixed over BLE or the local API, which
 * is the whole difference between a bad config and a brick.
 *
 * A mismatched chipset is deliberately NOT rewritten to match the running
 * chip. The pin numbers in such a config were chosen for different hardware,
 * so adopting it silently would instantiate devices on whatever GPIOs those
 * numbers happen to name here. Refusing it and staying reachable is the
 * weaker action and the safer one.
 *
 * TERMINATION. EZPI_DEVCFG_STAMP_AND_REBOOT is returned only when
 * chipset_key_present is false, and stamping the key makes it true, so the
 * input that produced a reboot cannot recur after that reboot. At most one
 * restart can be attributed to this path. tests/host/test_devcfg.c drives
 * every combination of the three inputs through a model of the boot cycle and
 * fails if any of them restarts more than once, and keeps a copy of the old
 * unconditional behaviour to show that it does not terminate.
 */

#ifndef _EZLOPI_UTIL_DEVCFG_H_
#define _EZLOPI_UTIL_DEVCFG_H_

/*******************************************************************************
 *                          Include Files
 *******************************************************************************/
#include <stdbool.h>

#ifdef __cplusplus
extern "C"
{
#endif

    /*******************************************************************************
     *                          Type & Macro Declarations
     *******************************************************************************/

    /**
     * @brief What EZPI_device_prepare() should do about a configuration parse.
     */
    typedef enum e_ezpi_devcfg_action
    {
        /** The configuration parsed. Carry on booting; nothing to repair. */
        EZPI_DEVCFG_PROCEED = 0,

        /**
         * The "chipset" key was missing. Add it, persist the configuration and
         * restart, so the next boot parses it. Safe to act on because the key
         * is absent, which is what bounds this to a single restart.
         */
        EZPI_DEVCFG_STAMP_AND_REBOOT,

        /**
         * The configuration is faulty and restarting cannot change that.
         * Finish booting anyway, so the hub stays reachable and can be given a
         * corrected configuration.
         *
         * Not necessarily with no devices. ezlopi_device_parse_json_v3()
         * instantiates each device as it walks the list and reports only the
         * outcome of the last entry it touched, so a failure on a late entry
         * leaves the earlier devices already built and working. The two
         * chipset failures do give an empty list, because they return before
         * the device loop is entered.
         */
        EZPI_DEVCFG_CONTINUE_BOOT,
    } e_ezpi_devcfg_action_t;

    /*******************************************************************************
     *                          Function Prototypes
     *******************************************************************************/

    /**
     * @brief   Decide how to react to the result of parsing the stored config.
     *
     * @param   parse_ok            Whether the configuration parsed.
     * @param   chipset_key_present Whether the stored configuration already
     *                              carries a "chipset" key, irrespective of
     *                              its type or value. If it is there, stamping
     *                              it again is a no-op that is never read.
     * @param   target_name_known   Whether this firmware build knows the name
     *                              to stamp for the chip it is compiled for.
     *                              False on a target with no branch in the
     *                              stamping code, where a restart would repair
     *                              nothing.
     *
     * @return  The action to take; see e_ezpi_devcfg_action_t.
     */
    e_ezpi_devcfg_action_t EZPI_util_devcfg_recovery(bool parse_ok,
                                                    bool chipset_key_present,
                                                    bool target_name_known);

#ifdef __cplusplus
}
#endif

#endif // _EZLOPI_UTIL_DEVCFG_H_

/*******************************************************************************
 *                          End of File
 *******************************************************************************/
