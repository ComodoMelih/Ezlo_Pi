/* ***************************************************************************
 * @file ezlopi_core_scenes_suntime.c
 * @brief The decidable parts of the isSunState when-method.
 *
 * WHY THIS IS A SEPARATE TRANSLATION UNIT
 *
 * __issunsate_update_sunstate_tm needed three things it did not have: a query
 * built from the coordinates the user actually set, a parse of the reply, and
 * a failure path that can be reached. All three are pure functions of their
 * inputs, and none of them can be exercised on the hardware this was written
 * without -- but all three can be exercised on a host, which is where the
 * mistakes in this kind of code live. A 12-hour clock parser that gets noon
 * and midnight wrong, or a coordinate formatter that truncates at a buffer
 * boundary, is not a defect any amount of staring finds reliably.
 *
 * So they live here, in a file that includes cjext and the error enum and
 * nothing else of consequence, and tests/host/test_suntime.c runs them under
 * ASan. The caller keeps the parts that need a radio and a clock.
 * ***************************************************************************/

/*******************************************************************************
 *                          Include Files
 *******************************************************************************/
#include "sdkconfig.h"

#ifdef CONFIG_EZPI_SERV_ENABLE_MESHBOTS

#include <stdio.h>
#include <string.h>

#include "ezlopi_util_trace.h"

#include "cjext.h"
#include "ezlopi_cloud_constants.h"
#include "ezlopi_core_scenes_suntime.h"
#include "EZLOPI_USER_CONFIG.h"

/*******************************************************************************
 *                          Static Function Prototypes
 *******************************************************************************/
static ezlopi_error_t __read_clock_field(const char **cursor, int *out);
static const char *__suntime_key(uint8_t sunstate_mode);

/*******************************************************************************
 *                          Extern Function Definitions
 *******************************************************************************/

ezlopi_error_t EZPI_core_suntime_parse_clock(const char *str, int *hour, int *min, int *sec)
{
    if ((NULL == str) || (NULL == hour) || (NULL == min) || (NULL == sec))
    {
        return EZPI_FAILED;
    }

    const char *p = str;
    int h = 0;
    int m = 0;
    int s = 0;

    while (' ' == *p)
    {
        p++;
    }

    if (EZPI_SUCCESS != __read_clock_field(&p, &h))
    {
        return EZPI_FAILED;
    }
    if (':' != *p)
    {
        return EZPI_FAILED;
    }
    p++;
    if (EZPI_SUCCESS != __read_clock_field(&p, &m))
    {
        return EZPI_FAILED;
    }
    if (':' != *p)
    {
        return EZPI_FAILED;
    }
    p++;
    if (EZPI_SUCCESS != __read_clock_field(&p, &s))
    {
        return EZPI_FAILED;
    }

    while (' ' == *p)
    {
        p++;
    }

    if (('A' == *p) || ('a' == *p) || ('P' == *p) || ('p' == *p))
    {
        const int is_pm = (('P' == *p) || ('p' == *p));
        p++;
        if (('M' != *p) && ('m' != *p))
        {
            return EZPI_FAILED;
        }
        p++;
        while (' ' == *p)
        {
            p++;
        }
        if ('\0' != *p)
        {
            return EZPI_FAILED;
        }

        /* A 12-hour reading has no hour 0 and no hour 13, so either one means
        ** the string is not what it appears to be. Accepting them would mean
        ** inventing a convention the sender does not share. */
        if ((h < 1) || (h > 12))
        {
            return EZPI_FAILED;
        }

        /* 12 is the exception in both directions: 12 AM is 00:xx and 12 PM is
        ** 12:xx. Only the hours 1-11 shift by twelve. */
        if (12 == h)
        {
            h = (is_pm) ? 12 : 0;
        }
        else if (is_pm)
        {
            h += 12;
        }
    }
    else
    {
        /* No meridiem: a 24-hour reading, and nothing may follow it. */
        if ('\0' != *p)
        {
            return EZPI_FAILED;
        }
        if ((h < 0) || (h > 23))
        {
            return EZPI_FAILED;
        }
    }

    if ((m < 0) || (m > 59) || (s < 0) || (s > 59))
    {
        return EZPI_FAILED;
    }

    /* Written only now that every field is known good, so a rejected string
    ** leaves the caller's variables exactly as it found them. */
    *hour = h;
    *min = m;
    *sec = s;

    return EZPI_SUCCESS;
}

ezlopi_error_t EZPI_core_suntime_build_query(char *dst, uint32_t dst_len, const char *coords_json)
{
    if ((NULL == dst) || (0 == dst_len) || (NULL == coords_json))
    {
        return EZPI_FAILED;
    }

    dst[0] = '\0';

    cJSON *cj_coords = cJSON_Parse(__FUNCTION__, coords_json);
    if (NULL == cj_coords)
    {
        TRACE_E("stored coordinates are not valid JSON");
        return EZPI_ERR_JSON_PARSE_FAILED;
    }

    ezlopi_error_t ret = EZPI_FAILED;

    /* ezlopi_latitude_str / ezlopi_longitude_str rather than literals: these
    ** are the same constants EZPI_hub_coordinates_set used to build the object
    ** that was stored, so the two spellings cannot drift apart. */
    cJSON *cj_lat = cJSON_GetObjectItem(__FUNCTION__, cj_coords, ezlopi_latitude_str);
    cJSON *cj_lng = cJSON_GetObjectItem(__FUNCTION__, cj_coords, ezlopi_longitude_str);

    if (cJSON_IsNumber(cj_lat) && cJSON_IsNumber(cj_lng))
    {
        const double lat = cj_lat->valuedouble;
        const double lng = cj_lng->valuedouble;

        if ((lat >= -90.0) && (lat <= 90.0) && (lng >= -180.0) && (lng <= 180.0))
        {
            /* Six decimals is about 0.1 m of latitude -- far finer than
            ** anything that moves a sunrise by a whole second -- and bounds
            ** the output length, which the truncation check below relies on. */
            const int n = snprintf(dst, dst_len, "json?lat=%.6f&lng=%.6f", lat, lng);

            if ((n > 0) && ((uint32_t)n < dst_len))
            {
                ret = EZPI_SUCCESS;
            }
            else
            {
                /* snprintf already wrote a truncated path and terminated it,
                ** which is the dangerous state: "json?lat=27.70&lng=8" is a
                ** well-formed request for the wrong place. Clear it. */
                dst[0] = '\0';
                TRACE_E("coordinate query needs %d bytes, buffer holds %u", n, (unsigned)dst_len);
            }
        }
        else
        {
            TRACE_E("stored coordinates are out of range");
        }
    }
    else
    {
        TRACE_E("no coordinates stored; set them before using a sunrise/sunset scene");
    }

    cJSON_Delete(__FUNCTION__, cj_coords);

    return ret;
}

ezlopi_error_t EZPI_core_suntime_from_reply(const char *body, uint8_t sunstate_mode, int *hour, int *min, int *sec)
{
    if ((NULL == body) || (NULL == hour) || (NULL == min) || (NULL == sec))
    {
        return EZPI_FAILED;
    }

    const char *key = __suntime_key(sunstate_mode);
    if (NULL == key)
    {
        TRACE_E("sunstate mode %u is neither sunrise nor sunset", (unsigned)sunstate_mode);
        return EZPI_FAILED;
    }

    cJSON *cj_reply = cJSON_Parse(__FUNCTION__, body);
    if (NULL == cj_reply)
    {
        TRACE_E("suntime reply is not valid JSON");
        return EZPI_ERR_JSON_PARSE_FAILED;
    }

    ezlopi_error_t ret = EZPI_FAILED;

    /* The service's own field names, so they are literals here rather than
    ** ezlopi_* constants. ezlopi_sunrise_str happens to read "sunrise" too,
    ** but it is the name of a *scene field*; borrowing it would tie this
    ** request to a cloud-API spelling that has no reason to stay in step. */
    cJSON *cj_status = cJSON_GetObjectItem(__FUNCTION__, cj_reply, "status");
    const char *status = (cJSON_IsString(cj_status)) ? cj_status->valuestring : NULL;

    if ((NULL == status) || (0 != strcmp(status, "OK")))
    {
        TRACE_E("suntime reply status is %s", (status) ? status : "absent");
    }
    else
    {
        cJSON *cj_results = cJSON_GetObjectItem(__FUNCTION__, cj_reply, "results");
        cJSON *cj_time = NULL;

        if (cJSON_IsObject(cj_results))
        {
            cj_time = cJSON_GetObjectItem(__FUNCTION__, cj_results, key);
        }
        if (NULL == cj_time)
        {
            /* Also accepted at the top level. The wrapper is the shape the
            ** service documents; a reply without it is still unambiguous, and
            ** reading both costs one lookup. */
            cj_time = cJSON_GetObjectItem(__FUNCTION__, cj_reply, key);
        }

        if (false == cJSON_IsString(cj_time))
        {
            TRACE_E("suntime reply has no '%s' string", key);
        }
        else
        {
            ret = EZPI_core_suntime_parse_clock(cj_time->valuestring, hour, min, sec);
            if (EZPI_SUCCESS != ret)
            {
                TRACE_E("could not read '%s' as a clock time: '%s'", key, cj_time->valuestring);
            }
        }
    }

    cJSON_Delete(__FUNCTION__, cj_reply);

    return ret;
}

/*******************************************************************************
 *                          Static Function Definitions
 *******************************************************************************/

/**
 * Read one or two decimal digits, leaving the cursor on the first character
 * that is not part of the number.
 *
 * Deliberately not strtoul. The offset parser a few hundred lines away in
 * ezlopi_core_scenes_when_methods_helper_functions.c does
 * `strtoul(time_diff, &ptr1, 10)` and then `strtoul(ptr1 + 1, ...)`, which
 * steps past the terminator whenever the field it just read was the last one
 * -- and guards it with `if (NULL != ptr1)`, a test strtoul never fails. This
 * returns a status instead, and the caller checks for the ':' itself.
 *
 * A third digit is a rejection rather than an overflow: "245:00:00" is not a
 * clock reading, and stopping after two digits would silently accept it as
 * 24 followed by junk.
 */
static ezlopi_error_t __read_clock_field(const char **cursor, int *out)
{
    const char *q = *cursor;
    int value = 0;
    int digits = 0;

    while ((*q >= '0') && (*q <= '9') && (digits < 2))
    {
        value = (value * 10) + (*q - '0');
        q++;
        digits++;
    }

    if (0 == digits)
    {
        return EZPI_FAILED;
    }
    if ((*q >= '0') && (*q <= '9'))
    {
        return EZPI_FAILED;
    }

    *cursor = q;
    *out = value;

    return EZPI_SUCCESS;
}

static const char *__suntime_key(uint8_t sunstate_mode)
{
    const char *key = NULL;

    if (EZPI_SUNTIME_MODE_SUNRISE == sunstate_mode)
    {
        key = "sunrise";
    }
    else if (EZPI_SUNTIME_MODE_SUNSET == sunstate_mode)
    {
        key = "sunset";
    }

    return key;
}

#endif // CONFIG_EZPI_SERV_ENABLE_MESHBOTS

/*******************************************************************************
 *                          End of File
 *******************************************************************************/
