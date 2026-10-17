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
 * @file    ezlopi_cloud_coordinates.h
 * @brief   Declarations for cloud coordinate functions
 * @author
 * @version 1.0
 * @date    February 19th, 2024 12:00 PM
 */

#ifndef __EZLOPI_CLOUD_COORDINSTES_H__
#define __EZLOPI_CLOUD_COORDINSTES_H__

/*******************************************************************************
 *                          Include Files
 *******************************************************************************/
#include <stdint.h>

#include "cjext.h"
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

/**
 * @brief Size of the buffer the stored coordinate pair is rendered into.
 *
 * The longest string the renderer can produce is
 * {"latitude":-90.000000,"longitude":-180.000000} -- 47 characters including
 * the braces, 48 with the terminator -- because the two values are bounded
 * before they get there and printed with a fixed six decimal places. 64 leaves
 * room without being a guess; the caller still checks snprintf's return, since
 * a bound that is reasoned about and not enforced is not a bound.
 *
 * The 47 was then measured rather than left as arithmetic in a comment: the
 * widest pair rendered through the same format is 47 bytes, 48 with the
 * terminator, 16 bytes of headroom.
 *
 * Which means the snprintf check cannot currently fire, and that is recorded
 * rather than hidden: deleting it is an equivalent mutant, the one mutant of
 * twenty in this component's sweep that tests/host/test_coords does not
 * detect. It stays because what makes it unreachable is the range check
 * upstream of it, and a buffer whose safety depends on a bound enforced
 * somewhere else is one edit away from not being safe. If the format or the
 * bounds change, this is the line that notices.
 */
#define COORDINATES_JSON_MAX 64

    /*******************************************************************************
     *                          Extern Data Declarations
     *******************************************************************************/

    /*******************************************************************************
     *                          Extern Function Prototypes
     *******************************************************************************/
    /**
     * @brief Function to handle hub.coordinate.set method
     *
     * @param cjson_request Pointer to incoming JONS
     * @param cjson_response Pointer to outgoing JONS
     */
    void EZPI_hub_coordinates_set(cJSON *cjson_request, cJSON *cjson_response);
    /**
     * @brief Function to handle hub.coordinate.get method
     *
     * @param cjson_request Pointer to incoming JONS
     * @param cjson_response Pointer to outgoing JONS
     */
    void EZPI_hub_coordinates_get(cJSON *cj_request, cJSON *cj_response);
    /**
     * @brief Function to return currnet longitude
     *
     * @return double
     * @retval Current longitude
     */
    double EZPI_cloud_get_longitude();
    /**
     * @brief Function to return currnet latitude
     *
     * @return double
     * @retval Current latitude
     */
    double EZPI_cloud_get_latitude();

    /**
     * @brief Load the stored coordinate pair into the in-RAM cache.
     *
     * Call once at boot, after NVS is up. Until this runs,
     * EZPI_cloud_get_latitude() and EZPI_cloud_get_longitude() return 0, which
     * is what hub.info.get reported after every reboot before this existed --
     * while hub.coordinates.get, which reads NVS directly, reported the real
     * stored pair. Two readers of one datum, disagreeing.
     *
     * Nothing stored, or something stored that is not a usable pair, leaves the
     * cache at 0,0 and says so in the trace. There is no default location,
     * because a location the user never chose produces answers that look right
     * -- a sunrise time, correct for somewhere else.
     */
    void EZPI_cloud_coordinates_init(void);

    /**
     * @brief Read a coordinate pair out of a cJSON object.
     *
     * Accepts the object only if `latitude` and `longitude` are both present,
     * both numbers, and within [-90,90] and [-180,180] respectively. NaN and
     * the infinities fail the range test rather than being special-cased.
     *
     * Used on two inputs that have the same shape for a reason: the params of a
     * hub.coordinates.set call, and the object this component stores in NVS.
     *
     * @param cj_coords the object; not modified
     * @param lat       out, degrees
     * @param lng       out, degrees
     *
     * @return EZPI_SUCCESS when both outputs were written. On failure neither
     *         is touched, so a caller that ignores the return value reads
     *         whatever it had before rather than a half-updated pair.
     */
    ezlopi_error_t EZPI_cloud_coordinates_from_cjson(cJSON *cj_coords, double *lat, double *lng);

    /**
     * @brief Parse a stored coordinate pair.
     *
     * The string form of EZPI_cloud_coordinates_from_cjson, for what
     * EZPI_core_nvs_read_latidtude_longitude() hands back, e.g.
     * {"latitude":27.700769,"longitude":85.300140}.
     *
     * @param coords_json NUL-terminated JSON; not modified
     * @param lat         out, degrees
     * @param lng         out, degrees
     *
     * @return EZPI_SUCCESS when both outputs were written; outputs untouched
     *         otherwise.
     */
    ezlopi_error_t EZPI_cloud_coordinates_parse(const char *coords_json, double *lat, double *lng);

#ifdef __cplusplus
}
#endif

#endif // __EZLOPI_CLOUD_COORDINSTES_H__

/*******************************************************************************
 *                          End of File
 *******************************************************************************/
