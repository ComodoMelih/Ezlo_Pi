#ifndef __EZLOPI_CORE_SCENES_SUNTIME_H__
#define __EZLOPI_CORE_SCENES_SUNTIME_H__

/* ***************************************************************************
 * @file ezlopi_core_scenes_suntime.h
 * @brief Turning a sunrisesunset.io reply into a clock time, and a stored
 *        latitude/longitude into the query that asks for it.
 *
 * These three functions are the parts of the `isSunState` when-method that can
 * be decided without a network, a clock or an NVS partition, which is why they
 * are separated from __issunsate_update_sunstate_tm rather than written inside
 * it. Everything here is a pure function of its arguments and is covered by
 * tests/host/test_suntime.c.
 * ***************************************************************************/

#include "sdkconfig.h"

#ifdef CONFIG_EZPI_SERV_ENABLE_MESHBOTS

#include <stdint.h>
#include "ezlopi_core_errors.h"

#ifdef __cplusplus
extern "C"
{
#endif

/* The s_sunstate_data_t.sunstate_mode encoding, which predates this file:
** 1 = sunrise, 2 = sunset, 0 = unset. Named here so the comparisons in the
** parser are readable and so a caller cannot pass 1 meaning "sunset". */
#define EZPI_SUNTIME_MODE_SUNRISE 1
#define EZPI_SUNTIME_MODE_SUNSET 2

    /**
     * Parse a clock reading into 24-hour hour/minute/second.
     *
     * Accepts both of the forms sunrisesunset.io can return, since the format
     * depends on a query parameter this firmware does not send and therefore
     * does not control:
     *
     *     "5:59:11 AM"    12-hour with a meridiem, one or two hour digits
     *     "17:59:11"      24-hour
     *
     * Strict on purpose. A reading it cannot account for in full -- trailing
     * characters, a missing colon, a three-digit field, an hour of 0 or 13
     * alongside "AM" -- is rejected rather than partially accepted, because
     * the caller's only alternative to a correct time is to not fire the
     * scene, and that is a better outcome than firing at a time derived from
     * a misread string.
     *
     * The two midnight/noon cases are the ones worth stating: "12:00:00 AM" is
     * hour 0 and "12:00:00 PM" is hour 12. Adding 12 to every PM reading gets
     * noon wrong by twelve hours, and in the other direction a sunset parsed
     * as 00:xx would make an `isSunState` scene fire just after midnight.
     *
     * @param str   the reading; not modified
     * @param hour  out, 0-23
     * @param min   out, 0-59
     * @param sec   out, 0-59
     *
     * @return EZPI_SUCCESS only when the whole string was accounted for. On
     *         failure the three outputs are left untouched, so a caller that
     *         ignores the return value does not silently read a half-parsed
     *         time -- it reads whatever it had before.
     */
    ezlopi_error_t EZPI_core_suntime_parse_clock(const char *str, int *hour, int *min, int *sec);

    /**
     * Build the request path for a stored coordinate pair.
     *
     * `coords_json` is what EZPI_core_nvs_read_latidtude_longitude() returns:
     * the `params` object of the hub.coordinates.set call, printed, e.g.
     * {"latitude":27.700769,"longitude":85.300140}.
     *
     * The result is a path with no leading slash -- "json?lat=..&lng=.." --
     * because __ezlopi_http_generate_request writes "GET /%s" and supplies the
     * slash itself.
     *
     * Absent, non-numeric or out-of-range coordinates are a failure, not a
     * cause to substitute a default. A default here would be a location the
     * user never chose, and sunrise at that location is a plausible-looking
     * wrong answer -- the hardest kind to notice. Failing tells the caller to
     * leave the scene alone, and the trace says the coordinates need setting.
     *
     * @param dst          buffer for the path; set to "" on any failure
     * @param dst_len      its size in bytes, terminator included
     * @param coords_json  the stored JSON; not modified
     *
     * @return EZPI_SUCCESS when `dst` holds the complete path.
     */
    ezlopi_error_t EZPI_core_suntime_build_query(char *dst, uint32_t dst_len, const char *coords_json);

    /**
     * Pull one suntime out of a sunrisesunset.io reply body.
     *
     * CAUTION ON THE REPLY SHAPE. The field names below are the ones the
     * service documents; they have NOT been checked against a live reply,
     * because the environment this was written in has neither network access
     * nor the hardware. That is survivable only because of how this function
     * fails: a body whose shape does not match is rejected, the caller clears
     * `curr_tm_day`, and the scene does not fire. So a wrong guess here costs
     * an `isSunState` scene that never triggers and says why in the trace --
     * not one that triggers at the wrong time. It is written this way on
     * purpose, and the first thing to check against real hardware.
     *
     *     {"results":{"sunrise":"5:59:11 AM","sunset":"6:04:22 PM",...},
     *      "status":"OK"}
     *
     * `status` is required to read "OK". The times are read from `results`,
     * and also accepted at the top level, so that a reply which omits the
     * wrapper still parses.
     *
     * @param body            NUL-terminated reply body
     * @param sunstate_mode   EZPI_SUNTIME_MODE_SUNRISE or _SUNSET
     * @param hour            out, 0-23
     * @param min             out, 0-59
     * @param sec             out, 0-59
     *
     * @return EZPI_SUCCESS only when the requested time was found and parsed.
     *         The outputs are untouched on failure.
     */
    ezlopi_error_t EZPI_core_suntime_from_reply(const char *body, uint8_t sunstate_mode, int *hour, int *min, int *sec);

#ifdef __cplusplus
}
#endif

#endif // CONFIG_EZPI_SERV_ENABLE_MESHBOTS
#endif // __EZLOPI_CORE_SCENES_SUNTIME_H__
