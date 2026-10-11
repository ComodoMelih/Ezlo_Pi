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
 * @file    ezlopi_util_ha.h
 * @brief   Describe an EzloPi item as a Home Assistant MQTT entity.
 *
 * This component answers one question and nothing else: given an item's
 * declared name, value type, scale and whether it has a setter, what kind of
 * Home Assistant entity is it, and with which device class, unit and state
 * class?
 *
 * It is separated out, and declares no REQUIRES, so that the answer can be
 * tested on a desktop over every combination the firmware can actually
 * produce. That matters more here than it looks. A wrong mapping does not
 * crash and does not fail to build -- Home Assistant simply rejects the
 * discovery message, or worse accepts it and shows the entity with the wrong
 * icon, the wrong unit, or in the wrong part of the UI. None of that is
 * visible without a running broker, a running Home Assistant, and a board.
 *
 * The pairing rule is the part worth knowing about. Home Assistant validates
 * device_class against unit_of_measurement and rejects the whole discovery
 * payload when they disagree, so a driver that declares, say, a temperature
 * value type with a percent scale would otherwise silently publish an entity
 * that never appears. EZPI_ha_describe() therefore drops the device class
 * rather than the unit whenever the two do not go together: an entity with no
 * device class still works, it just gets a generic icon.
 */

#ifndef _EZLOPI_UTIL_HA_H_
#define _EZLOPI_UTIL_HA_H_

/*******************************************************************************
 *                          Include Files
 *******************************************************************************/
#include <stdbool.h>
#include <stddef.h>

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

    /**
     * @brief The Home Assistant MQTT platforms an EzloPi item can map onto.
     *
     * Deliberately short. Every item that is a number becomes a sensor or, if
     * it can be written, a number; every item that is a boolean becomes a
     * binary_sensor or a switch. Richer platforms (light, cover, climate) need
     * several items to be combined into one entity, which needs knowledge this
     * component does not have and is recorded as a gap in the service README.
     */
    typedef enum e_ezpi_ha_component
    {
        EZPI_HA_COMPONENT_NONE = 0,  /*!< Not representable; do not publish. */
        EZPI_HA_COMPONENT_SENSOR,    /*!< Read-only value. */
        EZPI_HA_COMPONENT_BINARY_SENSOR, /*!< Read-only on/off. */
        EZPI_HA_COMPONENT_SWITCH,    /*!< Writable on/off. */
        EZPI_HA_COMPONENT_NUMBER,    /*!< Writable bounded number. */
    } e_ezpi_ha_component_t;

    /**
     * @brief What to publish in a discovery message for one item.
     *
     * Every string member is either NULL or a pointer to a string literal with
     * static lifetime, so the struct can be copied and outlive the call.
     */
    typedef struct s_ezpi_ha_entity
    {
        e_ezpi_ha_component_t component;
        const char *device_class; /*!< NULL when none fits, or when none is safe. */
        const char *unit;         /*!< NULL when the quantity is unitless. */
        const char *state_class;  /*!< NULL, or "measurement". */
        double min;               /*!< NUMBER only; 0 otherwise. */
        double max;               /*!< NUMBER only; 0 otherwise. */
    } s_ezpi_ha_entity_t;

    /*******************************************************************************
     *                          Extern Function Prototypes
     *******************************************************************************/

    /**
     * @brief The Home Assistant platform name used in a discovery topic.
     *
     * @param component Component to name.
     * @return The platform name, or NULL for EZPI_HA_COMPONENT_NONE and for
     *         any value outside the enum.
     */
    const char *EZPI_ha_component_str(e_ezpi_ha_component_t component);

    /**
     * @brief Translate an EzloPi scale name into a unit symbol.
     *
     * EzloPi spells its scales out in words ("celsius", "micro_gram_per_cubic
     * _meter"); Home Assistant wants the symbol ("°C", "µg/m³"). The returned
     * strings are UTF-8 and have static lifetime.
     *
     * @param scale EzloPi scale name, may be NULL.
     * @return Unit symbol, or NULL if @p scale is NULL, empty or unknown. An
     *         unknown scale is reported rather than passed through, because
     *         publishing "north_pole_degress" as a unit would put that word on
     *         every reading in the Home Assistant UI.
     */
    const char *EZPI_ha_unit_from_scale(const char *scale);

    /**
     * @brief Decide how one item should appear in Home Assistant.
     *
     * @param item_name  Item name, as in item->cloud_properties.item_name.
     * @param value_type Value type, as in item->cloud_properties.value_type.
     * @param scale      Scale, as in item->cloud_properties.scale; may be NULL.
     * @param has_setter Whether the item can be written.
     * @param out        Filled in on success; zeroed first, so it is safe to
     *                   read after a false return.
     *
     * @return true when the item maps onto an entity worth publishing, false
     *         when it does not. False is a normal, expected answer: an item
     *         whose value is an array or an opaque token has no sensible
     *         Home Assistant representation, and publishing one anyway would
     *         add a permanently "unknown" entity to the user's dashboard.
     */
    bool EZPI_ha_describe(const char *item_name,
                          const char *value_type,
                          const char *scale,
                          bool has_setter,
                          s_ezpi_ha_entity_t *out);

    /**
     * @brief Reduce arbitrary text to something usable as an MQTT object id.
     *
     * Home Assistant accepts only [a-zA-Z0-9_-] in the object id and node id
     * segments of a discovery topic, and device names here come from the user
     * -- they routinely contain spaces, punctuation and non-ASCII. Every run of
     * unacceptable bytes collapses to a single underscore, letters are
     * lowercased, and leading and trailing underscores are dropped.
     *
     * Always NUL-terminates when @p dst_size is non-zero, and never writes
     * more than @p dst_size bytes. Truncation is silent by design: the caller
     * appends an item id to guarantee uniqueness, so a clipped name is cosmetic
     * where a refusal to publish would not be.
     *
     * @param dst      Destination buffer; may be NULL only if @p dst_size is 0.
     * @param dst_size Size of @p dst including the NUL.
     * @param src      Text to reduce; NULL is treated as empty.
     * @return Number of bytes written to @p dst, excluding the NUL. Zero means
     *         nothing usable was left, which the caller must handle rather
     *         than publish an empty topic segment.
     */
    size_t EZPI_ha_object_id(char *dst, size_t dst_size, const char *src);

#ifdef __cplusplus
}
#endif

#endif // _EZLOPI_UTIL_HA_H_

/*******************************************************************************
 *                          End of File
 *******************************************************************************/
