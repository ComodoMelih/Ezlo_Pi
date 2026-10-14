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
 * @file    ezlopi_util_devcfgval.h
 * @brief   Whether a device configuration is worth storing
 * @version 1.0
 * @date    13th OCT 2025
 *
 * WHY THIS EXISTS
 *
 * Storing the device configuration is how this firmware is told which sensors
 * exist and which GPIOs they are on. Until now the only two ways in were the
 * BLE dynamic-config characteristic and the UART command channel -- not the BLE
 * *provisioning* characteristic, which writes connection data and never touches
 * the device config -- and neither checks anything about what it was handed
 * beyond "cJSON parsed it". From ezlopi_service_ble_dynamic_config.c:243:
 *
 *     cJSON *cjson_config = cJSON_Parse(__FUNCTION__, decoded_data);
 *     if (cjson_config)
 *     {
 *         if (EZPI_core_factory_info_v3_set_ezlopi_config(cjson_config))
 *
 * and the same shape at ezlopi_service_uart.c:1397.
 *
 * So `{"hello":"world"}` is accepted and written to flash. It is valid JSON
 * and it is not a device configuration. Nothing complains at the time. On the
 * next boot EZPI_device_prepare() reads it back, ezlopi_device_parse_json_v3()
 * reports EZPI_ERR_JSON_PARSE_FAILED because there is no "chipset"
 * (ezlopi_core_devices.c:978), the recovery rule stamps the chipset name and
 * restarts once, and the boot after that finds a chipset that matches and no
 * "dev_detail" at all -- which is not an error to the parser, so it reports
 * success with an empty device list.
 *
 * What the user sees is a controller that works and has forgotten every
 * sensor, with nothing connecting that to the configuration they sent some
 * minutes earlier.
 *
 * This is the check that should have been there. It is separate from the
 * parser for the reason the rest of tests/host exists: ezlopi_core_devices.c
 * cannot be host-compiled -- it reads the factory-info partition, drives the
 * device table and calls esp_restart() -- and the decision about whether a
 * configuration is acceptable is exactly the part worth testing.
 *
 * It is NOT a second parser. It answers one question, with no side effects,
 * and it allocates nothing.
 *
 * WHAT IT ACCEPTS, AND WHY THAT MATTERS MORE THAN WHAT IT REJECTS
 *
 * The Ezlo app writes these configurations, and this firmware has to keep
 * working with it. So the rule is deliberately asymmetric: reject only shapes
 * that *cannot* work on this build, and ignore everything else. Unknown
 * members, extra members, and anything this file has no opinion about are
 * accepted, because a configuration carrying a key added by a newer app must
 * not be refused by an older hub.
 *
 * Each rejection below is a shape where the stored configuration would produce
 * a device list the user did not ask for:
 *
 *   - no "chipset", not a string, or naming a different chip.
 *     ezlopi_device_parse_json_v3() returns before it reaches the device loop
 *     in all three cases, so the hub boots with an empty device list. A
 *     mismatched chipset is refused rather than adopted on purpose: the pin
 *     numbers in it were chosen for different hardware, so honouring it would
 *     instantiate devices on whatever GPIOs those numbers happen to name here.
 *     The same reasoning is set out at greater length in ezlopi_util_devcfg.h.
 *
 *   - no "dev_detail", or a "dev_detail" that is not an array. The parser
 *     walks it with cJSON_ArrayForEach, which expands to a walk of ->child and
 *     so happily iterates the members of an *object* instead. Refusing the
 *     wrong type is cheaper than reasoning about what that would build.
 *
 *   - a device entry that is not an object, or has no string "device_id".
 *     The parser sets error = EZPI_ERR_JSON_PARSE_FAILED for this
 *     (ezlopi_core_devices.c:934) and then carries on, so the outcome is worse
 *     than a clean refusal: the entry is still instantiated if its id_item is
 *     usable, and whether the *configuration* is reported as broken depends on
 *     whether a later entry overwrites `error` with EZPI_SUCCESS. A hub whose
 *     boot verdict turns on the ordering of the device list is not something
 *     to leave reachable from the network, so this is refused before the write.
 *
 *   - a device entry whose "id_item" is absent, not a number, or zero. This is
 *     the quiet one. The parser reads id_item, and only looks for a matching
 *     driver `if (0 != id_item)`, so an entry without a usable id_item is
 *     skipped in silence -- no error, no device, no log line naming the entry.
 *     That is precisely the failure that is impossible to diagnose from the
 *     outside, so it is refused here instead.
 *
 * WHAT IT DOES NOT CHECK
 *
 * Whether an id_item names a driver this image was built with. That depends on
 * the device table in ezlopi_core_devices_list.c, which is not available here
 * and differs between builds. An id_item naming a driver that was compiled out
 * is a real way to get a missing device, and the caller is better placed to say
 * so, because it can walk EZPI_core_devices_list_get_list_v3() and this cannot.
 * The handler that will call this reports it as its own reason.
 *
 * Nor does it check that the GPIOs named are free, valid for the target, or
 * not claimed twice. That needs the HAL and is a larger piece of work; it is
 * listed under Known gaps in the changelog rather than half-done here.
 */

#ifndef _EZLOPI_UTIL_DEVCFGVAL_H_
#define _EZLOPI_UTIL_DEVCFGVAL_H_

/*******************************************************************************
 *                          Include Files
 *******************************************************************************/
#include <stdint.h>

#include "cjext.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /*******************************************************************************
     *                          Type & Macro Declarations
     *******************************************************************************/

    /**
     * @brief Why a device configuration was refused, or that it was accepted.
     *
     * Ordered so that EZPI_DEVCFGVAL_OK is zero and therefore falsy, matching
     * the `if (reason)` style the callers use.
     */
    typedef enum e_ezpi_devcfgval
    {
        /** Acceptable. Not a promise that every device will appear -- see
         *  "WHAT IT DOES NOT CHECK" in this file's header comment. */
        EZPI_DEVCFGVAL_OK = 0,

        /** Not a JSON object. A configuration is an object. */
        EZPI_DEVCFGVAL_NOT_OBJECT,

        /** Serialising this would not fit the field it is stored in. */
        EZPI_DEVCFGVAL_TOO_LARGE,

        /** No "chipset" member. */
        EZPI_DEVCFGVAL_CHIPSET_MISSING,

        /** "chipset" is present but is not a string. */
        EZPI_DEVCFGVAL_CHIPSET_NOT_STRING,

        /** "chipset" names a different chip than this firmware is built for. */
        EZPI_DEVCFGVAL_CHIPSET_MISMATCH,

        /** No "dev_detail" member. */
        EZPI_DEVCFGVAL_DEVLIST_MISSING,

        /** "dev_detail" is present but is not an array. */
        EZPI_DEVCFGVAL_DEVLIST_NOT_ARRAY,

        /** An entry of "dev_detail" is not an object. */
        EZPI_DEVCFGVAL_DEVICE_NOT_OBJECT,

        /** An entry has no "device_id", or it is not a string. */
        EZPI_DEVCFGVAL_DEVICE_ID_BAD,

        /** An entry's "id_item" is absent, not a number, or zero. */
        EZPI_DEVCFGVAL_ITEM_ID_BAD,
    } e_ezpi_devcfgval_t;

    /*******************************************************************************
     *                          Function Prototypes
     *******************************************************************************/

    /**
     * @brief   Decide whether a device configuration is worth storing.
     *
     * No side effects and no allocation. Does not modify the tree.
     *
     * @param   root                The parsed configuration.
     * @param   expected_chipset    The chipset name this firmware accepts, as
     *                              returned by the target's branch in
     *                              ezlopi_core_devices.c. NULL means this build
     *                              has no name for its target, in which case the
     *                              chipset cannot be checked and is not: the
     *                              configuration is judged on everything else.
     *                              Refusing outright would make such a target
     *                              unconfigurable, and accepting blindly is what
     *                              the boot path already does there.
     * @param   serialised_len      Length of the serialised form, excluding the
     *                              terminator, or 0 to skip the size check. The
     *                              caller measures this because it is the one
     *                              holding the serialised string.
     * @param   region_size         Size of the field it will be stored in,
     *                              including room for the terminator. 0 skips
     *                              the size check.
     * @param   bad_index           out, optional: for the four per-entry
     *                              reasons, the index in "dev_detail" of the
     *                              offending entry, so the caller can name it.
     *                              Set to -1 when the reason is not per-entry.
     *                              May be NULL.
     *
     * @return  EZPI_DEVCFGVAL_OK, or the first problem found. "First" is in
     *          document order, so the reported entry is the earliest bad one
     *          rather than an arbitrary one.
     */
    e_ezpi_devcfgval_t EZPI_util_devcfgval_check(const cJSON *root,
                                                 const char *expected_chipset,
                                                 uint32_t serialised_len,
                                                 uint32_t region_size,
                                                 int *bad_index);

    /**
     * @brief   A short, stable, machine-readable token for a reason.
     *
     * Intended for the "data" member of a JSON-RPC error, where the Ezlo
     * convention is a dotted token rather than prose. Never NULL, including for
     * a value outside the enum.
     *
     * @param   reason  The value to describe.
     * @return  A static string; the caller must not free it.
     */
    const char *EZPI_util_devcfgval_token(e_ezpi_devcfgval_t reason);

#ifdef __cplusplus
}
#endif

#endif // _EZLOPI_UTIL_DEVCFGVAL_H_

/*******************************************************************************
 *                          End of File
 *******************************************************************************/
