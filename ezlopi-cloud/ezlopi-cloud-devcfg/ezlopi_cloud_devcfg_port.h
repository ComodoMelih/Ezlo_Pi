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
 * @file    ezlopi_cloud_devcfg_port.h
 * @brief   The things the config API needs from the rest of the firmware
 * @version 1.0
 * @date    13th OCT 2025
 *
 * WHY THIS FILE EXISTS
 *
 * The handlers in ezlopi_cloud_devcfg.c decide whether a configuration may be
 * stored, what to say when it may not, and which entry to name. That decision
 * is the part worth testing, and the reason it is not tested in the two paths
 * that already write configurations today -- BLE dynamic-config and UART -- is
 * that both of them reach straight into ezlopi-core-factory-info, which reads a
 * flash partition, and ezlopi-core-devices, whose header pulls in the whole
 * HAL. Neither compiles on a host, so neither does anything that includes them.
 *
 * So the handler does not include them. It calls the seven functions below, and
 * ezlopi_cloud_devcfg_port.c -- which is the only file here that does include
 * those headers -- forwards each one. The port is seven forwarding bodies with
 * no branching worth the name; the host test replaces them with recording fakes
 * and exercises everything above.
 *
 * This is not a layer added for its own sake. It is drawn exactly where the
 * host build stops working, and the shape of it is the list of things a
 * configuration decision genuinely depends on:
 *
 *   - what is stored now, and how to release it
 *   - how to store a replacement
 *   - what chipset name this build accepts
 *   - how large the field is
 *   - whether an id_item names a driver this image was actually built with
 *   - which drivers this image was built with at all
 *
 * The last two are what EZPI_util_devcfgval_check() deliberately does not
 * answer, because they depend on the device table in
 * ezlopi_core_devices_list.c, which differs between builds. They belong here.
 */

#ifndef _EZLOPI_CLOUD_DEVCFG_PORT_H_
#define _EZLOPI_CLOUD_DEVCFG_PORT_H_

/*******************************************************************************
 *                          Include Files
 *******************************************************************************/
#include <stdbool.h>
#include <stdint.h>

#include "cjext.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /*******************************************************************************
     *                          Function Prototypes
     *******************************************************************************/

    /**
     * @brief   Read the stored device configuration.
     *
     * @return  A freshly allocated NUL-terminated string, to be released with
     *          EZPI_CLOUD_devcfg_port_free(), or NULL when nothing readable is
     *          stored. A blank or never-written field reads back as NULL, which
     *          is not an error: it is a hub that has not been configured yet.
     */
    char *EZPI_CLOUD_devcfg_port_read(void);

    /**
     * @brief   Release a string from EZPI_CLOUD_devcfg_port_read().
     *
     * Separate from plain free() because the firmware's allocator is wrapped
     * for accounting, and because the three existing readers of this field
     * disagree about who frees it -- one of them carries a comment saying not
     * to, copied from an unrelated field that really is shared. Naming the
     * release makes the ownership unambiguous here.
     *
     * @param   str     The string, or NULL.
     */
    void EZPI_CLOUD_devcfg_port_free(char *str);

    /**
     * @brief   Store a device configuration, replacing whatever is there.
     *
     * Does not take ownership of @p root and does not modify it.
     *
     * Not const, and not by oversight: cJSON_PrintPreallocated() takes a
     * non-const item, so the function this forwards to cannot take one either.
     * A const parameter here would only move the cast into the port and make
     * this signature claim something the layer below does not.
     *
     * @param   root    The configuration to serialise and store.
     * @return  true if it was written.
     */
    bool EZPI_CLOUD_devcfg_port_write(cJSON *root);

    /**
     * @brief   The chipset name this build expects in a configuration.
     *
     * Forwards EZPI_core_device_expected_chipset_name(), so the name checked
     * before a write is by construction the name the boot parser compares
     * against.
     *
     * @return  A static string, or NULL on a target this firmware has no name
     *          for -- in which case the chipset is not checked at all, which is
     *          what the boot parser does on such a target too.
     */
    const char *EZPI_CLOUD_devcfg_port_chipset(void);

    /**
     * @brief   Size of the field a configuration is stored in, in bytes,
     *          including room for the terminator.
     *
     * @return  The field size, or 0 if this build does not want the size
     *          checked. A serialised configuration must be shorter than this.
     */
    uint32_t EZPI_CLOUD_devcfg_port_region_size(void);

    /**
     * @brief   Whether an id_item names a driver compiled into this image.
     *
     * The boot parser walks the device table looking for this exact match and,
     * finding nothing, moves to the next entry without a word. An entry naming
     * a driver that was configured out therefore disappears on the next boot
     * and nothing anywhere says why, which is the failure this API exists to
     * convert into an error at the moment of writing.
     *
     * @param   id_item     The entry's id_item, as the parser would read it.
     * @return  true if some row of the device table claims that id.
     */
    bool EZPI_CLOUD_devcfg_port_driver_exists(uint32_t id_item);

    /**
     * @brief   Read one row of this image's device table.
     *
     * The companion to driver_exists(). That one answers "may I use this
     * id_item", which is the question the write path has; this one answers
     * "which id_items are there at all", which is the question a configuration
     * UI has before the user has typed anything. Without it, the only way to
     * discover what an image supports is to send a configuration and be
     * refused, one driver at a time.
     *
     * Enumerated by index rather than returned as a block, so the handler
     * needs no knowledge of how the table is laid out and the host test can
     * substitute a table of any length, including an empty one.
     *
     * @param   index       Row to read, from 0. An index at or past the end of
     *                      the table is not an error; it is how a caller finds
     *                      the end.
     * @param   name        out, may be NULL: the row's name, a pointer into
     *                      static storage which the caller must neither free
     *                      nor modify. Can itself be NULL if the table holds
     *                      no name for that row. Untouched when false is
     *                      returned.
     * @param   id_item     out, may be NULL: the row's id_item. Untouched when
     *                      false is returned.
     * @return  true if @p index names a real row.
     */
    bool EZPI_CLOUD_devcfg_port_driver_at(int index, const char **name,
                                          uint32_t *id_item);

#ifdef __cplusplus
}
#endif

#endif // _EZLOPI_CLOUD_DEVCFG_PORT_H_

/*******************************************************************************
 *                          End of File
 *******************************************************************************/
