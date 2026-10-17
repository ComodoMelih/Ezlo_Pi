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

#include <stdbool.h>
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

    /**
     * Parse an `hms_interval` offset into a count of seconds.
     *
     * The number of fields chooses their meaning, which is the rule the
     * shipped parser meant to implement and never could:
     *
     *     "30"         30 seconds
     *     "5:30"       5 minutes 30 seconds
     *     "1:05:30"    1 hour 5 minutes 30 seconds
     *
     * One or two digits per field, nothing before or after, no sign. Rejects
     * rather than salvages: a minute or second above 59, an hour above 23, a
     * fourth field, a trailing colon, a space, an empty string.
     *
     * The range check is the part that matters, and not for the usual reason.
     * An out-of-range offset does not fault and does not get noticed -- the
     * caller compares the moment it produces against the wall clock for
     * equality, so "99:99:99" yields a trigger at 81:39:39 that no clock ever
     * reaches. The scene stays in the list, reports no error, and never fires.
     *
     * @param str       the offset; not modified
     * @param offs_sec  out, 0-86399, untouched on failure
     *
     * @return EZPI_SUCCESS only when the whole string was accounted for.
     */
    ezlopi_error_t EZPI_core_suntime_parse_offset(const char *str, int *offs_sec);

    /**
     * Move a suntime by a signed number of seconds, within the day.
     *
     * `offs_sec` is negative for "before sunrise" and positive for "after", so
     * the two directions are one code path; the result wraps at midnight in
     * both.
     *
     * Seconds-since-midnight internally, on purpose. Subtracting hours,
     * minutes and seconds as three separate fields requires a borrow between
     * them, and getting that wrong is not conspicuous: it yields a time that
     * looks like a time. The shipped code reduced each field independently and
     * put "20 minutes before" a 06:10:00 sunrise at 06:50:00.
     *
     * @param sun_hour  0-23, the suntime
     * @param sun_min   0-59
     * @param sun_sec   0-59
     * @param offs_sec  -86399 to 86399
     * @param hour      out, 0-23, untouched on failure
     * @param min       out, 0-59
     * @param sec       out, 0-59
     *
     * @return EZPI_SUCCESS unless an input was out of range.
     */
    ezlopi_error_t EZPI_core_suntime_shift(int sun_hour, int sun_min, int sun_sec, int offs_sec, int *hour, int *min, int *sec);

    /* ***********************************************************************
     * HOW OFTEN A FAILED LOOKUP IS RETRIED
     *
     * The three functions above decide what a reply means. These three decide
     * whether to ask again, and they are here for the same reason: the policy
     * is a function of a day number and a time of day, so it can be tested,
     * while the thing it governs is a TLS connection that cannot.
     *
     * The cost of not having them was measured, not guessed.
     * __scenes_loop calls ezpi_scene_proto_thread(scene_node, 1000), and the
     * protothread's PT_WAIT_UNTIL releases on
     * (xTaskGetTickCount() - ctx->curr_ticks) > routine_delay_ms -- so every
     * when-condition is re-evaluated about once a second. A failed suntime
     * lookup left no state behind to throttle on: __issunsate_update_sunstate_tm
     * cleared curr_tm_day on entry, ISSUNSTATE_get_suntime saw it still clear,
     * deleted the whole user_arg block, and the next evaluation allocated a
     * fresh zeroed one and started over. A hub that could not reach
     * api.sunrisesunset.io therefore opened in the order of 86,400 TLS
     * connections a day, per isSunState scene, indefinitely.
     *
     * WHY A TIMESTAMP AND NOT AN ATTEMPT COUNT
     *
     * "Stop after N tries today" is the obvious shape and it is worse than no
     * backoff at all for the case that actually happens. A hub reboots, the
     * scene starts evaluating before DHCP and DNS are up, and N attempts are
     * spent in N seconds -- after which the scene is finished for the day even
     * though the network came up a minute later. The budget has to be spent
     * over the day, which means remembering when rather than how many.
     *
     * The ladder is 60 s, 2, 4, 8, 16 and then 32 minutes, held there: exactly
     * 50 attempts across a full day of outage instead of 86,400, and the first
     * retry still inside a minute so a hub whose network arrives late is not
     * punished for it.
     *
     * That 50 is counted, not estimated. This comment said "about 48" until
     * test_backoff_a_full_day_of_outage was written, which runs the real loop
     * over all 86,400 seconds and reports the number it actually got; the
     * estimate was two low. Five attempts land while the ladder is still
     * climbing -- at 0, 60, 180, 420 and 900 seconds -- and the remaining 45
     * are 1,860 plus a multiple of 1,920.
     *
     * WHAT IT COSTS
     *
     * A configuration failure -- no coordinates stored, an unreadable offset
     * field -- backs off on the same ladder, because nothing here can tell it
     * apart from an outage. So after hub.coordinates.set on a hub that has been
     * failing for a while, the scene can take up to 32 minutes to pick the
     * coordinates up. That is the deliberate trade: a bounded one-off delay
     * after a setup action, against a permanent connection storm.
     *
     * THE CLOCK IS THE CALLER'S, AND IT MAY BE WRONG
     *
     * now_sod is derived from the same `struct tm` the when-condition is being
     * evaluated against, so before SNTP has synced it is whatever the RTC says.
     * That is survivable in both directions. A clock that jumps forward makes
     * next_attempt_sod look due and costs one early attempt; a clock that jumps
     * backward delays one attempt by at most the remainder of the day, and the
     * day change clears the state outright. Neither can wedge a scene, because
     * tm_mday moving at all is a reset.
     * *********************************************************************** */

/* 60 s doubling six times: 60, 120, 240, 480, 960, 1920. */
#define EZPI_SUNTIME_BACKOFF_BASE_SEC 60
#define EZPI_SUNTIME_BACKOFF_MAX_STEP 6

    /**
     * How long a failed lookup waits before the next one.
     *
     * Kept separate from the state so the ladder itself can be asserted, and
     * so the trace can print the delay it is about to impose rather than
     * leaving a reader to work it out from the step number.
     *
     * @param step  1 for the first failure; 0 means no failure yet
     * @return seconds; 0 for step 0, and clamped to the top of the ladder for
     *         any step above EZPI_SUNTIME_BACKOFF_MAX_STEP
     */
    int EZPI_core_suntime_backoff_delay(int step);

    /* The retry state for one isSunState scene. Embedded in s_sunstate_data_t,
    ** which is the block ISSUNSTATE_get_suntime keeps in
    ** when_block->fields->user_arg -- so it lives exactly as long as the scene
    ** does and is zeroed with the rest of it on allocation. All-zero means
    ** "nothing attempted", which is the correct starting state. */
    typedef struct s_suntime_backoff
    {
        /* The day this state describes. A different tm_mday discards it: a new
        ** day needs a new suntime regardless, so there is nothing to carry
        ** over, and a multi-day outage restarts at 60 s each midnight. */
        int attempt_tm_day;

        /* Seconds since midnight before which no attempt is made. May sit past
        ** the end of the day -- 86,399 plus the top of the ladder is 88,319 --
        ** and that is not clamped on purpose: "not again today" is a correct
        ** answer, and the day rollover is what ends it. */
        int next_attempt_sod;

        /* Consecutive failures today, saturating at
        ** EZPI_SUNTIME_BACKOFF_MAX_STEP. 0 after a success. */
        int backoff_step;
    } s_suntime_backoff_t;

    /**
     * Whether the lookup should run now.
     *
     * NOT A PURE PREDICATE -- it writes to `bo`. A tm_mday that differs from
     * the stored one resets the state here, because this is the only function
     * of the three the caller is guaranteed to reach on every evaluation, and
     * a reset that depended on the caller remembering to ask for it would be
     * missed on exactly the path that matters.
     *
     * A NULL `bo` returns true. There is no state to throttle on, and refusing
     * the lookup because the struct is missing would turn an allocation problem
     * into a scene that never fires.
     *
     * @param bo       retry state; may be NULL
     * @param tm_mday  today's day-of-month, 1-31
     * @param now_sod  seconds since midnight, 0-86399
     */
    bool EZPI_core_suntime_should_attempt(s_suntime_backoff_t *bo, int tm_mday, int now_sod);

    /**
     * Record that the lookup just failed, and set when the next one may run.
     *
     * Self-contained: it takes the day rather than assuming
     * EZPI_core_suntime_should_attempt ran first, so the two cannot disagree
     * about which day the state belongs to.
     */
    void EZPI_core_suntime_note_failure(s_suntime_backoff_t *bo, int tm_mday, int now_sod);

    /**
     * Record that the lookup succeeded, clearing the backoff.
     *
     * The day is deliberately not touched. It is set by
     * EZPI_core_suntime_should_attempt on the evaluation that led here, and a
     * step of 0 already means nothing is being withheld.
     */
    void EZPI_core_suntime_note_success(s_suntime_backoff_t *bo);

#ifdef __cplusplus
}
#endif

#endif // CONFIG_EZPI_SERV_ENABLE_MESHBOTS
#endif // __EZLOPI_CORE_SCENES_SUNTIME_H__
