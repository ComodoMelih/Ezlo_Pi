/* Host stub for freertos/task.h.
 *
 * ezlopi_core_scenes_v2.h includes it unconditionally. Nothing in the scene
 * destructor -- the only scenes translation unit compiled on a host -- calls
 * into the scheduler, so the names are all that is needed. TickType_t is here
 * rather than in FreeRTOS.h to match where FreeRTOS itself declares it, so
 * that a test which one day needs a real tick type knows where to look.
 */
#ifndef _HOST_STUB_FREERTOS_TASK_H_
#define _HOST_STUB_FREERTOS_TASK_H_

#include <stdint.h>

typedef uint32_t TickType_t;

/* A no-op delay and the two spellings of the tick-period macro, added for
 * ezlopi_core_sntp.c: EZPI_core_sntp_init() waits on portTICK_PERIOD_MS and
 * EZPI_core_sntp_get_current_time_sec_abc() on portTICK_RATE_MS, the older
 * name ESP-IDF still provides. Both are needed because both appear in one
 * translation unit.
 *
 * Deliberately a no-op rather than a real sleep: no host test here asserts
 * anything about elapsed time, and a stub that slept would make the suite
 * slower without making it stricter. The consequence is worth naming -- a test
 * cannot use vTaskDelay to let time pass, so anything needing a particular
 * clock reading has to be handed one rather than wait for it.
 *
 * 1 ms per tick is this project's real tick period: all four
 * sdkconfig.defaults.* pin CONFIG_FREERTOS_HZ=1000, so arithmetic a caller
 * does around these macros means the same thing here as on hardware. */
#define portTICK_PERIOD_MS ((TickType_t)1)
#define portTICK_RATE_MS portTICK_PERIOD_MS

static inline void vTaskDelay(TickType_t ticks)
{
    (void)ticks;
}

/* Declared, not defined, and the reason is worth recording: the firmware calls
 * xTaskGetTickCount() from
 * ezlopi_core_scenes_when_methods_helper_functions.c without that file
 * including any header which declares it, so the IDF build only gets a
 * declaration by accident of include order. On a host it shows up as
 * -Wimplicit-function-declaration, and under C99's old rule that would give it
 * a default `int` return -- silently mis-typing the uint32_t tick arithmetic
 * the WHEN_function_for_* family does with the result. This declaration
 * matches FreeRTOS's own, so that arithmetic means the same thing in both
 * builds.
 *
 * No definition here. The callers are the pulse/latch/follow/repeat timing
 * helpers, which no host test drives, so whoever links that translation unit
 * gets an undefined symbol unless they stub it deliberately. That is the
 * correct outcome: it stops a test wandering into the timing logic and passing
 * on the strength of a stub's invented clock. */
TickType_t xTaskGetTickCount(void);

#endif /* _HOST_STUB_FREERTOS_TASK_H_ */
