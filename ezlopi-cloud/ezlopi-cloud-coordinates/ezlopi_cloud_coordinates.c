/** ===========================================================================
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
 * @file    ezlopi_cloud_coordinates.c
 * @brief   Definitions for cloud coordinate functions
 * @author
 * @author
 * @version 1.0
 * @date    February 19th, 2024 12:00 PM
 */

/*******************************************************************************
 *                          Include Files
 *******************************************************************************/

#include <stdio.h>
#include <stdbool.h>

#include "cjext.h"
#include "ezlopi_util_trace.h"

/* ezlopi_core_cjson_macros.h was included for CJSON_GET_VALUE_DOUBLE, which is
** no longer used here: that macro zeroes its output when the key is absent, so
** reading a pair through it cannot distinguish "longitude was 0" from
** "longitude was not sent". The two cases need different answers, so the
** fields are read directly. */
#include "ezlopi_core_nvs.h"

#include "ezlopi_cloud_constants.h"
#include "ezlopi_cloud_coordinates.h"

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
static bool __coords_in_range(double lat, double lng);
static void __prepare_error(cJSON *cj_response, int error_code, const char *error_message);

/*******************************************************************************
 *                          Static Data Definitions
 *******************************************************************************/

/* The in-RAM copy of the stored pair, and the only thing EZPI_cloud_get_latitude
** and EZPI_cloud_get_longitude return.
**
** These are a cache over NVS, not a second place to keep the coordinates, and
** the invariant worth stating is that they agree with what is in flash. Two
** things maintain it: EZPI_cloud_coordinates_init loads them at boot, and
** EZPI_hub_coordinates_set assigns them only after the write to NVS has
** succeeded. Nothing else writes them.
**
** Before that init existed, nothing loaded them -- they were zero from reset
** until a hub.coordinates.set arrived, while EZPI_hub_coordinates_get read NVS
** directly and reported the real stored pair. So the two readers of the same
** datum disagreed after every reboot, and the one that was wrong was the one
** hub.info.get uses. */
static double sg_latitude;
static double sg_longitude;

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

ezlopi_error_t EZPI_cloud_coordinates_from_cjson(cJSON *cj_coords, double *lat, double *lng)
{
    ezlopi_error_t error = EZPI_ERR_JSON_PARSE_FAILED;

    if (cj_coords && lat && lng)
    {
        cJSON *cj_lat = cJSON_GetObjectItem(__FUNCTION__, cj_coords, ezlopi_latitude_str);
        cJSON *cj_lng = cJSON_GetObjectItem(__FUNCTION__, cj_coords, ezlopi_longitude_str);

        /* Both have to be there and both have to be numbers. A pair with one
        ** member missing is not a location, and the alternative -- taking the
        ** one that arrived and leaving the other at whatever it was -- produces
        ** a coordinate that is half what the user asked for and half something
        ** they set earlier. That is a plausible-looking wrong location, which
        ** is the hardest kind to notice: every reading off it, including
        ** sunrise and sunset, is wrong by a consistent amount. */
        if (cj_lat && (cj_lat->type & cJSON_Number) && cj_lng && (cj_lng->type & cJSON_Number))
        {
            /* Also rejects NaN and the infinities, by arithmetic rather than by
            ** testing for them: every comparison against a NaN is false, and an
            ** infinity fails the bound. cJSON produces both from input a client
            ** can send -- "1e999" parses to inf through strtod. */
            if (__coords_in_range(cj_lat->valuedouble, cj_lng->valuedouble))
            {
                *lat = cj_lat->valuedouble;
                *lng = cj_lng->valuedouble;
                error = EZPI_SUCCESS;
            }
        }
    }

    return error;
}

ezlopi_error_t EZPI_cloud_coordinates_parse(const char *coords_json, double *lat, double *lng)
{
    ezlopi_error_t error = EZPI_ERR_JSON_PARSE_FAILED;

    if (coords_json)
    {
        cJSON *cj_coords = cJSON_Parse(__FUNCTION__, coords_json);
        if (cj_coords)
        {
            error = EZPI_cloud_coordinates_from_cjson(cj_coords, lat, lng);
            cJSON_Delete(__FUNCTION__, cj_coords);
        }
    }

    return error;
}

void EZPI_cloud_coordinates_init(void)
{
    char *stored = EZPI_core_nvs_read_latidtude_longitude();

    if (stored)
    {
        double lat = 0.0;
        double lng = 0.0;

        if (EZPI_SUCCESS == EZPI_cloud_coordinates_parse(stored, &lat, &lng))
        {
            sg_latitude = lat;
            sg_longitude = lng;
            TRACE_I("coordinates: restored %.6f, %.6f from nvs", lat, lng);
        }
        else
        {
            /* Left at 0,0 deliberately rather than guessed at. The stored
            ** string is traced because it is the only evidence of how it got
            ** into a shape this cannot read, and it is not a secret. */
            TRACE_W("coordinates: stored value is not a usable pair, ignoring: %s", stored);
        }

        ezlopi_free(__FUNCTION__, stored);
    }
    else
    {
        TRACE_W("coordinates: none stored; hub.info.get reports 0,0 until hub.coordinates.set");
    }
}

void EZPI_hub_coordinates_set(cJSON *cj_request, cJSON *cj_response)
{
    cJSON_AddObjectToObject(__FUNCTION__, cj_response, ezlopi_result_str);

    if (cj_request)
    {
        cJSON *cj_params = cJSON_GetObjectItem(__FUNCTION__, cj_request, ezlopi_params_str);
        if (cj_params)
        {
            double lat = 0.0;
            double lng = 0.0;

            if (EZPI_SUCCESS == EZPI_cloud_coordinates_from_cjson(cj_params, &lat, &lng))
            {
                /* Stored as a canonical two-member object built from the two
                ** numbers that were accepted, rather than by printing the
                ** request's params wholesale as this used to. Two reasons, and
                ** the first is the one that matters: what comes back out of
                ** EZPI_hub_coordinates_get and what the cache holds are then
                ** the same two values by construction, so the two readers
                ** cannot drift. The second is that params is attacker-shaped --
                ** printing it stored whatever else the caller put in the
                ** object, in a 256-byte buffer, under a key named for
                ** coordinates. */
                char lat_long_str[COORDINATES_JSON_MAX];
                int n = snprintf(lat_long_str, sizeof(lat_long_str), "{\"%s\":%.6f,\"%s\":%.6f}",
                                 ezlopi_latitude_str, lat, ezlopi_longitude_str, lng);

                if ((n > 0) && ((uint32_t)n < sizeof(lat_long_str)))
                {
                    /* The cache is assigned only once flash has taken the
                    ** value. If the write fails, the getters keep reporting the
                    ** pair that is actually stored instead of one that would
                    ** vanish at the next boot -- which is the same class of
                    ** disagreement this whole change is about, and it would be
                    ** perverse to introduce it here while fixing it there. */
                    if (EZPI_SUCCESS == EZPI_core_nvs_write_latitude_longitude(lat_long_str))
                    {
                        sg_latitude = lat;
                        sg_longitude = lng;
                    }
                    else
                    {
                        TRACE_E("coordinates: nvs write failed; keeping %.6f, %.6f", sg_latitude, sg_longitude);
                        __prepare_error(cj_response, EZPI_ERR_NVS_WRITE_FAILED, "could not store coordinates");
                    }
                }
                else
                {
                    TRACE_E("coordinates: %.6f, %.6f did not fit in %u bytes", lat, lng, (unsigned)sizeof(lat_long_str));
                    __prepare_error(cj_response, EZPI_ERR_JSON_PARSE_FAILED, "could not store coordinates");
                }
            }
            else
            {
                TRACE_W("coordinates: params rejected; need numeric latitude in [-90,90] and longitude in [-180,180]");
                __prepare_error(cj_response, EZPI_ERR_JSON_PARSE_FAILED, "latitude and longitude must be numbers in range");
            }
        }
    }
}

void EZPI_hub_coordinates_get(cJSON *cj_request, cJSON *cj_response)
{
    char *lat_long_vals = EZPI_core_nvs_read_latidtude_longitude();
    if (lat_long_vals)
    {
        cJSON_AddRawToObject(__FUNCTION__, cj_response, ezlopi_result_str, lat_long_vals);
        ezlopi_free(__FUNCTION__, lat_long_vals);
    }
}

double EZPI_cloud_get_latitude()
{
    return sg_latitude;
}

double EZPI_cloud_get_longitude()
{
    return sg_longitude;
}

/*******************************************************************************
 *                          Static Function Definitions
 *******************************************************************************/

static bool __coords_in_range(double lat, double lng)
{
    return ((lat >= -90.0) && (lat <= 90.0) && (lng >= -180.0) && (lng <= 180.0));
}

/**
 * @brief Add a JSON-RPC style error object to a response.
 *
 * Follows the shape ezlopi_cloud_devcfg.c uses, including its quirk: "result"
 * has already been added by the time this runs, so a refused request carries
 * both an empty result and an error. That is not what JSON-RPC describes and it
 * is what every method in this tree does, so matching it is the conservative
 * choice -- a client reading "result" unconditionally keeps working.
 *
 * @param cj_response   the response object
 * @param error_code    one of the EZPI_ERR_* values
 * @param error_message human-readable reason
 */
static void __prepare_error(cJSON *cj_response, int error_code, const char *error_message)
{
    cJSON *cj_error = cJSON_AddObjectToObject(__FUNCTION__, cj_response, ezlopi_error_str);

    if (cj_error)
    {
        cJSON_AddNumberToObject(__FUNCTION__, cj_error, ezlopi_code_str, error_code);
        cJSON_AddStringToObject(__FUNCTION__, cj_error, ezlopi_message_str, error_message);
        cJSON_AddStringToObject(__FUNCTION__, cj_error, ezlopi_data_str, ezlopi__str);
    }
}

/*******************************************************************************
 *                          End of File
 *******************************************************************************/
