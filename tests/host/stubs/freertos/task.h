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

#endif /* _HOST_STUB_FREERTOS_TASK_H_ */
